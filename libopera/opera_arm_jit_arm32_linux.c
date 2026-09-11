/* ---------------------------------------------------------------------------
 * opera_arm_jit_arm32_linux.c — Armv7-A (AAPCS32) encoding backend for the
 * ARM60 block-JIT.
 *
 * STATUS: ALL EMITTER CLASSES LIVE and oracle-exact — DP (6 tight
 * classes), branch, SDT-load, SDT-literal, MUL, SDT-store, BDT
 * (bring-up history in jit.md).  f300/f400/f900/f1500 match the
 * cache engine; 400-frame three-way lane arm32 = x86 = aarch64,
 * 0 divergent rows.
 * The remaining C-step residue is decode-gated full-class words
 * (SWI, SDS, register-shifted full DP, T-bit BDT forms) that no
 * backend inlines; the chain-word hook stays return 0 like every
 * other backend.
 *
 * ENABLEMENT: armv7+ Linux (movw/movt) — default ON per the selection
 * matrix (full battery + WAV receipts, 2026-09-01).  Windows-on-ARM32
 * is a non-goal (thumb-only OS vs A32 emitter — see the arena hook).
 *
 * REGISTER PLAN (callee-saved r4-r11 live across the whole trampoline
 * run; AAPCS32 gives 8 — two fewer than the x86-64 pin map needs, so
 * the two read-only poll pointers (cdrom_restart, madam_fsm) spill to
 * the block frame instead of pins and are reloaded at their (3) poll
 * sites — semantically safe (read-only within a block), the perf
 * consequence priced in the portability plan):
 *   r4 = &CPU                (pin)
 *   r5 = g_clio_fiqpend      (pin: the poll POINTER value, matching
 *                              aarch64's x20 — the tail derefs once)
 *   r9 = remaining           (was w24; set at entry, live across the
 *                              dispatch so block tails read it directly)
 *   r7 = charge              (was w23; zeroed at dispatch)
 *   r8/r10/r11 = scratch     (r9 reserved as platform register where
 *                              relevant; never used as a pin)
 *   [sp,#0] = budget         (spilled once at entry, re-read at epilog)
 * Frame: push {r4-r11,lr} (36 bytes) then sub sp,#4 (budget slot).
 *
 * TRAMPOLINE: every word below is assembler-extracted from
 * /tmp/zigtest/enc/tramp_arm32_ref.s (zig cc -target arm-linux-musleabi
 * .arch armv7-a; readelf -x .text, byte-reversed to LE words) — see
 * jit.md's arm32 session log.  The literal-pool loads the assembler
 * emitted for `ldr rX,=const` are replaced at emission time by
 * movw/movt pairs (probe80-verified:
 *   movw Rd,#imm16 = 0xE3000000|((imm>>12)<<16)|(Rd<<12)|(imm&0xFFF)
 *   movt Rd,#imm16 = 0xE3400000|((imm>>12)<<16)|(Rd<<12)|(imm&0xFFF)
 * ) plus a dereference word where a pointer VALUE is loaded.
 *
 * ARM CONDITION CODES: identical 4-bit field, identical semantics to
 * the guest's own — the OJ_Jcc map is the ARM cond code itself.
 *
 * ICACHE: __builtin___clear_cache after every arena commit.
 * ------------------------------------------------------------------------- */

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* --------------------------- trampoline-level reasons ---------------------- */
/* (shared with opera_arm_jit.c — the numeric values are the ABI) */
#define TRAMP_X_INTERP     100u
#define TRAMP_X_UNALIGNED  101u
#define TRAMP_X_COMPILE    102u

/* --------------------------- emitter state -------------------------------- */
#define OJ_BUF     (32u << 10)
#define OJ_MAXFIX  2048u
#define OJ_MAXLAB  512u          /* 64 words x 5 RS labels + tail guards
                                    * + entry0, with headroom [advisory-caught
                                    * overflow at 320] */
#define OJ_LAB_FULL 0xFFFFFFFEu   /* sentinel: labels exhausted — the
                                    * emitter bails to C-step */

static uint8_t  s_oj[OJ_BUF];
static uint32_t s_oj_len;
static uint32_t s_oj_word_off[JIT_MAX_WORDS];
static uint32_t s_oj_block_pc;
static uint32_t s_oj_lab_entry0;   /* prologue guard trap: C-step word 0 */
static uint32_t s_oj_fix_at[OJ_MAXFIX];
static uint32_t s_oj_fix_lab[OJ_MAXFIX];
static uint8_t  s_oj_fix_cond[OJ_MAXFIX]; /* 0=b 1=b<cond> (full 4-bit) */
static uint32_t s_oj_nfix;
static uint32_t s_oj_lab[OJ_MAXLAB];
static uint32_t s_oj_nlab;

/* ARM32 condition codes (identical to guest ARM) */
#define AA_EQ 0u
#define AA_NE 1u
#define AA_CS 2u
#define AA_CC 3u
#define AA_MI 4u
#define AA_PL 5u
#define AA_VS 6u
#define AA_VC 7u
#define AA_HI 8u
#define AA_LS 9u
#define AA_GE 10u
#define AA_LT 11u
#define AA_GT 12u
#define AA_LE 13u
#define AA_AL 14u

/* CPU member displacement helpers (arm_core_t: USER[16]@0, CPSR@176,
 * SPSR@152 — same layout the aarch64 backend verified) */
#define OJ_U(n_)   ((uint32_t)(offsetof(arm_core_t, USER) + (4u * (n_))))
#define OJ_U15     (OJ_U(15))
#define OJ_CPSR    ((uint32_t)(offsetof(arm_core_t, CPSR)))
#define OJ_SPSR    ((uint32_t)(offsetof(arm_core_t, SPSR)))

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

/* reachability flags the shared core pokes (rel32 checks — vacuously
 * true on arm32: every out-of-block jump is the literal-pool route) */
int       s_oj_abs_ok;
int       s_oj_rip_ok;

/* --------------------------- word writers (trampoline) --------------------- */

static void t_w(uint32_t const w_)
{
  t_cur_[t_len_ + 0] = (uint8_t)(w_ >>  0);
  t_cur_[t_len_ + 1] = (uint8_t)(w_ >>  8);
  t_cur_[t_len_ + 2] = (uint8_t)(w_ >> 16);
  t_cur_[t_len_ + 3] = (uint8_t)(w_ >> 24);
  t_len_ += 4;
}

/* movw/movt chain — armv7+.  The .arch armv7-a directive is REQUIRED
 * in probe sources; the C emitter needs no directive (the TU compiles
 * with -march=armv7-a via the target).  probe80 laws:
 *   movw = 0xE3000000 | (immhi<<16) | (Rd<<12) | imm12
 *   movt = 0xE3400000 | (immhi<<16) | (Rd<<12) | imm12 */
static void t_movabs(uint32_t const reg_, uint64_t const imm_)
{
  uint32_t const v  = (uint32_t)imm_;   /* arm32: addresses are 32-bit */
  uint32_t const lo = (v & 0xFFFFu);
  uint32_t const hi = ((v >> 16) & 0xFFFFu);

  t_w(0xE3000000u | ((lo >> 12) << 16) | (reg_ << 12) | (lo & 0xFFFu));
  if(hi)
    t_w(0xE3400000u | ((hi >> 12) << 16) | (reg_ << 12) | (hi & 0xFFFu));
}

/* intra-trampoline branches: LABEL-ALLOC design (the aarch64 mirror).
 * ARM b/b<cond> use imm24 in WORDS — the fixup pass resolves each
 * recorded label. */
#define T_MAXFIX 256
#define T_MAXLAB 64
static uint32_t t_fix_at[T_MAXFIX];
static uint32_t t_fix_lab[T_MAXFIX];
static uint8_t  t_fix_cond[T_MAXFIX];   /* 0xFF = b, else the 4-bit cond
                                           * (a real 0 = AA_EQ is CONDITIONAL —
                                           * the marker must not collide) */
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

/* b label — 0xEA000000 | (imm24 & 0xFFFFFF) */
static void t_b(uint32_t const lab_)
{
  if(t_fix_n < T_MAXFIX)
    {
      t_fix_at[t_fix_n]  = t_len_;
      t_fix_lab[t_fix_n] = lab_;
      t_fix_cond[t_fix_n] = 0xFFu;
      t_fix_n++;
    }
  t_w(0xEA000000u);
}

/* b<cond> label — cond in bits 31-28 */
static void t_bcond(uint32_t const cond_, uint32_t const lab_)
{
  if(t_fix_n < T_MAXFIX)
    {
      t_fix_at[t_fix_n]  = t_len_;
      t_fix_lab[t_fix_n] = lab_;
      t_fix_cond[t_fix_n] = (uint8_t)(cond_ & 0xFu);
      t_fix_n++;
    }
  t_w(((cond_ & 0xFu) << 28) | 0x0A000000u);
}

static void t_fixup(void)
{
  uint32_t i;

  for(i = 0; i < t_fix_n; i++)
    {
      uint32_t const at   = t_fix_at[i];
      uint32_t const dst  = t_lab[t_fix_lab[i]];
      int32_t  const d    = ((int32_t)dst) - ((int32_t)at) - 8;
      int32_t  const dw   = d / 4;
      uint32_t       *w   = (uint32_t *)(void *)&t_cur_[at];
      uint32_t const cur  = *w;

      if(d & 3)
        continue;               /* cannot happen on a 4KB page */

      if(t_fix_cond[i] != 0xFFu)
        {
          /* b<cond>: imm24 in words, cond preserved from placeholder */
          if((dw < -8388608) || (dw > 8388607))
            continue;
          *w = (cur & 0xFF000000u) | (((uint32_t)dw) & 0x00FFFFFFu);
        }
      else
        {
          /* b: imm24 in words */
          *w = 0xEA000000u | (((uint32_t)dw) & 0x00FFFFFFu);
        }
    }
}

/* arena hooks, forward-declared: the trampoline builder materializes its
 * page via ojb_alloc_arena/ojb_after_commit before their definitions below */
static uint8_t *ojb_alloc_arena(uint32_t const size_);
static void     ojb_after_commit(void *const base_, uint32_t const len_);

/* ---------------------------------------------------------------------------
 * The trampoline — every word below is assembler-extracted from
 * tramp_arm32_ref.s (see file header for the extraction protocol).
 *
 * Frame/ABI summary (matches the reference decode map in jit.md):
 *   entry(r0 = budget_remaining):
 *     push {r4-r11,lr} ; sub sp,#4 ; str r0,[sp] ; mov r9,r0
 *     pins: r4=&CPU, r5=*g_clio_fiqpend (the FIQ poll reads [r5])
 *   dispatch:
 *     ldr r0,[r4,#60] (USER[15]) ; tst #3 (unaligned) ; lsr #2 ;
 *     gate vs g_jit_ram_words (bhs interp) ; table walk (32-bit entries,
 *     ldr r1,[r1,r0,lsl #2]) ; null -> compile ; code -> bx r1
 *   stubs: subs r9,r9,r7 (charge fold) then mov r0,#reason ; b epilog
 *   X_FIQ: fold ; movw/movt r1=&arm_fiq_vector ; mov lr,pc ; bx r1
 *     (armv5 interworking-safe call — blx reg needs armv5t)
 *   epilog:
 *     movw/movt r1=&g_jit_cycles ; ldr r2,[sp] ; sub r2,r2,r9 ;
 *     str r2,[r1] ; add sp,#4 ; pop {r4-r11,pc}
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
  t_w(0xE92D4FF0u);              /* push {r4-r11,lr}              */
  t_w(0xE24DD004u);              /* sub  sp,sp,#4                 */
  t_w(0xE58D0000u);              /* str  r0,[sp]   budget         */
  t_w(0xE1A09000u);              /* mov  r9,r0     remaining      */

  /* pins: r4 = &CPU ; r5 = *g_clio_fiqpend (the poll pointer value) */
  t_movabs(4,(uint64_t)(uintptr_t)&CPU);
  t_movabs(1,(uint64_t)(uintptr_t)&g_clio_fiqpend);
  t_w(0xE5915000u);              /* ldr  r5,[r1]                  */

  /* ------------------------------ dispatch ----------------------------- */
  t_lab_here(l_loop);
  t_w(0xE594003Cu);              /* ldr  r0,[r4,#60]  pc = USER[15] */
  t_w(0xE3100003u);              /* tst  r0,#3                      */
  t_bcond(AA_NE, l_slow_unaligned);
  t_w(0xE1A00120u);              /* lsr  r0,r0,#2                   */
  /* Dispatch window must mirror arm_jit_pc_index EXACTLY (see the
   * aarch64/x86 gate note): DRAM window only; ROM/hole pcs take the
   * slow path. */
  t_movabs(1,(uint64_t)(uintptr_t)&g_jit_ram_words);
  t_w(0xE5911000u);              /* ldr  r1,[r1]  ram_words        */
  t_w(0xE1500001u);              /* cmp  r0,r1   (unsigned below)  */
  t_bcond(AA_CS, l_slow_interp); /* bhs  -> interp (C = unsigned>=) */
  t_movabs(1,(uint64_t)(uintptr_t)&g_jit_table);
  t_w(0xE5911000u);              /* ldr  r1,[r1]  table base       */
  t_w(0xE7911100u);              /* ldr  r1,[r1,r0,lsl #2] block  */
  t_w(0xE3510000u);              /* cmp  r1,#0                      */
  t_bcond(AA_EQ, l_slow_compile);
  t_w(0xE5911000u);              /* ldr  r1,[r1]  code             */
  t_w(0xE3A07000u);              /* mov  r7,#0    charge = 0        */
  t_w(0xE12FFF11u);              /* bx   r1      (never returns)   */

  /* ---------------------------- X_NEXT -------------------------------- */
  l_xnext = t_len_;
  t_w(0xE0599007u);              /* subs r9,r9,r7  fold            */
  t_bcond(AA_LE, l_slow_budget);
  t_b(l_loop);

  /* ---------------------------- X_FIQ -------------------------------- */
  l_xfiq = t_len_;
  t_w(0xE0599007u);              /* subs r9,r9,r7  fold            */
  t_movabs(1,(uint64_t)(uintptr_t)&arm_fiq_vector);
  t_w(0xE1A0E00Fu);              /* mov  lr,pc (interworking call) */
  t_w(0xE12FFF11u);              /* bx   r1                        */
  t_w(0xE3A00001u);              /* mov  r0,#1   JIT_X_FIQ         */
  t_b(l_epilog);



  /* --------------------- plain exit stubs ------------------------------ */
  l_xbudget = t_len_;
  t_w(0xE0599007u);              /* fold                           */
  t_w(0xE3A00003u);              /* mov r0,#3   BUDGET             */
  t_b(l_epilog);

  l_xrst = t_len_;
  t_w(0xE0599007u);              /* fold                           */
  t_w(0xE3A00002u);              /* mov r0,#2   RST                */
  t_b(l_epilog);

  l_xfsm = t_len_;
  t_w(0xE0599007u);              /* fold                           */
  t_w(0xE3A00004u);              /* mov r0,#4   FSM                */
  t_b(l_epilog);

  l_xcstep = t_len_;
  t_w(0xE0599007u);              /* fold                           */
  t_w(0xE3A00005u);              /* mov r0,#5   CSTEP              */
  t_b(l_epilog);

  /* --------------------- slow stubs (dispatch-level) ------------------- */
  t_lab_here(l_slow_interp);
  t_w(0xE3A00064u);              /* mov r0,#100  INTERP            */
  t_b(l_epilog);

  t_lab_here(l_slow_unaligned);
  t_w(0xE3A00065u);              /* mov r0,#101  UNALIGNED         */
  t_b(l_epilog);

  t_lab_here(l_slow_compile);
  t_w(0xE3A00066u);              /* mov r0,#102  COMPILE           */
  t_b(l_epilog);

  t_lab_here(l_slow_budget);
  t_w(0xE3A00003u);              /* mov r0,#3   BUDGET              */
  t_b(l_epilog);

  /* ------------------------------ epilog ------------------------------- */
  t_lab_here(l_epilog);
  t_movabs(1,(uint64_t)(uintptr_t)&g_jit_cycles);
  t_w(0xE59D2000u);              /* ldr  r2,[sp]   budget          */
  t_w(0xE0422009u);              /* sub  r2,r2,r9  total           */
  t_w(0xE5812000u);              /* str  r2,[r1]   g_jit_cycles    */
  t_w(0xE28DD004u);              /* add  sp,sp,#4                  */
  t_w(0xE8BD8FF0u);              /* pop  {r4-r11,pc}               */

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
  /* bounded: the DP register-shift route allocates 5 labels/word, so a
   * 64-word all-RS block needs 64*5+3 = 323 — over the old 320 and
   * the overflow corrupted neighbors SILENTLY (advisory-caught: the
   * arm32 DP hang; aarch64 is latently exposed on other content). */
  if(s_oj_nlab >= OJ_MAXLAB)
    return OJ_LAB_FULL;
  return s_oj_nlab++;
}

static void oj_lab_here(uint32_t const lab_)
{
  s_oj_lab[lab_] = s_oj_len;
}

/* ARM32: fixed 4-byte words; the byte stream collapses onto the word API. */
static void oj_u32(uint32_t const w_)
{
  s_oj[s_oj_len + 0] = (uint8_t)(w_ >>  0);
  s_oj[s_oj_len + 1] = (uint8_t)(w_ >>  8);
  s_oj[s_oj_len + 2] = (uint8_t)(w_ >> 16);
  s_oj[s_oj_len + 3] = (uint8_t)(w_ >> 24);
  s_oj_len += 4;
}

/* the x86 backend's byte stream has no arm32 counterpart (words are
 * 4 bytes); guarded no-op like the aarch64 one */
static void oj_u8(uint32_t const b_)
{
  (void)b_;
}

static void oj_u64(uint64_t const v_)
{
  oj_u32((uint32_t)v_);
  oj_u32((uint32_t)(v_ >> 32));
}

/* intra-block b (imm24): placeholder + fixup.  Class is recorded
 * EXPLICITLY (never sniffed from the opcode). */
static void oj_jmp(uint32_t const lab_)
{
  if(s_oj_nfix < OJ_MAXFIX)
    {
      s_oj_fix_at[s_oj_nfix]  = s_oj_len;
      s_oj_fix_lab[s_oj_nfix] = lab_;
      s_oj_fix_cond[s_oj_nfix] = 15u;   /* AL: bits 31-28 = 1110 by construction */
      s_oj_nfix++;
    }
  oj_u32(0xEA000000u);
}

/* intra-block b<cond>: cond_ is the ARM cond code (0..14) */
static void oj_jcc(uint32_t const cond_, uint32_t const lab_)
{
  if(s_oj_nfix < OJ_MAXFIX)
    {
      s_oj_fix_at[s_oj_nfix]  = s_oj_len;
      s_oj_fix_lab[s_oj_nfix] = lab_;
      s_oj_fix_cond[s_oj_nfix] = (uint8_t)(cond_ & 0xFu);
      s_oj_nfix++;
    }
  oj_u32(((cond_ & 0xFu) << 28) | 0x0A000000u);
}

/* the x86 jcc-map translates onto ARM conds directly (identical code
 * space as the guest's own — one of the port's simplifications) */
#define OJ_JZ(l)   oj_jcc(AA_EQ, (l))
#define OJ_JNZ(l)  oj_jcc(AA_NE, (l))
#define OJ_JC(l)   oj_jcc(AA_CS, (l))
#define OJ_JNC(l)  oj_jcc(AA_CC, (l))
#define OJ_JA(l)   oj_jcc(AA_HI, (l))   /* hi */
#define OJ_JBE(l)  oj_jcc(AA_LS, (l))   /* ls */
#define OJ_JHS(l)  oj_jcc(AA_CS, (l))   /* cs: unsigned >= */
#define OJ_JLS(l)  oj_jcc(AA_CC, (l))   /* cc: unsigned <  */
#define OJ_JG(l)   oj_jcc(AA_GT, (l))   /* gt */
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
      int32_t  const d   = ((int32_t)dst) - ((int32_t)at) - 8;
      int32_t  const dw  = d / 4;
      uint32_t       *w  = (uint32_t *)(void *)&s_oj[at];
      uint32_t const cur = *w;

      if(dst == 0xFFFFFFFFu)
        return -1;

      if(d & 3)
        return -1;

      /* b/b<cond>: imm24 in words, +-32MB — always fits inside the
       * 32KB arena and its fixup window; cond preserved from the
       * placeholder's bits 31-28. */
      if((dw < -8388608) || (dw > 8388607))
        return -1;
      *w = (cur & 0xFF000000u) | (((uint32_t)dw) & 0x00FFFFFFu);
    }

  return 0;
}

/* movw/movt chain (32-bit constants — same laws as t_movabs) */
static void oj_movabs(uint32_t const reg_, uint64_t const imm_)
{
  uint32_t const v  = (uint32_t)imm_;
  uint32_t const lo = (v & 0xFFFFu);
  uint32_t const hi = ((v >> 16) & 0xFFFFu);

  oj_u32(0xE3000000u | ((lo >> 12) << 16) | (reg_ << 12) | (lo & 0xFFFu));
  if(hi)
    oj_u32(0xE3400000u | ((hi >> 12) << 16) | (reg_ << 12) | (hi & 0xFFFu));
}

/* out-of-block jump into the shared trampoline: LITERAL-POOL ROUTE.
 *   ldr pc,[pc,#-4] ; .word target
 * One word + one literal — correct at ANY distance (no rel32 window,
 * no arena-placement constraint; oj_abs_patch is a no-op).  Commits
 * are 16-byte aligned so the literal is always aligned.  probe80:
 *   ldr pc,[pc,#-4] = 0xE51FF004 */
static void oj_jmp_abs(uint64_t const addr_)
{
  oj_u32(0xE51FF004u);            /* ldr pc,[pc,#-4]              */
  oj_u32((uint32_t)addr_);
}

/* no-op: the literal route bakes the target inline (see oj_jmp_abs) */
static void oj_abs_patch(uint8_t const *const code_)
{
  (void)code_;
}

/* CPU member access: ldr/str Rt,[r4,#disp] (imm12 unsigned, unscaled
 * — CPU member offsets are all < 4KB so the form always fits).
 * probe80: ldr r1,[r4,#176] = E59410B0 ; str = E58410B0 */
static void oj_ld_cpu(uint32_t const reg_, uint32_t const disp_)
{
  oj_u32(0xE5900000u | (4u << 16) | (reg_ << 12) | disp_);
}

static void oj_st_cpu(uint32_t const reg_, uint32_t const disp_)
{
  oj_u32(0xE5800000u | (4u << 16) | (reg_ << 12) | disp_);
}

/* 32-bit guest constant into a register: movz/movk w-chain equivalent
 * (movw/movt pair; probe80) */
static void oj_mov32(uint32_t const reg_, uint32_t const imm_)
{
  uint32_t const lo = (imm_      ) & 0xFFFFu;
  uint32_t const hi = (imm_ >> 16) & 0xFFFFu;

  oj_u32(0xE3000000u | ((lo >> 12) << 16) | (reg_ << 12) | (lo & 0xFFFu));
  if(hi)
    oj_u32(0xE3400000u | ((hi >> 12) << 16) | (reg_ << 12) | (hi & 0xFFFu));
}

/* --------------------------- block prologue --------------------------------
 * Entry guards for the two hoisted poll STATIC pointers.  arm32 has no
 * spare pins for them (r4/r5 pinned, r6=remaining, r7=charge), so the
 * prologue MATERIALIZIZES the poll pointers fresh each block entry:
 *   movw/movt r10,&g_cdrom_restart_poll ; ldrb r10,[r10] ; cmp #0 ;
 *     bne entry0
 *   movw/movt r10,&g_madam_fsm_poll ; ldr r10,[r10] ; cmp #2 ; beq entry0
 *   (FIQ hoist) movw/movt r10,&g_clio_fiqpend... — r5 IS the fiqpend
 *   VALUE pin: ldr r2,[r5] ; cmp #0 ; bne entry0
 * r10/r11 are pure scratch at word boundaries (the shared ABI pins
 * only r4-r7), so both die at the guard exits safely.
 * ------------------------------------------------------------------------- */
static void oj_prologue(void)
{
  uint32_t const l_ng2      = oj_lab_alloc();
  uint32_t const l_ng3_fall = oj_lab_alloc();

  /* rst poll: the static holds the poll pointer — deref, ldrb, cmp.
   * ARM ldr/ldrb set NO flags: the explicit cmp is load-bearing
   * (probe84-verified encodings).  r10 dies at the guard exits (pure
   * scratch at word boundaries). */
  oj_mov32(10u,(uint32_t)(uintptr_t)&g_cdrom_restart_poll);
  oj_u32(0xE59AA000u);           /* ldr  r10,[r10]  (deref)       */
  oj_u32(0xE5DAA000u);           /* ldrb r10,[r10]  rst poll     */
  oj_u32(0xE35A0000u);           /* cmp  r10,#0                   */
  OJ_JZ(l_ng2);
  oj_jmp(s_oj_lab_entry0);
  oj_lab_here(l_ng2);

  /* fsm poll: ldr w; cmp #2 (FSM_INPROCESS); pending -> entry0 */
  oj_mov32(10u,(uint32_t)(uintptr_t)&g_madam_fsm_poll);
  oj_u32(0xE59AA000u);           /* ldr  r10,[r10]  (deref)       */
  oj_u32(0xE35A0002u);           /* cmp  r10,#2  FSM_INPROCESS    */
  OJ_JNZ(l_ng3_fall);
  oj_jmp(s_oj_lab_entry0);
  oj_lab_here(l_ng3_fall);

  /* FIQ hoist (default ON, the aarch64 parity): r5 IS the fiqpend
   * VALUE pin — deref once, cmp, pending -> entry0 (the C side then
   * runs the vector at the exact cache timing point). */
  if(g_jit_fiqhoist && !g_jit_dsp_threaded)
    {
      uint32_t const l_ng4 = oj_lab_alloc();

      oj_u32(0xE5952000u);       /* ldr  r2,[r5]                  */
      oj_u32(0xE3520000u);      /* cmp  r2,#0 (ldr sets no flags) */
      OJ_JZ(l_ng4);
      oj_jmp(s_oj_lab_entry0);
      oj_lab_here(l_ng4);
    }
}


/* ---------------------------------------------------------------------------
 * Per-word tail: SCYCLE charge + FIQ poll + budget check (the x86
 * shape exactly; r7 = charge, r9 = remaining, r5 = the fiqpend
 * pointer-value pin — one deref like the aarch64 [x20]).
 * ------------------------------------------------------------------------- */
static void oj_tail(uint32_t const u15_at_tail_)
{
  uint32_t const l_nofiq = oj_lab_alloc();
  uint32_t const l_nobud = oj_lab_alloc();


  oj_u32(0xE2877001u);           /* add r7,r7,#1  SCYCLE           */

  if(!(g_jit_fiqhoist && !g_jit_dsp_threaded))
    {
      /* if(!ISF && *fpend) -> X_FIQ with USER[15]=u15_at_tail_ */
      oj_u32(0xE5952000u);       /* ldr  r2,[r5]   (fiqpend value)  */
      oj_u32(0xE3520000u);       /* cmp  r2,#0     (ldr sets no flags!) */
      OJ_JZ(l_nofiq);
      oj_ld_cpu(2u, OJ_CPSR);    /* ldr  r2,[r4,#CPSR]             */
      oj_u32(0xE3120040u);       /* tst  r2,#0x40  ISF              */
      OJ_JNZ(l_nofiq);
      oj_mov32(3u, u15_at_tail_);
      oj_st_cpu(3u, OJ_U15);
      oj_jmp_abs(g_jit_tramp_xfiq);
      oj_lab_here(l_nofiq);
    }

  if(!g_jit_budbatch)
    {
      oj_u32(0xE1570009u);      /* cmp  r7,r9: charge<remaining?
                                    (r9 = remaining, the trampoline pin;
                                    r6 is NOT a pin — the advisory caught
                                    this reading garbage) */
      OJ_JL(l_nobud);           /* charge below remaining: run on */
      oj_mov32(3u, u15_at_tail_);
      oj_st_cpu(3u, OJ_U15);
      oj_jmp_abs(g_jit_tramp_xbudget);
    }
  oj_lab_here(l_nobud);
}

/* charge an intra-word extra (beyond the tail's SCYCLE) */
static void oj_charge_extra(uint32_t const extra_)
{
  if(!extra_)
    return;

  if(extra_ <= 0xFFu)
    oj_u32(0xE2877000u | extra_);   /* add r7,r7,#extra (imm form for
                                      the common case — no scratch
                                      touched, matching aarch64) */
  else
    {
      /* PORT NOTE (the r12 choice): the >imm8 path clobbers its scratch
       * register, so the scratch must be a register that is DEAD at
       * every charge_extra call site. The DP emitter keeps op2 live in
       * r11 from the operand load until the ALU op consumes it, and
       * the aarch64 template calls charge_extra in that window (e.g.
       * the aarch64 1475 site: after the ALU, before cond_close/tail —
       * safe there only because its op2 w9 is dead by then, and the
       * arm32 1450 rd==15 site is after the result store so r11 is dead
       * there too). r12 (the pre-shift save) dies at the ALU op —
       * before every post-ALU charge site — so it is the safe scratch
       * for this path. Any emitter class porting next must re-verify
       * its own budgets against this contract. */
      oj_mov32(12u, extra_);
      oj_u32(0xE087700Cu);          /* add r7,r7,r12   */
    }
}

static void oj_exit_next(uint32_t const u15_)
{
  oj_mov32(3u, u15_);
  oj_st_cpu(3u, OJ_U15);
  oj_jmp_abs(g_jit_tramp_xnext);
}

static void oj_exit_cstep(uint32_t const word_pc_)
{
  oj_mov32(3u, word_pc_);
  oj_st_cpu(3u, OJ_U15);
  oj_jmp_abs(g_jit_tramp_xcstep);
}

#define OJ_NO_COND 0xFFFFFFFFu

/* ---------------------------------------------------------------------------
 * Conditional-guard emission (the oj_cond_prefix translation).
 * x86: mov eax,[CPSR]; shr 28; mov edx,cross[cond]; bt edx,eax; jnc skip.
 * aarch64: ldr/lsr/movz-table-entry/lsrv/tst/b.eq — 6 words.
 * ARM32 NATIVE (selftest7-verified 256/256 vs cond_flags_cross on
 * real ARM under qemu): the host's own APSR evaluates the guest's
 * condition directly.  msr APSR_nzcvq writes ONLY N/Z/C/V/Q
 * (mask 1000 — no mode bits, no T-bit touch; probe83:
 * msr APSR_nzcvq,rN = 0xE128F000|Rn), then b<inverse> skips the
 * body when the guest cond is FALSE:
 *   ldr  r2,[r4,#CPSR]          (oj_ld_cpu)
 *   msr  APSR_nzcvq, r2         (0xE128F002)
 *   b<inv> l_skip              (inverse cond = cond ^ 1: every ARM
 *                              cond pair is adjacent — eq/ne, cs/cc,
 *                              mi/pl, vs/vc, hi/ls, ge/lt, gt/le)
 * 3 words vs aarch64's 6 — the ARM32 dividend of encoding in the
 * guest's own ISA.
 *
 * CAVEAT for the emitter port: between the msr and the b<inv>, NO
 * flag-setting instruction may intervene.  The emitters may not
 * fold flag-consuming ops into this window (selftest7's first draft
 * failed exactly this way — a compiler-inserted cmp between msr and
 * b.cond read the WRONG flags; the msr must sit immediately before
 * the branch it serves).
 * ------------------------------------------------------------------------- */
static uint32_t oj_word_cond(uint32_t const cmd_)
{
  uint32_t const cond = (cmd_ >> 28);
  uint32_t const l_skip = oj_lab_alloc();

  if(cond == 0xEu)
    return OJ_NO_COND;
  if(l_skip == OJ_LAB_FULL)
    return OJ_LAB_FULL;  /* labels exhausted: the emitter MUST see this
                           * and bail to C-step — returning OJ_NO_COND
                           * here would execute the word unguarded, and
                           * close() would then write s_oj_lab[0xFFFFFFFE]
                           * (a ~4G-element wild index into the 512-entry
                           * array — the hang mechanism: corruption or
                           * SIGSEGV inside the compile loop) */

  oj_ld_cpu(2u, OJ_CPSR);
  oj_u32(0xE128F002u);          /* msr APSR_nzcvq, r2            */
  oj_jcc(cond ^ 1u, l_skip);    /* b<inverse>: guest cond false -> skip */
  return l_skip;
}

static void oj_word_cond_close(uint32_t const l_skip_)
{
  if((l_skip_ != OJ_NO_COND) && (l_skip_ != OJ_LAB_FULL))
    oj_lab_here(l_skip_);
}

/* NV (cond 0xF) words never execute — the caller tails past them.
 * MUST run before oj_word_cond: with cond=0xF the inverse branch
 * would be bNV, which armv7 executes as a HINT (not a branch), so
 * the body would always run.  (cond_flags_cross[0xF] == 0x0000.) */
static int oj_word_cond_is_nv(uint32_t const cmd_)
{
  return (cond_flags_cross[cmd_ >> 28] == 0);
}
/* ---------------------------------------------------------------------------
 * Per-word emitters — MILESTONE 0: every class returns 0 (C-step
 * boundary).  The trampoline/ABI/arena/icache/dispatch path is what
 * the oracle validates; per-class emitters land next, class by class.
 *
 * EMITTER-PORT INVARIANTS (violation = silent divergence, not a crash):
 *   V1  CONDITIONAL WORDS: oj_word_cond is LIVE (msr APSR_nzcvq +
 *       b<inverse>).  Every emitter that takes the aarch64 template's
 *       l_skip = oj_word_cond(cmd_) shape MUST also call
 *       oj_word_cond_close(l_skip) before EVERY exit (bail and tail
 *       alike) — a missing close leaves the skip branch dangling to
 *       an unbound label and oj_resolve() fails the whole block.
 *   V2  CHARGE DISCIPLINE on bail paths: a mid-emitter return 0 (the
 *       shift-edge / RRX C-step bails) must come AFTER zero charge
 *       contributions — never call oj_charge_extra or oj_tail before
 *       the bail decision.  oj_u32 emissions before the bail are dead
 *       bytes after the block's cstep exit (harmless), but any
 *       charge counted for a word that C-steps would double-charge
 *       the block and move the budget stop word.  (Verified
 *       discipline in the aarch64 DP: all three shift-edge bails
 *       precede their first charge site.)
 *   V3  FLAGS WINDOW: after the oj_word_cond msr, the guest NZCV sit
 *       in the HOST APSR.  The b<inverse> must be the NEXT emitted
 *       flag-consuming instruction; any folded compare between them
 *       reads the wrong flags (selftest7's first draft failed
 *       exactly this way under a compiler-inserted cmp).
 *   V4  DIAG-BISECT GATES: port the aarch64 per-emitter heads'
 *       if(getenv("OPERA_JIT_NO_<CLS>")) return 0; lines VERBATIM
 *       (NO_DP, NO_BRANCH, NO_MUL, NO_SDTL, NO_LIT, NO_BDT plus the
 *       BDT family NO_BDT_PC/_STM/_LDM, NO_SDTS, NO_SDTSR).  These
 *       are the first move of any lane-divergence bisect — a class
 *       that C-steps by env gate must produce the exact cache-engine
 *       oracle, isolating the divergence to the remaining classes.
 *       DELIBERATE (the aarch64 closure kept them through its own
 *       cleanup): compile-path getenv is per-block, not per-word.
 *       When running the 400-frame oracle, beware: any NO_* var set
 *       in the environment silently degrades that class to C-step.
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * DP flag machinery (the aarch64 w17/w28 helpers translated; all
 * words probe84/85/86-verified.  r3 = carry, r10 = result, r8/r12
 * scratch, r4 = &CPU).
 * ------------------------------------------------------------------------- */
static void oj_get_arm_c_r3(void)
{
  oj_ld_cpu(3u, OJ_CPSR);
  oj_u32(0xE1A03EA3u);      /* lsr r3,r3,#29 = (CPSR>>29)&1
                               [probe95: the old word E1A03E63 encoded
                               typ=3 (ROR) imm5=28 = ror r3,r3,#28 -
                               garbage read-back of C; the correct LSR
                               form is 0xE1A00000|(3<<12)|(1<<5)|(29<<7)|3] */
}

/* carry_out (r3 = 0/1) -> C bit: CPSR = (CPSR & ~C) | (carry<<29).
 *   ldr r12,[r4,#CPSR] ; bic r12,r12,#0x20000000 ;
 *   lsl r3,r3,#29 (orr r3,r3,lsl#29 shape) ; orr r12,r12,r3 ;
 *   str r12,[r4,#CPSR]
 * probe85: orr r10,r10,r11,lsl #29 = E18AAE8B — the orr rX,rX,rY,lsl#29
 * fused form works: orr r12,r12,r3,lsl #29 = 0xE18CC63(3)... build:
 *   orr rD,rN,rM,lsl #imm5 = 0xE1800000|(N<<16)|(D<<12)|(imm5<<7)|M
 *   = orr r12,r12,r3,lsl#29: E18C C E03? -> 0xE1800000|(12<<16)|(12<<12)|(29<<7)|3
 * bic r12,r12,#0x20000000 = E3CCC102 (probe84). */
static void oj_arm_set_c_r3(void)
{
  oj_ld_cpu(12u, OJ_CPSR);
  oj_u32(0xE3CCC202u);      /* bic r12,r12,#0x20000000 (clear C)
                                    [probe94: the old word E3CCC102
                                    encoded imm rot=1 -> 0x02 ror 2 =
                                    0x80000000, clearing N instead of
                                    C; the C bit never cleared and the
                                    stale N survived every set_c -
                                    the f255 timing-skew root cause;
                                    0x20000000 = 0x02 ror 4, rot=2] */
  /* the fused orr below shifts the 0/1 carry into bit29 inline —
   * an explicit shift here would destroy the value (advisory-caught:
   * the draft had a stray lsr r3,r3,#29 between bic and orr). */
  oj_u32(0xE18CCE83u);      /* orr r12,r12,r3,lsl #29
                               = 0xE1800000|(12<<16)|(12<<12)|(29<<7)|3
                               (fully computed; probe84 fused-orr law) */
  oj_st_cpu(12u, OJ_CPSR);
}

/* ARM_SET_ZN(result in r10): N = bit31, Z = (r==0); C/V preserved.
 *   ldr r12,[r4,#CPSR] ; bic r12,r12,#0xC0000000 (clear N,Z) ;
 *   and r3,r10,#0x80000000 ; adds r2,r10,#0?? — Z from cmp:
 *   cmp r10,#0 ; moveq r3plus... build:
 *   bic r12,r12,#0xC0000000 = E3CCC10F (probe84: 0xF0000000 form
 *   scaled — 0xC0000000 = imm8 0xC0 rot 2 => E3CCC102? see probe:
 *   bic r12,r12,#0xC0000000 = E3CCC103 from probe84) ;
 *   and r8,r10,#0x80000000 = E20A8102 (probe84) ;
 *   cmp r10,#0 = E35A0000 ; orreq r12,r12,#0x40000000 (038CC101
 *   probe84) ; orr r12,r12,r8 = E18C8008 ;
 *   str r12,[r4,#CPSR]                                        */
static void oj_arm_set_zn_r10(void)
{
  oj_ld_cpu(12u, OJ_CPSR);
  oj_u32(0xE3CCC103u);      /* bic r12,r12,#0xC0000000 (clear N,Z) */
  oj_u32(0xE20A8102u);      /* and r8,r10,#0x80000000 (N mask)     */
  oj_u32(0xE35A0000u);      /* cmp r10,#0                          */
  oj_u32(0x038CC101u);      /* orreq r12,r12,#0x40000000 (Z)       */
  oj_u32(0xE18CC008u);      /* orr r12,r12,r8 (N) [probe96: the old
                                    word E18C8008 had Rd=8 — the merge
                                    landed in scratch r8 and the CPSR's
                                    N stayed stale forever] */
  oj_st_cpu(12u, OJ_CPSR);
}

/* NZCV-from-host-flags fold (after adds/subs set them): ONE register
 * read — the A32 dividend:
 *   mrs r8,APSR = 0xE10F8000 (probe85) ; lsr r8,r8,#28 = E1A08E28 ;
 *   ldr r12,[r4,#CPSR] ; bic r12,r12,#0xF0000000 (E3CCC20F probe84) ;
 *   orr r12,r12,r8,lsl #28 ; str r12,[r4,#CPSR]                    */
static void oj_arm_set_cvzn_fold(void)
{
  oj_u32(0xE10F8000u);      /* mrs r8,APSR                        */
  oj_u32(0xE1A08E28u);      /* lsr r8,r8,#28:
                               0xE1A00000|(8<<12)|(1<<5)|(28<<7)|8 */
  oj_ld_cpu(12u, OJ_CPSR);
  oj_u32(0xE3CCC20Fu);      /* bic r12,r12,#0xF0000000            */
  oj_u32(0xE1800000u | (12u << 16) | (12u << 12) | (28u << 7) | 8u);
                            /* orr r12,r12,r8,lsl #28             */
  oj_st_cpu(12u, OJ_CPSR);
}

/* ---------------------------------------------------------------------------
 * The DP emitter (TIGHT_DP_IMM / _RI / _RS / _PC / _PCREL / _RMPC
 * shared driver — the aarch64 1006-1479 template translated to A32).
 *
 * Register conventions (probe84/85/86-verified words; pins r4=&CPU,
 * r5=fiqpend-value, r7=charge, r9=remaining are NEVER touched):
 *   r6  = op1 (USER[rn] or pc_k+8 const)      r11 = op2
 *   r10 = result                               r8 = common scratch
 *   r12 = pre-shift save (dies at the ALU op; also the >imm8 scratch in
 *        oj_charge_extra — r11 must NOT be used there: it is the live
 *        op2 between the operand load and the ALU op)
 * A32 DP laws (probe84/85):
 *   ALU reg:  0xE0000000|(opc<<21)|(S<<20)|(Rn<<16)|(Rd<<12)|shifter
 *   shifter imm:  (typ<<5)|(imm5<<7)|Rm   ; reg: (typ<<5)|(1<<4)|(Rs<<8)|Rm
 *   ALU imm12: 0xE0000000|(opc<<21)|(S<<20)|(Rn<<16)|(Rd<<12)|(rot<<8)|imm8
 *   shift rD,rM,#imm5,typ = 0xE1A00000|(D<<12)|(typ<<5)|(imm5<<7)|M
 *   mov rD,rM = 0xE1A00000|(D<<12)|M ; mvn rD,rM = 0xE1E00000|(D<<12)|M
 *   mrs rD,CPSR = 0xE10F0000|(D<<12)
 *   lsr r3,r11,#31 = E1A03FAB (carry = bit31) ; lsr r3,r12,#31 = E1A03FAC
 *   ldr r2,[r4,#CPSR=176] = E59420B0 ; and r2,r2,#0x1F = E202201F
 *   lsr r2,r2,#28 = E1A02E22
 * ------------------------------------------------------------------------- */
static int oj_emit_dp(uint32_t const cmd_, uint32_t const aux_,
                      uint32_t const pc_k_, uint32_t const cls_)
{
  if(getenv("OPERA_JIT_NO_DP")) return 0;   /* diag bisect [V4] */
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
  if(l_skip == OJ_LAB_FULL)
    return 0;                     /* labels exhausted: C-step the word */

  /* ---------------- MRS (opc5 16/20 alias) ----------------
   * USER[rd] = bit22 ? SPSR[arm_mode_table[CPSR&0x1F]] : CPSR.
   * MSR (18/22) never reaches the tight classes (decode filters).
   * Words (probe84/86 shapes):
   *   and r2,r2,#0x1F = E202201F ; ldr r2,[r4,#176] = E59420B0
   *   ldr r11,[r4,r2] = E794B002 (SPSR via mode table — the table
   *   lookup needs the C helper shape: ldr base from arm_mode_table) */
  if((opc5 == 16u) || (opc5 == 20u))
    {
      if((cmd_ >> 22) & 1)
        {
          /* SPSR[arm_mode_table[CPSR&0x1F]] — needs the mode-table
           * load; C-step this rare form for class-1 (measured
           * frequency in the aarch64 bring-up: only MRS CPSR ever
           * appeared). */
          oj_word_cond_close(l_skip);
          return 0;
        }
      oj_ld_cpu(11u, OJ_CPSR);
      oj_st_cpu(11u, OJ_U(rd));
      oj_charge_extra(0);
      oj_word_cond_close(l_skip);
      oj_tail(pc_k_ + 4);
      return 1;
    }

  /* ---------------- op2 into r11 ---------------- */
  if((cls_ == ARM_CLS_TIGHT_DP_IMM) ||
     (((cls_ == ARM_CLS_TIGHT_DP_PC) || (cls_ == ARM_CLS_TIGHT_DP_PCREL)) &&
      (cmd_ & (1u << 25))))
    {
      /* immediate + rot: op2 = ROR(imm8, rot*2).  Materialize imm8
       * then ror by the CONSTANT (rot2 is compile-time; imm5 covers
       * 1..31 — rot2 is even 0..30 so imm5=rot2 fits).  rot2==0 skips.
       * Carry (S+logic) = bit31 of rotated = bit(rot2-1) of original. */
      oj_mov32(11u, (cmd_ & 0xFF));
      { uint32_t const rot2 = ((cmd_ >> 7) & 0x1E);
        if(rot2)
          {
            oj_u32(0xE1A00000u | (11u << 12) | (3u << 5) |
                   (rot2 << 7) | 11u);          /* ror r11,r11,#rot2 */
            if(s_bit && logic)
              /* carry = bit31 of the rotated value:
               * lsr r3,r11,#31 = E1A03FAB */
              oj_u32(0xE1A03FABu);
          }
        else if(s_bit && logic)
          oj_get_arm_c_r3();      /* rot2==0: carry = current C */
      }
    }
  else if(cls_ == ARM_CLS_TIGHT_DP_RMPC)
    {
      /* rm == 15: op2 = pc_k + 8 (const), static shift per aux_ */
      oj_mov32(11u, pc_k_ + 8u);
      if(s_bit && logic)
        oj_u32(0xE1A0C00Bu);   /* mov r12,r11 — save pre-shift */
      { uint32_t const sh  = (aux_ & 0x3F);
        uint32_t const typ = ((aux_ >> 8) & 7);
        if((typ == 0) && (sh != 0) && (sh < 32))   /* LSL */
          {
            oj_u32(0xE1A00000u | (11u << 12) | (0u << 5) |
                   (sh << 7) | 11u);
            if(s_bit && logic)   /* carry = bit(32-sh) of original */
              oj_u32(0xE1A00000u | (3u << 12) | (1u << 5) |
                     ((32u - sh) << 7) | 12u);  /* lsr r3,r12,#(32-sh) */
          }
        else if((typ == 1) && (sh != 0) && (sh < 32))    /* LSR */
          {
            oj_u32(0xE1A00000u | (11u << 12) | (1u << 5) |
                   (sh << 7) | 11u);
            if(s_bit && logic)   /* carry = bit(sh-1) */
              oj_u32(0xE1A00000u | (3u << 12) | (1u << 5) |
                     ((sh - 1u) << 7) | 12u);
          }
        else if((typ == 2) && (sh != 0) && (sh < 32))    /* ASR */
          {
            oj_u32(0xE1A00000u | (11u << 12) | (2u << 5) |
                   (sh << 7) | 11u);
            if(s_bit && logic)   /* carry = bit(sh-1) */
              oj_u32(0xE1A00000u | (3u << 12) | (1u << 5) |
                     ((sh - 1u) << 7) | 12u);
          }
        else if((typ == 3) && (sh != 0))                  /* ROR */
          {
            oj_u32(0xE1A00000u | (11u << 12) | (3u << 5) |
                   (sh << 7) | 11u);
            if(s_bit && logic)   /* carry = result bit31 */
              oj_u32(0xE1A03FABu);   /* lsr r3,r11,#31 */
          }
        else
          { oj_word_cond_close(l_skip); return 0; }  /* RRX/edge: C-step */
      }
    }
  else
    {
      oj_ld_cpu(11u, OJ_U(cmd_ & 0xF));       /* r11 = USER[rm] */
      if((cls_ == ARM_CLS_TIGHT_DP_RI) ||
         (cls_ == ARM_CLS_TIGHT_DP_PC) || (cls_ == ARM_CLS_TIGHT_DP_PCREL))
        {
          /* register + static shift (aux_ holds promoted shift) */
          uint32_t const sh  = (aux_ & 0x3F);
          uint32_t const typ = ((aux_ >> 8) & 7);
          if((s_bit && logic) && ((sh != 0) || (typ != 0)))
            oj_u32(0xE1A0C00Bu);   /* mov r12,r11 — save pre-shift */
          if((sh != 0) || (typ != 0))
            {
              if((typ == 0) && (sh > 0) && (sh < 32))        /* LSL */
                {
                  oj_u32(0xE1A00000u | (11u << 12) |
                         (sh << 7) | 11u);
                  if(s_bit && logic)   /* carry = bit(32-sh) */
                    oj_u32(0xE1A00000u | (3u << 12) | (1u << 5) |
                           ((32u - sh) << 7) | 12u);
                }
              else if((typ == 1) && (sh > 0) && (sh < 32))   /* LSR */
                {
                  oj_u32(0xE1A00000u | (11u << 12) | (1u << 5) |
                         (sh << 7) | 11u);
                  if(s_bit && logic)   /* carry = bit(sh-1) */
                    { if(sh == 1u)   /* lsr #0 is LSR #32 on ARM (0),
                                      * not the aarch64 identity whose
                                      * lsl#29 extract relies on the
                                      * full value surviving: read bit0
                                      * directly */
                        oj_u32(0xE20C3001u);  /* and r3,r12,#1 [probe:
                                      the old word E2033001 had rn=r3 —
                                      it read the HOST r3's garbage
                                      instead of the pre-shift save
                                      r12, so the sh==1 carry was
                                      always a stale bit] */
                      else
                        oj_u32(0xE1A00000u | (3u << 12) | (1u << 5) |
                               ((sh - 1u) << 7) | 12u); }
                }
              else if((typ == 2) && (sh > 0) && (sh < 32))   /* ASR */
                {
                  oj_u32(0xE1A00000u | (11u << 12) | (2u << 5) |
                         (sh << 7) | 11u);
                  if(s_bit && logic)   /* carry = bit(sh-1) */
                    { if(sh == 1u)   /* same ARM lsr#0 trap as LSR */
                        oj_u32(0xE20C3001u);  /* and r3,r12,#1 (rn=r12) */
                      else
                        oj_u32(0xE1A00000u | (3u << 12) | (1u << 5) |
                               ((sh - 1u) << 7) | 12u); }
                }
              else if((typ == 3) && (sh > 0))                /* ROR */
                {
                  oj_u32(0xE1A00000u | (11u << 12) | (3u << 5) |
                         (sh << 7) | 11u);
                  if(s_bit && logic)   /* carry = result bit31 */
                    oj_u32(0xE1A03FABu);
                }
              else if((typ == 4))                            /* RRX */
                { oj_word_cond_close(l_skip); return 0; }
              else if((sh == 32) && (typ == 0))    /* LSL 32: val 0, carry bit0 */
                {
                  oj_u32(0xE3A0B000u);      /* mov r11,#0 */
                  if(s_bit && logic)
                    oj_u32(0xE20C3001u);  /* and r3,r12,#1 [probe85-class] */
                }
              else if((sh == 32) && (typ == 1))    /* LSR 32: val 0, carry bit31 */
                {
                  oj_u32(0xE3A0B000u);      /* mov r11,#0 */
                  if(s_bit && logic)
                    oj_u32(0xE1A03FACu);  /* lsr r3,r12,#31 */
                }
              else if((sh == 32) && (typ == 2))    /* ASR 32: sign, carry bit31 */
                {
                  oj_u32(0xE1A0B0CBu | (31u << 7));  /* asr r11,r11,#31:
                                    0xE1A00000|(11<<12)|(2<<5)|(31<<7)|11 */
                  if(s_bit && logic)
                    oj_u32(0xE1A03FACu);  /* lsr r3,r12,#31 */
                }
              else
                { oj_word_cond_close(l_skip); return 0; }
            }
          else if(s_bit && logic)
            oj_get_arm_c_r3();   /* no shift: carry = current C */
        }
      else
        {
          /* register shift: amount = USER[rs] & 0xFF at runtime —
           * the exact ARM_SHIFT_NSC boundary walk (aarch64 1203-1300
           * translated; all words probe84/85-verified forms).
           * r11 = value, r12 = pre-shift copy, r8 = amount,
           * r3 = carry_out. */
          uint32_t const l_s0   = oj_lab_alloc();
          uint32_t const l_big  = oj_lab_alloc();
          uint32_t const l_s32  = oj_lab_alloc();
          uint32_t const l_done = oj_lab_alloc();
          uint32_t const l_r0   = oj_lab_alloc();
          uint32_t const t      = ((cmd_ >> 5) & 3u);

          if((l_s0 == OJ_LAB_FULL) || (l_big == OJ_LAB_FULL) ||
             (l_s32 == OJ_LAB_FULL) || (l_done == OJ_LAB_FULL) ||
             (l_r0 == OJ_LAB_FULL))
            { oj_word_cond_close(l_skip); return 0; }  /* C-step */

          oj_u32(0xE1A0C00Bu);   /* mov r12,r11 — save pre-shift */
          oj_ld_cpu(8u, OJ_U((cmd_ >> 8) & 0xF));  /* r8 = USER[rs] */
          oj_u32(0xE20880FFu);  /* and r8,r8,#0xFF [probe88: the old
                                    hand-derivation emitted 0xE208800F =
                                    a 4-BIT mask — shift amounts >= 16
                                    truncated to 0..15, garbage op2, guest
                                    hang; the 5th bug of the hand-assembly
                                    batch that ended systematic probing] */
          oj_u32(0xE3580000u);   /* cmp r8,#0                        */
          OJ_JZ(l_s0);

          if((t == 0) || (t == 1))
            {
              /* LSL / LSR */
              oj_u32(0xE3580020u);   /* cmp r8,#32                     */
              OJ_JA(l_big);           /* unsigned > (hi)                */
              OJ_JZ(l_s32);
              if(t == 0)
                {
                  oj_u32(0xE1A0B81Bu); /* lsl r11,r11,r8 [probe87:
                                        0xE1A00000|(11<<12)|(typ<<5)|(1<<4)|(Rs<<8)|Rm] */
                  /* carry = bit(32-s) of original: rsb r3,r8,#0(=neg);
                   * lsr r3,r12,r3 ; and r3,r3,#1                        */
                  oj_u32(0xE2683020u); /* rsb r3,r8,#32 — LSL carry =
                                        bit(32-s) of the pre-shift value.
                                        ARM register-shift amounts mask
                                        to 8 bits: the two's-complement
                                        negation rsb #0 gave (2^32-s)&
                                        0xFF = 256-s >= 32, so lsr
                                        always yielded 0 and the carry
                                        was wrongly 0 for every 1<=s<=31
                                        (the f255 timing-skew root cause);
                                        32-s lands in 0..31. */
                  oj_u32(0xE1A0333Cu); /* lsr r3,r12,r3                  */
                  oj_u32(0xE2033001u); /* and r3,r3,#1                   */
                }
              else
                {
                  oj_u32(0xE1A0B83Bu); /* lsr r11,r11,r8                 */
                  /* carry = bit(s-1) */
                  oj_u32(0xE2483001u); /* sub r3,r8,#1 [probe97: the old
                                        word E2433001 encoded rn=r3 —
                                        'sub r3,r3,#1' — so the LSR-reg
                                        carry shifted by (r3-1), a stale
                                        guard scratch value, instead of
                                        (r8-1) = the shift amount minus
                                        one; every register-shifted LSR
                                        S-bit carry was wrong whenever
                                        the stale r3 didn't coincide with
                                        r8 — the f260 lane-bubble root
                                        cause] */
                  oj_u32(0xE1A0333Cu); /* lsr r3,r12,r3                  */
                  oj_u32(0xE2033001u); /* and r3,r3,#1                   */
                }
              oj_jmp(l_done);

              oj_lab_here(l_s32);
              if(t == 0)
                oj_u32(0xE20C3001u);  /* and r3,r12,#1 (carry = bit0)  */
              else
                oj_u32(0xE1A03FACu);  /* lsr r3,r12,#31 (carry=bit31)  */
              oj_u32(0xE3A0B000u);    /* mov r11,#0 (val = 0)          */
              oj_jmp(l_done);

              oj_lab_here(l_big);
              oj_u32(0xE3A03000u);    /* mov r3,#0 (carry = 0)         */
              oj_u32(0xE3A0B000u);    /* mov r11,#0 (val = 0)          */
              oj_jmp(l_done);
            }
          else if(t == 2)
            {
              /* ASR: s <= 31 normal; s >= 32 sign-fill, carry = sign.
               * [aarch64 fix class] the boundary test must be JG
               * (only s>31 takes the fill), NOT JLE. */
              oj_u32(0xE358001Fu);   /* cmp r8,#31                      */
              OJ_JG(l_big);
              oj_u32(0xE1A0B85Bu);   /* asr r11,r11,r8                  */
              /* carry = bit(s-1) */
              oj_u32(0xE2483001u);   /* sub r3,r8,#1 [probe97: the old
                                      word E2433001 encoded rn=r3 — see
                                      the LSR note above] */
              oj_u32(0xE1A0333Cu);   /* lsr r3,r12,r3                   */
              oj_u32(0xE2033001u);   /* and r3,r3,#1                    */
              oj_jmp(l_done);

              oj_lab_here(l_big);
              oj_u32(0xE1A03FACu);   /* lsr r3,r12,#31 (carry = sign)  */
              oj_u32(0xE1A0B0CBu | (31u << 7));  /* asr r11,r12,#31:
                                    sign-fill 0xE1A00000|(11<<12)|(2<<5)|(31<<7)|12 */
              oj_jmp(l_done);
            }
          else
            {
              /* ROR: s&31 != 0 rotate; s&31 == 0 (s>0) val unchanged,
               * carry = v>>31 — the ARM_SHIFT_NSC case-3 semantics */
              oj_u32(0xE208801Fu);   /* and r8,r8,#31 (was and r17,w28,#31
                                       — the aarch64 0x12001391 shape) */
              OJ_JZ(l_r0);           /* cbz -> jz                        */
              oj_u32(0xE1A0B87Bu);   /* ror r11,r11,r8                   */
              oj_u32(0xE1A03FABu);   /* lsr r3,r11,#31 (carry = bit31 of
                                      * the rotated result)              */
              oj_jmp(l_done);

              oj_lab_here(l_r0);
              oj_u32(0xE1A03FACu);   /* lsr r3,r12,#31 (carry; val kept) */
              oj_jmp(l_done);
            }

          oj_lab_here(l_s0);
          oj_get_arm_c_r3();        /* carry = current C; val unchanged  */
          oj_lab_here(l_done);
        }
    }

  /* ---------------- op1 into r6 ---------------- */
  rn = ((cmd_ >> 16) & 0xF);
  if(cls_ == ARM_CLS_TIGHT_DP_PCREL)
    oj_mov32(6u, pc_k_ + 8u);                 /* op1 = pc_k + 8 */
  else
    oj_ld_cpu(6u, OJ_U(rn));                  /* r6 = USER[rn] */

  /* ---------------- shifter carry into C for S+logic ---------------- */
  if(s_bit && logic)
    oj_arm_set_c_r3();   /* r3 = shifter carry (routes above set it) */

  /* ---------------- ALU ---------------- */
  switch(opcode)
    {
    case 0:  /* AND */
      oj_u32(0xE006A00Bu);           /* and r10,r6,r11 */
      break;
    case 1:  /* EOR */
      oj_u32(0xE026A00Bu);           /* eor r10,r6,r11 */
      break;
    case 2:  /* SUB */
      oj_u32(s_bit ? 0xE056A00Bu : 0xE046A00Bu);  /* subs/sub */
      break;
    case 3:  /* RSB */
      oj_u32(s_bit ? 0xE076A00Bu : 0xE066A00Bu);  /* rsbs/rsb */
      break;
    case 4:  /* ADD */
      oj_u32(s_bit ? 0xE096A00Bu : 0xE086A00Bu);  /* adds/add */
      break;
    case 5:  /* ADC */
      /* carry-in: adcs reads the HOST C — the guest CPSR NZCV must be
       * in the host APSR first (the oj_word_cond msr already did it
       * for conditional words; unconditional ones need it here). */
      oj_ld_cpu(8u, OJ_CPSR);
      oj_u32(0xE128F008u);         /* msr APSR_nzcvq, r8              */
      oj_u32(s_bit ? 0xE0B6A00Bu : 0xE0A6A00Bu);  /* adcs/adc */
      break;
    case 6:  /* SBC */
      oj_ld_cpu(8u, OJ_CPSR);
      oj_u32(0xE128F008u);         /* msr APSR_nzcvq, r8              */
      oj_u32(s_bit ? 0xE0D6A00Bu : 0xE0C6A00Bu);  /* sbcs/sbc */
      break;
    case 7:  /* RSC */
      oj_ld_cpu(8u, OJ_CPSR);
      oj_u32(0xE128F008u);         /* msr APSR_nzcvq, r8              */
      oj_u32(s_bit ? 0xE0F6A00Bu : 0xE0E6A00Bu);  /* rscs/rsc */
      break;
    case 8:  /* TST: compute into r10 — the S+logic zn fold reads r10 */
      oj_u32(0xE006A00Bu);           /* and r10,r6,r11 (case-0 word) */
      break;
    case 9:  /* TEQ: compute into r10 — the S+logic zn fold reads r10 */
      oj_u32(0xE026A00Bu);           /* eor r10,r6,r11 (case-1 word) */
      break;
    case 10: /* CMP: subs r10,r6,r11 — flags set, result in r10 */
      oj_u32(0xE056A00Bu);           /* subs r10,r6,r11 */
      break;
    case 11: /* CMN: adds r10,r6,r11 — flags set */
      oj_u32(0xE096A00Bu);           /* adds r10,r6,r11 */
      break;
    case 12: /* ORR */
      oj_u32(0xE186A00Bu);           /* orr r10,r6,r11 */
      break;
    case 13: /* MOV */
      oj_u32(0xE1A0A00Bu);           /* mov r10,r11 */
      break;
    case 14: /* BIC */
      oj_u32(0xE1C6A00Bu);           /* bic r10,r6,r11 */
      break;
    case 15: /* MVN: operand is r11 (op2), NOT r6 (op1) — the aarch64
              * probe68 bug class.  mvn rD,rM = 0xE1E00000|(D<<12)|M */
      oj_u32(0xE1E0A00Bu);           /* mvn r10,r11 */
      break;
    default:
      oj_word_cond_close(l_skip);
      return 0;
    }

  /* ---------------- flags + writeback ---------------- */
  if(s_bit)
    {
      if(logic)
        oj_arm_set_zn_r10();
      else
        oj_arm_set_cvzn_fold();
    }

  if((opcode != 8) && (opcode != 9) && (opcode != 10) && (opcode != 11))
    oj_st_cpu(10u, OJ_U(rd));        /* USER[rd] = result */

  /* rd == 15 (pc write, e.g. `mov pc, #imm`): S=0 forms fully
   * inlineable; the ALU result already landed in USER[15]; the word
   * terminates the block (the taken tail re-reads USER[15] through
   * the trampoline stubs).  S=1 (subs pc,...) restores CPSR from
   * SPSR with a bank switch: those stay full-class C words. */
  if(rd == 15 && !s_bit)
    {
      oj_charge_extra(ICYCLE + NCYCLE);
      {
        /* the oj_tail shape WITHOUT the u15 materialization (the
         * ALU result already sits in USER[15] and must survive) */
        uint32_t const l_nofiq = oj_lab_alloc();
        uint32_t const l_nobud = oj_lab_alloc();


        oj_u32(0xE2877001u);       /* add r7,r7,#1  SCYCLE        */
        oj_u32(0xE5952000u);       /* ldr r2,[r5]                 */
        oj_u32(0xE3520000u);       /* cmp r2,#0                   */
        OJ_JZ(l_nofiq);
        oj_ld_cpu(2u, OJ_CPSR);
        oj_u32(0xE3120040u);       /* tst r2,#0x40  ISF           */
        OJ_JNZ(l_nofiq);
        oj_jmp_abs(g_jit_tramp_xfiq);
        oj_lab_here(l_nofiq);
        if(!g_jit_budbatch)
          {
            oj_u32(0xE1570009u);   /* cmp r7,r9                   */
            OJ_JL(l_nobud);
            oj_jmp_abs(g_jit_tramp_xbudget);
          }
        oj_lab_here(l_nobud);
        oj_jmp_abs(g_jit_tramp_xnext);
      }

      /* cond-fail lands here: nothing executed, ordinary tail — the
       * skipped word still costs its SCYCLE (aarch64 fix class). */
      oj_word_cond_close(l_skip);
      oj_tail(pc_k_ + 4);
      oj_exit_next(pc_k_ + 4);
      return 2;                 /* block terminates at the pc write */
    }

  /* RS words cost an extra ICYCLE over the base SCYCLE (the reg-
   * shifted operand's pipeline stall). */
  oj_charge_extra((cls_ == ARM_CLS_TIGHT_DP_RS) ? 1 : 0);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  return 1;
}

/* ---------------------------------------------------------------------------
 * TIGHT_MUL — includes the rd == rm quirk and the exact
 * ((calcbits(rs) + 5) >> 1) - 1 (clamped to 16) runtime cost
 * (the aarch64 1560-1647 / x86 1706-1800 template translated).
 *
 * Register plan (probe104 words; pins untouched): r11 = USER[rs] /
 * walk scratch (the multiplier), r8 = USER[rm], r6 = USER[rn]
 * (accumulate), r10 = result, r7 = charge pin.  ARM's `mul Rd,Rm,Rs`
 * requires Rd != Rm on classic cores (UNPREDICTABLE otherwise — and
 * armcc-era silicon enforces it), so the product is computed as
 * `mul r10,r8,r11` with rm staged in r8, never mul r10,r10,r11.
 * clz is an ARMv5T+ instruction — available (armv7-a target).
 * Words (probe104):
 *   clz r11,r11 = E16FBF1B ; rsb r11,r11,#0 = E26BB000
 *   add r11,r11,#32 = E28BB020 ; add r11,r11,#5 = E28BB005
 *   lsr r11,r11,#1 = E1A0B0AB ; sub r11,r11,#1 = E24BB001
 *   cmp r11,#16 = E35B0010 ; mov r11,#1 = E3A0B001
 *   mov r11,#16 = E3A0B010 ; mov r10,#0 = E3A0A000
 *   add r7,r7,r11 = E087700B
 *   mul r10,r8,r11 = E00A0B98 ; mla r10,r8,r11,r6 = E02A6B98
 * ------------------------------------------------------------------------- */
static int oj_emit_mul(uint32_t const cmd_, uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_MUL")) return 0;   /* diag bisect [V4] */
  uint32_t l_skip;
  uint32_t l_cb0, l_cbd, l_clamp;
  uint32_t const rm = (cmd_ & 0xF);
  uint32_t const rs = ((cmd_ >> 8) & 0xF);
  uint32_t const rn = ((cmd_ >> 12) & 0xF);
  uint32_t const rd = ((cmd_ >> 16) & 0xF);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4u);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  if(l_skip == OJ_LAB_FULL)
    return 0;                     /* labels exhausted: C-step the word */


  /* cost walk: r11 = USER[rs]; calcbits -> ((n+5)>>1)-1 clamp 16; the
   * charge pin r7 absorbs it (aarch64's w23 add shape). */
  oj_ld_cpu(11u, OJ_U(rs));
  l_cb0   = oj_lab_alloc();
  l_cbd   = oj_lab_alloc();
  l_clamp = oj_lab_alloc();
  if((l_cb0 == OJ_LAB_FULL) || (l_cbd == OJ_LAB_FULL) ||
     (l_clamp == OJ_LAB_FULL))
    { oj_word_cond_close(l_skip); return 0; }
  oj_u32(0xE35B0000u);           /* cmp r11,#0 (zero test)             */
  OJ_JZ(l_cb0);
  oj_u32(0xE16FBF1Bu);           /* clz r11,r11                        */
  oj_u32(0xE26BB000u);           /* rsb r11,r11,#0 (-clz)              */
  oj_u32(0xE28BB020u);           /* add r11,r11,#32 -> 32-clz = the bit
                                  * length [the aarch64 bug fixed by
                                  * probe73: stopping at -clz made every
                                  * nonzero rs produce a negative
                                  * calcbits and the clamp pinned 16 —
                                  * up-to-15x overcharge]              */
  oj_jmp(l_cbd);
  oj_lab_here(l_cb0);
  oj_u32(0xE3A0B001u);           /* mov r11,#1 (calcbits(0)==1)        */
  oj_lab_here(l_cbd);
  oj_u32(0xE28BB005u);           /* add r11,r11,#5                     */
  oj_u32(0xE1A0B0ABu);           /* lsr r11,r11,#1                     */
  oj_u32(0xE24BB001u);           /* sub r11,r11,#1                     */
  oj_u32(0xE35B0010u);           /* cmp r11,#16                        */
  OJ_JLE(l_clamp);               /* <= 16: keep                        */
  oj_u32(0xE3A0B010u);           /* mov r11,#16                        */
  oj_lab_here(l_clamp);
  oj_u32(0xE087700Bu);           /* add r7,r7,r11 (charge)             */

  /* result: r11 = USER[rs] again (the walk clobbered it), r8 = USER[rm],
   * product `mul r10,r8,r11` (Rd != Rm required on ARM). */
  if(rd == rm)
    {
      /* quirk: rd == rm keeps old accumulate/0 semantics, no multiply */
      if(cmd_ & (1u << 21))
        oj_ld_cpu(10u, OJ_U(rn));      /* r10 = USER[rn]             */
      else
        oj_u32(0xE3A0A000u);           /* mov r10,#0                  */
    }
  else
    {
      oj_ld_cpu(8u, OJ_U(rm));         /* r8 = USER[rm]               */
      oj_ld_cpu(11u, OJ_U(rs));        /* r11 = USER[rs] (reload)     */
      if(cmd_ & (1u << 21))
        {
          oj_ld_cpu(6u, OJ_U(rn));     /* r6 = USER[rn] (accumulate)  */
          oj_u32(0xE02A6B98u);         /* mla r10,r8,r11,r6           */
        }
      else
        oj_u32(0xE00A0B98u);           /* mul r10,r8,r11              */
    }

  if(cmd_ & (1u << 20))
    oj_arm_set_zn_r10();

  oj_st_cpu(10u, OJ_U(rd));

  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4u);
  return 1;
}

static int oj_emit_branch(uint32_t const cmd_, uint32_t const pc_k_)
{
  int32_t  const off    = ((int32_t)(cmd_ << 8)) >> 6;
  if(getenv("OPERA_JIT_NO_BRANCH")) return 0;   /* diag bisect [V4] */
  uint32_t const target = (uint32_t)(pc_k_ + 8 + off);
  uint32_t       l_fall;

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_fall = oj_word_cond(cmd_);
  if(l_fall == OJ_LAB_FULL)
    return 0;                   /* labels exhausted: C-step */

  /* taken path */
  if(cmd_ & (1 << 24))            /* L: USER[14] = pc_k + 4 (return addr) */
    {
      oj_mov32(2u, pc_k_ + 4u);   /* r2 = return address (dead here: the
                                   * guard's msr consumed it)          */
      oj_st_cpu(2u, OJ_U(14));
    }
  oj_charge_extra(SCYCLE + NCYCLE);
  oj_tail(target);
  /* backward-edge threading: a static target already emitted inside
   * this same block becomes a direct in-block jump (register
   * invariants hold at every word start: r4/r5/r7/r9 pinned,
   * r6/r8/r10/r11/r12 transient). */
  if((target < pc_k_) && (target >= s_oj_block_pc))
    {
      uint32_t const tw = ((target - s_oj_block_pc) >> 2);
      uint32_t const at = s_oj_word_off[tw];
      /* PREFIT/budbatch: one check per backedge pass.
       * cmp r7(charge),r9(remaining); blt ok (charge<remaining) —
       * the same operand order and word as oj_tail's budget check
       * (E1570009 probe-verified).  l_be_ok needs no OJ_LAB_FULL
       * guard: the compile loop's headroom guard (opera_arm_jit.c
       * "nlab > OJ_MAXLAB-32") breaks BEFORE any word with < 32
       * labels left; the branch emitter allocates <= 7. */
      if(g_jit_budbatch)
        {
          uint32_t const l_be_ok = oj_lab_alloc();
          oj_u32(0xE1570009u);        /* cmp r7,r9                        */
          OJ_JL(l_be_ok);             /* charge < remaining -> continue  */
          oj_mov32(3u, target);
          oj_st_cpu(3u, OJ_U15);
          oj_jmp_abs(g_jit_tramp_xbudget);
          oj_lab_here(l_be_ok);
        }
      /* in-block backward jump, emitted DIRECTLY with the known offset
       * (x86's jmp rel32 shape, translated): the target word is inside
       * this block, so at < s_oj_len and the displacement is a compile-
       * time constant.  Avoids the aarch64 l_back label indirection,
       * which carries the OJ_LAB_FULL wild-write hazard documented at
       * oj_word_cond (unprotected s_oj_lab[l_back] = at). */
      oj_u32(0xEA000000u | ((((uint32_t)(at - s_oj_len - 8u)) >> 2)
                            & 0x00FFFFFFu));
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
 * SDT loads (LDI = imm offset, LDR = reg offset, plus the NP load shapes
 * routed from the dispatcher) — the aarch64 1661-1789 template translated.
 *
 * Register conventions (probe98/99-verified words; pins r4=&CPU, r5=fiqpend,
 * r7=charge, r9=remaining NEVER touched):
 *   r6  = base USER[rn] -> new base (survives the whole body)
 *   r11 = offset (imm12 const or USER[rm] shifted in place; no carry
 *        semantics in SDT shifts, so no pre-shift save)
 *   r8  = tbas
 *   r10 = static materialization scratch (&RAM_SIZE, then &DRAM)
 *   r12 = RAM_SIZE value / word-path rotation amount
 *   r3  = loaded value (dead before the tail's own r3 use)
 * Words (probe99):
 *   rsb r11,r11,#0 = E26BB000 ; add r6,r6,r11 = E086600B
 *   mov r8,r6 = E1A08006     ; ldr r12,[r10] = E59AC000
 *   cmp r8,r12 = E158000C    ; eor r8,r8,#3 = E2288003
 *   ldrb r3,[r12,r8] = E7DC3008 ; and r12,r8,#3 = E208C003
 *   mov r12,r12,lsl #3 = E1A0C18C ; bic r8,r8,#3 = E3C88003
 *   ldr r3,[r12,r8] = E79C3008 ; ror r3,r3,r12 = E1A03C73
 *   ldr r10,[r10] = E59AA000 (prologue shape: DRAM pointer deref)
 * Semantics (arm_sdt_body_np, opera_arm.c:1968): tbas=base for post-index
 * (P=0) and base+oper2 for pre-index (P=1, the new base); load byte =
 * DRAM[tbas^3], word = LE32 at DRAM[tbas&~3] rotated right by (tbas&3)*8;
 * writeback USER[rn]=base iff (W || !P); then USER[rd]=val (the value
 * store wins on rn==rd).  Slow exit (tbas >= RAM_SIZE, unsigned) stays
 * SIDE-EFFECT-FREE — the C-step re-executes the word from scratch.
 * ------------------------------------------------------------------------- */
static int oj_emit_sdt_load(uint32_t const cmd_, uint32_t const aux_,
                            uint32_t const pc_k_, uint32_t const cls_)
{
  if(getenv("OPERA_JIT_NO_SDTL")) return 0;   /* diag bisect [V4] */
  uint32_t l_skip;
  uint32_t l_slow;
  uint32_t const rn = ((cmd_ >> 16) & 0xF);
  uint32_t const rd = ((cmd_ >> 12) & 0xF);
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  if(l_skip == OJ_LAB_FULL)
    return 0;                     /* labels exhausted: C-step the word */
  l_slow = oj_lab_alloc();
  if(l_slow == OJ_LAB_FULL)
    { oj_word_cond_close(l_skip); return 0; }

  /* r6 = base = USER[rn] */
  oj_ld_cpu(6u, OJ_U(rn));

  /* r11 = offset */
  if(cls_ == ARM_CLS_TIGHT_SDT_LDI)
    {
      oj_mov32(11u, (cmd_ & 0x0FFFu));   /* imm12 */
    }
  else
    {
      oj_ld_cpu(11u, OJ_U(cmd_ & 0xF));   /* r11 = USER[rm] */
      /* promoted static shift (aux_), applied in place — SDT shifts have
       * NO carry semantics (no flags from the shifter), so no pre-shift
       * save is needed.  The packer promotes sh==0 (typ!=0) to sh=32/typ4
       * (LSR#0->32, ASR#0->32, ROR#0->RRX), so the ladder is complete. */
      {
        uint32_t const sh  = (aux_ & 0x3F);
        uint32_t const typ = ((aux_ >> 8) & 7);
        if((typ == 0) && (sh > 0) && (sh < 32))
          oj_u32(0xE1A00000u | (11u << 12) | (0u << 5) |
                 (sh << 7) | 11u);               /* lsl r11,r11,#sh */
        else if((typ == 1) && (sh > 0) && (sh < 32))
          oj_u32(0xE1A00000u | (11u << 12) | (1u << 5) |
                 (sh << 7) | 11u);               /* lsr r11,r11,#sh */
        else if((typ == 2) && (sh > 0) && (sh < 32))
          oj_u32(0xE1A00000u | (11u << 12) | (2u << 5) |
                 (sh << 7) | 11u);               /* asr r11,r11,#sh */
        else if((typ == 3) && (sh > 0))
          oj_u32(0xE1A00000u | (11u << 12) | (3u << 5) |
                 (sh << 7) | 11u);               /* ror r11,r11,#sh */
        else if((sh == 32) && (typ == 0 || typ == 1))
          oj_u32(0xE3A0B000u);                   /* mov r11,#0 */
        else if((sh == 32) && (typ == 2))
          oj_u32(0xE1A00000u | (11u << 12) | (2u << 5) |
                 (31u << 7) | 11u);              /* asr r11,r11,#31 */
        else if(typ == 4)
          { oj_word_cond_close(l_skip); return 0; }  /* RRX: C-step [V2] */
        /* (sh==0,typ==0): identity — nothing to emit */
      }
    }

  if(!(cmd_ & (1 << 23)))
    oj_u32(0xE26BB000u);         /* rsb r11,r11,#0 (U=0: negate offset) */

  if(cmd_ & (1 << 24))
    {
      oj_u32(0xE086600Bu);       /* add r6,r6,r11 (pre: new base)       */
      oj_u32(0xE1A08006u);       /* mov r8,r6 (tbas = new base)         */
    }
  else
    {
      oj_u32(0xE1A08006u);       /* mov r8,r6 (tbas = OLD base)        */
      oj_u32(0xE086600Bu);       /* add r6,r6,r11 (new base)           */
    }
  /* single raw-tbas range check (the aarch64 shape): tbas >= RAM_SIZE
   * (unsigned) -> slow.  RAM_SIZE is a 32-bit lvalue: materialize its
   * address, load the value into r12 (dies at the cmp); then re-load
   * r10 with the DRAM POINTER (deref) — r10 is the load BASE in both
   * paths, keeping r12 free for the word-path rotation [probe100: the
   * first draft read [r12,r8] with r12 still holding RAM_SIZE — the
   * segv the OPERA_JIT_NO_SDTL bisect pinned in 3 words of log]. */
  oj_mov32(10u, (uint32_t)(uintptr_t)&RAM_SIZE);
  oj_u32(0xE59AC000u);           /* ldr r12,[r10]  (RAM_SIZE value)    */
  oj_u32(0xE158000Cu);           /* cmp r8,r12 (unsigned for bhs)      */
  OJ_JHS(l_slow);
  oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM);
  oj_u32(0xE59AA000u);           /* ldr r10,[r10]  (DRAM pointer)      */

  if(is_byte)
    {
      /* val = DRAM[tbas ^ 3] (BE byte-within-word selection) */
      oj_u32(0xE2288003u);       /* eor r8,r8,#3                       */
      oj_u32(0xE7DA3008u);       /* ldrb r3,[r10,r8]                   */
    }
  else
    {
      /* word: rot = (tbas & 3) * 8 saved in r12 BEFORE the masked load;
       * tbas &= ~3 in r8; val = LE32 at DRAM[tbas&~3] (host byte order
       * on LE arm32 — the raw ldr reads it in host order); then
       * ror r3,r3,r12 (rotate-by-0 is identity — no l_norot label,
       * unlike the x86 shape whose shl-based rot needed it). */
      oj_u32(0xE208C003u);       /* and r12,r8,#3                      */
      oj_u32(0xE1A0C18Cu);       /* lsl r12,r12,#3                     */
      oj_u32(0xE3C88003u);       /* bic r8,r8,#3                       */
      oj_u32(0xE79A3008u);       /* ldr r3,[r10,r8]                    */
      oj_u32(0xE1A03C73u);       /* ror r3,r3,r12                      */
    }

  /* writeback AFTER the load (slow exit stays side-effect free);
   * (W || !P) holds for every tight class (P=0,W=0 shapes never reach
   * the tight decode) and for both NP classes (P=0,W=1 by definition) */
  if((cmd_ & (1 << 21)) || !(cmd_ & (1 << 24)))
    oj_st_cpu(6u, OJ_U(rn));     /* USER[rn] = new base                */
  oj_st_cpu(3u, OJ_U(rd));      /* USER[rd] = val (wins on rn==rd)    */

  oj_charge_extra(NCYCLE + ICYCLE);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);             /* jump over the slow stub            */
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * SDT stores (STI = imm, STR = reg offset) — the aarch64 2205-2406
 * template translated.  The shared body assumes offset already in r11.
 *
 * Register plan (probe105/106 words; pins untouched): r6 = base/new-
 * base (SURVIVES the gates — arm32's mov32 scratch is r10 only, so
 * unlike aarch64's w16 it needs no park), r11 = offset -> word index,
 * r8 = tbas (byte domain for the range/HIRES gates), r10 = static
 * scratch -> array base -> DRAM base, r12 = loaded gate values,
 * r3 = stored value.
 *
 * Gate order (must match the C body): RAM_SIZE range -> HIRES fanout
 * (HIRESMODE && tbas >= DRAM_SIZE -> slow) -> invalidation probe
 * (word_cov[tbas>>2] != 0 -> slow, the C kill path).  The store is
 * byte = DRAM[tbas^3], word = DRAM[tbas&~3].  Writeback (W || !P)
 * after the store.
 * Words (probe105/106): strb r3,[r10,r8] = E7CA3008 ;
 *   str r3,[r10,r8] = E78A3008 ; ldrb r12,[r10,r11] = E7DAC00B ;
 *   ldrb r12,[r10] = E5DAC000 ; ldr r12,[r10] = E59AC000 ;
 *   lsr r11,r8,#2 = E1A0B128 ; mov r8,r6 = E1A08006
 * ------------------------------------------------------------------------- */
static int oj_sdt_store_body(uint32_t const cmd_, uint32_t const pc_k_)
{
  uint32_t const l_slow = oj_lab_alloc();
  uint32_t const l_nh   = oj_lab_alloc();
  uint32_t const l_ck   = oj_lab_alloc();
  uint32_t const l_go   = oj_lab_alloc();
  uint32_t const rn = ((cmd_ >> 16) & 0xF);
  uint32_t const rd = ((cmd_ >> 12) & 0xF);
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  oj_ld_cpu(6u, OJ_U(rn));        /* r6 = base                          */

  if(!(cmd_ & (1 << 23)))
    oj_u32(0xE26BB000u);          /* rsb r11,r11,#0 (negate offset)    */

  if(cmd_ & (1 << 24))
    {
      oj_u32(0xE086600Bu);        /* add r6,r6,r11 (pre: new base)      */
      oj_u32(0xE1A08006u);        /* mov r8,r6 (tbas = address)         */
    }
  else
    {
      oj_u32(0xE1A08006u);        /* mov r8,r6 (tbas = old base)        */
      oj_u32(0xE086600Bu);        /* add r6,r6,r11 (new base)           */
    }

  /* range gate vs RAM_SIZE (byte domain; the word store masks tbas
   * for the access itself, not for the gates) */
  oj_mov32(10u, (uint32_t)(uintptr_t)&RAM_SIZE);
  oj_u32(0xE59AC000u);           /* ldr r12,[r10] (RAM_SIZE)           */
  oj_u32(0xE158000Cu);           /* cmp r8,r12                         */
  OJ_JHS(l_slow);

  /* HIRES gate: HIRESMODE && tbas >= DRAM_SIZE -> slow */
  oj_mov32(10u, (uint32_t)(uintptr_t)&HIRESMODE);
  oj_u32(0xE5DAC000u);           /* ldrb r12,[r10]                     */
  oj_u32(0xE35C0000u);           /* cmp r12,#0 (not hires?)            */
  OJ_JZ(l_nh);
  oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM_SIZE);
  oj_u32(0xE59AC000u);           /* ldr r12,[r10] (DRAM_SIZE)          */
  oj_u32(0xE158000Cu);           /* cmp r8,r12 (byte domain)           */
  OJ_JHS(l_slow);
  oj_lab_here(l_nh);

  /* invalidation probe (ALL stores, byte and word): word index =
   * tbas >> 2 (identical for both paths); idx < g_jit_ram_words ->
   * probe g_jit_word_cov[idx]; covered -> slow (the C kill path).
   * Out of window -> clean store. */
  oj_u32(0xE1A0B128u);           /* lsr r11,r8,#2 (word index)         */
  oj_mov32(10u, (uint32_t)(uintptr_t)&g_jit_ram_words);
  oj_u32(0xE59AC000u);           /* ldr r12,[r10] (ram_words)          */
  oj_u32(0xE15B000Cu);           /* cmp r11,r12 (idx vs limit)         */
  OJ_JLS(l_ck);                  /* idx < limit: probe                 */
  oj_jmp(l_go);                  /* out of window: clean store         */
  oj_lab_here(l_ck);
  oj_mov32(10u, (uint32_t)(uintptr_t)&g_jit_word_cov);
  oj_u32(0xE59AA000u);           /* ldr r10,[r10] (array base)         */
  oj_u32(0xE7DAC00Bu);           /* ldrb r12,[r10,r11]                 */
  oj_u32(0xE35C0000u);           /* cmp r12,#0                         */
  OJ_JNZ(l_slow);                /* covered -> C kill path             */
  oj_lab_here(l_go);

  /* the store itself */
  if(is_byte)
    {
      oj_u32(0xE2288003u);       /* eor r8,r8,#3 (BE byte select)      */
      oj_ld_cpu(3u, OJ_U(rd));    /* r3 = the value                     */
      oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM);
      oj_u32(0xE59AA000u);       /* ldr r10,[r10] (DRAM pointer)       */
      oj_u32(0xE7CA3008u);       /* strb r3,[r10,r8]                   */
    }
  else
    {
      oj_u32(0xE3C88003u);       /* bic r8,r8,#3 (align)               */
      oj_ld_cpu(3u, OJ_U(rd));    /* r3 = the value                     */
      oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM);
      oj_u32(0xE59AA000u);       /* ldr r10,[r10] (DRAM pointer)       */
      oj_u32(0xE78A3008u);       /* str r3,[r10,r8]                    */
    }

  /* writeback: r6 = new base (SURVIVED the gates — no park needed
   * on arm32; aarch64 parks in w27 because its w16 dies at movabs) */
  if((cmd_ & (1 << 21)) || !(cmd_ & (1 << 24)))
    oj_st_cpu(6u, OJ_U(rn));

  oj_charge_extra((uint32_t)(2 * NCYCLE - SCYCLE));
  /* NOTE: no oj_tail here — the tail belongs to the EMITTER after
   * oj_word_cond_close, so the cond-fail skip path runs it too (the
   * cache charges a cond-failed word its 1 SCYCLE and budget-checks
   * there; a body-internal tail would let the skip path bypass both). */
  {
    /* the clean path must NOT fall into the slow stub below: every
     * clean store would otherwise charge twice and re-execute
     * through the C-step (the x86 l_done shape). */
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
  if(getenv("OPERA_JIT_NO_SDTS")) return 0;   /* diag bisect [V4] */
  uint32_t l_skip;

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4u);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  if(l_skip == OJ_LAB_FULL)
    return 0;                     /* labels exhausted: C-step the word */

  oj_mov32(11u, (cmd_ & 0x0FFFu));   /* r11 = imm12 offset             */
  {
    int const r_ = oj_sdt_store_body(cmd_, pc_k_);
    oj_word_cond_close(l_skip);   /* skip path joins here: the cond-failed
                                   * word still pays its SCYCLE + budget
                                   * check via the tail below (both
                                   * paths converge on one tail)        */
    oj_tail(pc_k_ + 4u);
    return r_;
  }
}

static int oj_emit_sdt_store_r(uint32_t const cmd_, uint32_t const aux_,
                               uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_SDTSR")) return 0;  /* diag bisect [V4] */
  uint32_t l_skip;
  uint32_t const sh  = (aux_ & 0x3F);
  uint32_t const typ = ((aux_ >> 8) & 7);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4u);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  if(l_skip == OJ_LAB_FULL)
    return 0;                     /* labels exhausted: C-step the word */

  /* r11 = USER[rm] with the promoted static shift — the same in-place
   * ladder as the SDT-load emitter (no carry semantics in SDT). */
  oj_ld_cpu(11u, OJ_U(cmd_ & 0xF));
  if((sh != 0) || (typ != 0))
    {
      if((typ == 0) && (sh > 0) && (sh < 32))
        oj_u32(0xE1A00000u | (11u << 12) | (0u << 5) |
               (sh << 7) | 11u);               /* lsl r11,r11,#sh */
      else if((typ == 1) && (sh > 0) && (sh < 32))
        oj_u32(0xE1A00000u | (11u << 12) | (1u << 5) |
               (sh << 7) | 11u);               /* lsr r11,r11,#sh */
      else if((typ == 2) && (sh > 0) && (sh < 32))
        oj_u32(0xE1A00000u | (11u << 12) | (2u << 5) |
               (sh << 7) | 11u);               /* asr r11,r11,#sh */
      else if((typ == 3) && (sh > 0))
        oj_u32(0xE1A00000u | (11u << 12) | (3u << 5) |
               (sh << 7) | 11u);               /* ror r11,r11,#sh */
      else if((sh == 32) && ((typ == 0) || (typ == 1)))
        oj_u32(0xE3A0B000u);                   /* mov r11,#0 */
      else if((sh == 32) && (typ == 2))
        oj_u32(0xE1A00000u | (11u << 12) | (2u << 5) |
               (31u << 7) | 11u);              /* asr r11,r11,#31 */
      else
        { oj_word_cond_close(l_skip); return 0; }  /* RRX: C-step [V2] */
    }

  {
    int const r_ = oj_sdt_store_body(cmd_, pc_k_);
    oj_word_cond_close(l_skip);   /* skip path joins here — one shared tail */
    oj_tail(pc_k_ + 4u);
    return r_;
  }
}
/* ---------------------------------------------------------------------------
 * PC-relative literal load (LDR rd,[pc,#imm] et al.) — ARM_CLS_SDT_IMM
 * words with rn==15, P=1, W=0, rd!=15 (the aarch64 1791-1873 / x86
 * 2030-2110 template translated).  The address is a COMPILE-TIME
 * constant (pc_k_ + 8 +/- imm12), so the rora of the word path is
 * static too — a compile-time imm5 `ror`, closer to the aarch64
 * template's static form than sdt_load's register rotation.
 *
 * Register plan (probe99/100/103 words; pins untouched): r8 = addr,
 * r10 = static scratch -> DRAM base (the load base), r12 = RAM_SIZE
 * value, r3 = loaded value.  Baking the address creates no
 * invalidation hazard: DRAM contents stay runtime-fresh per read
 * (each execution re-loads through the DRAM pointer).
 * ------------------------------------------------------------------------- */
static int oj_emit_sdt_literal(uint32_t const cmd_, uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_LIT")) return 0;    /* diag bisect [V4] */
  uint32_t l_skip;
  uint32_t l_slow;
  uint32_t addr;
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  /* gate down to the plain literal shape (the template's gates) */
  if((cmd_ & (1 << 20)) == 0)
    return 0;                     /* loads only                        */
  if(((cmd_ >> 16) & 0xF) != 0xF)
    return 0;                     /* rn == pc                          */
  if(((cmd_ >> 12) & 0xF) == 0xF)
    return 0;                     /* rd != pc                          */
  if(!(cmd_ & (1 << 24)) || (cmd_ & (1 << 21)))
    return 0;                     /* P=1, W=0                          */

  addr = (pc_k_ + 8u);
  if(cmd_ & (1 << 23))
    addr += (cmd_ & 0x0FFFu);
  else
    addr -= (cmd_ & 0x0FFFu);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4u);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  if(l_skip == OJ_LAB_FULL)
    return 0;                     /* labels exhausted: C-step the word  */
  l_slow = oj_lab_alloc();
  if(l_slow == OJ_LAB_FULL)
    { oj_word_cond_close(l_skip); return 0; }

  /* r8 = addr (compile-time constant — bake it) */
  oj_mov32(8u, addr);

  /* range check addr vs RAM_SIZE (runtime), then load DRAM[addr^3] /
   * DRAM[addr&~3] through the DRAM pointer — same shapes as sdt_load
   * (probe99/100), r10 the load base, r12 free after the cmp. */
  oj_mov32(10u, (uint32_t)(uintptr_t)&RAM_SIZE);
  oj_u32(0xE59AC000u);           /* ldr r12,[r10]  (RAM_SIZE value)    */
  oj_u32(0xE158000Cu);           /* cmp r8,r12 (unsigned for bhs)      */
  OJ_JHS(l_slow);
  oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM);
  oj_u32(0xE59AA000u);           /* ldr r10,[r10]  (DRAM pointer)      */

  if(is_byte)
    {
      /* val = DRAM[addr ^ 3] — the XOR bakes into the mov32 */
      oj_mov32(8u, (addr ^ 3u));
      oj_u32(0xE7DA3008u);       /* ldrb r3,[r10,r8]                   */
    }
  else
    {
      oj_mov32(8u, (addr & ~3u));
      oj_u32(0xE79A3008u);       /* ldr r3,[r10,r8]                    */
      if(addr & 3)
        /* static rotation: ror r3,r3,#(addr&3)*8 (probe103 law —
         * 0xE1A00000|(3<<12)|(3<<5)|(imm5<<7)|3)                  */
        oj_u32(0xE1A03463u | (((addr & 3u) * 8u) << 7));
    }

  oj_st_cpu(3u, OJ_U((cmd_ >> 12) & 0xF));   /* USER[rd] = val        */

  oj_charge_extra(NCYCLE + ICYCLE);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4u);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);             /* jump over the slow stub            */
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * BDT (LDM/STM) — the aarch64 1898-2188 template translated.
 *
 * Register plan (probe107/108 words; pins untouched): r6 = the
 * first-element address (derived ONCE — it survives every gate, no
 * aarch64-style re-derivation: the only mov32 scratch is r10), r11 =
 * the transfer address (mov r6 once, then add #4 per element), r3 =
 * value, r8 = window-gate scratch, r10 = static scratch -> array/
 * DRAM base, r12 = loaded gate values.
 *
 * Gate ladder: window gate ([first, first+4(n-1)] within RAM_SIZE)
 * -> stores only: HIRES fanout (end >= DRAM_SIZE -> slow) and the
 * word_cov invalidation probes (FIRST and LAST covered word) ->
 * per-register transfers -> writeback (bit21) -> LDM {..,pc} ends
 * the block with a hand-rolled tail (the loaded pc must survive in
 * USER[15]).
 * Words (probe107/108): sub/add r6,#imm12 = E2466xxx/E2866xxx ;
 *   add r8,r6,#imm12 = E2868xxx ; mov r8,r6 = E1A08006 ;
 *   bic r8,r6,#3 = E3C68003 ; mov r11,r6 = E1A0B006 ;
 *   add r11,r11,#4 = E28BB004 ; ldr/str r3,[r10,r11] = E79A300B/
 *   E78A300B
 * ------------------------------------------------------------------------- */
static int oj_emit_bdt(uint32_t const cmd_, uint32_t const pc_k_)
{
  if(getenv("OPERA_JIT_NO_BDT")) return 0;   /* diag bisect [V4] */
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

  /* compile-time gates (the template's gates verbatim) */
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
  if(l_skip == OJ_LAB_FULL)
    return 0;                     /* labels exhausted: C-step the word */
  l_slow = oj_lab_alloc();
  if(l_slow == OJ_LAB_FULL)
    { oj_word_cond_close(l_skip); return 0; }

  /* ---- first element address (r6, derived ONCE — survives) ---- */
  oj_ld_cpu(6u, OJ_U(rn));          /* r6 = base                       */
  if(!(cmd_ & (1 << 24)))          /* P = 0 */
    {
      if(!(cmd_ & (1 << 23)))      /* DA: first = base - 4*(n-1) */
        {
          if(n > 1)
            oj_u32(0xE2466000u | (4u * (n - 1u)));
        }
    }
  else                             /* P = 1 */
    {
      if(cmd_ & (1 << 23))         /* IB: first = base + 4 */
        oj_u32(0xE2866004u);
      else                         /* DB: first = base - 4*n */
        oj_u32(0xE2466000u | (4u * n));
    }

  /* ---- window gate: [first, first + 4*(n-1)] within RAM_SIZE ---- */
  /* r8 = r6 & ~3; + 4*(n-1) if n>1; >= RAM_SIZE -> slow */
  oj_u32(0xE3C68003u);           /* bic r8,r6,#3                       */
  if(n > 1)
    oj_u32(0xE2888000u | (4u * (n - 1u)));   /* add r8,r8,#4(n-1)       */
  oj_mov32(10u, (uint32_t)(uintptr_t)&RAM_SIZE);
  oj_u32(0xE59AC000u);           /* ldr r12,[r10] (RAM_SIZE)           */
  oj_u32(0xE158000Cu);           /* cmp r8,r12                         */
  OJ_JHS(l_slow);

  if(!is_ldm)
    {
      /* stores: HIRES fanout gate (end >= DRAM_SIZE -> slow) */
      uint32_t const l_nh = oj_lab_alloc();
      oj_mov32(10u, (uint32_t)(uintptr_t)&HIRESMODE);
      oj_u32(0xE5DAC000u);       /* ldrb r12,[r10]                     */
      oj_u32(0xE35C0000u);       /* cmp r12,#0 (not hires?)            */
      OJ_JZ(l_nh);
      oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM_SIZE);
      oj_u32(0xE59AC000u);       /* ldr r12,[r10] (DRAM_SIZE)          */
      oj_u32(0xE158000Cu);       /* cmp r8,r12 (window end)           */
      OJ_JHS(l_slow);
      oj_lab_here(l_nh);

      /* Invalidation probes on EVERY word in the transfer window, not
       * just the first and last.  The two-point w0/wN probe let a
       * multi-register STM whose window strictly contained a block in
       * its interior overwrite compiled code without invalidating it
       * (missed self-modifying-code kill: stale block, guest-visible
       * divergence).  Bounded loop over all n words: r11 = current
       * word address (starts at wN, steps down by 4), r8 = remaining
       * count, r3 = word index scratch, r10 = static mov32 scratch,
       * r12 = loaded values.  Covered -> l_slow, where the C cstep
       * re-executes the whole STM through the hooked per-element
       * writes and kills exactly like a single store would.
       * Out-of-window words (VRAM targets past g_jit_ram_words) skip
       * the probe and continue the loop. */
      {
        uint32_t const l_ni  = oj_lab_alloc();  /* loop done           */
        uint32_t const l_top = oj_lab_alloc();  /* probe head          */
        uint32_t const l_ck2 = oj_lab_alloc();  /* probe body          */
        uint32_t const l_sk  = oj_lab_alloc();  /* skip one            */
        oj_u32(0xE3A08000u | n);  /* mov r8,#n (count)                   */
        oj_u32(0xE3C6B003u);      /* bic r11,r6,#3 (w0)                  */
        if(n > 1)
          oj_u32(0xE28BB000u | (4u * (n - 1u))); /* add r11,r11,#4(n-1) */
        oj_lab_here(l_top);
        oj_u32(0xE1A0312Bu);      /* lsr r3,r11,#2 (idx)                 */
        oj_mov32(10u, (uint32_t)(uintptr_t)&g_jit_ram_words);
        oj_u32(0xE59AC000u);      /* ldr r12,[r10] (ram_words)          */
        oj_u32(0xE153000Cu);      /* cmp r3,r12                         */
        OJ_JLS(l_ck2);            /* idx < limit: run the probe         */
        oj_jmp(l_sk);             /* idx >= limit: out of window, skip   */
        oj_lab_here(l_ck2);
        oj_mov32(10u, (uint32_t)(uintptr_t)&g_jit_word_cov);
        oj_u32(0xE59AA000u);      /* ldr r10,[r10] (array base)         */
        oj_u32(0xE7DAC003u);      /* ldrb r12,[r10,r3]                  */
        oj_u32(0xE35C0000u);      /* cmp r12,#0                         */
        OJ_JNZ(l_slow);           /* covered -> C kill path             */
        oj_lab_here(l_sk);
        oj_u32(0xE2508001u);      /* subs r8,r8,#1                      */
        OJ_JZ(l_ni);              /* count exhausted: done              */
        oj_u32(0xE24BB004u);      /* sub r11,r11,#4                     */
        oj_jmp(l_top);
        oj_lab_here(l_ni);
      }
    }

      /* ---- STM writeback preload (rn in the list, a listed register
       * below rn): stm_accur writes USER[rn] = base_ BEFORE the store
       * loop, so the rn slot in memory receives base_, not the
       * original register value (opera_arm.c stm_accur preload).  The
       * LDM preload (ldm_accur) fires for ANY rn-in-list shape but the
       * rn slot's load overwrites it, so emitting it is harmless and
       * keeps the twin shapes aligned.  r6 holds the first-element
       * address and SURVIVES the transfers (no re-derivation needed,
       * unlike the aarch64 twins whose w16 is clobbered); compute wb
       * into the r3 scratch and store to USER[rn]. */
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

          oj_u32(0xE1A03006u);     /* mov r3,r6 (first)                 */
          if(adj)
            oj_u32((sub ? 0xE2433000u : 0xE2833000u) | adj);
          oj_st_cpu(3u, OJ_U(rn));
        }

  /* ---- transfers: element i at first + 4*i (r6 SURVIVES — no
   * re-derivation; r11 = the transfer address, +4 per element) ---- */
  disp = 0;
  for(i = 0; i < 16; i++)
    {
      if(!((list >> i) & 1))
        continue;

      if(disp)
        oj_u32(0xE28BB004u);     /* add r11,r11,#4 — next element      */
      else
        oj_u32(0xE1A0B006u);    /* mov r11,r6 — first transfer addr    */

      if(is_ldm)
        {
          oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM);
          oj_u32(0xE59AA000u);   /* ldr r10,[r10] (DRAM pointer)       */
          oj_u32(0xE79A300Bu);   /* ldr r3,[r10,r11]                    */
          oj_st_cpu(3u, OJ_U(i));
        }
      else
        {
          if(i != 15)
            oj_ld_cpu(3u, OJ_U(i));
          else
            oj_mov32(3u, pc_k_ + 12u);   /* probed USER[15] value      */
          oj_mov32(10u, (uint32_t)(uintptr_t)&DRAM);
          oj_u32(0xE59AA000u);   /* ldr r10,[r10] (DRAM pointer)       */
          oj_u32(0xE78A300Bu);   /* str r3,[r10,r11]                    */
        }
      disp += 4;
    }

  /* ---- writeback (bit21), after transfers ----
   * Skip the rn-in-list preload shapes: USER[rn] already holds base_
   * there (the preload above), so applying +/- 4n again would double
   * it. */
  if((cmd_ & (1 << 21)) &&
     (!is_ldm
      ? !(list & (1u << rn)) || !(list & ((1u << rn) - 1u))
      : !(list & (1u << rn))))
    {
      uint32_t const adj = (4u * n);

      oj_ld_cpu(6u, OJ_U(rn));       /* r6 = ORIGINAL base              */
      if(cmd_ & (1 << 23))
        oj_u32(0xE2866000u | adj);   /* add r6,r6,#4n (+)              */
      else
        oj_u32(0xE2466000u | adj);   /* sub r6,r6,#4n (-)              */
      oj_st_cpu(6u, OJ_U(rn));
    }

  if(is_ldm && (list & 0x8000))
    {
      /* LDM {.., pc}: the loaded pc is in USER[15]; hand-rolled tail
       * (the DP rd==15 shape), no imm u15 materialization. */
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


        oj_u32(0xE2877001u);       /* add r7,r7,#1  SCYCLE              */
        oj_u32(0xE5952000u);       /* ldr r2,[r5]                       */
        oj_u32(0xE3520000u);       /* cmp r2,#0                         */
        OJ_JZ(l_nofiq);
        oj_ld_cpu(2u, OJ_CPSR);
        oj_u32(0xE3120040u);       /* tst r2,#0x40  ISF                 */
        OJ_JNZ(l_nofiq);
        oj_jmp_abs(g_jit_tramp_xfiq);
        oj_lab_here(l_nofiq);
        if(!g_jit_budbatch)
          {
            oj_u32(0xE1570009u);   /* cmp r7,r9                         */
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
    oj_jmp(l_done);             /* jump over the slow stub            */
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

/* --------------------------- arena hooks ---------------------------------- */

/* RWX arena.  Linux: anonymous mmap (same as the aarch64 hook).
 *
 * WINDOWS-ON-ARM32: NOT SUPPORTED, BY DESIGN — Windows on ARM32
 * (Windows RT, armv7) requires thumb-2 code for user mode; this
 * backend emits ARM-mode words (every encoding in this file is
 * A32).  A Windows port would be a full thumb-2 REWRITE (different
 * encoders for every word, not an arena delta), and the selection
 * matrix never routes _WIN32 arm here.  Verified empirically: zig's
 * arm-windows-gnu target rejects ARM-mode functions outright
 * ("target does not support ARM mode execution" — SEH unwind on
 * WoA assumes thumb).  If someone ever needs it: thumb-2 has the
 * same primitive set (movw/movt/lsl/ldrb exist in T32), so the
 * per-word probe loop would port, but every constant in this file
 * would need re-derivation. */
static uint8_t *ojb_alloc_arena(uint32_t const size_)
{
  void *const p = mmap(NULL,(size_t)size_,
                       (PROT_READ | PROT_WRITE | PROT_EXEC),
                       (MAP_PRIVATE | MAP_ANONYMOUS),-1,0);
  return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
}

/* icache coherence: MANDATORY on arm32.  The core calls this at both
 * commit sites. */
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
