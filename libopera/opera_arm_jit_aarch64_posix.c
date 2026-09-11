/* ---------------------------------------------------------------------------
 * opera_arm_jit_aarch64_posix.c — AArch64 (AAPCS64) encoding backend for the
 * ARM60 block-JIT, Linux/macOS.
 *
 * OS-NEUTRAL EMITTER: the AAPCS64 ABI is identical on Linux/macOS and
 * Windows (x16/x17 IP0/IP1 scratch, x19-x28 callee-saved, x29/x30
 * frame; x18 is the Windows TEB register and is never used here).
 * The Windows permutation (VirtualAlloc + CFG arena hooks, otherwise
 * byte-identical emitter) lives in opera_arm_jit_aarch64_win.c.
 *
 * ENABLEMENT: see opera_arm_jit_backend.h.  Non-Windows aarch64 is
 * default ON (full lane parity + oracle byte-exact, 2026-08-31).
 * ------------------------------------------------------------------------- */

#include <sys/mman.h>

#ifdef _WIN32
#error "opera_arm_jit_aarch64_posix.c is the Linux/macOS backend; Windows aarch64 must use opera_arm_jit_aarch64_win.c (check opera_arm_jit_backend.h)"
#endif

/* ---------------------------------------------------------------------------
 * (register map / encoding discipline — the canonical header comment
 * above and jit.md's aarch64 sections carry the details)
 * ------------------------------------------------------------------------- */

/* --------------------------- trampoline-level reasons ---------------------- */
#define TRAMP_X_INTERP     100u   /* pc outside the fetch windows        */
#define TRAMP_X_UNALIGNED  101u   /* unaligned pc: C-step one word       */
#define TRAMP_X_COMPILE    102u   /* no block at this pc yet             */

/* --------------------------- emitter state -------------------------------- */

#define OJ_BUF     (32u << 10)
#define OJ_MAXFIX  2048u
#define OJ_MAXLAB  320u

static uint8_t  s_oj[OJ_BUF];
static uint32_t s_oj_len;
static uint32_t s_oj_word_off[JIT_MAX_WORDS];
static uint32_t s_oj_block_pc;
static uint32_t s_oj_lab_entry0;   /* prologue guard trap: C-step word 0 */
static uint32_t s_oj_fix_at[OJ_MAXFIX];
static uint32_t s_oj_fix_lab[OJ_MAXFIX];
static uint8_t  s_oj_fix_cond[OJ_MAXFIX]; /* 0=b 1=b.cond 2=cbz */
static uint32_t s_oj_nfix;
static uint32_t s_oj_lab[OJ_MAXLAB];
static uint32_t s_oj_nlab;

/* A64 registers used by block code (encoding numbers) */
#define OJ_X16  16u
#define OJ_X17  17u
#define OJ_X19  19u      /* &CPU */
#define OJ_W23  23u      /* charge */
#define OJ_W24  24u      /* remaining */

/* CPU member byte offsets (arm_core_t: USER[16] first, CPSR at 176) */
#define OJ_U(n_)   ((uint32_t)(offsetof(arm_core_t, USER) + (4u * (n_))))
#define OJ_U15     (OJ_U(15))
#define OJ_CPSR    ((uint32_t)(offsetof(arm_core_t, CPSR)))
#define OJ_SPSR    ((uint32_t)(offsetof(arm_core_t, SPSR)))

/* A64 condition codes (bits[3:0] of b.cond):
 * 0=eq 1=ne 2=cs/hs 3=cc/lo 4=mi 5=pl 6=vs 7=vc
 * 8=hi 9=ls 10=ge 11=lt 12=gt 13=le 14=al */
#define AA_EQ 0u
#define AA_NE 1u
#define AA_HS 2u
#define AA_LO 3u
#define AA_GE 10u
#define AA_LT 11u
#define AA_LE 13u

/* --------------------------- shared trampoline ---------------------------- */

static uint8_t   *g_jit_tramp_page;
static uintptr_t  g_jit_tramp_entry;
static uintptr_t  g_jit_tramp_xnext;
static uintptr_t  g_jit_tramp_xfiq;
static uintptr_t  g_jit_tramp_xbudget;
static uintptr_t  g_jit_tramp_xrst;
static uintptr_t  g_jit_tramp_xfsm;
static uintptr_t  g_jit_tramp_xcstep;
static uint8_t    t_cur_[8192];
static uint32_t   t_len_;

/* rel32 range flags the shared core computes at init (opera_arm_jit.c).
 * On A64 every trampoline exit goes through the INDIRECT ROUTE, so both
 * are semantically dead — but the core still writes them; they must
 * exist.  Kept 0: nothing reads them on this backend. */
static int       s_oj_abs_ok;
static int       s_oj_rip_ok;

/* --------------------------- word writers (trampoline) --------------------- */

static void t_w(uint32_t const w_)
{
  t_cur_[t_len_ + 0] = (uint8_t)(w_ >>  0);
  t_cur_[t_len_ + 1] = (uint8_t)(w_ >>  8);
  t_cur_[t_len_ + 2] = (uint8_t)(w_ >> 16);
  t_cur_[t_len_ + 3] = (uint8_t)(w_ >> 24);
  t_len_ += 4;
}

static void t_q(uint64_t const v_)
{
  t_cur_[t_len_ + 0] = (uint8_t)(v_ >>  0);
  t_cur_[t_len_ + 1] = (uint8_t)(v_ >>  8);
  t_cur_[t_len_ + 2] = (uint8_t)(v_ >> 16);
  t_cur_[t_len_ + 3] = (uint8_t)(v_ >> 24);
  t_cur_[t_len_ + 4] = (uint8_t)(v_ >> 32);
  t_cur_[t_len_ + 5] = (uint8_t)(v_ >> 40);
  t_cur_[t_len_ + 6] = (uint8_t)(v_ >> 48);
  t_cur_[t_len_ + 7] = (uint8_t)(v_ >> 56);
  t_len_ += 8;
}

/* movz + movk chain (execution-verified materialization: movz at the
 * first nonzero 16-bit part, movk at later nonzero parts, movz #0 for
 * an all-zero constant) */
static void t_movabs(uint32_t const reg_, uint64_t const imm_)
{
  uint32_t hw;
  int      first = 1;

  for(hw = 0; hw < 4; hw++)
    {
      uint16_t const part = (uint16_t)((imm_ >> (16 * hw)) & 0xFFFFu);
      if(first && (part != 0))
        {
          t_w(0xD2800000u | (hw << 21) | ((uint32_t)part << 5) | reg_);
          first = 0;
        }
      else if(!first && (part != 0))
        {
          t_w(0xF2800000u | (hw << 21) | ((uint32_t)part << 5) | reg_);
        }
    }
  if(first)
    t_w(0xD2800000u | reg_);
}

/* intra-trampoline branches: LABEL-ALLOC design, the exact mirror of
 * the block emitter's oj_fix_at/oj_fix_lab machinery.  t_b/t_bcond
 * record a LABEL (not a position); t_lab_here() binds it; one pass at
 * the end resolves every fixup against its bound label.  No sentinel
 * positions, no by-index assignment, no byte-sniffing, no ordering
 * coupling between forward and backward branches, and the OPERA_JIT_STATS
 * shift is structurally impossible (labels are symbolic). */
#define T_MAXFIX 256
#define T_MAXLAB 64
static uint32_t t_fix_at[T_MAXFIX];
static uint32_t t_fix_lab[T_MAXFIX];
static uint8_t  t_fix_cond[T_MAXFIX];   /* 1 = b.cond (cond in low bits) */
static uint32_t t_fix_n;
static uint32_t t_lab[T_MAXLAB];
static uint32_t t_nlab;

static uint32_t t_lab_alloc(void)
{
  return t_nlab++;
}

static void t_lab_here(uint32_t const lab_)
{
  t_lab[lab_] = t_len_;
}

static void t_b(uint32_t const lab_)
{
  if(t_fix_n < T_MAXFIX)
    {
      t_fix_at[t_fix_n]  = t_len_;
      t_fix_lab[t_fix_n] = lab_;
      t_fix_cond[t_fix_n] = 0;
      t_fix_n++;
    }
  t_w(0x14000000u);
}

static void t_bcond(uint32_t const cond_, uint32_t const lab_)
{
  if(t_fix_n < T_MAXFIX)
    {
      t_fix_at[t_fix_n]  = t_len_;
      t_fix_lab[t_fix_n] = lab_;
      t_fix_cond[t_fix_n] = 1;
      t_fix_n++;
    }
  t_w(0x54000000u | (cond_ & 0xFu));
}

/* cbz x17, label — 64-bit pointer test, imm19, label-based like the
 * branches (the x86 `jz -> compile` translation; unsigned pointer). */
static void t_bcbz(uint32_t const lab_)
{
  if(t_fix_n < T_MAXFIX)
    {
      t_fix_at[t_fix_n]  = t_len_;
      t_fix_lab[t_fix_n] = lab_;
      t_fix_cond[t_fix_n] = 2;      /* cbz class */
      t_fix_n++;
    }
  t_w(0xB4000000u | (17u));          /* cbz x17, placeholder */
}

static void t_fixup(void)
{
  uint32_t i;

  for(i = 0; i < t_fix_n; i++)
    {
      uint32_t const at   = t_fix_at[i];
      uint32_t const dst  = t_lab[t_fix_lab[i]];
      int64_t  const d    = ((int64_t)dst) - ((int64_t)at);
      int64_t  const dw   = d / 4;
      uint32_t       *w   = (uint32_t *)(void *)&t_cur_[at];
      uint32_t const cur  = *w;

      if((d & 3) || (dw < -33554432) || (dw > 33554431))
        continue;               /* cannot happen on a 4KB page */

      if(t_fix_cond[i] == 1)
        {
          /* b.cond: imm19 (in words), cond preserved from the placeholder */
          if((dw < -16384) || (dw > 16383))
            continue;
          *w = 0x54000000u | (((uint32_t)dw & 0x7FFFFu) << 5) |
               (cur & 0x1Fu);
        }
      else if(t_fix_cond[i] == 2)
        {
          /* cbz: imm19 (in words), Rt=17 from the placeholder */
          if((dw < -16384) || (dw > 16383))
            continue;
          *w = 0xB4000000u | (((uint32_t)dw & 0x7FFFFu) << 5) |
               (cur & 0x1Fu);
        }
      else
        {
          /* b: imm26 (in words) */
          *w = 0x14000000u | (((uint32_t)dw) & 0x03FFFFFFu);
        }
    }
}

/* arena hooks, forward-declared: the trampoline builder materializes its
 * page via ojb_alloc_arena/ojb_after_commit before their definitions below */
static uint8_t *ojb_alloc_arena(uint32_t const size_);
static void     ojb_after_commit(void *const base_, uint32_t const len_);

/* ---------------------------------------------------------------------------
 * The trampoline — word-for-word translation of tramp_ref.s (assembled,
 * byte-extracted, listed in jit.md's encoding-table session log):
 *
 *   entry: w0 = budget_remaining.  [sp,#96]=remaining, [sp,#104]=budget.
 *   dispatch: pc = ldr w28,[x19,#60]; tst #3 (b.ne unaligned);
 *     lsr w17,w28,#2; cmp vs entries (b.hs interp); ldr x16 = table;
 *     ldr x17,[x16,x17,lsl#3]; cbz -> compile; ldr x17,[x17] = code;
 *     ldr w24,[sp,#96]; movz w23,#0; br x17.
 *   stubs: fold charge (ldr w26; subs w26,w26,w23; str w26) then
 *     mov w0,#reason; b epilog.
 *   X_FIQ: blr arm_fiq_vector (void/void; SP stays 16-aligned — no
 *     x86-style sub/add shim exists to translate).
 *   epilog: g_jit_cycles = budget - remaining; ldp chain; ret.
 *
 * Label positions are captured as they are emitted; forward branches
 * (towards labels not yet reached) are re-patched in the fixup pass
 * over t_fix_*, which carries the final position of every target.
 * ------------------------------------------------------------------------- */


static uint32_t opera_arm_jit_build_trampoline(void)
{
  uint32_t const l_loop           = t_lab_alloc();
  uint32_t const l_slow_unaligned = t_lab_alloc();
  uint32_t const l_slow_interp    = t_lab_alloc();
  uint32_t const l_slow_compile   = t_lab_alloc();
  uint32_t const l_slow_budget    = t_lab_alloc();
  uint32_t const l_epilog         = t_lab_alloc();

  uint32_t l_xnext, l_xfiq;
           uint32_t l_xbudget, l_xrst, l_xfsm, l_xcstep;

  t_len_  = 0;
  t_fix_n = 0;


  /* ------------------------------ entry ------------------------------ */
  t_w(0xA9B97BFDu);              /* stp x29,x30,[sp,#-112]!     */
  t_w(0xA90153F3u);              /* stp x19,x20,[sp,#16]        */
  t_w(0xA9025BF5u);              /* stp x21,x22,[sp,#32]        */
  t_w(0xA90363F7u);              /* stp x23,x24,[sp,#48]        */
  t_w(0xA9046BF9u);              /* stp x25,x26,[sp,#64]        */
  t_w(0xA90573FBu);              /* stp x27,x28,[sp,#80]        */

  t_w(0xB90063E0u);              /* str w0,[sp,#96]  remaining  */
  t_w(0xB9006BE0u);              /* str w0,[sp,#104] budget    */

  t_movabs(19,(uint64_t)(uintptr_t)&CPU);
  t_movabs(16,(uint64_t)(uintptr_t)&g_clio_fiqpend);
  t_w(0xF9400214u);              /* ldr x20,[x16]               */
  t_movabs(16,(uint64_t)(uintptr_t)&g_cdrom_restart_poll);
  t_w(0xF9400215u);              /* ldr x21,[x16]               */
  t_movabs(16,(uint64_t)(uintptr_t)&g_madam_fsm_poll);
  t_w(0xF9400216u);              /* ldr x22,[x16]               */

  /* ------------------------------ dispatch ----------------------------- */
  t_lab_here(l_loop);
  t_w(0xB9403E7Cu);              /* ldr w28,[x19,#60]  pc       */
  t_w(0x7200079Fu);              /* tst w28,#3                  */
  t_bcond(AA_NE, l_slow_unaligned);
  t_w(0x53027F91u);              /* lsr w17,w28,#2              */
  /* Dispatch window must mirror arm_jit_pc_index EXACTLY: the table
   * covers RAM [0,RAM_SIZE) -> [0,ram_words), plus the two ROM banks
   * via XOR-mapped indices (never raw pc>>2).  A raw pc>>2 gate lets
   * ROM/VRAM-range pcs land inside the table range with a -1 window
   * index, arming the compiler with (uint32_t)-1 (wild table walk,
   * fetch_word_at SEGV).  DRAM dispatch stays the fast path; ROM
   * dispatches go through the slow path once per block compile, then
   * hit this same gate -> table walk is index-correct.  This mirrors
   * arm_jit_pc_index's RAM fast path: pc < RAM_SIZE iff pc>>2 <
   * ram_words (RAM_SIZE is a multiple of 4). */
  t_movabs(16,(uint64_t)(uintptr_t)&g_jit_ram_words);
  t_w(0xB9400219u);              /* ldr w25,[x16]  ram_words    */
  t_w(0x6B19023Fu);              /* cmp w17,w25  (unsigned!)    */
  t_bcond(AA_HS, l_slow_interp);
  t_movabs(16,(uint64_t)(uintptr_t)&g_jit_table);
  t_w(0xF9400210u);              /* ldr x16,[x16]  table base   */
  t_w(0xF8717A11u);              /* ldr x17,[x16,x17,lsl#3]     */
  t_bcbz(l_slow_compile);        /* cbz x17,slow_compile        */
  t_w(0xF9400231u);              /* ldr x17,[x17]  code         */
  t_w(0xB94063F8u);              /* ldr w24,[sp,#96] remaining  */
  t_w(0x52800017u);              /* movz w23,#0  charge = 0     */
  t_w(0xD61F0220u);              /* br x17  (never returns)     */

  /* ---------------------------- X_NEXT -------------------------------- */
  l_xnext = t_len_;
  t_w(0xB94063FAu);              /* ldr w26,[sp,#96]            */
  t_w(0x6B17035Au);              /* subs w26,w26,w23  fold      */
  t_w(0xB90063FAu);              /* str w26,[sp,#96]            */
  t_w(0x7100035Fu);              /* cmp w26,#0                   */
  t_bcond(AA_LE, l_slow_budget);
  t_b(l_loop);                   /* -> loop                      */

  /* ---------------------------- X_FIQ -------------------------------- */
  l_xfiq = t_len_;
  t_w(0xB94063FAu);              /* ldr w26,[sp,#96]            */
  t_w(0x6B17035Au);              /* subs w26,w26,w23  fold      */
  t_w(0xB90063FAu);              /* str w26,[sp,#96]            */
  t_movabs(16,(uint64_t)(uintptr_t)&arm_fiq_vector);
  t_w(0xD63F0200u);              /* blr x16                     */
  t_w(0x52800020u);              /* mov w0,#1  JIT_X_FIQ         */
  t_b(l_epilog);



  /* --------------------- plain exit stubs ------------------------------ */
  l_xbudget = t_len_;
  t_w(0xB94063FAu); t_w(0x6B17035Au); t_w(0xB90063FAu);
  t_w(0x52800060u);              /* mov w0,#3  JIT_X_BUDGET      */
  t_b(l_epilog);

  l_xrst = t_len_;
  t_w(0xB94063FAu); t_w(0x6B17035Au); t_w(0xB90063FAu);
  t_w(0x52800040u);              /* mov w0,#2  JIT_X_RST         */
  t_b(l_epilog);

  l_xfsm = t_len_;
  t_w(0xB94063FAu); t_w(0x6B17035Au); t_w(0xB90063FAu);
  t_w(0x52800080u);              /* mov w0,#4  JIT_X_FSM         */
  t_b(l_epilog);

  l_xcstep = t_len_;
  t_w(0xB94063FAu); t_w(0x6B17035Au); t_w(0xB90063FAu);
  t_w(0x528000A0u);              /* mov w0,#5  JIT_X_CSTEP       */
  t_b(l_epilog);

  /* --------------------- slow stubs (dispatch-level) ------------------- */
  t_lab_here(l_slow_interp);
  t_w(0x52800C80u);              /* mov w0,#100 INTERP           */
  t_b(l_epilog);

  t_lab_here(l_slow_unaligned);
  t_w(0x52800CA0u);              /* mov w0,#101 UNALIGNED        */
  t_b(l_epilog);

  t_lab_here(l_slow_compile);
  t_w(0x52800CC0u);              /* mov w0,#102 COMPILE          */
  t_b(l_epilog);

  t_lab_here(l_slow_budget);
  t_w(0x52800060u);              /* mov w0,#3  BUDGET            */
  t_b(l_epilog);

  /* ------------------------------ epilog ------------------------------- */
  t_lab_here(l_epilog);
  t_movabs(16,(uint64_t)(uintptr_t)&g_jit_cycles);
  t_w(0xB94063FAu);              /* ldr w26,[sp,#96] remaining  */
  t_w(0xB9406BFBu);              /* ldr w27,[sp,#104] budget    */
  t_w(0x6B1A037Bu);              /* subs w27,w27,w26 -> total  */
  t_w(0xB900021Bu);              /* str w27,[x16]              */
  t_w(0xA94573FBu);              /* ldp x27,x28,[sp,#80]       */
  t_w(0xA9446BF9u);              /* ldp x25,x26,[sp,#64]       */
  t_w(0xA94363F7u);              /* ldp x23,x24,[sp,#48]       */
  t_w(0xA9425BF5u);              /* ldp x21,x22,[sp,#32]       */
  t_w(0xA94153F3u);              /* ldp x19,x20,[sp,#16]       */
  t_w(0xA8C77BFDu);              /* ldp x29,x30,[sp],#112      */
  t_w(0xD65F03C0u);              /* ret                        */

  t_fixup();

  /* page materialization + icache flush (the memcpy is a commit) */
  g_jit_tramp_page = ojb_alloc_arena(8192);
  if(!g_jit_tramp_page)
    return 1;
  memcpy(g_jit_tramp_page,t_cur_,t_len_);
  ojb_after_commit(g_jit_tramp_page,t_len_);

  g_jit_tramp_entry  = (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xnext = (uintptr_t)l_xnext      + (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xfiq  = (uintptr_t)l_xfiq       + (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xbudget = (uintptr_t)l_xbudget + (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xrst    = (uintptr_t)l_xrst     + (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xfsm    = (uintptr_t)l_xfsm     + (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xcstep  = (uintptr_t)l_xcstep   + (uintptr_t)g_jit_tramp_page;

  return 0;
}

/* ---------------------------------------------------------------------------
 * Emission primitives (block bodies into s_oj).
 * ------------------------------------------------------------------------- */

static void oj_reset(void)
{
  uint32_t i;
  s_oj_len  = 0;
  s_oj_nfix = 0;
  s_oj_nlab = 0;
  for(i = 0; i < OJ_MAXLAB; i++)
    s_oj_lab[i] = 0xFFFFFFFFu;
}

static uint32_t oj_lab_alloc(void)
{
  return s_oj_nlab++;
}

static void oj_lab_here(uint32_t const lab_)
{
  s_oj_lab[lab_] = s_oj_len;
}

/* A64: fixed 4-byte words; the byte stream collapses onto the word API. */
static void oj_u32(uint32_t const w_)
{
  s_oj[s_oj_len + 0] = (uint8_t)(w_ >>  0);
  s_oj[s_oj_len + 1] = (uint8_t)(w_ >>  8);
  s_oj[s_oj_len + 2] = (uint8_t)(w_ >> 16);
  s_oj[s_oj_len + 3] = (uint8_t)(w_ >> 24);
  s_oj_len += 4;
}

/* The x86 backend's byte stream has no A64 counterpart: A64 words are
 * 4 bytes.  The shared core never emits raw bytes itself (oj_u8 use is
 * internal to x86 emitters), so this is a guarded no-op. */
static void oj_u8(uint32_t const b_)
{
  (void)b_;
}

static void oj_u64(uint64_t const v_)
{
  oj_u32((uint32_t)v_);
  oj_u32((uint32_t)(v_ >> 32));
}

/* intra-block b (imm26): placeholder + fixup, range-checked at resolve.
 * Class is recorded EXPLICITLY (0=b) — never sniffed from the opcode:
 * the x86 backend's 0x05 sniff was wrong on A64 (b.cond=0x2A, b=0x0A). */
static void oj_jmp(uint32_t const lab_)
{
  if(s_oj_nfix < OJ_MAXFIX)
    {
      s_oj_fix_at[s_oj_nfix]  = s_oj_len;
      s_oj_fix_lab[s_oj_nfix] = lab_;
      s_oj_fix_cond[s_oj_nfix] = 0u;
      s_oj_nfix++;
    }
  oj_u32(0x14000000u);
}

/* intra-block b.cond (imm19): cond_ is the A64 cond code (0..13) */
static void oj_jcc(uint32_t const cond_, uint32_t const lab_)
{
  if(s_oj_nfix < OJ_MAXFIX)
    {
      s_oj_fix_at[s_oj_nfix]  = s_oj_len;
      s_oj_fix_lab[s_oj_nfix] = lab_;
      s_oj_fix_cond[s_oj_nfix] = 1u;
      s_oj_nfix++;
    }
  oj_u32(0x54000000u | (cond_ & 0xFu));
}

/* the x86 jcc-map translates onto A64 conds directly */
#define OJ_JZ(l)   oj_jcc(AA_EQ, (l))
#define OJ_JNZ(l)  oj_jcc(AA_NE, (l))
#define OJ_JC(l)   oj_jcc(AA_HS, (l))
#define OJ_JNC(l)  oj_jcc(AA_LO, (l))
#define OJ_JA(l)   oj_jcc(8u, (l))   /* hi */
#define OJ_JBE(l)  oj_jcc(9u, (l))   /* ls */
#define OJ_JHS(l)  oj_jcc(AA_HS, (l)) /* cs: unsigned >= */
#define OJ_JLS(l)  oj_jcc(AA_LO, (l)) /* cc: unsigned <  */
#define OJ_JG(l)   oj_jcc(12u, (l))  /* gt */
#define OJ_JL(l)   oj_jcc(AA_LT, (l))
#define OJ_JGE(l)  oj_jcc(AA_GE, (l))
#define OJ_JLE(l)  oj_jcc(AA_LE, (l))

static int oj_resolve(void)
{
  uint32_t i;

  for(i = 0; i < s_oj_nfix; i++)
    {
      uint32_t const at  = s_oj_fix_at[i];
      uint32_t const dst = s_oj_lab[s_oj_fix_lab[i]];
      int64_t  const d   = ((int64_t)dst) - ((int64_t)at);
      uint32_t       *w  = (uint32_t *)(void *)&s_oj[at];
      uint32_t const cur = *w;

      if(dst == 0xFFFFFFFFu)
        return -1;

      if((d & 3))
        return -1;

      if(s_oj_fix_cond[i] == 1u)
        {
          /* b.cond: imm19 (in words), +-1MB */
          int64_t const dw = d / 4;

          if((dw < -16384) || (dw > 16383))
            return -1;
          *w = 0x54000000u | (((uint32_t)dw & 0x7FFFFu) << 5) |
               (cur & 0x1Fu);
        }
      else if(s_oj_fix_cond[i] == 2u)
        {
          /* cbz/cbnz: imm19 (in words) */
          int64_t const dw = d / 4;

          if((dw < -16384) || (dw > 16383))
            return -1;
          *w = (cur & 0xFF00001Fu) | (((uint32_t)dw & 0x7FFFFu) << 5);
        }
      else
        {
          /* b: imm26 (in words), +-128MB */
          int64_t const dw = d / 4;

          if((dw < -33554432) || (dw > 33554431))
            return -1;
          *w = 0x14000000u | (((uint32_t)dw) & 0x03FFFFFFu);
        }
    }

  return 0;
}

/* movz + movk chain into a 64-bit register (same shape as t_movabs) */
static void oj_movabs(uint32_t const reg_, uint64_t const imm_)
{
  uint32_t hw;
  int      first = 1;

  for(hw = 0; hw < 4; hw++)
    {
      uint16_t const part = (uint16_t)((imm_ >> (16 * hw)) & 0xFFFFu);
      if(first && (part != 0))
        {
          oj_u32(0xD2800000u | (hw << 21) | ((uint32_t)part << 5) | reg_);
          first = 0;
        }
      else if(!first && (part != 0))
        {
          oj_u32(0xF2800000u | (hw << 21) | ((uint32_t)part << 5) | reg_);
        }
    }
  if(first)
    oj_u32(0xD2800000u | reg_);
}

/* out-of-block jump into the shared trampoline: INDIRECT ROUTE.
 * ldr x17,[pc,#8] ; br x17 ; .quad target — correct at ANY distance, so
 * oj_abs_patch() is a genuine no-op and arena placement never constrains
 * emission (the x86 rel32 machinery drops away entirely).  Commits are
 * 16-byte aligned so the inline .quad is always aligned. */
static void oj_jmp_abs(uint64_t const addr_)
{
  oj_u32(0x58000051u);            /* ldr x17,[pc,#8]  imm19=2    */
  oj_u32(0xD61F0220u);            /* br x17                     */
  oj_u64(addr_);
}

/* no-op: the indirect route bakes the target inline (see oj_jmp_abs) */
static void oj_abs_patch(uint8_t const *const code_)
{
  (void)code_;
}

/* CPU member access: ldr/str w Rt,[x19,disp] (unsigned offset, imm12
 * scaled by 4 — CPU member offsets are all < 16KB so the form always
 * fits; offsetof(arm_core_t,USER)=0, USER[15]=60, CPSR=176) */
static void oj_ld_cpu(uint32_t const reg_, uint32_t const disp_)
{
  oj_u32(0xB9400000u | ((disp_ >> 2) << 10) | (OJ_X19 << 5) | reg_);
}

static void oj_st_cpu(uint32_t const reg_, uint32_t const disp_)
{
  oj_u32(0xB9000000u | ((disp_ >> 2) << 10) | (OJ_X19 << 5) | reg_);
}

/* 32-bit guest constant into a register: movz/movk w-chain (w-form:
 * 0x52800000 movz / 0x72800000 movk, hw slots 0/1) */
static void oj_mov32(uint32_t const reg_, uint32_t const imm_)
{
  uint32_t const lo = (imm_      ) & 0xFFFFu;
  uint32_t const hi = (imm_ >> 16) & 0xFFFFu;

  if(lo)
    oj_u32(0x52800000u | ((uint32_t)lo << 5) | reg_);
  else
    oj_u32(0x52800000u | reg_);             /* movz w, #0 */

  if(hi)
    oj_u32(0x72800000u | (1u << 21) | (hi << 5) | reg_);
}

/* --------------------------- block prologue --------------------------------
 * Mirrors the x86 oj_prologue: entry guards for the two hoisted poll
 * pointers (rst byte / fsm dword) plus the FIQ hoist when the default
 * g_jit_fiqhoist && !g_jit_dsp_threaded config is active.  Pins are in
 * x20/x21/x22; any pending -> jump to s_oj_lab_entry0 (C-step word 0).
 * ------------------------------------------------------------------------- */
static void oj_prologue(void)
{
  uint32_t const l_ng2      = oj_lab_alloc();
  uint32_t const l_ng3_fall = oj_lab_alloc();

  /* rst poll: ldrb w17,[x21]; cmp #0 (ldr sets NO flags on
   * AArch64 — the explicit cmp is load-bearing!); b.eq ok */
  oj_u32(0x394002B1u);           /* ldrb w17,[x21]              */
  oj_u32(0x7100023Fu);           /* cmp w17,#0  [probe33]       */
  OJ_JZ(l_ng2);
  oj_jmp(s_oj_lab_entry0);
  oj_lab_here(l_ng2);

  /* fsm poll: ldr w17,[x22]; cmp w17,#2 (FSM_INPROCESS); b.ne -> ok */
  oj_u32(0xB94002D1u);           /* ldr w17,[x22]               */
  oj_u32(0x71000A3Fu);           /* cmp w17,#2  [probe33: sets flags] */
  OJ_JNZ(l_ng3_fall);
  oj_jmp(s_oj_lab_entry0);
  oj_lab_here(l_ng3_fall);

  /* FIQ hoist (default ON): ldr w17,[x20]; cbz -> ok */
  if(g_jit_fiqhoist && !g_jit_dsp_threaded)
    {
      uint32_t const l_ng4 = oj_lab_alloc();

      oj_u32(0xB9400291u);       /* ldr w17,[x20]              */
      oj_u32(0x7100023Fu);       /* cmp w17,#0  [probe33]       */
      OJ_JZ(l_ng4);
      oj_jmp(s_oj_lab_entry0);
      oj_lab_here(l_ng4);
    }
}



/* ---------------------------------------------------------------------------
 * Per-word tail: SCYCLE charge + FIQ poll + budget check (the x86 shape
 * exactly; w23 = charge, w24 = remaining).
 * ------------------------------------------------------------------------- */
static void oj_tail(uint32_t const u15_at_tail_)
{
  uint32_t const l_nofiq = oj_lab_alloc();
  uint32_t const l_nobud = oj_lab_alloc();


  oj_u32(0x110006F7u);           /* add w23,w23,#1  SCYCLE      */

  if(!(g_jit_fiqhoist && !g_jit_dsp_threaded))
    {
      /* if(!ISF && *fpend) -> X_FIQ with USER[15]=u15_at_tail_ */
      oj_u32(0xB9400291u);       /* ldr w17,[x20]              */
      oj_u32(0x7100023Fu);      /* cmp w17,#0 (ldr sets no flags!) */
      OJ_JZ(l_nofiq);
      oj_ld_cpu(17u, OJ_CPSR);   /* ldr w17,[x19,#176]         */
      oj_u32(0x721A023Fu);       /* tst w17,#0x40  ISF          */
      OJ_JNZ(l_nofiq);
      oj_mov32(OJ_X17, u15_at_tail_);
      oj_st_cpu(OJ_X17, OJ_U15);
      oj_jmp_abs(g_jit_tramp_xfiq);
      oj_lab_here(l_nofiq);
    }

  if(!g_jit_budbatch)
    {
      oj_u32(0x6B1802FFu);       /* cmp w23,w24: charge<remaining? [probe17] */
      OJ_JL(l_nobud);           /* charge below remaining: run on  */
      oj_mov32(OJ_X17, u15_at_tail_);
      oj_st_cpu(OJ_X17, OJ_U15);
      oj_jmp_abs(g_jit_tramp_xbudget);
    }
  oj_lab_here(l_nobud);
}

/* charge an intra-word extra (beyond the tail's SCYCLE) */
static void oj_charge_extra(uint32_t const extra_)
{
  if(!extra_)
    return;

  oj_u32(0x11000000u | ((uint32_t)extra_ << 10) |
         (OJ_W23 << 5) | OJ_W23);   /* add w23,w23,#extra */
}

static void oj_exit_next(uint32_t const u15_)
{
  oj_mov32(OJ_X17, u15_);
  oj_st_cpu(OJ_X17, OJ_U15);
  oj_jmp_abs(g_jit_tramp_xnext);
}

static void oj_exit_cstep(uint32_t const word_pc_)
{
  oj_mov32(OJ_X17, word_pc_);
  oj_st_cpu(OJ_X17, OJ_U15);
  oj_jmp_abs(g_jit_tramp_xcstep);
}

/* ---------------------------------------------------------------------------
 * Per-word emitters — MILESTONE 0: every class returns 0 (C-step
 * boundary).  The trampoline/ABI/arena/icache/dispatch path is what the
 * oracle validates; per-class emitters land next, class by class.
 * ------------------------------------------------------------------------- */
#define OJ_NO_COND 0xFFFFFFFFu

/* ---------------------------------------------------------------------------
 * Conditional-guard emission (the oj_cond_prefix translation).
 * x86: mov eax,[CPSR]; shr 28; mov edx,cross[cond]; bt edx,eax; jnc skip.
 * A64 (baked-entry design — selftest6-verified over all 256 verdicts):
 *   ldr  w17,[x19,#CPSR]
 *   lsr  w17,w17,#28          0x531C7E31  [probe10]
 *   movz w28,#cross[cond]     0x52800000|entry<<5|28  (cond is
 *                               compile-time per word; the 16-bit truth
 *                               table entry bakes in one word)
 *   lsrv w17,w28,w17          0x1AD12791  [probe10: bit(nibble) of entry]
 *   tst  w17,#1               0x7200023F  [probe9]
 *   b.eq l_skip               (verdict 0 -> cond false)
 * (The first design indexed the table BY NIBBLE and tested bit0 — an
 * advisory decode caught it computing cross[nibble]&1, meaningless;
 * the corrected design was execution-verified by selftest6.)
 * ------------------------------------------------------------------------- */
static uint32_t oj_word_cond(uint32_t const cmd_)
{
  uint32_t const cond = (cmd_ >> 28);
  uint32_t const l_skip = oj_lab_alloc();
  uint16_t      const entry = cond_flags_cross[cond];

  if(cond == 0xEu)
    return OJ_NO_COND;

  /* cond is COMPILE-TIME per word: bake the 16-bit truth table entry
   * (movz, one word) and use the runtime cpsr nibble as the VARIABLE
   * shift amount.  bit(nibble) of the entry is the verdict:
   *   ldr  w17,[x19,#CPSR]
   *   lsr  w17,w17,#28                 (0x531C7E31)
   *   movz w28,#entry                  (0x52800000|entry<<5|28)
   *   lsrv w17,w28,w17                 (0x1AD12791: w17=entry>>nib)
   *   tst  w17,#1                      (0x7200023F)
   *   b.eq l_skip                      (verdict 0 -> cond false)   */
  oj_ld_cpu(17u, OJ_CPSR);
  oj_u32(0x531C7E31u);               /* lsr w17,w17,#28            */
  oj_u32(0x52800000u | (((uint32_t)entry) << 5) | 28u);   /* movz w28 */
  oj_u32(0x1AD12791u);               /* lsrv w17,w28,w17           */
  oj_u32(0x7200023Fu);               /* tst w17,#1                 */
  OJ_JZ(l_skip);
  return l_skip;
}

static void oj_word_cond_close(uint32_t const l_skip_)
{
  if(l_skip_ != OJ_NO_COND)
    oj_lab_here(l_skip_);
}

static int oj_word_cond_is_nv(uint32_t const cmd_)
{
  return (cond_flags_cross[cmd_ >> 28] == 0);
}

/* ---------------------------------------------------------------------------
 * DP machinery (mirrors the x86 shapes; w8=op1, w9=op2, w10=result,
 * w27/w28 scratch, w17 common scratch; x19=&CPU never clobbered).
 * All encodings extracted from probe6/8/9/10 + the verified table.
 * ------------------------------------------------------------------------- */

/* lsr w17,w17,#28 (nibble extract) = 0x531C7E31  [probe10]
 * lsrv w17,w28,w17                  = 0x1AD12791  [probe10]
 * movz w28,#imm16                   = 0x52800000|imm<<5|28
 * tst w17,#1                        = 0x7200023F  [probe9]
 * ldrh w17,[x16]                    = 0x79400211  [probe9]
 * ldr w17,[x16,w17,uxtw#2]          = 0xB8715A11  [probe9]
 */

/* ---------------------------------------------------------------------------
 * DP machinery (mirrors the x86 shapes; w8=op1, w9=op2, w10=result,
 * w27/w28 scratch, w17 common scratch; x19=&CPU never clobbered).
 * EVERY constant below is EXTRACTED from probe10/probe11 output — the
 * derivation-based scaffold that was here first had 9 wrong words
 * (Rn=27 decodes, an imm-form/register-form mixup, an imm6=14 lsl#14
 * posing as lsl#28, and a movz-class constant posing as cset).
 * ------------------------------------------------------------------------- */

/* w17 = ARM C (CPSR bit 29):
 *   ldr w17,[x19,#CPSR] ; lsr w17,w17,#29
 *   lsr w17,w17,#29 = 0x531D7E31  [probe11: Rd=17 Rn=17 immr=29 imms=31] */
static void oj_get_arm_c_w17(void)
{
  oj_ld_cpu(17u, OJ_CPSR);
  oj_u32(0x531D7E31u);               /* lsr w17,w17,#29            */
}

/* carry_out (w17 = 0/1) -> C bit: CPSR = (CPSR & ~C) | (carry<<29).
 * x86: oj_and_cpu(CPSR,0xdfffffff) + oj_shl_r10(29) + oj_or_cpu_r10.
 * A64:
 *   ldr  w28,[x19,#CPSR]
 *   and  w28,w28,#0xdfffffff   0x12027B9C  [probe11]
 *   lsl  w17,w17,#29           0x53030A31  [probe11]
 *   orr  w28,w28,w17           0x2A11039C  [probe11]
 *   str  w28,[x19,#CPSR]                                             */
static void oj_arm_set_c_w17(void)
{
  oj_ld_cpu(28u, OJ_CPSR);
  oj_u32(0x12027B9Cu);               /* and w28,w28,#0xdfffffff    */
  oj_u32(0x53030A31u);               /* lsl w17,w17,#29            */
  oj_u32(0x2A11039Cu);               /* orr w28,w28,w17            */
  oj_st_cpu(28u, OJ_CPSR);
}

/* ARM_SET_ZN(result in w10): N = bit31, Z = (r==0); C/V preserved.
 * x86: oj_arm_set_zn. A64:
 *   ldr  w28,[x19,#CPSR]
 *   and  w28,w28,#0x3fffffff   0x1200779C  [probe11: clear N,Z]
 *   and  w17,w10,#0x80000000  0x12010151  [probe11: N mask]
 *   cmp  w10,#0               0x7100015F  [probe11: Z from result]
 *   cset w27,eq               0x1A9F17FB  [probe11]
 *   lsl  w27,w27,#30          0x5302077B  [probe11]
 *   orr  w28,w28,w17          0x2A11039C  [probe11]
 *   orr  w28,w28,w27          0x2A1B039C  [probe11]
 *   str  w28,[x19,#CPSR]                                             */
static void oj_arm_set_zn_w10(void)
{
  oj_ld_cpu(28u, OJ_CPSR);
  oj_u32(0x1200779Cu);               /* and w28,w28,#0x3fffffff    */
  oj_u32(0x12010151u);               /* and w17,w10,#0x80000000    */
  oj_u32(0x7100015Fu);               /* cmp w10,#0                  */
  oj_u32(0x1A9F17FBu);               /* cset w27,eq                 */
  oj_u32(0x5302077Bu);               /* lsl w27,w27,#30             */
  oj_u32(0x2A11039Cu);               /* orr w28,w28,w17             */
  oj_u32(0x2A1B039Cu);               /* orr w28,w28,w27             */
  oj_st_cpu(28u, OJ_CPSR);
}

/* NZCV-from-host-flags fold (after adds/subs set them):
 * x86: pushfq/pop + nibble assembly. A64 is one register read:
 *   mrs  x17,nzcv             0xD53B4211  [probe11: Rt=17]
 *   lsr  w17,w17,#28          0x531C7E31  [probe10: Rd=17 Rn=17 immr=28]
 *   ldr  w28,[x19,#CPSR]
 *   and  w28,w28,#0xcfffffff  0x1202779C  [probe11: keep N,Z only]
 *   orr  w28,w28,w17,lsl#28   0x2A11739C  [probe11]
 *   str  w28,[x19,#CPSR]                                             */
static void oj_arm_set_cvzn_fold(void)
{
  oj_u32(0xD53B4211u);               /* mrs x17,nzcv               */
  oj_u32(0x531C7E31u);               /* lsr w17,w17,#28            */
  oj_ld_cpu(28u, OJ_CPSR);
  oj_u32(0x12006F9Cu);               /* and w28,w28,#0x0fffffff [probe42]:
                                        clear N,Z,C,V — the OR below
                                        must fully replace the nibble,
                                        0xcfffffff kept stale N/Z */
  oj_u32(0x2A11739Cu);               /* orr w28,w28,w17,lsl#28     */
  oj_st_cpu(28u, OJ_CPSR);
}

/* ---------------------------------------------------------------------------
 * The DP emitter (TIGHT_DP_IMM / TIGHT_DP_RI / TIGHT_DP_RS / _PC /
 * _PCREL / _RMPC shared driver — the oj_emit_dp translation).
 *
 * Register conventions (all encodings probe12-verified):
 *   w8  = op1 (USER[rn] or pc_k+8 const)   w9  = op2
 *   w10 = result                            w17/w27/w28 scratch
 *   x19 = &CPU untouched throughout.
 *
 * ALU words (base | Rm<<16 | Rn<<5 | Rd):
 *   and=0x0A000000 eor=0x4A000000 sub=0x4B000000 add=0x0B000000
 *   adc=0x1A000000 sbc=0x5A000000 orr=0x2A000000 bic=0x0A200000
 *   ands=0x6A000000 cmp=0x6B000000 cmn=0x2B000000
 *   mvn w17,w8 = orn w17,wzr,w8 = 0x2A200000|(8<<16)|(31<<5)|17
 *   mov w10,w9 = orr w10,wzr,w9 = 0x2A000000|(9<<16)|(31<<5)|10
 * ------------------------------------------------------------------------- */
static int oj_emit_dp(uint32_t const cmd_, uint32_t const aux_,
                      uint32_t const pc_k_, uint32_t const cls_)
{
  if(getenv("OPERA_JIT_NO_DP")) return 0;   /* diag bisect */
  uint32_t l_skip;
  uint32_t rn;
  uint32_t const rd  = ((cmd_ >> 12) & 0xF);
  uint32_t const opc5 = ((cmd_ >> 20) & 0x1F);
  uint32_t const opcode = (opc5 >> 1);
  int      const s_bit  = (int)(opc5 & 1);
  int      const logic  = is_logic[opcode];

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);

  /* ---------------- MRS (opc5 16/20 alias) ----------------
   * ARM_ALU_Exec cases 16/20: USER[rd] = bit22 ? SPSR[arm_
   * mode_table[CPSR & 0x1F]] : CPSR.  The generic opcode
   * switch below would mis-map opc5 20 to CMP (opcode 10) —
   * handle MRS here, mirroring the x86 oj_alu prologue.  MSR
   * (18/22) never reaches the tight classes (decode filters).
   * Words [probe66]:
   *   and  w9,w9,#0x1F        12001129
   *   ldrb w9,[x16,w9,uxtw]   38694A09
   *   lsl  w28,w9,#2          531E753C
   *   ldr  w9,[x19,w28,uxtw]  B87C4A69  (SPSR base via w28) */
  if((opc5 == 16u) || (opc5 == 20u))
    {
      if((cmd_ >> 22) & 1)
        {
          oj_ld_cpu(9u, OJ_CPSR);
          oj_u32(0x12001129u);   /* and w9,w9,#0x1F            */
          oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&arm_mode_table[0]);
          oj_u32(0x38694A09u);   /* ldrb w9,[x16,w9,uxtw]     */
          oj_u32(0x531E753Cu);   /* lsl w28,w9,#2             */
          oj_u32(0x11000000u | ((OJ_SPSR) << 10) | (28u << 5) | 28u);
                                  /* add w28,w28,#SPSR          */
          oj_u32(0xB87C4A69u);   /* ldr w9,[x19,w28,uxtw]     */
        }
      else
        oj_ld_cpu(9u, OJ_CPSR);
      oj_st_cpu(9u, OJ_U(rd));
      oj_charge_extra(0);
      oj_word_cond_close(l_skip);
      oj_tail(pc_k_ + 4);
      return 1;
    }


  /* ---------------- op2 into w9 ---------------- */
  if((cls_ == ARM_CLS_TIGHT_DP_IMM) ||
     (((cls_ == ARM_CLS_TIGHT_DP_PC) || (cls_ == ARM_CLS_TIGHT_DP_PCREL)) &&
      (cmd_ & (1u << 25))))
    {
      /* immediate + rot: op2 = ROR(imm8, rot*2).  rot is compile-time:
       * materialize imm8 then ror by a CONSTANT via the variable-shift
       * reg? Simpler + exact: movz w9,#imm8 ; ror w9,w9,rot2 (imm form
       * only allows 1..31 — rot2 can be 0 or any even up to 30).  For
       * rot2 == 0 skip.  imm8 <= 255 fits movz.
       * ror w9,w9,#imm = 0x13800000-class — probe13 pending; use the
       * verified variable-shift instead via a w28 hold. */
      oj_u32(0x52800000u | (((uint32_t)(cmd_ & 0xFF)) << 5) | 9u);
      { uint32_t const rot2 = ((cmd_ >> 7) & 0x1E);
        if(rot2)
          {
            oj_u32(0x13890129u | (rot2 << 10));  /* ror w9,w9,#rot2 */
            if(s_bit && logic)
              /* ROR carry = result bit31 (== pre-shift bit(rot2-1)):
               * lsr w17,w9,#31 = 0x531F7D31 [probe77: the encoding is
               * 0x53100000|(31<<10)|(9<<5)|17 — the old word 0x531F7E31
               * had Rn=17 (read stale w17, not the rotated w9!) and
               * produced carry=old-C instead of the rotated bit31] */
              oj_u32(0x531F7D31u);
          }
        else if(s_bit && logic)
          oj_get_arm_c_w17();      /* rot2==0: carry = current C */
      }
    }
  else if(cls_ == ARM_CLS_TIGHT_DP_RMPC)
    {
      /* rm == 15: op2 = pc_k + 8 (const), static shift per aux_ */
      oj_mov32(9u, pc_k_ + 8u);
      if(s_bit && logic)
        oj_u32(0x2A0903FBu);     /* mov w27,w9 — save pre-shift [probe15] */
      { uint32_t const sh  = (aux_ & 0x3F);
        uint32_t const typ = ((aux_ >> 8) & 7);
        if((typ == 0) && (sh != 0) && (sh < 32))   /* LSL */
          {
            oj_u32(0x53000000u | (((32u - sh) & 31u) << 16) |
                   ((31u - sh) << 10) | (9u << 5) | 9u);
            if(s_bit && logic)   /* carry = bit(32-sh) of original */
              oj_u32(0x53000000u | ((32u - sh) << 16) | (31u << 10) |
                     (27u << 5) | 17u);  /* lsr w17,w27,#(32-sh) */
          }
        else if((typ == 1) && (sh != 0) && (sh < 32))    /* LSR */
          {
            oj_u32(0x53000000u | (sh << 16) | (31u << 10) | (9u << 5) | 9u);
            if(s_bit && logic)   /* carry = bit(sh-1) */
              oj_u32(0x53000000u | ((sh - 1u) << 16) | (31u << 10) |
                     (27u << 5) | 17u);  /* lsr w17,w27,#(sh-1) */
          }
        else if((typ == 2) && (sh != 0) && (sh < 32))    /* ASR */
          {
            oj_u32(0x13000000u | (sh << 16) | (31u << 10) |
                   (9u << 5) | 9u);
            if(s_bit && logic)   /* carry = bit(sh-1) */
              oj_u32(0x53000000u | ((sh - 1u) << 16) | (31u << 10) |
                     (27u << 5) | 17u);
          }
        else if((typ == 3) && (sh != 0))                  /* ROR */
          {
            oj_u32(0x13890129u | (sh << 10));
            if(s_bit && logic)   /* carry = result bit31 */
              oj_u32(0x531F7D31u);   /* lsr w17,w9,#31 [probe77: the old word
                                      * 531F7E31 read stale w17 — Rn must be 9] */
          }
        else
          { oj_word_cond_close(l_skip); return 0; }  /* RRX/edge: C-step */
      }
    }
  else
    {
      oj_ld_cpu(9u, OJ_U(cmd_ & 0xF));        /* w9 = USER[rm] */
      if((cls_ == ARM_CLS_TIGHT_DP_RI) ||
         (cls_ == ARM_CLS_TIGHT_DP_PC) || (cls_ == ARM_CLS_TIGHT_DP_PCREL))
        {
          /* register + static shift (aux_ holds promoted shift) */
          uint32_t const sh  = (aux_ & 0x3F);
          uint32_t const typ = ((aux_ >> 8) & 7);
          if((s_bit && logic) && ((sh != 0) || (typ != 0)))
            oj_u32(0x2A0903FBu);   /* mov w27,w9 — save pre-shift [probe15] */
          if((sh != 0) || (typ != 0))
            {
              /* promoted static shift: sh may be 0->32 per the classifier */
              if((typ == 0) && (sh > 0) && (sh < 32))        /* LSL */
                {
                  oj_u32(0x53000000u | (((32u - sh) & 31u) << 16) |
                         ((31u - sh) << 10) | (9u << 5) | 9u);
                  if(s_bit && logic)   /* carry = bit(32-sh) */
                    oj_u32(0x53000000u | ((32u - sh) << 16) | (31u << 10) |
                           (27u << 5) | 17u);  /* lsr w17,w27,#(32-sh) */
                }
              else if((typ == 1) && (sh > 0) && (sh < 32))   /* LSR */
                {
                  oj_u32(0x53000000u | (sh << 16) | (31u << 10) |
                         (9u << 5) | 9u);
                  if(s_bit && logic)   /* carry = bit(sh-1) */
                    oj_u32(0x53000000u | ((sh - 1u) << 16) | (31u << 10) |
                           (27u << 5) | 17u);  /* lsr w17,w27,#(sh-1) */
                }
              else if((typ == 2) && (sh > 0) && (sh < 32))   /* ASR */
                {
                  oj_u32(0x13000000u | (sh << 16) | (31u << 10) |
                         (9u << 5) | 9u);
                  if(s_bit && logic)   /* carry = bit(sh-1) */
                    oj_u32(0x53000000u | ((sh - 1u) << 16) | (31u << 10) |
                           (27u << 5) | 17u);
                }
              else if((typ == 3) && (sh > 0))                /* ROR */
                {
                  oj_u32(0x13890129u | (sh << 10));
                  if(s_bit && logic)   /* carry = result bit31 */
                    oj_u32(0x531F7D31u);   /* lsr w17,w9,#31 [probe77: old word
                                      * 531F7E31 read stale w17 — Rn must be 9] */
                }
              else if((typ == 4))                            /* RRX */
                { oj_word_cond_close(l_skip); return 0; }
              else if((sh == 32) && (typ == 0))    /* LSL 32: val 0, carry bit0 */
                {
                  oj_u32(0x52800000u | (9u));      /* movz w9,#0 */
                  if(s_bit && logic)
                    oj_u32(0x12000771u);  /* and w17,w27,#1 [probe16]:
                                           LSL32 carry = bit0 of original */
                }
              else if((sh == 32) && (typ == 1))    /* LSR 32: val 0, carry bit31 */
                {
                  oj_u32(0x52800000u | (9u));
                  if(s_bit && logic)
                    oj_u32(0x531F7F71u);   /* lsr w17,w27,#31 [probe15] */
                }
              else if((sh == 32) && (typ == 2))    /* ASR 32: sign, carry bit31 */
                {
                  oj_u32(0x13000000u | (31u << 16) | (31u << 10) |
                         (9u << 5) | 9u);
                  if(s_bit && logic)
                    oj_u32(0x531F7F71u);   /* lsr w17,w27,#31 [probe15] */
                }
              else
                { oj_word_cond_close(l_skip); return 0; }
            }
          else if(s_bit && logic)
            oj_get_arm_c_w17();   /* no shift: carry = current C */
        }
      else
        {
          /* register shift: amount = USER[rs] & 0xFF at runtime.
           * ARM semantics differ from A64 variable shifts at the
           * 0/32/>32 boundaries (A64 masks the amount mod 32), so
           * the exact ARM_SHIFT_NSC boundary walk is emitted — the
           * oj_shift_reg translation.  All words probe69/70/71-
           * verified; register roles: w9 = value (from USER[rm],
           * loaded above), w27 = pre-shift copy, w28 = amount,
           * w17 = carry_out.  The carry lands in w17 exactly like
           * the static-shift routes above, so the S+logic store
           * (oj_arm_set_c_w17) picks it up unchanged. */
          uint32_t const l_s0   = oj_lab_alloc();
          uint32_t const l_big  = oj_lab_alloc();
          uint32_t const l_s32  = oj_lab_alloc();
          uint32_t const l_done = oj_lab_alloc();
          uint32_t const l_r0   = oj_lab_alloc();
          uint32_t const t      = ((cmd_ >> 5) & 3u);

          oj_u32(0x2A0903FBu);   /* mov w27,w9 — save pre-shift [probe72] */
          oj_ld_cpu(28u, OJ_U((cmd_ >> 8) & 0xF));  /* w28 = USER[rs]  */
          oj_u32(0x12001F9Cu);   /* and w28,w28,#0xff (rs & 0xFF)       */

          oj_u32(0x7100039Fu);   /* cmp w28,#0                          */
          OJ_JZ(l_s0);

          if((t == 0) || (t == 1))
            {
              /* LSL / LSR */
              oj_u32(0x7100839Fu);   /* cmp w28,#32                      */
              OJ_JA(l_big);           /* unsigned >                       */
              OJ_JZ(l_s32);
              if(t == 0)
                {
                  oj_u32(0x1ADC2129u); /* lslv w9,w9,w28                 */
                  /* carry = bit(32-s) of original: neg w17,w28 ;
                   * lsrv w17,w27,w17 ; and w17,w17,#1                   */
                  oj_u32(0x4B1C03F1u); /* neg w17,w28                     */
                  oj_u32(0x1AD12771u); /* lsrv w17,w27,w17                */
                  oj_u32(0x12000231u); /* and w17,w17,#1                  */
                }
              else
                {
                  oj_u32(0x1ADC2529u); /* lsrv w9,w9,w28                  */
                  /* carry = bit(s-1): sub w17,w28,#1 ; lsrv w17,w27,w17 ;
                   * and w17,w17,#1                                      */
                  oj_u32(0x51000791u); /* sub w17,w28,#1                  */
                  oj_u32(0x1AD12771u); /* lsrv w17,w27,w17                */
                  oj_u32(0x12000231u); /* and w17,w17,#1                  */
                }
              oj_jmp(l_done);

              oj_lab_here(l_s32);
              if(t == 0)
                oj_u32(0x12000371u);  /* and w17,w27,#1 (carry = bit0)    */
              else
                oj_u32(0x531F7F71u);  /* lsr w17,w27,#31 (carry = bit31)  */
              oj_u32(0x52800009u);    /* movz w9,#0 (val = 0)             */
              oj_jmp(l_done);

              oj_lab_here(l_big);
              oj_u32(0x52800011u);    /* movz w17,#0 (carry = 0)          */
              oj_u32(0x52800009u);    /* movz w9,#0 (val = 0)             */
              oj_jmp(l_done);
            }
          else if(t == 2)
            {
              /* ASR: s <= 31 normal; s >= 32 sign-fill, carry = sign.
               * [fix] the boundary test was JLE (s<=31 -> sign-fill!)
               * — inverted: every in-range ASR collapsed to the
               * sign-fill path. JG: only s>31 takes the fill. */
              oj_u32(0x71007F9Fu);   /* cmp w28,#31                        */
              OJ_JG(l_big);
              oj_u32(0x1ADC2929u);   /* asrv w9,w9,w28                     */
              /* carry = bit(s-1) */
              oj_u32(0x51000791u);   /* sub w17,w28,#1                     */
              oj_u32(0x1AD12771u);   /* lsrv w17,w27,w17                   */
              oj_u32(0x12000231u);   /* and w17,w17,#1                     */
              oj_jmp(l_done);

              oj_lab_here(l_big);
              oj_u32(0x531F7F71u);   /* lsr w17,w27,#31 (carry = sign)    */
              oj_u32(0x131F7F69u);   /* asr w9,w27,#31 (val = sign-fill)  */
              oj_jmp(l_done);
            }
          else
            {
              /* ROR: s&31 != 0 rotate; s&31 == 0 (s>0) val unchanged,
               * carry = v>>31 — matches the x86 'test cl,31' shape and
               * the ARM_SHIFT_NSC case-3 semantics (shift_&=31 first,
               * carry from the FULL shift-1, whose low 5 bits equal the
               * result bit31 after the masked rotate) */
              oj_u32(0x12001391u);   /* and w17,w28,#31                    */
              oj_u32(0x34000051u);   /* cbz w17,l_r0                       */
              oj_u32(0x1ADC2D29u);   /* rorv w9,w9,w28                     */
              oj_u32(0x531F7D31u);   /* lsr w17,w9,#31 (carry = bit31 of
                                      * the rotated result)                */
              oj_jmp(l_done);

              oj_lab_here(l_r0);
              oj_u32(0x531F7F71u);   /* lsr w17,w27,#31 (carry; val kept)  */
              oj_jmp(l_done);
            }

          oj_lab_here(l_s0);
          oj_get_arm_c_w17();        /* carry = current C; val unchanged  */
          oj_lab_here(l_done);
        }
    }

  /* ---------------- op1 into w8 ---------------- */
  rn = ((cmd_ >> 16) & 0xF);
  if(cls_ == ARM_CLS_TIGHT_DP_PCREL)
    oj_mov32(8u, pc_k_ + 8u);                 /* op1 = pc_k + 8 */
  else
    oj_ld_cpu(8u, OJ_U(rn));                  /* w8 = USER[rn] */

  /* ---------------- shifter carry into C for S+logic ----------------
   * ARM_SHIFT_NSC carry for the promoted static shift that produced w9
   * (op2 pre-shift value still needed — the shift words above already
   * consumed it, so the carry bit is computed from the shifted w9 and
   * the shift parameters exactly as ARM_SHIFT_NSC does):
   *   rot2 (IMM): carry = bit(rot2-1) of the UNshifted imm8 — must be
   *     computed BEFORE the ror (extract bit before rotating).
   *   LSL sh: carry = bit(32-sh) of original; LSR sh: bit(sh-1);
   *   ASR sh: bit(sh-1); ROR sh: bit(sh-1); RRX: bit0.
   * The x86 backend computes this inside oj_shift_static via cf_r10.
   * A64: extract the bit into w17 with a variable-shift of a SAVED
   * copy.  To keep the emitted sequence exact, the op2 routes above
   * save the pre-shift value in w27 when S+logic needs it. */
  if(s_bit && logic)
    oj_arm_set_c_w17();   /* w17 = shifter carry (routes below set it) */

  /* ---------------- ALU ---------------- */
  switch(opcode)
    {
    case 0:  /* AND */
      oj_u32(0x0A09010Au);           /* and w10,w8,w9 */
      break;
    case 1:  /* EOR */
      oj_u32(0x4A09010Au);           /* eor w10,w8,w9 */
      break;
    case 2:  /* SUB */
      oj_u32(s_bit ? 0x6B09010Au : 0x4B09010Au);  /* subs/sub [probe38] */
      break;
    case 3:  /* RSB */
      oj_u32(s_bit ? 0x6B08012Au : 0x4B08012Au);  /* subs/sub [probe38] */
      break;
    case 4:  /* ADD */
      oj_u32(s_bit ? 0x2B09010Au : 0x0B09010Au);  /* adds/add [probe38] */
      break;
    case 5:  /* ADC */
      /* borrow/carry-in: adcs reads the HOST C — materialize the
       * guest CPSR NZCV into the host first (the x86's oj_bt_cpsr_c
       * twin).  Guest CPSR bits 31-28 are exactly N,Z,C,V, and msr
       * nzcv only consumes the top nibble, so ldr+msr is exact.
       * [probe64: msr nzcv,x17 = D51B4211] */
      oj_ld_cpu(17u, OJ_CPSR);
      oj_u32(0xD51B4211u);         /* msr nzcv,x17                    */
      oj_u32(s_bit ? 0x3A09010Au : 0x1A09010Au);  /* adcs/adc [probe38] */
      break;
    case 6:  /* SBC */
      oj_ld_cpu(17u, OJ_CPSR);
      oj_u32(0xD51B4211u);         /* msr nzcv,x17                    */
      oj_u32(s_bit ? 0x7A09010Au : 0x5A09010Au);  /* sbcs/sbc [probe38] */
      break;
    case 7:  /* RSC */
      oj_ld_cpu(17u, OJ_CPSR);
      oj_u32(0xD51B4211u);         /* msr nzcv,x17                    */
      oj_u32(s_bit ? 0x7A08012Au : 0x5A08012Au);  /* sbcs/sbc [probe38] */
      break;
    case 8:  /* TST: compute into w10 — the S+logic zn fold reads w10 */
      oj_u32(0x0A09010Au);           /* and w10,w8,w9 (case-0 word) */
      break;
    case 9:  /* TEQ: compute into w10 — the S+logic zn fold reads w10 */
      oj_u32(0x4A09010Au);           /* eor w10,w8,w9 (case-1 word) */
      break;
    case 10: /* CMP: subs wzr,w8,w9 — flags set, result discarded */
      oj_u32(0x6B09011Fu);           /* cmp w8,w9 [probe38-class] */
      break;
    case 11: /* CMN: adds wzr,w8,w9 — flags set */
      oj_u32(0x2B09011Fu);           /* cmn w8,w9 [probe38-class] */
      break;
    case 12: /* ORR */
      oj_u32(0x2A09010Au);           /* orr w10,w8,w9 */
      break;
    case 13: /* MOV */
      oj_u32(0x2A0903EAu);           /* mov w10,w9 */
      break;
    case 14: /* BIC */
      oj_u32(0x0A29010Au);           /* bic w10,w8,w9 */
      break;
    case 15: /* MVN */
      /* MVN rd,op2 = ~op2 — the operand is w9 (op2), NOT w8
       * (op1).  [probe68: mvn w17,w9 = 2A2903F1] — the old
       * 2A2803F1 inverted w8 and computed ~USER[rn]. */
      oj_u32(0x2A2903F1u);           /* mvn w17,w9 */
      oj_u32(0x2A1103EAu);           /* mov w10,w17 = orr w10,wzr,w17 [probe13] */
      break;
    default:
      oj_word_cond_close(l_skip);
      return 0;
    }

  /* ---------------- flags + writeback ---------------- */
  if(s_bit)
    {
      if(logic)
        oj_arm_set_zn_w10();
      else
        oj_arm_set_cvzn_fold();      /* adds/subs host flags — see NOTE */
    }

  if((opcode != 8) && (opcode != 9) && (opcode != 10) && (opcode != 11))
    oj_st_cpu(10u, OJ_U(rd));        /* USER[rd] = result */

  /* rd == 15 (pc write, e.g. `mov pc, #imm`): S=0 forms are fully
   * inlineable.  The generic path above already stored the result
   * into USER[15]; the word terminates the block with the taken
   * tail reading that dynamic pc straight through the trampoline
   * stubs (they re-read USER[15]), never materialising an imm32
   * u15 that would clobber it.  S=1 (subs pc, ...) restores CPSR
   * from SPSR with a bank switch: those stay full-class C words.
   * Cost: ICYCLE + NCYCLE on top of the tail (arm_dp_tail's
   * `cycp -= (ICYCLE+NCYCLE)` under the -SCYCLE base).  [x86
   * parity: see oj_emit_dp's rd==15 branch] */
  if(rd == 15 && !s_bit)
    {
      oj_charge_extra(ICYCLE + NCYCLE);
      {
        /* the oj_tail shape WITHOUT the u15 materialization (the
         * ALU result already landed in USER[15] and must survive);
         * the taken path jumps straight to X_NEXT. */
        uint32_t const l_nofiq = oj_lab_alloc();
        uint32_t const l_nobud = oj_lab_alloc();


        oj_u32(0x110006F7u);       /* add w23,w23,#1  SCYCLE        */
        oj_u32(0xB9400291u);       /* ldr w17,[x20]                 */
        oj_u32(0x7100023Fu);      /* cmp w17,#0 (ldr sets no flags!) */
        OJ_JZ(l_nofiq);
        oj_ld_cpu(17u, OJ_CPSR);   /* ldr w17,[x19,#CPSR]           */
        oj_u32(0x721A023Fu);      /* tst w17,#0x40  ISF            */
        OJ_JNZ(l_nofiq);
        oj_jmp_abs(g_jit_tramp_xfiq);
        oj_lab_here(l_nofiq);
        if(!g_jit_budbatch)
          {
            oj_u32(0x6B1802FFu);   /* cmp w23,w24                   */
            OJ_JL(l_nobud);
            oj_jmp_abs(g_jit_tramp_xbudget);
          }
        oj_lab_here(l_nobud);
        oj_jmp_abs(g_jit_tramp_xnext);
      }

      /* cond-fail lands here: nothing executed, ordinary tail — the
       * skipped word still costs its SCYCLE (the cache's ARM_CACHE_
       * COND_OK early-return leaves the loop's -SCYCLE base).  The
       * x86 emits oj_tail here; the aarch64 transcription dropped it,
       * undercharging every skipped TIGHT_DP_PC word by 1. */
      oj_word_cond_close(l_skip);
      oj_tail(pc_k_ + 4);
      oj_exit_next(pc_k_ + 4);
      return 2;                 /* block terminates at the pc write */
    }

  /* RS words cost an extra ICYCLE over the base SCYCLE (the reg-
   * shifted operand's pipeline stall) — x86 parity: oj_emit_dp's
   * oj_charge_extra((cls_ == ARM_CLS_TIGHT_DP_RS) ? 1 : 0). */
  oj_charge_extra((cls_ == ARM_CLS_TIGHT_DP_RS) ? 1 : 0);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  return 1;
}

static int oj_emit_branch(uint32_t const cmd_, uint32_t const pc_k_)
{
  int32_t  const off    = ((int32_t)(cmd_ << 8)) >> 6;
  if(getenv("OPERA_JIT_NO_BRANCH")) return 0;   /* diag bisect */
  uint32_t const target = (uint32_t)(pc_k_ + 8 + off);
  uint32_t       l_fall;

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_fall = oj_word_cond(cmd_);

  /* taken path */
  if(cmd_ & (1 << 24))            /* L: USER[14] = pc_k + 4 (return addr) */
    {
      oj_mov32(17u, pc_k_ + 4u);
      oj_st_cpu(17u, OJ_U(14));
    }
  oj_charge_extra(SCYCLE + NCYCLE);
  oj_tail(target);
  /* backward-edge threading: a static target already emitted inside this
   * same block becomes a direct in-block jump (register invariants hold
   * at every word start: w23/w24 pinned, w8-w10/w17/w27/w28 transient). */
  if((target < pc_k_) && (target >= s_oj_block_pc))
    {
      uint32_t const tw = ((target - s_oj_block_pc) >> 2);
      uint32_t const at = s_oj_word_off[tw];
      /* PREFIT/budbatch: one check per backedge pass.
       * x86: cmp r12d(charge),r13d(remaining); jl ok (charge<remaining).
       * A64 keeps the SAME operand order: cmp w23,w24; b.lt ok. */
      if(g_jit_budbatch)
        {
          uint32_t const l_be_ok = oj_lab_alloc();
          oj_u32(0x6B1802FFu);        /* cmp w23,w24 [probe17] */
          OJ_JL(l_be_ok);             /* charge < remaining -> continue */
          oj_mov32(OJ_X17, target);
          oj_st_cpu(OJ_X17, OJ_U15);
          oj_jmp_abs(g_jit_tramp_xbudget);
          oj_lab_here(l_be_ok);
        }
      /* in-block backward jump: bind a fresh label to the emitted
       * position, then a normal b (the fixup machinery is positional
       * under the hood: s_oj_lab[l] = byte position). */
      {
        uint32_t const l_back = oj_lab_alloc();
        s_oj_lab[l_back] = at;
        oj_jmp(l_back);
      }
    }
  else
    oj_exit_next(target);

  /* fallthrough path (cond-fail): no extra charge */
  if(l_fall != OJ_NO_COND)
    {
      oj_lab_here(l_fall);
      oj_tail(pc_k_ + 4);
      oj_exit_next(pc_k_ + 4);
    }

  return 2;                   /* block ends after this word */
}

/* ---------------------------------------------------------------------------
 * TIGHT_MUL — includes the rd == rm quirk and the exact
 * ((calcbits(rs) + 5) >> 1) - 1 (clamped to 16) runtime cost.
 * Words [probe18/probe19-extracted]:
 *   subs wzr,w17,w17 = 6B11023F (zero test)
 *   clz w17,w17 = 5AC01231 ; neg w17,w17 = 4B1103F1 (32-clz)
 *   add w17,#5 = 11001631 ; lsr#1 = 53017E31 ; sub#1 = 51000631
 *   cmp w17,#16 = 7100423F ; mov w17,#16 = 52800211 ; mov w17,#1 = 52800031
 *   add w23,w23,w17 = 0B1102F7
 *   mul w10,w10,w17 = 1B117D4A ; madd w10,w10,w17,w8 = 1B11214A
 *   mov w9,w10 = 2A0A03E9 ; movz w10,#0 = 5280000A
 * ------------------------------------------------------------------------- */
static int oj_emit_mul(uint32_t const cmd_, uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_MUL")) return 0;   /* diag bisect */
  uint32_t l_skip;
  uint32_t l_cb0, l_cbd, l_clamp;
  uint32_t const rm = (cmd_ & 0xF);
  uint32_t const rs = ((cmd_ >> 8) & 0xF);
  uint32_t const rn = ((cmd_ >> 12) & 0xF);
  uint32_t const rd = ((cmd_ >> 16) & 0xF);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);


  /* cost walk: w17 = USER[rs]; calcbits -> ((n+5)>>1)-1 clamp 16; w23 += */
  oj_ld_cpu(17u, OJ_U(rs));
  l_cb0   = oj_lab_alloc();
  l_cbd   = oj_lab_alloc();
  l_clamp = oj_lab_alloc();
  { uint32_t const p_cbz = s_oj_len;
    oj_u32(0x34000031u);           /* cbz w17, +l_cb0 [probe20]        */
    if(s_oj_nfix < OJ_MAXFIX)
      {
        s_oj_fix_at[s_oj_nfix]   = p_cbz;
        s_oj_fix_lab[s_oj_nfix]  = l_cb0;
        s_oj_fix_cond[s_oj_nfix] = 2u;   /* cbz class */
        s_oj_nfix++;
      }
  }
  oj_u32(0x5AC01231u);           /* clz w17,w17                      */
  oj_u32(0x4B1103F1u);           /* neg w17,w17 = -clz                */
  oj_u32(0x11008231u);           /* add w17,w17,#32 [probe73] -> 32-clz
                                    (the bit length; the old ladder
                                    stopped at -clz, so every nonzero rs
                                    made (5-clz) negative, the logical
                                    lsr produced ~2^30, and the clamp
                                    pinned the charge at 16 — the MUL
                                    words overcharged by up to 15) */
  oj_jmp(l_cbd);
  oj_lab_here(l_cb0);
  oj_u32(0x52800031u);           /* mov w17,#1 (calcbits(0)==1)      */
  oj_lab_here(l_cbd);
  oj_u32(0x11001631u);           /* add w17,w17,#5                   */
  oj_u32(0x53017E31u);           /* lsr w17,w17,#1                   */
  oj_u32(0x51000631u);           /* sub w17,w17,#1                   */
  oj_u32(0x7100423Fu);           /* cmp w17,#16                      */
  OJ_JLE(l_clamp);               /* <= 16: keep                       */
  oj_u32(0x52800211u);           /* mov w17,#16                       */
  oj_lab_here(l_clamp);
  oj_u32(0x0B1102F7u);           /* add w23,w23,w17                  */

  /* result (w10 = product; w17 = USER[rs] again — the walk clobbered it) */
  oj_ld_cpu(17u, OJ_U(rs));
  if(rd == rm)
    {
      /* quirk: rd == rm keeps old accumulate/0 semantics, no multiply */
      if(cmd_ & (1 << 21))
        oj_ld_cpu(10u, OJ_U(rn));
      else
        oj_u32(0x5280000Au);     /* movz w10,#0                       */
    }
  else
    {
      oj_ld_cpu(10u, OJ_U(rm));  /* w10 = USER[rm]                    */
      if(cmd_ & (1 << 21))
        {
          oj_ld_cpu(8u, OJ_U(rn));   /* w8 = USER[rn] (accumulate)     */
          oj_u32(0x1B11214Au);   /* madd w10,w10,w17,w8              */
        }
      else
        oj_u32(0x1B117D4Au);     /* mul w10,w10,w17                  */
    }

  if(cmd_ & (1 << 20))
    oj_arm_set_zn_w10();

  oj_st_cpu(10u, OJ_U(rd));

  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  return 1;
}

/* ---------------------------------------------------------------------------
 * SDT loads (LDI = imm offset, LDR = reg offset) — the oj_emit_sdt_load
 * translation.  Conventions: w16 = base, w17 = offset/addr/val scratch.
 * Words [probe21-extracted]:
 *   neg w17,w17 = 4B1103F1 ; add w17,w17,w16 = 0B100231
 *   and w17,#~3 = 121E7631 ; cmp w17,w21 = 6B15023F
 *   ldrb w17,[x16,w17,uxtw] = 38714A11 ; ldr = B8714A11 ; lsl#2 = B8715A11
 *   and w17,#3 = 12000631 ; lsl w17,#3 = 531D7231
 *   ror w17,w17,w8 = 1AC82E31 ; eor w17,#3 = 52000631
 * RAM_SIZE gate: the range check reads the CORE's RAM_SIZE static via
 * movabs + ldr (the aarch64 indirect route; no rip-relative exists).
 * ------------------------------------------------------------------------- */
static int oj_emit_sdt_load(uint32_t const cmd_, uint32_t const aux_,
                            uint32_t const pc_k_, uint32_t const cls_)
{
  if(getenv("OPERA_JIT_NO_SDTL")) return 0;   /* diag bisect */
  uint32_t l_skip, l_slow, l_norot;
  uint32_t const rn = ((cmd_ >> 16) & 0xF);
  uint32_t const rd = ((cmd_ >> 12) & 0xF);
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip  = oj_word_cond(cmd_);
  l_slow  = oj_lab_alloc();
  l_norot = oj_lab_alloc();

  /* w16 = base (USER[rn]); w17 = offset */
  oj_ld_cpu(16u, OJ_U(rn));

  if(cls_ == ARM_CLS_TIGHT_SDT_LDI)
    {
      oj_mov32(17u, (cmd_ & 0x0FFFu));      /* imm12 */
    }
  else
    {
      oj_ld_cpu(17u, OJ_U(cmd_ & 0xF));      /* w17 = USER[rm] */
      /* promoted static shift (aux_) — reuse the DP shift laws on w17 */
      {
        uint32_t const sh  = (aux_ & 0x3F);
        uint32_t const typ = ((aux_ >> 8) & 7);
        if((typ == 0) && (sh > 0) && (sh < 32))
          oj_u32(0x53000000u | (((32u - sh) & 31u) << 16) |
                 ((31u - sh) << 10) | (17u << 5) | 17u);   /* lsl w17 */
        else if((typ == 1) && (sh > 0) && (sh < 32))
          oj_u32(0x53000000u | (sh << 16) | (31u << 10) |
                 (17u << 5) | 17u);                        /* lsr w17 */
        else if((typ == 2) && (sh > 0) && (sh < 32))
          oj_u32(0x13000000u | (sh << 16) | (31u << 10) |
                 (17u << 5) | 17u);                        /* asr w17 */
        else if((typ == 3) && (sh > 0))
          oj_u32(0x13890129u | (sh << 10));                /* ror w17 */
        else if((sh == 32) && ((typ == 0) || (typ == 1)))
          oj_u32(0x52800000u | 17u);                       /* movz w17,#0 */
        else if((sh == 32) && (typ == 2))
          oj_u32(0x13000000u | (31u << 16) | (31u << 10) |
                 (17u << 5) | 17u);                        /* asr #31 */
        else if(typ == 4)
          { oj_word_cond_close(l_skip); return 0; }        /* RRX: C-step */
      }
    }

  if(!(cmd_ & (1 << 23)))
    oj_u32(0x4B1103F1u);           /* neg w17,w17 (subtract offset)  */

  if(cmd_ & (1 << 24))
    {
      oj_u32(0x0B100230u);         /* add w16,w17,w16 (pre-index): the
                                      new base lives in w16              */
      oj_u32(0x2A1003FCu);          /* mov w28,w16 [probe25]: tbas       */
    }
  else
    {
      oj_u32(0x2A1003FCu);          /* mov w28,w16 [probe25]: tbas = the
                                      OLD base (the address)            */
      oj_u32(0x0B100230u);        /* add w16,w17,w16: w16 = new base   */
    }
  oj_u32(0x2A1003E8u);            /* mov w8,w16: park the new base — w16
                                      dies at the DRAM movabs, w17 at the
                                      RAM_SIZE gate, w27 carries the word
                                      path's rotation; w8 is free in the
                                      load emitters (DP-only register)   */

  /* range check on w28 = tbas vs RAM_SIZE (core static, indirect route).
   * x86: cmp edx,[RAM_SIZE]; jae slow.  A64: ldr w17,[x16=RAM_SIZE];
   * cmp w28,w17 (Rd=31 — the composed word had Rd=28, WRITING tbas);
   * b.hs slow (unsigned). */
  oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&RAM_SIZE);
  oj_u32(0xB9400211u);             /* ldr w17,[x16]  (RAM_SIZE)        */
  oj_u32(0x6B11039Fu);             /* cmp w28,w17 [probe22]            */
  OJ_JHS(l_slow);                  /* tbas >= RAM_SIZE (unsigned) -> slow */

  if(is_byte)
    {
      /* byte: val = DRAM[tbas ^ 3] (BE byte-within-word selection).
       * Value into w9: the writeback below still needs w17 (the new
       * base), and a slow exit must leave USER[rn] untouched. */
      oj_u32(0x5200079Cu);         /* eor w28,w28,#3 [probe23]          */
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
      oj_u32(0x387C4A09u);         /* ldrb w9,[x16,w28,uxtw] [probe25]  */
    }
  else
    {
      /* word: rot amount = (tbas & 3) * 8 (LSL #3), saved in w27
       * BEFORE the load; tbas masked to ~3 in w28 for the aligned
       * load; value into w9; then ror w9,w9,w27. */
      oj_u32(0x1200079Bu);         /* and w27,w28,#3 [probe22]          */
      oj_u32(0x531D737Bu);         /* lsl w27,w27,#3 [probe23]         */
      oj_u32(0x121E779Cu);         /* and w28,w28,#~3 [probe46: was 121E763C = 'and w28,w17' — Rn field wrong, stores computed garbage index] */
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
      oj_u32(0xB87C4A09u);         /* ldr w9,[x16,w28,uxtw] [probe25]   */
      oj_u32(0x1ADB2D29u);         /* ror w9,w9,w27 [probe25]          */
    }

  /* writeback AFTER the load (slow-exit stays side-effect free);
   * new base is parked in w8; value in w9 */
  if((cmd_ & (1 << 21)) || !(cmd_ & (1 << 24)))
    oj_st_cpu(8u, OJ_U(rn));       /* USER[rn] = new base (w8)         */
  oj_st_cpu(9u, OJ_U(rd));        /* USER[rd] = val (w9)               */

  oj_charge_extra(NCYCLE + ICYCLE);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

static int oj_emit_sdt_literal(uint32_t const cmd_, uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_LIT")) return 0;   /* diag bisect */
  uint32_t l_skip, l_slow, addr;
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  /* gate down to the plain literal shape (the x86 gates verbatim) */
  if((cmd_ & (1 << 20)) == 0)
    return 0;
  if(((cmd_ >> 16) & 0xF) != 0xF)
    return 0;
  if(((cmd_ >> 12) & 0xF) == 0xF)
    return 0;
  if(!(cmd_ & (1 << 24)) || (cmd_ & (1 << 21)))
    return 0;

  addr = (pc_k_ + 8);
  if(cmd_ & (1 << 23))
    addr += (cmd_ & 0x0FFF);
  else
    addr -= (cmd_ & 0x0FFF);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  l_slow = oj_lab_alloc();

  /* the address is a COMPILE-TIME constant: bake it.  w28 = addr,
   * compare vs RAM_SIZE (runtime), then load DRAM[addr] / DRAM[addr^3].
   * Words [probe22/25/26]: movz w28,#imm16 = 52800000|imm<<5|28 (32-bit
   * constants via movz/movk as needed — use oj_mov32 on w28). */
  oj_mov32(28u, addr);
  oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&RAM_SIZE);
  oj_u32(0xB9400211u);             /* ldr w17,[x16] (RAM_SIZE)         */
  oj_u32(0x6B11039Fu);             /* cmp w28,w17                      */
  OJ_JHS(l_slow);

  if(is_byte)
    {
      /* byte: val = DRAM[addr ^ 3] — the XOR bakes into the mov32 */
      oj_mov32(28u, (addr ^ 3u));
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
      oj_u32(0x387C4A09u);         /* ldrb w9,[x16,w28,uxtw]            */
    }
  else
    {
      uint32_t const aligned = (addr & ~3u);

      oj_mov32(28u, aligned);
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
      oj_u32(0xB87C4A09u);         /* ldr w9,[x16,w28,uxtw]            */
      if(addr & 3)
        {
          /* ror w9,w9,#(addr&3)*8 — compile-time imm: ror w9 base
           * 0x13890129-class with Rd/Rn=9: recompose per field math
           * and verify: ror w9,w9,#N = 0x13890129|(N<<10) [probe13b] */
          oj_u32(0x13890129u | (((addr & 3u) * 8u) << 10));
        }
    }

  oj_st_cpu(9u, OJ_U((cmd_ >> 12) & 0xF));

  oj_charge_extra(NCYCLE + ICYCLE);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

static void oj_cov_probe(uint32_t const l_ck_, uint32_t const l_slow_)
{
  (void)l_ck_; (void)l_slow_;
}

static void oj_bdt_probe(uint32_t const l_ck_, uint32_t const l_slow_)
{
  (void)l_ck_; (void)l_slow_;
}

/* ---------------------------------------------------------------------------
 * BDT (LDM/STM) — the oj_emit_bdt translation.  Register conventions:
 *   w16 = base/first-element address, w28 = the transfer address,
 *   w9  = value, w17 = scratch.  Words [probe29-extracted]:
 *   sub w16,w16,#imm = 51000000|(imm<<10)|(16<<5)|16
 *   add w16,w16,#imm = 11000000|(imm<<10)|(16<<5)|16
 *   add w28,w16,#imm = 11000000|(imm<<10)|(16<<5)|28
 *   ldr/str w9,[x16,w28,uxtw(#2)] = B87C4A09/B83C4A09/B87C5A09/B83C5A09
 *   cmp w28,#imm = 71000000|(imm<<10)|(28<<5)|31
 *   add w28,w28,#4 = 1100139C
 * The per-element address = w0 + 4*i with i a COMPILE-TIME index: bake
 * the disp into add w28,w16,#(4*i) immediate words.
 * ------------------------------------------------------------------------- */
static int oj_emit_bdt(uint32_t const cmd_, uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_BDT")) return 0;   /* diag bisect */
  uint32_t l_skip, l_slow;
  uint32_t const list = (cmd_ & 0xFFFF);
  uint32_t const rn   = ((cmd_ >> 16) & 0xF);
  int      const is_ldm = ((cmd_ & (1 << 20)) != 0);
  if(getenv("OPERA_JIT_NO_BDT_PC") && is_ldm && (list & 0x8000u))
    return 0;                    /* diag bisect: ldm-with-pc only */
  if(getenv("OPERA_JIT_NO_BDT_STM") && !is_ldm)
    return 0;                    /* diag bisect: stm forms only */
  if(getenv("OPERA_JIT_NO_BDT_LDM") && is_ldm)
    return 0;                    /* diag bisect: all ldm forms */
  uint32_t       n  = 0;
  uint32_t       i;
  uint32_t       disp;

  /* compile-time gates (the x86 gates verbatim) */
  if(cmd_ & (1 << 22))          /* S bit: usr-bank transfer */
    return 0;
  if(rn == 0xF)                 /* pc-relative base */
    return 0;
  if(list == 0)
    return 0;

  for(i = 0; i < 16; i++)
    n += ((list >> i) & 1);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  l_slow = oj_lab_alloc();

  /* ---- first element address ---- */
  oj_ld_cpu(16u, OJ_U(rn));       /* w16 = base */
  if(!(cmd_ & (1 << 24)))         /* P = 0 */
    {
      if(!(cmd_ & (1 << 23)))     /* DA: first = base - 4*(n-1) */
        {
          if(n > 1)
            oj_u32(0x51000000u | ((4u * (n - 1u)) << 10) |
                   (16u << 5) | 16u);   /* sub w16,w16,#4(n-1) */
        }
    }
  else                            /* P = 1 */
    {
      if(cmd_ & (1 << 23))        /* IB: first = base + 4 */
        oj_u32(0x11000000u | (4u << 10) | (16u << 5) | 16u);
      else                        /* DB: first = base - 4*n */
        oj_u32(0x51000000u | ((4u * n) << 10) | (16u << 5) | 16u);
    }

  /* ---- window gate: [w0, w0 + 4*(n-1)] within RAM_SIZE ---- */
  /* w17 = w0 & ~3; +4*(n-1) if n>1; >= RAM_SIZE -> slow */
  oj_u32(0x2A1003F1u);            /* mov w17,w16 [probe30]            */
  oj_u32(0x121E7631u);            /* and w17,w17,#~3                  */
  if(n > 1)
    oj_u32(0x11000000u | ((4u * (n - 1u)) << 10) |
           (17u << 5) | 17u);     /* add w17,w17,#4(n-1)              */
  oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&RAM_SIZE);
  {
    /* NOTE: w16 must survive for the transfers — reload below. */
    uint32_t const p_tmp = 0;
    (void)p_tmp;
  }
  oj_u32(0xB9400209u);            /* ldr w9,[x16] (RAM_SIZE) [probe47: was B9400211 = 'ldr w17' — Rt wrong] */
  oj_u32(0x6B09023Fu);            /* cmp w17,w9                        */
  OJ_JHS(l_slow);
  /* restore w16 = base for the address re-derivation */
  oj_ld_cpu(16u, OJ_U(rn));
  if(!(cmd_ & (1 << 24)))
    {
      if(!(cmd_ & (1 << 23)))
        {
          if(n > 1)
            oj_u32(0x51000000u | ((4u * (n - 1u)) << 10) |
                   (16u << 5) | 16u);
        }
    }
  else
    {
      if(cmd_ & (1 << 23))
        oj_u32(0x11000000u | (4u << 10) | (16u << 5) | 16u);
      else
        oj_u32(0x51000000u | ((4u * n) << 10) | (16u << 5) | 16u);
    }

  if(!is_ldm)
    {
      /* stores: HIRES fanout gate */
      uint32_t const l_nh = oj_lab_alloc();
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&HIRESMODE);
      oj_u32(0x39400209u);        /* ldrb w9,[x16]                    */
      {
        uint32_t const p_cbz = s_oj_len;
        oj_u32(0x34000009u);      /* cbz w9, +l_nh                    */
        if(s_oj_nfix < OJ_MAXFIX)
          {
            s_oj_fix_at[s_oj_nfix]   = p_cbz;
            s_oj_fix_lab[s_oj_nfix]  = l_nh;
            s_oj_fix_cond[s_oj_nfix] = 2u;
            s_oj_nfix++;
          }
      }
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM_SIZE);
      oj_u32(0xB9400209u);        /* ldr w9,[x16] (DRAM_SIZE) [probe47: was B9400211 = 'ldr w17' — Rt wrong] */
      oj_u32(0x6B09023Fu);        /* cmp w17,w9 (window end)           */
      OJ_JHS(l_slow);
      oj_lab_here(l_nh);

      /* Invalidation probes on EVERY word in the transfer window, not
       * just the first (the old single w0 probe — and the x86 family's
       * two-point w0/wN probe — let a multi-register STM whose window
       * strictly contained a block in its interior overwrite compiled
       * code without invalidating it: missed self-modifying-code kill,
       * stale block, guest-visible divergence).  Bounded loop over all
       * n words: w17 = current word address (starts at wN, steps down
       * by 4), w9 = remaining count, w27 = word index scratch, w28 =
       * load scratch.  Covered -> l_slow, where the C cstep re-executes
       * the whole STM through the hooked per-element writes and kills
       * exactly like a single store would.  Out-of-window words (VRAM
       * targets past g_jit_ram_words) skip the probe and continue. */
      {
        uint32_t const l_ni  = oj_lab_alloc();   /* loop done         */
        uint32_t const l_top  = oj_lab_alloc();   /* probe head        */
        uint32_t const l_ck2  = oj_lab_alloc();   /* probe body        */
        uint32_t const l_sk   = oj_lab_alloc();   /* skip one          */
        oj_ld_cpu(16u, OJ_U(rn));
        if(!(cmd_ & (1 << 24)))
          {
            if(!(cmd_ & (1 << 23)))
              { if(n > 1)
                  oj_u32(0x51000000u | ((4u * (n - 1u)) << 10) |
                         (16u << 5) | 16u); }
          }
        else
          {
            if(cmd_ & (1 << 23))
              oj_u32(0x11000000u | (4u << 10) | (16u << 5) | 16u);
            else
              oj_u32(0x51000000u | ((4u * n) << 10) | (16u << 5) | 16u);
          }
        oj_u32(0x2A1003F1u);      /* mov w17,w16 (first-element addr)  */
        oj_u32(0x121E7631u);      /* and w17,w17,#~3 (w0)             */
        if(n > 1)
          oj_u32(0x11000000u | ((4u * (n - 1u)) << 10) |
                 (17u << 5) | 17u); /* add w17,w17,#4(n-1) (wN)         */
        oj_u32(0x52800009u | (n << 5));  /* mov w9,#n (count)          */
        oj_lab_here(l_top);
        oj_u32(0x53027E3Bu);   /* lsr w27,w17,#2 (idx; from the file's
                                 * probe30 lsr w17,w17,#2=53027E31
                                 * with Rm=17,Rd=27)                   */
        oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&g_jit_ram_words);
        oj_u32(0xB940021Cu);   /* ldr w28,[x16] (limit; ldr w9,[x16]
                                 * =B9400209 with Rt=28)               */
        oj_u32(0x6B1C037Fu);   /* cmp w27,w28                        */
        OJ_JLS(l_ck2);
        oj_jmp(l_sk);
        oj_lab_here(l_ck2);
        oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&g_jit_word_cov);
        oj_u32(0xF9400210u);   /* ldr x16,[x16] (array base)         */
        oj_u32(0x387B4A1Cu);   /* ldrb w28,[x16,w27,uxtw] (probe48's
                                 * 38714A11 with Rm=27,Rt=28)          */
        oj_u32(0x7100039Fu);   /* cmp w28,#0                         */
        OJ_JNZ(l_slow);
        oj_lab_here(l_sk);
        oj_u32(0x71000529u);   /* subs w9,w9,#1                     */
        OJ_JZ(l_ni);
        oj_u32(0x51001231u);   /* sub w17,w17,#4                    */
        oj_jmp(l_top);
        oj_lab_here(l_ni);
      /* the probe's movabs clobbered w16 (cov/ram_words bases live
       * in x16): re-derive the first-element address before the
       * transfer loop consumes it */
      oj_ld_cpu(16u, OJ_U(rn));
      if(!(cmd_ & (1 << 24)))
        {
          if(!(cmd_ & (1 << 23)))
            { if(n > 1)
                oj_u32(0x51000000u | ((4u * (n - 1u)) << 10) |
                       (16u << 5) | 16u); }
        }
      else
        {
          if(cmd_ & (1 << 23))
            oj_u32(0x11000000u | (4u << 10) | (16u << 5) | 16u);
          else
            oj_u32(0x51000000u | ((4u * n) << 10) | (16u << 5) | 16u);
        }

      /* ---- STM writeback preload (rn in the list, a listed register
       * below rn): stm_accur writes USER[rn] = base_ BEFORE the store
       * loop, so the rn slot in memory receives base_, not the
       * original register value (opera_arm.c stm_accur preload).  The
       * LDM preload (ldm_accur) fires for ANY rn-in-list shape but the
       * rn slot's load overwrites it, so emitting it is harmless and
       * keeps the twin shapes aligned.  w16 holds the first-element
       * address here; compute wb = first +/- adj and store to USER[rn],
       * then re-derive w16 = first for the transfer loop. */
      if((cmd_ & (1u << 21)) && (list & (1u << rn)) &&
         (is_ldm || (list & ((1u << rn) - 1u))))
        {
          uint32_t adj = 0;        /* |writeback - first| */
          int      sub = 0;

          if(!(cmd_ & (1 << 24)))
            {
              if(cmd_ & (1 << 23))  { adj = 4u * n; }          /* IA */
              else                  { adj = 4; sub = 1; }     /* DA */
            }
          else
            {
              if(cmd_ & (1 << 23))  { adj = 4u * (n - 1u); }   /* IB */
              /* DB: writeback == first */
            }

          if(adj)
            oj_u32((sub ? 0x51000000u : 0x11000000u) |
                   ((uint32_t)adj << 10) | (16u << 5) | 16u);
          oj_st_cpu(16u, OJ_U(rn));

          /* re-derive w16 = first-element address for the transfer
           * loop: the store above left USER[rn] = base_ (the
           * writeback), and first0 = base_ inverted per mode:
           *   DA: first0 = base_ + 4 ; IA: first0 = base_ - 4n
           *   DB: first0 = base_ (identity) ; IB: first0 = base_ - 4(n-1) */
          oj_ld_cpu(16u, OJ_U(rn));
          if(!(cmd_ & (1 << 24)))
            {
              if(!(cmd_ & (1 << 23)))
                oj_u32(0x11000000u | (4u << 10) | (16u << 5) | 16u);           /* DA: +4 */
              else
                oj_u32(0x51000000u | ((4u * n) << 10) | (16u << 5) | 16u);      /* IA: -4n */
            }
          else
            {
              if(cmd_ & (1 << 23))
                { if(n > 1)
                    oj_u32(0x51000000u | ((4u * (n - 1u)) << 10) |
                           (16u << 5) | 16u); }                                 /* IB: -4(n-1) */
              /* DB: first0 == base_ (identity) */
            }
        }
      }
    }

  /* ---- transfers: element i at w0 + 4*i (disp baked) ---- */
  disp = 0;
  for(i = 0; i < 16; i++)
    {
      if(!((list >> i) & 1))
        continue;

      /* w28 = the transfer address.  CRITICAL: x16 (hence w16)
       * is clobbered by every transfer's movabs &DRAM / ldr
       * x16,[x16] pair, so disp must never be re-derived from
       * w16 after the first transfer.  Element 0 captures the
       * base (mov w28,w16); every later element INCREMENTS the
       * surviving w28 (add w28,w28,#4 = 1100139C [probe61]) —
       * the old 'add w28,w16,#disp' [probe29] form read the
       * destroyed w16 and scattered slots 1..n across DRAM. */
      if(disp)
        oj_u32(0x1100139Cu);        /* add w28,w28,#4 — probe61            */
      else
        oj_u32(0x2A1003FCu);        /* mov w28,w16 — probe24               */

      if(is_ldm)
        {
          oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
          oj_u32(0xB87C4A09u);  /* ldr w9,[x16,w28,uxtw]             */
          oj_st_cpu(9u, OJ_U(i));
        }
      else
        {
          if(i != 15)
            oj_ld_cpu(9u, OJ_U(i));
          else
            oj_mov32(9u, pc_k_ + 12u);   /* probed USER[15] value */
          oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
          oj_u32(0xB83C4A09u);  /* str w9,[x16,w28,uxtw]             */
        }
      disp += 4;
    }

  /* ---- writeback (bit21), after transfers ----
   * The transfer loop above clobbers x16 with the DRAM base, so
   * w16 no longer holds the first-element address here.  Re-derive
   * the writeback from USER[rn] directly: wb = base +/- 4*n by the
   * U bit (ARM semantics, identical for all P forms).  Skip the
   * rn-in-list preload shapes: USER[rn] already holds base_ there
   * (the preload above), so applying +/- 4n again would double it. */
  if((cmd_ & (1 << 21)) &&
     (!is_ldm
      ? !(list & (1u << rn)) || !(list & ((1u << rn) - 1u))
      : !(list & (1u << rn))))
    {
      uint32_t const adj = (4u * n);

      oj_ld_cpu(16u, OJ_U(rn));       /* w16 = ORIGINAL base          */
      if(cmd_ & (1 << 23))
        oj_u32(0x11000000u | (adj << 10) | (16u << 5) | 16u);  /* + */
      else
        oj_u32(0x51000000u | (adj << 10) | (16u << 5) | 16u);  /* - */
      oj_st_cpu(16u, OJ_U(rn));
    }

  if(is_ldm && (list & 0x8000))
    {
      /* LDM {.., pc}: the loaded pc is in USER[15]; hand-rolled tail
       * (the x86 shape), no imm u15 materialization. */
      uint32_t const l_go = oj_lab_alloc();
      oj_jmp(l_go);
      oj_lab_here(l_slow);
        oj_exit_cstep(pc_k_);
      oj_lab_here(l_go);

      oj_charge_extra(n + 5);
      {
        /* the oj_tail shape WITHOUT the u15 materialization (the
         * loaded pc must survive in USER[15]); the taken path jumps
         * straight to X_NEXT. */
        uint32_t const l_nofiq = oj_lab_alloc();
        uint32_t const l_nobud = oj_lab_alloc();


        oj_u32(0x110006F7u);       /* add w23,w23,#1  SCYCLE        */
        oj_u32(0xB9400291u);       /* ldr w17,[x20]                 */
        oj_u32(0x7100023Fu);      /* cmp w17,#0 (ldr sets no flags!) */
        OJ_JZ(l_nofiq);
        oj_ld_cpu(17u, OJ_CPSR);   /* ldr w17,[x19,#CPSR]           */
        oj_u32(0x721A023Fu);       /* tst w17,#0x40  ISF            */
        OJ_JNZ(l_nofiq);
        oj_jmp_abs(g_jit_tramp_xfiq);
        oj_lab_here(l_nofiq);
        if(!g_jit_budbatch)
          {
            oj_u32(0x6B1802FFu);   /* cmp w23,w24                   */
            OJ_JL(l_nobud);
            oj_jmp_abs(g_jit_tramp_xbudget);
          }
        oj_lab_here(l_nobud);
        oj_jmp_abs(g_jit_tramp_xnext);
      }

      /* cond-fail lands here: no transfers happened, USER[15] unsynced */
      oj_word_cond_close(l_skip);
      oj_tail(pc_k_ + 4);
      oj_exit_next(pc_k_ + 4);
      return 2;
    }

  oj_charge_extra(n + 2);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * SDT stores (STI = imm, STR = reg offset) — the oj_emit_sdt_store /
 * oj_emit_sdt_store_r translation.  Register conventions:
 *   w16 = base, w17 = offset then new-base then scratch, w28 = tbas (BYTE
 *   domain for the range/HIRES gates), w9 = value / word index scratch.
 * Words [probe21/22/23/24/25/26-extracted]:
 *   ldrb w9,[x16] = 39400209 ; ldr w9,[x16] = B9400209
 *   strb w9,[x16,w28,uxtw] = 383C4A09 ; str = B83C4A09
 *   lsr w17,w28,#2 = 53027F91 ; cmp w17,w9 = 6B09023F
 *   ldrb w9,[x16,w17,uxtw] = 38714A09 ; cmp w9,#0 = 7100013F
 *   mov w9,w28 = 2A1C03E9 ; mov w28,w17 = 2A1103FC ; mov w28,w16 = 2A1003FC
 *   eor w28,w28,#3 = 5200079C ; and w28,#~3 = 121E763C
 *   neg w17,w17 = 4B1103F1 ; add w17,w17,w16 = 0B100231
 * ------------------------------------------------------------------------- */

/* the shared store body: w17 = offset on entry */
static int oj_sdt_store_body(uint32_t const cmd_,
                             uint32_t const pc_k_)
{
  uint32_t const l_slow = oj_lab_alloc();
  uint32_t const l_nh   = oj_lab_alloc();
  uint32_t const l_ck   = oj_lab_alloc();
  uint32_t const l_go   = oj_lab_alloc();
  uint32_t const rn = ((cmd_ >> 16) & 0xF);
  uint32_t const rd = ((cmd_ >> 12) & 0xF);
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  oj_ld_cpu(16u, OJ_U(rn));        /* w16 = base                       */

  if(!(cmd_ & (1 << 23)))
    oj_u32(0x4B1103F1u);           /* neg w17,w17                      */

  if(cmd_ & (1 << 24))
    {
      oj_u32(0x0B100230u);         /* add w16,w17,w16: new base        */
      oj_u32(0x2A1003FCu);         /* mov w28,w16: tbas = address      */
      oj_u32(0x2A1003FBu);         /* mov w27,w16: keep (w16 dies at the
                                      DRAM movabs below; w17 dies at the
                                      RAM_SIZE gate; w27 is untouched
                                      through gates/probe/store)        */
    }
  else
    {
      oj_u32(0x2A1003FCu);         /* mov w28,w16: tbas = old base     */
      oj_u32(0x0B100230u);         /* add w16,w17,w16: new base        */
      oj_u32(0x2A1003FBu);         /* mov w27,w16: keep the new base   */
    }

  /* range gate vs RAM_SIZE (byte domain, both paths; the word store
   * masks tbas for the access itself, not for the gates) */
  oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&RAM_SIZE);
  oj_u32(0xB9400211u);             /* ldr w17,[x16] (RAM_SIZE)         */
  oj_u32(0x6B11039Fu);             /* cmp w28,w17                      */
  OJ_JHS(l_slow);                  /* tbas >= RAM_SIZE -> slow         */

  /* HIRES gate: HIRESMODE && tbas >= DRAM_SIZE -> slow */
  oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&HIRESMODE);
  oj_u32(0x39400209u);             /* ldrb w9,[x16]                    */
  {
    uint32_t const p_cbz = s_oj_len;
    oj_u32(0x34000009u);           /* cbz w9, +l_nh (not hires)        */
    if(s_oj_nfix < OJ_MAXFIX)
      {
        s_oj_fix_at[s_oj_nfix]   = p_cbz;
        s_oj_fix_lab[s_oj_nfix]  = l_nh;
        s_oj_fix_cond[s_oj_nfix] = 2u;
        s_oj_nfix++;
      }
  }
  oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM_SIZE);
  oj_u32(0xB9400211u);             /* ldr w17,[x16] (DRAM_SIZE)        */
  oj_u32(0x6B11039Fu);             /* cmp w28,w17 (byte domain)        */
  OJ_JHS(l_slow);
  oj_lab_here(l_nh);

  /* invalidation probe (ALL stores, byte and word): a byte write
   * to a covered word must invalidate exactly like a word write
   * (the covered word = tbas >> 2, identical for both paths).
   * idx = tbas >> 2; idx < g_jit_ram_words -> probe
   * g_jit_word_cov[idx]; covered -> slow (the C kill path).
   * Out of window -> clean store. */
  {
    oj_u32(0x53027F91u);           /* lsr w17,w28,#2 (word index)       */
    oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&g_jit_ram_words);
    oj_u32(0xB9400209u);           /* ldr w9,[x16] (ram_words) [probe47: was B9400211 = 'ldr w17' — Rt wrong] */
    oj_u32(0x6B09023Fu);           /* cmp w17,w9 (idx vs limit)        */
    OJ_JLS(l_ck);                  /* idx < limit: probe               */
    oj_jmp(l_go);                  /* out of window: clean store       */
    oj_lab_here(l_ck);
    oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&g_jit_word_cov);
    oj_u32(0xF9400210u);           /* ldr x16,[x16] (array base) [probe48: was ldr x17 — aliased base==index in the ldrb below] */
    oj_u32(0x38714A09u);           /* ldrb w9,[x16,w17,uxtw] [probe48: was [x17,w17] — base==index alias read cov+cov!] */
    oj_u32(0x7100013Fu);           /* cmp w9,#0                        */
    OJ_JNZ(l_slow);                /* covered -> C kill path           */
  }
  oj_lab_here(l_go);

  /* the store itself */
  if(is_byte)
    {
      oj_u32(0x5200079Cu);         /* eor w28,w28,#3 (BE byte select)  */
      oj_ld_cpu(9u, OJ_U(rd));     /* w9 = the value                   */
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
      oj_u32(0x383C4A09u);         /* strb w9,[x16,w28,uxtw]           */
    }
  else
    {
      oj_u32(0x121E779Cu);         /* and w28,w28,#~3 (align) [probe46: was 121E763C = 'and w28,w17' — Rn wrong] */
      oj_ld_cpu(9u, OJ_U(rd));     /* w9 = the value                   */
      oj_movabs(OJ_X16, (uint64_t)(uintptr_t)&DRAM);
      /* DRAM is a POINTER: dereference it (x16 = *(&DRAM)) */
      oj_u32(0xF9400210u);         /* ldr x16,[x16] [probe36] */
      oj_u32(0xB83C4A09u);         /* str w9,[x16,w28,uxtw]            */
    }

  /* writeback: new base is w27 (w16/w17 both die before this) */
  if((cmd_ & (1 << 21)) || !(cmd_ & (1 << 24)))
    oj_st_cpu(27u, OJ_U(rn));

  oj_charge_extra((uint32_t)(2 * NCYCLE - SCYCLE));
  /* NOTE: no oj_tail here — the tail belongs to the EMITTER after
   * oj_word_cond_close, so the cond-fail skip path runs it too (the
   * cache charges a cond-failed word its 1 SCYCLE and budget-checks
   * there; the old body-internal tail let the skip path bypass both). */
  {
    /* the clean tail must NOT fall into the slow stub below (the x86
     * l_done shape): every clean store would otherwise charge twice
     * and re-execute through the C-step. */
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

static int oj_emit_sdt_store(uint32_t const cmd_, uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_SDTS")) return 0;   /* diag bisect */
  uint32_t l_skip;

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  oj_mov32(17u, (cmd_ & 0x0FFFu));   /* w17 = imm12 */
  {
    int const r_ = oj_sdt_store_body(cmd_, pc_k_);
    oj_word_cond_close(l_skip);   /* skip path joins here: the cond-failed
                                   * word still pays its SCYCLE + budget
                                   * check via the tail below (the body's
                                   * clean path reaches here through the
                                   * l_done jump with the store charge on
                                   * w23 — both paths converge on one tail) */
    oj_tail(pc_k_ + 4);
    return r_;
  }
}

static int oj_emit_sdt_store_r(uint32_t const cmd_, uint32_t const aux_,
                               uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_SDTSR")) return 0;   /* diag bisect */
  uint32_t l_skip;
  uint32_t const sh  = (aux_ & 0x3F);
  uint32_t const typ = ((aux_ >> 8) & 7);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);

  /* w17 = USER[rm] with the promoted static shift (the DP shift laws,
   * w17 form: swap the Rd/Rn fields 9->17 — the laws below were
   * extracted for w9; recomposed per field math and verified by
   * probe27). */
  oj_ld_cpu(17u, OJ_U(cmd_ & 0xF));
  if((sh != 0) || (typ != 0))
    {
      if((typ == 0) && (sh > 0) && (sh < 32))        /* LSL */
        oj_u32(0x53000000u | (((32u - sh) & 31u) << 16) |
               ((31u - sh) << 10) | (17u << 5) | 17u);
      else if((typ == 1) && (sh > 0) && (sh < 32))   /* LSR */
        oj_u32(0x53000000u | (sh << 16) | (31u << 10) |
               (17u << 5) | 17u);
      else if((typ == 2) && (sh > 0) && (sh < 32))   /* ASR */
        oj_u32(0x13000000u | (sh << 16) | (31u << 10) |
               (17u << 5) | 17u);
      else if((typ == 3) && (sh > 0))                 /* ROR */
        oj_u32(0x13910231u | ((sh) << 10));           /* ror w17 base [probe27] */
      else if((sh == 32) && ((typ == 0) || (typ == 1)))
        oj_u32(0x52800000u | 17u);                    /* movz w17,#0 */
      else if((sh == 32) && (typ == 2))
        oj_u32(0x13000000u | (31u << 16) | (31u << 10) |
               (17u << 5) | 17u);                     /* asr #31 */
      else
        { oj_word_cond_close(l_skip); return 0; }    /* RRX: C-step */
    }

  {
    int const r_ = oj_sdt_store_body(cmd_, pc_k_);
    oj_word_cond_close(l_skip);   /* skip path joins here — same fix as the
                                   * imm store emitter: one shared tail */
    oj_tail(pc_k_ + 4);
    return r_;
  }
}

/* --------------------------- arena hooks ---------------------------------- */

/* RWX arena: anonymous mmap (the Windows twin uses VirtualAlloc + CFG
 * — see opera_arm_jit_aarch64_win.c). */
static uint8_t *ojb_alloc_arena(uint32_t const size_)
{
  void *const p = mmap(NULL,(size_t)size_,
                       (PROT_READ | PROT_WRITE | PROT_EXEC),
                       (MAP_PRIVATE | MAP_ANONYMOUS),-1,0);
  return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
}

/* icache coherence: MANDATORY on A64 (x86's no-op does not fly).
 * The core calls this at both commit sites. */
static void ojb_after_commit(void *const base_, uint32_t const len_)
{
  __builtin___clear_cache((char *)base_, (char *)base_ + (size_t)len_);
}
/* teardown: unmap the arena and the (8KB ojb_alloc_arena) trampoline page.
 * NULL-tolerant: destroy calls this always. */
static void ojb_shutdown(uint8_t *const arena_)
{
  if(arena_)
    munmap(arena_,(size_t)JIT_ARENA_SIZE);
  if(g_jit_tramp_page)
    munmap(g_jit_tramp_page,8192);
  g_jit_tramp_page = NULL;
}
