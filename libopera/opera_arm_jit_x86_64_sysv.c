#include <sys/mman.h>

#ifdef _WIN32
#error "opera_arm_jit_x86_64_sysv.c is the SysV backend; Windows x64 must use opera_arm_jit_x86_64_win.c (check opera_arm_jit_backend.h)"
#endif

/* ---------------------------------------------------------------------------
 * opera_arm_jit_x86_64_sysv.c — x86-64 SysV encoding backend for the ARM60
 * block-JIT (the original, gate-proven backend).
 *
 * Included into the jit TU by opera_arm.c AFTER the shared core headers;
 * defines every emitter symbol the shared core references by name.
 * Backend contract (see opera_arm_jit_backend.h): s_oj/s_oj_len/fixups/
 * labels, oj_* emission helpers, oj_emit_* per-word emitters, the
 * trampoline builder, the arena allocator.
 *
 * ENABLEMENT: (x86-64 && !Windows) — SysV AMD64 ABI only.  Win64 is a
 * separate backend (opera_arm_jit_x86_64_win.c) with its own ABI.
 * Gated by OPERA_JIT_ENABLE_X86_64_SYSV (computed in opera_arm.c from
 * arch+OS macros; master kill-switch OPERA_JIT_BACKENDS=0 disables all).
 *
 * Host register allocation / stack discipline / exit protocol:
 * (comment moved from opera_arm_jit.c verbatim)
 *   rbx = &CPU          rbp = *g_clio_fiqpend        (poll pointer)
 *   r14 = *g_cdrom_restart_poll                      (bool poll pointer)
 *   r15 = *g_madam_fsm_poll                          (poll pointer)
 *   r12d = block cycle accumulator (positive; mirrors `total -= cyc`)
 *   r13d = remaining budget for this slice (budget_ - total)
 *   r11  = &carry_out   rax/rcx/rdx/rsi/rdi/r8-r10   scratch
 * Block signature: uint32_t entry(int32_t budget_remaining) (edi).
 * SysV: entered by call (rsp=8 mod 16); six callee-saved pushes keep
 * rsp = 8 mod 16; prologue sub rsp,8 -> body rsp = 0 mod 16 (SSE-align).
 * ------------------------------------------------------------------------- */

/* ----------------------------- emitter -----------------------------------
 * Raw x86-64 emission into a 32KB scratch buffer, copied to the arena once
 * complete.  rel32 placeholder fix-ups are resolved at the end. */

#define OJ_BUF     (32u << 10)
#define OJ_MAXFIX  2048u
#define OJ_MAXLAB  320u

static uint8_t  s_oj[OJ_BUF];
static uint32_t s_oj_len;
/* emission-start offset and pc of each guest word in the in-flight block,
 * for backward-branch threading: a taken branch whose static target was
 * already emitted jumps there directly instead of round-tripping through
 * the dispatcher. */
static uint32_t s_oj_word_off[JIT_MAX_WORDS];
static uint32_t s_oj_block_pc;
static uint32_t s_oj_lab_entry0;   /* prologue guard trap: C-step word 0 */
static uint32_t s_oj_fix_at[OJ_MAXFIX];
static uint32_t s_oj_fix_lab[OJ_MAXFIX];
static uint32_t s_oj_nfix;
static uint32_t s_oj_lab[OJ_MAXLAB];
static uint32_t s_oj_nlab;

/* exit-stub labels, one set per compiled block */
/* shared trampoline: per-slice run loop + exit stubs.
 * Blocks are prologue-less (pins live across the slice) and returnless
 * (every exit path is an absolute jump into one of these stubs); r10d
 * carries the exit reason into the shared epilog, which computes the
 * run's charged total (budget_start - remaining) into g_jit_cycles and
 * returns the reason in eax to the C dispatcher. */
#define TRAMP_X_INTERP     100u   /* pc outside the fetch windows        */
#define TRAMP_X_UNALIGNED  101u   /* unaligned pc: C-step one word       */
#define TRAMP_X_COMPILE    102u   /* no block at this pc yet             */

static uint8_t   *g_jit_tramp_page;
static uintptr_t  g_jit_tramp_entry;
static uintptr_t  g_jit_tramp_xnext;
static uintptr_t  g_jit_tramp_xfiq;
static uintptr_t  g_jit_tramp_xbudget;
static uintptr_t  g_jit_tramp_xrst;
static uintptr_t  g_jit_tramp_xfsm;
static uintptr_t  g_jit_tramp_xcstep;
static uint8_t    t_cur_[4096];
static uint32_t   t_len_;

#define T1(b)            (t_cur_[t_len_++] = (uint8_t)(b))
#define T4(v)            do { uint32_t u_ = (v); \
                              memcpy(&t_cur_[t_len_],&u_,4); t_len_ += 4; \
                            } while(0)
#define T8(v)            do { uint64_t u_ = (v); \
                              memcpy(&t_cur_[t_len_],&u_,8); t_len_ += 8; \
                            } while(0)
#define TABS(r,a)        do { T1((r) ? (0x49) : (0x48)); T1(0xB8 + 0); } while(0)  /* unused */

/* registers used by the trampoline emitter (raw encoding numbers) */
#define T_RAX 0u
#define T_RBX 3u
#define T_RDX 2u
#define T_R11 11u

/* scratch-module local copy of oj_movabs for the tramp page (raw writer) */
static void t_movabs(uint32_t const reg_, uint64_t const imm64_)
{
  T1((reg_ > 7) ? 0x49 : 0x48);   /* REX.W (+REX.B for r8-r15) */
  T1(0xB8 + (reg_ & 7));
  T8(imm64_);
}

/* long jump inside the tramp page given a target position */
static void t_jmp_here(uint32_t const target_pos_)
{
  T1(0xE9);
  T4((uint32_t)((int64_t)target_pos_ - ((int64_t)t_len_ + 4)));
}

static void t_jcc_here(uint32_t const cc_, uint32_t const target_pos_)
{
  T1(0x0F);
  T1(cc_);
  T4((uint32_t)((int64_t)target_pos_ - ((int64_t)t_len_ + 4)));
}

static uint32_t opera_arm_jit_build_trampoline(void)
{
  uint32_t  l_loop;
  uint32_t  l_slow_interp;
  uint32_t  l_slow_unaligned;
  uint32_t  l_slow_compile;
  uint32_t  l_slow_budget;
  uint32_t  l_epilog;
  uint32_t  jumps_to_epilog[16];
  uint32_t  njumps;
  uint32_t  i;
  uint32_t  p_jnz_unaligned;
  uint32_t  p_jae_interp;
  uint32_t  p_jz_compile;
  uint32_t  p_jle_budget;

#define T_JMP_EPI()   do { T1(0xE9); jumps_to_epilog[njumps++] = t_len_; T4(0); } while(0)

  t_len_  = 0;
  njumps  = 0;

  /* ------------------------------ entry ------------------------------
   * rsp = 8 (mod 16) at entry (post-return-push).  One call spans a whole
   * ARM slice run; [rsp] = remaining budget, [rsp+8] = budget start. */
  T1(0x53);                       /* push rbx */
  T1(0x55);                       /* push rbp */
  T1(0x41); T1(0x54);             /* push r12 */
  T1(0x41); T1(0x55);             /* push r13 */
  T1(0x41); T1(0x56);             /* push r14 */
  T1(0x41); T1(0x57);             /* push r15 */
  T1(0x48); T1(0x83); T1(0xEC); T1(0x18);   /* sub rsp, 24 */
  T1(0x89); T1(0x3C); T1(0x24);             /* mov [rsp], edi   remaining  */
  T1(0x89); T1(0x7C); T1(0x24); T1(0x08);   /* mov [rsp+8], edi budget     */

  t_movabs(T_RBX, (uint64_t)(uintptr_t)&CPU);
  t_movabs(T_RAX, (uint64_t)(uintptr_t)&g_clio_fiqpend);
  T1(0x48); T1(0x8B); T1(0x28);           /* mov rbp, [rax] */
  t_movabs(T_RAX, (uint64_t)(uintptr_t)&g_cdrom_restart_poll);
  T1(0x4C); T1(0x8B); T1(0x30);           /* mov r14, [rax] */
  t_movabs(T_RAX, (uint64_t)(uintptr_t)&g_madam_fsm_poll);
  T1(0x4C); T1(0x8B); T1(0x38);           /* mov r15, [rax] */

  /* ------------------------------ dispatch -----------------------------
   * rsp = 0 (mod 16) here; `call rsi` pushes the return address, giving
   * rsp = 8 (mod 16) at block entry.  Blocks never return: every exit is
   * an absolute jump into one of the tramp stubs below. */
  l_loop = t_len_;
  T1(0x8B); T1(0x43); T1(0x3C);           /* mov eax, [rbx + 0x3C]  pc    */
  T1(0x89); T1(0xC1);                     /* mov ecx, eax                 */
  T1(0xA8); T1(0x03);                     /* test al, 3                   */
  T1(0x0F); T1(0x85); p_jnz_unaligned = t_len_; T4(0);   /* jnz -> slow  */
  T1(0xC1); T1(0xE9); T1(0x02);           /* shr ecx, 2                   */
  /* Gate must mirror arm_jit_pc_index: only the DRAM window
   * [0,RAM_SIZE) -> [0,ram_words) dispatches blocks (ROM pcs raw>>2
   * far exceed entries and always interped; pcs in [RAM_SIZE,
   * 4*entries) would otherwise pass with a -1 window index and arm
   * the compiler with (uint32_t)-1).  The C handler guards this too;
   * the tight gate keeps hole pcs off the C round-trip entirely. */
  t_movabs(T_RDX, (uint64_t)(uintptr_t)&g_jit_ram_words);
  T1(0x3B); T1(0x0A);                     /* cmp ecx, [rdx]               */
  T1(0x0F); T1(0x83); p_jae_interp = t_len_; T4(0);      /* jae -> slow  */
  t_movabs(T_RDX, (uint64_t)(uintptr_t)&g_jit_table);
  T1(0x48); T1(0x8B); T1(0x12);           /* mov rdx, [rdx]  (array base) */
  T1(0x48); T1(0x8B); T1(0x34); T1(0xCA); /* mov rsi, [rdx + rcx*8]       */
  T1(0x48); T1(0x85); T1(0xF6);           /* test rsi, rsi (struct ptr)   */
  T1(0x0F); T1(0x84); p_jz_compile = t_len_; T4(0);      /* jz  -> slow  */
  T1(0x48); T1(0x8B); T1(0x36);           /* mov rsi, [rsi]  (blk->code)  */
  T1(0x44); T1(0x8B); T1(0x2C); T1(0x24); /* mov r13d, [rsp]  remaining   */
  T1(0x45); T1(0x31); T1(0xE4);           /* xor r12d, r12d               */
  T1(0xFF); T1(0xD6);                     /* call rsi  (never returns)    */
  /* ---------------------------- X_NEXT -------------------------------
   * Block-exit stubs are entered at the callee level of `call rsi`: one
   * dead 8-byte return address below the [rsp]=remaining / [rsp+8]=budget
   * frame, so each drops it with `add rsp, 8` before folding r12d. */
  g_jit_tramp_xnext = t_len_;
  T1(0x48); T1(0x83); T1(0xC4); T1(0x08); /* add rsp, 8 (dead RA)         */
  T1(0x44); T1(0x29); T1(0x24); T1(0x24); /* sub [rsp], r12d              */
  T1(0x83); T1(0x3C); T1(0x24); T1(0x00); /* cmp dword [rsp], 0           */
  T1(0x0F); T1(0x8E); p_jle_budget = t_len_; T4(0);      /* jle -> slow  */
  t_jmp_here(l_loop);

  /* ---------------------------- X_FIQ -------------------------------- */
  g_jit_tramp_xfiq = t_len_;
  T1(0x48); T1(0x83); T1(0xC4); T1(0x08); /* add rsp, 8 (dead RA)         */
  T1(0x44); T1(0x29); T1(0x24); T1(0x24); /* sub [rsp], r12d              */
  t_movabs(T_RAX, (uint64_t)(uintptr_t)&arm_fiq_vector);
  T1(0xFF); T1(0xD0);                     /* call rax (rsp already 16-aligned
                                             * post dead-RA drop: no shim)  */
  T1(0x41); T1(0xBA); T4(JIT_X_FIQ);      /* mov r10d, JIT_X_FIQ          */
  T_JMP_EPI();

  /* --------------------- plain exit-reason stubs ---------------------- */
  g_jit_tramp_xbudget = t_len_;
  T1(0x48); T1(0x83); T1(0xC4); T1(0x08); /* add rsp, 8 (dead RA)         */
  T1(0x44); T1(0x29); T1(0x24); T1(0x24); /* sub [rsp], r12d              */
  T1(0x41); T1(0xBA); T4(JIT_X_BUDGET);   /* mov r10d, JIT_X_BUDGET       */
  T_JMP_EPI();

  g_jit_tramp_xrst = t_len_;
  T1(0x48); T1(0x83); T1(0xC4); T1(0x08); /* add rsp, 8 (dead RA)         */
  T1(0x44); T1(0x29); T1(0x24); T1(0x24); /* sub [rsp], r12d              */
  T1(0x41); T1(0xBA); T4(JIT_X_RST);      /* mov r10d, JIT_X_RST          */
  T_JMP_EPI();

  g_jit_tramp_xfsm = t_len_;
  T1(0x48); T1(0x83); T1(0xC4); T1(0x08); /* add rsp, 8 (dead RA)         */
  T1(0x44); T1(0x29); T1(0x24); T1(0x24); /* sub [rsp], r12d              */
  T1(0x41); T1(0xBA); T4(JIT_X_FSM);      /* mov r10d, JIT_X_FSM          */
  T_JMP_EPI();

  g_jit_tramp_xcstep = t_len_;
  T1(0x48); T1(0x83); T1(0xC4); T1(0x08); /* add rsp, 8 (dead RA)         */
  T1(0x44); T1(0x29); T1(0x24); T1(0x24); /* sub [rsp], r12d              */
  T1(0x41); T1(0xBA); T4(JIT_X_CSTEP);    /* mov r10d, JIT_X_CSTEP        */
  T_JMP_EPI();

  /* Slow stubs are entered from the dispatch loop directly (no `call rsi`
   * intervened, so the dead-RA `add rsp, 8` is NOT wanted here): [rsp] is
   * already the remaining-budget slot, and no block ran, so there is no
   * [rsp]-minus fold either. */
  l_slow_interp = t_len_;
  T1(0x41); T1(0xBA); T4(TRAMP_X_INTERP);
  T_JMP_EPI();

  l_slow_unaligned = t_len_;
  T1(0x41); T1(0xBA); T4(TRAMP_X_UNALIGNED);
  T_JMP_EPI();

  l_slow_compile = t_len_;
  T1(0x41); T1(0xBA); T4(TRAMP_X_COMPILE);
  T_JMP_EPI();

  l_slow_budget = t_len_;
  T1(0x41); T1(0xBA); T4(JIT_X_BUDGET);
  T_JMP_EPI();


  /* ------------------------------ epilog -------------------------------
   * Entered with r10d = exit reason; [rsp] holds the live remaining
   * budget (folded by the arriving stub; slow stubs fold nothing since
   * no block ran). */
  l_epilog = t_len_;
  T1(0x8B); T1(0x0C); T1(0x24);           /* mov ecx, [rsp]   remaining  */
  T1(0x8B); T1(0x54); T1(0x24); T1(0x08); /* mov edx, [rsp+8] budget     */
  T1(0x29); T1(0xCA);                     /* sub edx, ecx  -> total      */
  t_movabs(T_RAX, (uint64_t)(uintptr_t)&g_jit_cycles);
  T1(0x89); T1(0x10);                     /* mov [rax], edx               */
  T1(0x44); T1(0x89); T1(0xD0);           /* mov eax, r10d   reason      */
  T1(0x48); T1(0x83); T1(0xC4); T1(0x18); /* add rsp, 24                 */
  T1(0x41); T1(0x5F);                     /* pop r15 */
  T1(0x41); T1(0x5E);                     /* pop r14 */
  T1(0x41); T1(0x5D);                     /* pop r13 */
  T1(0x41); T1(0x5C);                     /* pop r12 */
  T1(0x5D);                               /* pop rbp */
  T1(0x5B);                               /* pop rbx */
  T1(0xC3);                               /* ret     */

  /* ------------------------------ fixups ------------------------------ */
  {
    uint32_t tmp;

    tmp = (uint32_t)((int64_t)l_slow_unaligned - ((int64_t)p_jnz_unaligned + 4));
    memcpy(&t_cur_[p_jnz_unaligned],&tmp,4);
    tmp = (uint32_t)((int64_t)l_slow_interp - ((int64_t)p_jae_interp + 4));
    memcpy(&t_cur_[p_jae_interp],&tmp,4);
    tmp = (uint32_t)((int64_t)l_slow_compile - ((int64_t)p_jz_compile + 4));
    memcpy(&t_cur_[p_jz_compile],&tmp,4);
    tmp = (uint32_t)((int64_t)l_slow_budget - ((int64_t)p_jle_budget + 4));
    memcpy(&t_cur_[p_jle_budget],&tmp,4);
  }

  for(i = 0; i < njumps; i++)
    {
      uint32_t const at = jumps_to_epilog[i];
      uint32_t const tmp = (uint32_t)((int64_t)l_epilog - ((int64_t)at + 4));
      memcpy(&t_cur_[at],&tmp,4);
    }

  /* page materialization */
  g_jit_tramp_page = (uint8_t *)mmap(NULL,4096,
                                     (PROT_READ | PROT_WRITE | PROT_EXEC),
                                     (MAP_PRIVATE | MAP_ANONYMOUS),-1,0);
  if(g_jit_tramp_page == MAP_FAILED)
    return 1;
  memcpy(g_jit_tramp_page,t_cur_,t_len_);

  g_jit_tramp_entry  = (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xnext += (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xfiq  += (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xbudget += (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xrst  += (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xfsm  += (uintptr_t)g_jit_tramp_page;
  g_jit_tramp_xcstep += (uintptr_t)g_jit_tramp_page;

#undef T_JMP_EPI
  return 0;
}

/* out-of-block trampoline jumps: jmp rel32, patched at commit */
#define OJ_MAXABS 512u
static uint32_t s_oj_abs_at[OJ_MAXABS];
static uint64_t s_oj_abs_dst[OJ_MAXABS];
static uint32_t s_oj_nabs;
static int      s_oj_abs_ok;    /* rel32 range verified at startup */

/* RIP-relative data operands: cmp/mov [rip+disp32] against .so/.bss
 * statics (DRAM, RAM_SIZE), patched on arena commit like the absolute
 * jumps.  Each entry: placeholder disp32 offset, target address, and
 * the operand tail length (rel32 base = end of the instruction). */
#define OJ_MAXRIP 512u
static uint32_t s_oj_rip_at[OJ_MAXRIP];   /* disp32 byte offset      */
static uint64_t s_oj_rip_dst[OJ_MAXRIP];  /* absolute target address */
static uint32_t s_oj_rip_tail[OJ_MAXRIP]; /* insn bytes after disp32 */
static uint32_t s_oj_nrip;
static int      s_oj_rip_ok;    /* .bss within +-2GB of both arena ends */

static void oj_reset(void)
{
  uint32_t i;
  s_oj_len  = 0;
  s_oj_nfix = 0;
  s_oj_nlab = 0;
  s_oj_nabs = 0;
  s_oj_nrip = 0;
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

static void oj_u8(uint32_t const b_)
{
  s_oj[s_oj_len++] = (uint8_t)b_;
}

static void oj_u32(uint32_t const v_)
{
  uint8_t *p = &s_oj[s_oj_len];
  p[0] = (uint8_t)v_;
  p[1] = (uint8_t)(v_ >>  8);
  p[2] = (uint8_t)(v_ >> 16);
  p[3] = (uint8_t)(v_ >> 24);
  s_oj_len += 4;
}

static void oj_u64(uint64_t const v_)
{
  int i;
  for(i = 0; i < 8; i++)
    oj_u8((uint32_t)(v_ >> (8 * i)));
}

/* records an imm32 field at the cursor to be patched to
 * (lab_off - (field_off + 4)); intra-block only */
static void oj_rel32(uint32_t const lab_)
{
  s_oj_fix_at[s_oj_nfix]  = s_oj_len;
  s_oj_fix_lab[s_oj_nfix] = lab_;
  s_oj_nfix++;
  s_oj_len += 4;
}

static void oj_jcc(uint32_t const cc_, uint32_t const lab_)
{
  oj_u8(0x0F);
  oj_u8(cc_);
  oj_rel32(lab_);
}

#define OJ_JNC(l)  oj_jcc(0x83, (l))
#define OJ_JC(l)   oj_jcc(0x82, (l))
#define OJ_JZ(l)   oj_jcc(0x84, (l))
#define OJ_JNZ(l)  oj_jcc(0x85, (l))
#define OJ_JA(l)   oj_jcc(0x87, (l))
#define OJ_JBE(l)  oj_jcc(0x86, (l))
#define OJ_JG(l)   oj_jcc(0x8F, (l))
#define OJ_JL(l)   oj_jcc(0x8C, (l))

static void oj_jmp(uint32_t const lab_)
{
  oj_u8(0xE9);
  oj_rel32(lab_);
}

static int oj_resolve(void)
{
  uint32_t i;

  for(i = 0; i < s_oj_nfix; i++)
    {
      uint32_t const at  = s_oj_fix_at[i];
      uint32_t const dst = s_oj_lab[s_oj_fix_lab[i]];
      int64_t  const d   = ((int64_t)dst) - ((int64_t)at + 4);
      uint32_t   u;
      int32_t    v;

      if(dst == 0xFFFFFFFFu)
        return -1;

      v = (int32_t)d;
      if((int64_t)v != d)
        return -1;

      u = (uint32_t)v;
      s_oj[at]   = (uint8_t)u;
      s_oj[at+1] = (uint8_t)(u >>  8);
      s_oj[at+2] = (uint8_t)(u >> 16);
      s_oj[at+3] = (uint8_t)(u >> 24);
    }

  return 0;
}

/* movabs r64, imm64 */
#define OJ_RAX 0u
#define OJ_RCX 1u
#define OJ_RDX 2u
#define OJ_RBX 3u
#define OJ_RSI 6u
#define OJ_RDI 7u
#define OJ_R10 10u
#define OJ_R11 11u

static void oj_movabs(uint32_t const reg_, uint64_t const imm_)
{
  oj_u8((reg_ > 7) ? 0x49 : 0x48);          /* REX.W | REX.B */
  oj_u8(0xB8 + (reg_ & 7));
  oj_u64(imm_);
}

/* out-of-block jump into the shared trampoline.  Emitted as jmp rel32
 * and patched after the block is committed to the arena (its final
 * address is only known then).  The trampoline page and the arena are
 * both mapped once at startup and never move, so the distance is fixed
 * for the process lifetime; the startup range check verifies it. */
static void oj_jmp_abs(uint64_t const addr_)
{
  if(s_oj_abs_ok && (s_oj_nabs < OJ_MAXABS) &&
     ((s_oj_len + 5u + 64u) < OJ_BUF))
    {
      s_oj_abs_at[s_oj_nabs] = s_oj_len;
      s_oj_abs_dst[s_oj_nabs] = addr_;
      s_oj_nabs++;
      oj_u8(0xE9);                  /* jmp rel32, patched on commit */
      oj_u32(0);
    }
  else
    {
      oj_movabs(OJ_RAX, addr_);
      oj_u8(0xFF);                  /* jmp rax */
      oj_u8(0xE0);
    }
}

/* patch the emitted rel32 jumps against the committed arena address */
static void oj_abs_patch(uint8_t const *const code_)
{
  uint32_t i;

  for(i = 0; i < s_oj_nabs; i++)
    {
      uint8_t       *at = (uint8_t *)(code_ + s_oj_abs_at[i] + 1u);
      int64_t  const sd = (int64_t)(s_oj_abs_dst[i] -
                           ((uint64_t)(code_ + s_oj_abs_at[i] + 5u)));

      if((sd < -0x80000000LL) || (sd > 0x7FFFFFFFLL))
        continue;                   /* cannot happen: range-verified */

      at[0] = (uint8_t)sd;
      at[1] = (uint8_t)(sd >>  8);
      at[2] = (uint8_t)(sd >> 16);
      at[3] = (uint8_t)(sd >> 24);
    }

  for(i = 0; i < s_oj_nrip; i++)
    {
      /* disp32 sits at s_oj_rip_at[i]; the instruction continues for
       * s_oj_rip_tail[i] bytes after the disp32, so the RIP base is
       * code_ + at + 4 + tail. */
      uint8_t       *at = (uint8_t *)(code_ + s_oj_rip_at[i]);
      uint64_t const base = ((uint64_t)(code_ + s_oj_rip_at[i] + 4u) +
                             (uint64_t)s_oj_rip_tail[i]);
      int64_t  const sd = (int64_t)(s_oj_rip_dst[i] - base);

      if((sd < -0x80000000LL) || (sd > 0x7FFFFFFFLL))
        continue;                   /* range re-verified per operand;
                                     * emitters only use RIP forms
                                     * when s_oj_rip_ok held */

      at[0] = (uint8_t)sd;
      at[1] = (uint8_t)(sd >>  8);
      at[2] = (uint8_t)(sd >> 16);
      at[3] = (uint8_t)(sd >> 24);
    }
}

/* cmp r32, [rip+disp32]  -- 3B 05 <rel32> (6 bytes, tail 0) */
static void oj_rip_cmp32(uint32_t const reg_, uint64_t const dst_)
{
  if(!s_oj_rip_ok || (s_oj_nrip >= OJ_MAXRIP))
    {
      oj_movabs(OJ_RCX, dst_);
      oj_u8(0x3B);                  /* cmp reg, [rcx] */
      oj_u8(0x00 | ((reg_ & 7) << 3) | 1u);
      return;
    }
  if(reg_ > 7)
    oj_u8(0x44);                    /* REX.R (extends the reg field) */
  oj_u8(0x3B);
  oj_u8(0x05 | ((reg_ & 7) << 3));
  s_oj_rip_at[s_oj_nrip]   = s_oj_len;   /* disp32 starts here */
  s_oj_rip_dst[s_oj_nrip]  = dst_;
  s_oj_rip_tail[s_oj_nrip] = 0;
  s_oj_nrip++;
  oj_u32(0);                        /* disp32, patched on commit */
}

/* cmp byte [rip+disp32], 0  -- 80 3D <rel32> 00 (7 bytes) */
static void oj_rip_cmp8z(uint64_t const dst_)
{
  if(!s_oj_rip_ok || (s_oj_nrip >= OJ_MAXRIP))
    {
      oj_movabs(OJ_RCX, dst_);
      oj_u8(0x80);                  /* cmp byte [rcx], 0 */
      oj_u8(0x39);
      oj_u8(0);
      return;
    }
  oj_u8(0x80);
  oj_u8(0x3D);
  s_oj_rip_at[s_oj_nrip]   = s_oj_len;
  s_oj_rip_dst[s_oj_nrip]  = dst_;
  s_oj_rip_tail[s_oj_nrip] = 1;      /* imm8 follows the disp32 */
  s_oj_nrip++;
  oj_u32(0);
  oj_u8(0);
}

/* test byte [rip+disp32], imm8  -- F6 05 <rel32> 01 (7 bytes) */
static void oj_rip_testb1(uint64_t const dst_)
{
  if(!s_oj_rip_ok || (s_oj_nrip >= OJ_MAXRIP))
    {
      oj_movabs(OJ_RCX, dst_);
      oj_u8(0xF6);                  /* test byte [rcx], 1 */
      oj_u8(0x01);
      oj_u8(0x01);
      return;
    }
  oj_u8(0xF6);
  oj_u8(0x05);
  s_oj_rip_at[s_oj_nrip]   = s_oj_len;
  s_oj_rip_dst[s_oj_nrip]  = dst_;
  s_oj_rip_tail[s_oj_nrip] = 1;      /* imm8 follows the disp32 */
  s_oj_nrip++;
  oj_u32(0);
  oj_u8(0x01);
}

/* mov r64, [rip+disp32]  -- 48 8B <mod 00 reg 101> <rel32> (7 bytes,
 * tail 0) */
static void oj_rip_ld64(uint32_t const reg_, uint64_t const dst_)
{
  if(!s_oj_rip_ok || (s_oj_nrip >= OJ_MAXRIP))
    {
      oj_movabs(reg_, dst_);
      oj_u8(0x48 | ((reg_ > 7) ? 1 : 0));  /* mov reg, [reg] */
      oj_u8(0x8B);
      oj_u8((uint8_t)(0x00 | ((reg_ & 7) << 3) | (reg_ & 7)));
      return;
    }
  oj_u8(0x48 | ((reg_ > 7) ? 1 : 0));
  oj_u8(0x8B);
  oj_u8(0x05 | ((reg_ & 7) << 3));
  s_oj_rip_at[s_oj_nrip]   = s_oj_len;   /* disp32 starts here */
  s_oj_rip_dst[s_oj_nrip]  = dst_;
  s_oj_rip_tail[s_oj_nrip] = 0;
  s_oj_nrip++;
  oj_u32(0);                        /* disp32, patched on commit */
}

/* mov r32, [rbx + disp32]  (CPU member load) */
static void oj_ld_cpu(uint32_t const reg_, uint32_t const disp_)
{
  if(reg_ > 7)
    oj_u8(0x44);                            /* REX.R */
  oj_u8(0x8B);
  oj_u8(0x83 | ((reg_ & 7) << 3));
  oj_u32(disp_);
}

/* mov [rbx + disp32], r32  (CPU member store) */
static void oj_st_cpu(uint32_t const reg_, uint32_t const disp_)
{
  if(reg_ > 7)
    oj_u8(0x44);                            /* REX.R */
  oj_u8(0x89);
  oj_u8(0x83 | ((reg_ & 7) << 3));
  oj_u32(disp_);
}

#define OJ_U(n_)   ((uint32_t)(offsetof(arm_core_t, USER) + (4u * (n_))))
#define OJ_U15     (OJ_U(15))
#define OJ_CPSR    ((uint32_t)(offsetof(arm_core_t, CPSR)))
#define OJ_SPSR    ((uint32_t)(offsetof(arm_core_t, SPSR)))

/* and dword [rbx + disp32], imm32 */
static void oj_and_cpu(uint32_t const disp_, uint32_t const imm_)
{
  oj_u8(0x81);
  oj_u8(0xA3);
  oj_u32(disp_);
  oj_u32(imm_);
}

/* or dword [rbx + disp32], r10d */
static void oj_or_cpu_r10(uint32_t const disp_)
{
  oj_u8(0x44);
  oj_u8(0x09);
  oj_u8(0x93);
  oj_u32(disp_);
}

/* or dword [rbx + disp32], edi */
static void oj_or_cpu_rdi(uint32_t const disp_)
{
  oj_u8(0x09);
  oj_u8(0xBB);
  oj_u32(disp_);
}

/* and r10d, imm8 */
static void oj_and_r10(uint32_t const imm8_)
{
  oj_u8(0x41);
  oj_u8(0x83);
  oj_u8(0xE2);
  oj_u8(imm8_);
}

/* shr r10d, imm8 */
static void oj_shr_r10(uint32_t const imm8_)
{
  oj_u8(0x41);
  oj_u8(0xC1);
  oj_u8(0xEA);
  oj_u8(imm8_);
}

/* shl r10d, imm8 */
static void oj_shl_r10(uint32_t const imm8_)
{
  oj_u8(0x41);
  oj_u8(0xC1);
  oj_u8(0xE2);
  oj_u8(imm8_);
}

/* setc r10b ; movzx r10d, r10b  (materialize host CF as 0/1 in r10d) */
static void oj_cf_r10(void)
{
  oj_u8(0x41);
  oj_u8(0x0F);
  oj_u8(0x92);
  oj_u8(0xC2);
  oj_u8(0x45);
  oj_u8(0x0F);
  oj_u8(0xB6);
  oj_u8(0xD2);
}

/* r10d = current ARM C flag (bit 29 of CPSR) as 0/1 */
static void oj_get_arm_c_r10(void)
{
  oj_ld_cpu(OJ_R10, OJ_CPSR);
  oj_shr_r10(29);
  oj_and_r10(1);
}

/* bt dword [rbx + OJ_CPSR], 29  -> host CF := ARM C */
static void oj_bt_cpsr_c(void)
{
  oj_u8(0x0F);
  oj_u8(0xBA);
  oj_u8(0xA3);
  oj_u32(OJ_CPSR);
  oj_u8(29);
}

/* ---------------------------------------------------------------------------
 * Shifter emission.
 *
 * Produce op2 in eax and the exact carry_out value ARM_SHIFT_NSC computes
 * materialized as 0/1 in r10d, then store it to the carry_out global so the
 * JIT lane keeps the file-static observable to the C handlers identical
 * (cached handlers also leave it stale-hot between instructions; it is
 * written before every read on every path on both engines).
 *
 * Operand6 = value already loaded in eax (from USER[rm]).
 * ------------------------------------------------------------------------- */

/* static (immediate / packed RI) shift: shift amount s_ and type t_ are
 * compile-time constants, already promoted exactly like
 * arm_cache_pack_shift (type 3 shift 0 -> type 4 RRX; others shift 0 -> 32).
 * s_ may be 32. */
static void oj_shift_static(uint32_t const s_, uint32_t const t_)
{
  switch(t_)
    {
    case 0: /* LSL */
      if(s_ == 0)
        {
          /* val unchanged; carry_out = ARM_GET_C() */
          oj_get_arm_c_r10();
        }
      else if(s_ == 32)
        {
          /* carry = (v << 31) >> 31 == v & 1 ; val = 0 */
          oj_u8(0x41);              /* mov r10d, eax */
          oj_u8(0x89);
          oj_u8(0xC2);
          oj_and_r10(1);
          oj_u8(0x31);              /* xor eax, eax */
          oj_u8(0xC0);
        }
      else
        {
          oj_u8(0xC1);              /* shl eax, s */
          oj_u8(0xE0);
          oj_u8(s_);
          oj_cf_r10();
        }
      break;
    case 1: /* LSR */
      if(s_ == 0)
        {
          oj_get_arm_c_r10();
        }
      else if(s_ == 32)
        {
          /* carry = v >> 31 ; val = 0 */
          oj_u8(0x41);
          oj_u8(0x89);
          oj_u8(0xC2);
          oj_shr_r10(31);
          oj_u8(0x31);
          oj_u8(0xC0);
        }
      else
        {
          oj_u8(0xC1);              /* shr eax, s */
          oj_u8(0xE8);
          oj_u8(s_);
          oj_cf_r10();
        }
      break;
    case 2: /* ASR */
      if(s_ == 0)
        {
          oj_get_arm_c_r10();
        }
      else if(s_ == 32)
        {
          /* carry = sign(v) ; val = sign-fill */
          oj_u8(0x0F);              /* bt eax, 31 */
          oj_u8(0xBA);
          oj_u8(0xE0);
          oj_u8(31);
          oj_cf_r10();
          oj_u8(0xC1);              /* sar eax, 31 */
          oj_u8(0xF8);
          oj_u8(31);
        }
      else
        {
          oj_u8(0xC1);              /* sar eax, s */
          oj_u8(0xF8);
          oj_u8(s_);
          oj_cf_r10();
        }
      break;
    case 3: /* ROR */
      if(s_ == 0)
        {
          /* implicit-rotate 0 (DP immediate): val unchanged, carry = C */
          oj_get_arm_c_r10();
        }
      else
        {
          oj_u8(0xC1);              /* ror eax, s */
          oj_u8(0xC8);
          oj_u8(s_);
          oj_cf_r10();
        }
      break;
    default: /* 4: RRX */
      oj_bt_cpsr_c();               /* CF := ARM C */
      oj_u8(0xD1);                  /* rcr eax, 1 -> val, CF := v & 1 */
      oj_u8(0xD8);
      oj_cf_r10();
      break;
    }

  /* carry_out global NOT stored: every reader in the engine
   * (ARM_SET_C in the DP tails) is preceded in the same C call by
   * an ARM_SHIFT_NSC write, and ARM_GET_C reads CPSR -- the
   * global's inter-instruction value is unobservable. */
}

/* register shift: amount = USER[rs] & 0xFF at runtime.  Emits the boundary
 * walk of ARM_SHIFT_NSC exactly:
 *   s==0  -> val = v, carry_out = ARM_GET_C()
 *   LSL   1..31: shl ; 32: (carry v&1, val 0) ; >32: (carry 0, val 0)
 *   LSR   1..31: shr ; 32: (carry v>>31, val 0); >32: (carry 0, val 0)
 *   ASR   1..31: sar ; >=32: (carry sign, val sign-fill)
 *   ROR   s&31 != 0: ror   ; s&31 == 0 (s>0): (carry v>>31, val unchanged)
 * RRX (type 4) never reaches here (register form stays type 3).
 * eax = USER[rm] value; ecx already loaded with (USER[rs] & 0xFF).
 * Result in eax, carry in r10d, carry_out global stored. */
static void oj_shift_reg(uint32_t const t_)
{
  uint32_t const l_s0   = oj_lab_alloc();
  uint32_t const l_big  = oj_lab_alloc();
  uint32_t const l_s32  = oj_lab_alloc();
  uint32_t const l_done = oj_lab_alloc();

  oj_u8(0x85);                    /* test ecx, ecx */
  oj_u8(0xC9);
  OJ_JZ(l_s0);

  switch(t_)
    {
    case 0: /* LSL */
    case 1: /* LSR */
      oj_u8(0x83);                /* cmp ecx, 32 */
      oj_u8((t_ == 0) ? 0xF9 : 0xF9);
      oj_u8(32);
      OJ_JA(l_big);
      OJ_JZ(l_s32);
      oj_u8(0xD3);                /* shl/shr eax, cl */
      oj_u8((t_ == 0) ? 0xE0 : 0xE8);
      oj_cf_r10();
      oj_jmp(l_done);
      oj_lab_here(l_s32);
      oj_u8(0x41);                /* mov r10d, eax */
      oj_u8(0x89);
      oj_u8(0xC2);
      if(t_ == 0)
        oj_and_r10(1);            /* LSL32: carry = v & 1 */
      else
        oj_shr_r10(31);           /* LSR32: carry = v >> 31 */
      oj_u8(0x31);                /* xor eax, eax */
      oj_u8(0xC0);
      oj_jmp(l_done);
      oj_lab_here(l_big);
      oj_u8(0x45);                /* xor r10d, r10d */
      oj_u8(0x31);
      oj_u8(0xD2);
      oj_u8(0x31);                /* xor eax, eax */
      oj_u8(0xC0);
      oj_jmp(l_done);
      break;
    case 2: /* ASR */
      oj_u8(0x83);                /* cmp ecx, 31 */
      oj_u8(0xF9);
      oj_u8(31);
      OJ_JG(l_big);
      oj_u8(0xD3);                /* sar eax, cl */
      oj_u8(0xF8);
      oj_cf_r10();
      oj_jmp(l_done);
      oj_lab_here(l_big);
      /* carry = sign = (v >> 31), val = sign-fill */
      oj_u8(0x0F);                /* bt eax, 31 */
      oj_u8(0xBA);
      oj_u8(0xE0);
      oj_u8(31);
      oj_cf_r10();
      oj_u8(0xC1);                /* sar eax, 31 */
      oj_u8(0xF8);
      oj_u8(31);
      oj_jmp(l_done);
      break;
    default: /* 3: ROR */
      oj_u8(0xF6);                /* test cl, 31 */
      oj_u8(0xC1);
      oj_u8(31);
      OJ_JZ(l_big);
      oj_u8(0xD3);                /* ror eax, cl */
      oj_u8(0xC8);
      oj_cf_r10();
      oj_jmp(l_done);
      oj_lab_here(l_big);
      /* val unchanged; carry = v >> 31 */
      oj_u8(0x41);                /* mov r10d, eax */
      oj_u8(0x89);
      oj_u8(0xC2);
      oj_shr_r10(31);
      oj_jmp(l_done);
      break;
    }

  oj_lab_here(l_s0);
  oj_get_arm_c_r10();             /* carry_out = ARM_GET_C(); val unchanged */
  oj_lab_here(l_done);
  /* no carry_out store -- see oj_shift_static */
}
/* ---------------------------------------------------------------------------
 * Flag patches.  All reproduce the exact bitwise writes of ARM_SET_ZN /
 * ARM_SET_C / ARM_SET_CV / ARM_SET_CV_sub: only the named bits of CPSR are
 * ever touched, in the same order the C code touches them.
 * ------------------------------------------------------------------------- */

/* ARM_SET_C(r10d): CPSR = (CPSR & 0xdfffffff) | (carry << 29) */
static void oj_arm_set_c_from_r10(void)
{
  oj_and_cpu(OJ_CPSR, 0xdfffffffu);
  oj_shl_r10(29);
  oj_or_cpu_r10(OJ_CPSR);
}

/* ARM_SET_ZN(val): N = val bit 31, Z = (val == 0); bits 28/29 bitwise
 * preserved.  reg_ holds the value (OJ_RAX or OJ_RDX).
 * clobbers ecx, r8d. */
static void oj_arm_set_zn(uint32_t const reg_)
{
  /* mov ecx, reg_ */
  oj_u8(0x89);
  oj_u8(0xC1 | ((reg_ & 7) << 3));        /* mod11 reg=reg_ rm=ecx */
  oj_u8(0x81);                            /* and ecx, 0x80000000 */
  oj_u8(0xE1);
  oj_u32(0x80000000u);
  /* test reg_, reg_ */
  oj_u8(0x85);
  oj_u8(0xC0 | ((reg_ & 7) << 3) | (reg_ & 7));
  oj_u8(0x41);                            /* sete r8b */
  oj_u8(0x0F);
  oj_u8(0x94);
  oj_u8(0xC0);
  oj_u8(0x45);                            /* movzx r8d, r8b */
  oj_u8(0x0F);
  oj_u8(0xB6);
  oj_u8(0xC0);
  oj_u8(0x41);                            /* shl r8d, 30 */
  oj_u8(0xC1);
  oj_u8(0xE0);
  oj_u8(30);
  oj_u8(0x44);                            /* or ecx, r8d */
  oj_u8(0x09);
  oj_u8(0xC1);
  oj_and_cpu(OJ_CPSR, 0x3fffffffu);
  oj_u8(0x09);                            /* or [rbx + CPSR], ecx */
  oj_u8(0x8B);
  oj_u32(OJ_CPSR);
}

/* ARM_SET_ZN + ARM_SET_CV (add) or ARM_SET_CV_sub (sub) from the host flags
 * of a 32-bit add/sub that just produced its result in edx.
 * Equivalence to the C bit-twiddles:
 *   add: ARM C = carry-out        = host CF          (not inverted)
 *   sub: ARM C = not-borrow       = host CF ^ 1
 *   both: ARM V = signed overflow = host OF
 *   ARM N = result bit 31 = host SF ; ARM Z = host ZF
 * Snapshot rflags first, then fold into the top nibble; all four bits are
 * written together (the C order ZN-then-CV / CV-then-ZN is unobservable:
 * disjoint bit fields).  clobbers ecx, edi, r8d, r9d. */
static void oj_arm_set_cvzn(int const sub_family_)
{
  oj_u8(0x41);                  /* mov r8d, edx   (save result) */
  oj_u8(0x89);
  oj_u8(0xD0);
  oj_u8(0x9C);                  /* pushfq */
  oj_u8(0x59);                  /* pop rcx */
  oj_u8(0x89);                  /* mov edi, ecx */
  oj_u8(0xCF);
  oj_u8(0xC1);                  /* shr edi, 11    -> V */
  oj_u8(0xEF);
  oj_u8(11);
  oj_u8(0x83);                  /* and edi, 1 */
  oj_u8(0xE7);
  oj_u8(1);
  oj_u8(0x41);                  /* mov r9d, ecx */
  oj_u8(0x89);
  oj_u8(0xC9);
  oj_u8(0x41);                  /* and r9d, 1     -> host CF */
  oj_u8(0x83);
  oj_u8(0xE1);
  oj_u8(1);
  if(sub_family_)
    {
      oj_u8(0x41);              /* xor r9d, 1     -> ARM C */
      oj_u8(0x83);
      oj_u8(0xF1);
      oj_u8(1);
    }
  oj_u8(0x41);                  /* shl r9d, 1     -> C to bit 1 */
  oj_u8(0xC1);
  oj_u8(0xE1);
  oj_u8(1);
  oj_u8(0x44);                  /* or edi, r9d */
  oj_u8(0x09);
  oj_u8(0xCF);
  oj_u8(0x41);                  /* mov r9d, ecx */
  oj_u8(0x89);
  oj_u8(0xC9);
  oj_u8(0x41);                  /* shr r9d, 6     -> Z(bit0) N(bit1) */
  oj_u8(0xC1);
  oj_u8(0xE9);
  oj_u8(6);
  oj_u8(0x41);                  /* and r9d, 3 */
  oj_u8(0x83);
  oj_u8(0xE1);
  oj_u8(3);
  oj_u8(0x41);                  /* shl r9d, 2     -> Z to bit2, N to bit3 */
  oj_u8(0xC1);
  oj_u8(0xE1);
  oj_u8(2);
  oj_u8(0x44);                  /* or edi, r9d    -> edi = NZCV nibble */
  oj_u8(0x09);
  oj_u8(0xCF);
  oj_u8(0xC1);                  /* shl edi, 28 */
  oj_u8(0xE7);
  oj_u8(28);
  oj_and_cpu(OJ_CPSR, 0x0fffffffu);
  oj_or_cpu_rdi(OJ_CPSR);
  oj_u8(0x44);                  /* mov edx, r8d   (restore result) */
  oj_u8(0x89);
  oj_u8(0xC2);
}

/* ---------------------------------------------------------------------------
 * ALU body.  Inputs: op1 in esi, op2 in eax, shifter carry in r10d
 * (already written to carry_out global).  Emits the full ARM_ALU_Exec
 * semantics for one opc5 = (cmd >> 20) & 0x1F value.  MSR (18/22) never
 * reaches here (decode keeps those words out of the tight classes).
 * ------------------------------------------------------------------------- */
static void oj_alu(uint32_t const cmd_)
{
  uint32_t const opc5   = ((cmd_ >> 20) & 0x1F);
  uint32_t const rd     = ((cmd_ >> 12) & 0xF);
  int      const s_bit  = (int)(opc5 & 1);
  uint32_t const opcode = (opc5 >> 1);
  int      const logic  = is_logic[opcode];

  /* MRS: opc5 16/20 alias (DP opcodes 8/10 with S=0); the oracle picks
   * the source register by instruction bit 22, not by the opcode */
  if((opc5 == 16) || (opc5 == 20))
    {
      oj_ld_cpu(OJ_RAX, OJ_CPSR);
      if((cmd_ >> 22) & 1)
        {
          /* SPSR[arm_mode_table[CPSR & 0x1F]]; table holds uint8_t */
          oj_u8(0x83);            /* and eax, 0x1F */
          oj_u8(0xE0);
          oj_u8(0x1F);
          oj_movabs(OJ_RDX, (uint64_t)(uintptr_t)&arm_mode_table[0]);
          oj_u8(0x0F);            /* movzx eax, byte [rdx + rax] */
          oj_u8(0xB6);
          oj_u8(0x04);
          oj_u8(0x02);
          oj_u8(0x8B);            /* mov eax, [rbx + rax*4 + SPSR] */
          oj_u8(0x84);
          oj_u8(0x83);
          oj_u32(OJ_SPSR);
        }
      oj_st_cpu(OJ_RAX, OJ_U(rd));
      return;
    }

  /* shifter carry into C for S+logic ops (pre-ALU, like the handlers) */
  if(s_bit && logic)
    oj_arm_set_c_from_r10();

  switch(opcode)
    {
    case 0: /* AND */
      oj_u8(0x21);                /* and eax, esi */
      oj_u8(0xF0);
      if(s_bit)
        oj_arm_set_zn(OJ_RAX);
      oj_st_cpu(OJ_RAX, OJ_U(rd));
      break;
    case 1: /* EOR */
      oj_u8(0x31);                /* xor eax, esi */
      oj_u8(0xF0);
      if(s_bit)
        oj_arm_set_zn(OJ_RAX);
      oj_st_cpu(OJ_RAX, OJ_U(rd));
      break;
    case 2: /* SUB */
      oj_u8(0x89);                /* mov edx, esi */
      oj_u8(0xF2);
      oj_u8(0x29);                /* sub edx, eax */
      oj_u8(0xC2);
      if(s_bit)
        oj_arm_set_cvzn(1);
      oj_st_cpu(OJ_RDX, OJ_U(rd));
      break;
    case 10: /* CMP: CV_sub + ZN, result discarded, never stored */
      oj_u8(0x89);                /* mov edx, esi */
      oj_u8(0xF2);
      oj_u8(0x29);                /* sub edx, eax */
      oj_u8(0xC2);
      oj_arm_set_cvzn(1);
      break;
    case 3: /* RSB */
      oj_u8(0x89);                /* mov edx, eax */
      oj_u8(0xC2);
      oj_u8(0x29);                /* sub edx, esi */
      oj_u8(0xF2);
      if(s_bit)
        oj_arm_set_cvzn(1);
      oj_st_cpu(OJ_RDX, OJ_U(rd));
      break;
    case 4: /* ADD */
      oj_u8(0x89);                /* mov edx, esi */
      oj_u8(0xF2);
      oj_u8(0x01);                /* add edx, eax */
      oj_u8(0xC2);
      if(s_bit)
        oj_arm_set_cvzn(0);
      oj_st_cpu(OJ_RDX, OJ_U(rd));
      break;
    case 11: /* CMN: CV + ZN, result discarded, never stored */
      oj_u8(0x89);                /* mov edx, esi */
      oj_u8(0xF2);
      oj_u8(0x01);                /* add edx, eax */
      oj_u8(0xC2);
      oj_arm_set_cvzn(0);
      break;
    case 5: /* ADC */
      oj_bt_cpsr_c();
      oj_u8(0x89);                /* mov edx, esi */
      oj_u8(0xF2);
      oj_u8(0x11);                /* adc edx, eax */
      oj_u8(0xC2);
      if(s_bit)
        oj_arm_set_cvzn(0);
      oj_st_cpu(OJ_RDX, OJ_U(rd));
      break;
    case 6: /* SBC */
      oj_bt_cpsr_c();
      oj_u8(0xF5);                /* cmc  (borrow-in = 1 - C) */
      oj_u8(0x89);                /* mov edx, esi */
      oj_u8(0xF2);
      oj_u8(0x19);                /* sbb edx, eax */
      oj_u8(0xC2);
      if(s_bit)
        oj_arm_set_cvzn(1);
      oj_st_cpu(OJ_RDX, OJ_U(rd));
      break;
    case 7: /* RSC */
      oj_bt_cpsr_c();
      oj_u8(0xF5);                /* cmc */
      oj_u8(0x89);                /* mov edx, eax */
      oj_u8(0xC2);
      oj_u8(0x19);                /* sbb edx, esi */
      oj_u8(0xF2);
      if(s_bit)
        oj_arm_set_cvzn(1);
      oj_st_cpu(OJ_RDX, OJ_U(rd));
      break;
    case 12: /* ORR */
      oj_u8(0x09);                /* or eax, esi */
      oj_u8(0xF0);
      if(s_bit)
        oj_arm_set_zn(OJ_RAX);
      oj_st_cpu(OJ_RAX, OJ_U(rd));
      break;
    case 13: /* MOV */
      if(s_bit)
        oj_arm_set_zn(OJ_RAX);
      oj_st_cpu(OJ_RAX, OJ_U(rd));
      break;
    case 14: /* BIC */
      oj_u8(0xF7);                /* not eax */
      oj_u8(0xD0);
      oj_u8(0x21);                /* and eax, esi */
      oj_u8(0xF0);
      if(s_bit)
        oj_arm_set_zn(OJ_RAX);
      oj_st_cpu(OJ_RAX, OJ_U(rd));
      break;
    case 15: /* MVN */
      oj_u8(0xF7);                /* not eax */
      oj_u8(0xD0);
      if(s_bit)
        oj_arm_set_zn(OJ_RAX);
      oj_st_cpu(OJ_RAX, OJ_U(rd));
      break;
    default: /* 8: TST, 9: TEQ (opcode 8/9 with S=1) */
      oj_u8(0x89);                /* mov edx, esi */
      oj_u8(0xF2);
      if(opcode == 8)
        {
          oj_u8(0x21);            /* and edx, eax */
          oj_u8(0xC2);
        }
      else
        {
          oj_u8(0x31);            /* xor edx, eax */
          oj_u8(0xC2);
        }
      oj_arm_set_zn(OJ_RDX);
      break;
    }
}
/* ---------------------------------------------------------------------------
 * Condition-code prefix.  Reproduces
 *   (cond_flags_cross[cmd >> 28] >> (CPU.CPSR >> 28)) & 1
 * via bt against the compile-time 16-bit mask.  Returns the skip label for
 * cond != AL; the caller emits the body, plants the skip label, then the
 * tail.  cond == NV (mask 0) is handled by the caller as tail-only.
 * ------------------------------------------------------------------------- */
static uint32_t oj_cond_prefix(uint32_t const cmd_)
{
  uint32_t const cond = (cmd_ >> 28);
  uint32_t const l_skip = oj_lab_alloc();

  oj_ld_cpu(OJ_RAX, OJ_CPSR);     /* mov eax, [rbx + CPSR] */
  oj_u8(0xC1);                    /* shr eax, 28 */
  oj_u8(0xE8);
  oj_u8(28);
  oj_u8(0xBA);                    /* mov edx, cond_flags_cross[cond] */
  oj_u32(cond_flags_cross[cond]);
  oj_u8(0x0F);                    /* bt edx, eax  -> CF = cond-true */
  oj_u8(0xA3);
  oj_u8(0xC2);
  OJ_JNC(l_skip);

  return l_skip;
}

/* ---------------------------------------------------------------------------
 * Per-word tail: SCYCLE charge into the r12d bank, the FIQ poll
 * (per-word by default; hoisted to the prologue once per block when
 * g_jit_fiqhoist && !g_jit_dsp_threaded, since with the DSP worker
 * thread off nothing can raise CLIO_FIQPEND mid-block), and the
 * budget check (r12d vs r13d) -- both engines pay this per word by
 * the slice contract (a mid-block budget stop is USER[15]-visible).
 * A block-wide budget PREFIT was considered and rejected: it stops
 * the slice at a different word boundary than per-word checks,
 * which is a timing-contract change (user-approval-gated).
 *
 * u15_at_tail is the CPU.USER[15] value the C loop would hold at this
 * word's tail; the FIQ exit stub materializes it before returning to the
 * dispatcher.  Budget-exit stubs used to double-materialize -- now gone. */
static void oj_tail(uint32_t const u15_at_tail_)
{
  uint32_t const l_nofiq = oj_lab_alloc();
  uint32_t const l_nobud = oj_lab_alloc();


  oj_u8(0x41);                    /* add r12d, 1   (SCYCLE charge) */
  oj_u8(0x83);
  oj_u8(0xC4);
  oj_u8(1);

  /* if(!ISF && *fpend) arm_fiq_vector(); -- hoisted to block
   * entry when g_jit_fiqhoist && !g_jit_dsp_threaded (see the
   * prologue); otherwise the per-word shape stays */
  if(!(g_jit_fiqhoist && !g_jit_dsp_threaded))
    {
      oj_u8(0x81);                /* cmp dword [rbp], 0 */
      oj_u8(0x7D);
      oj_u8(0x00);
      oj_u32(0);
      OJ_JZ(l_nofiq);
      oj_u8(0xF6);                /* test byte [rbx + CPSR], 0x40 (ISF) */
      oj_u8(0x83);
      oj_u32(OJ_CPSR);
      oj_u8(0x40);
      OJ_JNZ(l_nofiq);
      oj_u8(0xB8);                /* mov eax, u15_at_tail_ */
      oj_u32(u15_at_tail_);
      oj_st_cpu(OJ_RAX, OJ_U15);
      oj_jmp_abs(g_jit_tramp_xfiq);
      oj_lab_here(l_nofiq);
    }

  /* if(total >= budget_) break; --
   * the prologue proved static_charge <= remaining, so no
   * per-word budget check inside this block can fire (the
   * cumulative charge cannot reach the budget before the
   * block's last word, where the dispatcher's X_NEXT fold
   * check lands at the same word the per-word check would). */
  /* BUDGET_BATCH=1: elide the per-word check; stops move to block
   * boundaries via the X_NEXT fold (batch=1, <= block_len-1 words) --
   * a timing-contract change, user-gated */
  if(!g_jit_budbatch)
    {
      oj_u8(0x45);                /* cmp r12d, r13d */
      oj_u8(0x39);
      oj_u8(0xEC);
      OJ_JL(l_nobud);
      oj_u8(0xB8);                /* mov eax, u15_at_tail_ */
      oj_u32(u15_at_tail_);
      oj_st_cpu(OJ_RAX, OJ_U15);
      oj_jmp_abs(g_jit_tramp_xbudget);
    }
  /* bind l_nobud to the fallthrough unconditionally: a no-op
   * position write (0 emitted bytes) that keeps the label pool
   * fully bound under elision — resolve walks fixups only, but
   * this closes the unbound-label class entirely */
  oj_lab_here(l_nobud);
}

/* charge an intra-word extra (beyond the tail's SCYCLE), if any */
static void oj_charge_extra(uint32_t const extra_)
{
  if(!extra_)
    return;

  oj_u8(0x41);                    /* add r12d, extra */
  oj_u8(0x83);
  oj_u8(0xC4);
  oj_u8(extra_);
}

/* exit with JIT_X_NEXT / JIT_X_CSTEP at an explicit CPU.USER[15];
 * stubs live in the shared trampoline (see oj_jmp_abs) */
static void oj_exit_next(uint32_t const u15_)
{
  oj_u8(0xB8);                    /* mov eax, u15_ */
  oj_u32(u15_);
  oj_st_cpu(OJ_RAX, OJ_U15);
  oj_jmp_abs(g_jit_tramp_xnext);
}

static void oj_exit_cstep(uint32_t const word_pc_)
{
  oj_u8(0xB8);                    /* mov eax, word_pc_ */
  oj_u32(word_pc_);
  oj_st_cpu(OJ_RAX, OJ_U15);
  oj_jmp_abs(g_jit_tramp_xcstep);
}
/* ---------------------------------------------------------------------------
 * Per-word emission.
 *
 * Each returns non-zero if the word was consumed into the block, zero if the
 * block must end BEFORE this word (nothing emitted).  callers pass pc_k =
 * the word's guest pc.  USER[15] virtual convention: the C slice loop sets
 * CPU.USER[15] = pc_k + 4 before every handler, and the NP tight handlers
 * never touch it; the tail FIQ path observes exactly pc_k + 4 (or the
 * branch target for a taken branch) — oj_tail is given that value verbatim.
 *
 * A cond == NV (mask 0) word executes no body but still runs its tail; it
 * may continue the block (except no class can branch here since a taken
 * branch requires cond_true).
 * ------------------------------------------------------------------------- */

/* emits: optional cond prefix (returns skip label or OJ_NO_COND) */
#define OJ_NO_COND 0xFFFFFFFFu

static uint32_t oj_word_cond(uint32_t const cmd_)
{
  if((cmd_ >> 28) == 0xE)
    return OJ_NO_COND;
  return oj_cond_prefix(cmd_);
}

static void oj_word_cond_close(uint32_t const l_skip_)
{
  if(l_skip_ != OJ_NO_COND)
    oj_lab_here(l_skip_);
}

/* returns 0 when the word is cond-NV (never executes): tail-only word */
static int oj_word_cond_is_nv(uint32_t const cmd_)
{
  return (cond_flags_cross[cmd_ >> 28] == 0);
}

/* TIGHT_DP_IMM / TIGHT_DP_RI / TIGHT_DP_RS shared driver */
static int oj_emit_dp(uint32_t const cmd_,
                      uint32_t const aux_,
                      uint32_t const pc_k_,
                      uint32_t const cls_)
{
  uint32_t l_skip;
  uint32_t rn;

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);

  /* RMW fast path: DP-imm word with S=0, rd == rn, unrotated
   * operand (rot == 0) and an ALU opc in the memory-RMW-able set
   * compiles to a single read-modify-write uop through USER[rd]:
   * the current shape pays a dead carry materialization (3 uops,
   * S=0 consumers), an operand load, the ALU op and the store --
   * all collapsible into `op dword [rbx+rd*4], imm32`.  Flag
   * state, cond-guard and cycle charges are unchanged (S=0
   * writes no flags; the carry feed is unconsumed). */
  if((cls_ == ARM_CLS_TIGHT_DP_IMM) &&
     !((cmd_ >> 20) & 1) &&
     (((cmd_ >> 12) & 0xF) == ((cmd_ >> 16) & 0xF)) &&
     (((cmd_ >> 7) & 0x1E) == 0) &&
     (((cmd_ >> 21) & 0xF) == 0x0 ||   /* AND */
      ((cmd_ >> 21) & 0xF) == 0x1 ||   /* EOR */
      ((cmd_ >> 21) & 0xF) == 0x2 ||   /* SUB */
      ((cmd_ >> 21) & 0xF) == 0x4 ||   /* ADD */
      ((cmd_ >> 21) & 0xF) == 0xC ||   /* ORR */
      ((cmd_ >> 21) & 0xF) == 0xE))    /* BIC */
    {
      uint32_t const rd_rmw = ((cmd_ >> 12) & 0xF);
      uint32_t const opc_rmw = ((cmd_ >> 21) & 0xF);
      uint32_t       imm_rmw = (cmd_ & 0xFF);
      uint32_t       xop_rmw;      /* x86 /r extension: ADD=0 OR=1
                                    * AND=4 SUB=5 XOR=6 */

      if(opc_rmw == 0x0)      xop_rmw = 4;    /* AND */
      else if(opc_rmw == 0x1) xop_rmw = 6;    /* EOR -> xor */
      else if(opc_rmw == 0x2) xop_rmw = 5;    /* SUB -> sub */
      else if(opc_rmw == 0x4) xop_rmw = 0;    /* ADD -> add */
      else if(opc_rmw == 0xC) xop_rmw = 1;    /* ORR -> or  */
      else                    { xop_rmw = 4;  /* BIC -> and ~imm */
                               imm_rmw = ~imm_rmw; }

      /* same disp32 idiom as oj_and_cpu / oj_st_cpu:
       * 81 /r modrm=mod10 reg=ext rm=011(rbx), disp32=OJ_U(rd) */
      oj_u8(0x81);                /* op dword [rbx + OJ_U(rd)], imm32 */
      oj_u8((uint8_t)(0x83 | (xop_rmw << 3)));
      oj_u32(OJ_U(rd_rmw));
      oj_u32(imm_rmw);

      oj_word_cond_close(l_skip);
      oj_tail(pc_k_ + 4);
      return 1;
    }

  /* imm decode strictly by the instruction's own bit 25: the PC and
   * PCREL classes arrive from BOTH the immediate (case 0x2/0x3) and
   * the register (case 0x0/0x1) classifier branches */
  if((cls_ == ARM_CLS_TIGHT_DP_IMM) ||
     (((cls_ == ARM_CLS_TIGHT_DP_PC) || (cls_ == ARM_CLS_TIGHT_DP_PCREL)) &&
      (cmd_ & (1u << 25))))
    {
      oj_u8(0xB8);                  /* mov eax, imm8 */
      oj_u32(cmd_ & 0xFF);
      oj_shift_static(aux_ & 0x3F, 3);   /* ROR by rot*2 (0 ok) */
    }
  else if(cls_ == ARM_CLS_TIGHT_DP_RMPC)
    {
      /* rm == 15: the handler bumps USER[15] to pc_k+8 before reading
       * op2 -- a compile-time constant, static-shifted (RRX excluded
       * at the classifier), computed without any carry write-back
       * since the S-flag carry feed of the shifted constant is folded
       * in C anyway (S=0 only at the classifier) */
      uint32_t const sh  = (aux_ & 0x3F);
      uint32_t const typ = ((aux_ >> 8) & 7);

      oj_u8(0xB8);                  /* mov eax, imm32 (pc_k+8) */
      oj_u32(pc_k_ + 8u);
      if((typ == 0) && (sh != 0))
        {
          oj_u8(0xC1); oj_u8(0xE0); oj_u8(sh);      /* shl eax, sh */
        }
      else if(typ == 1)
        {
          if(sh == 32) { oj_u8(0x31); oj_u8(0xC0); }
          else { oj_u8(0xC1); oj_u8(0xE8); oj_u8(sh); }
        }
      else if(typ == 2)
        {
          if(sh == 32) { oj_u8(0xC1); oj_u8(0xF8); oj_u8(31); }
          else { oj_u8(0xC1); oj_u8(0xF8); oj_u8(sh); }
        }
      else if((typ == 3) && (sh != 0))
        {
          oj_u8(0xC1); oj_u8(0xC8); oj_u8(sh);      /* ror eax, sh */
        }
    }
  else
    {
      oj_ld_cpu(OJ_RAX, OJ_U(cmd_ & 0xF));   /* eax = USER[rm] */
      if((cls_ == ARM_CLS_TIGHT_DP_RI) ||
         (cls_ == ARM_CLS_TIGHT_DP_PC) || (cls_ == ARM_CLS_TIGHT_DP_PCREL))
        oj_shift_static(aux_ & 0x3F, ((aux_ >> 8) & 7));
      else
        {
          oj_ld_cpu(OJ_RCX, OJ_U((cmd_ >> 8) & 0xF));
          oj_u8(0x0F);                /* movzx ecx, cl   (rs & 0xFF) */
          oj_u8(0xB6);
          oj_u8(0xC9);
          oj_shift_reg((cmd_ >> 5) & 3);
        }
    }

  rn = ((cmd_ >> 16) & 0xF);
  if(cls_ == ARM_CLS_TIGHT_DP_PCREL)
    {
      /* op1 = USER[15] at handler time.  The slice loop pre-syncs
       * USER[15] = pc_k + 4, then the full handler does pc_tmp =
       * USER[15]; USER[15] += 4; op1 = USER[15] -- i.e. the ARM
       * pipeline value pc_k + 8. */
      oj_u8(0xBE);                  /* mov esi, imm32 */
      oj_u32(pc_k_ + 8u);
    }
  else
    oj_ld_cpu(OJ_RSI, OJ_U(rn));    /* esi = op1 */
  oj_alu(cmd_);

  /* rd == 15 (pc write, e.g. `mov pc, lr` returns): S=0 forms are fully
   * inlineable.  oj_alu already stored the result into USER[15]; the
   * word terminates the block with the taken tail reading that dynamic
   * pc straight through the trampoline stubs (they re-read USER[15]),
   * never materialising an imm32 u15 that would clobber it.  S=1
   * (subs pc, ...) restores CPSR from SPSR with a bank switch: those
   * stay full-class C words.  Cost: ICYCLE + NCYCLE on top of the tail
   * (arm_dp_tail's `cycp -= (ICYCLE+NCYCLE)` under the -SCYCLE base). */
  if(((cmd_ >> 12) & 0xF) == 0xF && !(cmd_ & (1 << 20)))
    {
      oj_charge_extra(ICYCLE + NCYCLE);
      {
        uint32_t const l_nofiq = oj_lab_alloc();
        uint32_t const l_nobud = oj_lab_alloc();
        oj_u8(0x41);                    /* add r12d, 1   (SCYCLE) */
        oj_u8(0x83);
        oj_u8(0xC4);
        oj_u8(1);
        oj_u8(0x81);                    /* cmp dword [rbp], 0 */
        oj_u8(0x7D);
        oj_u8(0x00);
        oj_u32(0);
        OJ_JZ(l_nofiq);
        oj_u8(0xF6);                    /* test byte [rbx + CPSR], 0x40 */
        oj_u8(0x83);
        oj_u32(OJ_CPSR);
        oj_u8(0x40);
        OJ_JNZ(l_nofiq);
        oj_jmp_abs(g_jit_tramp_xfiq);   /* USER[15] = result already */
        oj_lab_here(l_nofiq);
        if(!g_jit_budbatch)
          {
            oj_u8(0x45);                /* cmp r12d, r13d */
            oj_u8(0x39);
            oj_u8(0xEC);
            OJ_JL(l_nobud);
            oj_jmp_abs(g_jit_tramp_xbudget);
          }
        oj_lab_here(l_nobud);
        oj_jmp_abs(g_jit_tramp_xnext);
      }

      /* cond-fail lands here: nothing executed, ordinary tail */
      oj_word_cond_close(l_skip);
      oj_tail(pc_k_ + 4);
      oj_exit_next(pc_k_ + 4);
      return 2;                 /* block terminates at the pc write */
    }

  oj_charge_extra((cls_ == ARM_CLS_TIGHT_DP_RS) ? 1 : 0);   /* ICYCLE */
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  return 1;
}

/* TIGHT_MUL — includes the rd == rm quirks and the exact
 * ((calcbits(rs) + 5) >> 1) - 1 (clamped to 16) runtime cost. */
static int oj_emit_mul(uint32_t const cmd_,
                       uint32_t const pc_k_)
{
  uint32_t l_skip;
  uint32_t l_cb0;
  uint32_t l_cbd;
  uint32_t l_clamp;
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


  /* ecx = USER[rs]; edx = calcbits(ecx); r12d += min(((calcbits+5)>>1)-1,16) */
  oj_ld_cpu(OJ_RCX, OJ_U(rs));
  l_cb0   = oj_lab_alloc();
  l_cbd   = oj_lab_alloc();
  l_clamp = oj_lab_alloc();
  oj_u8(0x85);                    /* test ecx, ecx */
  oj_u8(0xC9);
  OJ_JZ(l_cb0);
  oj_u8(0x0F);                    /* bsr edx, ecx */
  oj_u8(0xBD);
  oj_u8(0xD1);
  oj_u8(0x83);                    /* add edx, 1  (== calcbits for n>0) */
  oj_u8(0xC2);
  oj_u8(1);
  oj_jmp(l_cbd);
  oj_lab_here(l_cb0);
  oj_u8(0xBA);                    /* mov edx, 1  (calcbits(0) == 1) */
  oj_u32(1);
  oj_lab_here(l_cbd);
  oj_u8(0x83);                    /* add edx, 5 */
  oj_u8(0xC2);
  oj_u8(5);
  oj_u8(0xD1);                    /* shr edx, 1 */
  oj_u8(0xEA);
  oj_u8(0x83);                    /* sub edx, 1 */
  oj_u8(0xEA);
  oj_u8(1);
  oj_u8(0x83);                    /* cmp edx, 16 */
  oj_u8(0xFA);
  oj_u8(16);
  OJ_JBE(l_clamp);
  oj_u8(0xBA);                    /* mov edx, 16 */
  oj_u32(16);
  oj_lab_here(l_clamp);
  oj_u8(0x41);                    /* add r12d, edx */
  oj_u8(0x01);
  oj_u8(0xD4);

  /* result */
  if(rd == rm)
    {
      /* quirk: rd == rm keeps old accumulate/0 semantics, no multiply */
      if(cmd_ & (1 << 21))
        oj_ld_cpu(OJ_RAX, OJ_U(rn));
      else
        {
          oj_u8(0x31);            /* xor eax, eax */
          oj_u8(0xC0);
        }
    }
  else
    {
      oj_ld_cpu(OJ_RAX, OJ_U(rm));
      oj_u8(0x0F);                /* imul eax, ecx */
      oj_u8(0xAF);
      oj_u8(0xC1);
      if(cmd_ & (1 << 21))
        {
          oj_u8(0x03);            /* add eax, [rbx + U(rn)] */
          oj_u8(0x83);
          oj_u32(OJ_U(rn));
        }
    }

  if(cmd_ & (1 << 20))
    oj_arm_set_zn(OJ_RAX);

  oj_st_cpu(OJ_RAX, OJ_U(rd));

  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  return 1;
}

/* TIGHT_BRANCH — always ends the block (except cond-NV, tail-only).
 * Taken cost: 1 + (SCYCLE+NCYCLE) -> 4; fallthrough cost: 1. */
static int oj_emit_branch(uint32_t const cmd_,
                          uint32_t const pc_k_)
{
  int32_t  const off    = ((int32_t)(cmd_ << 8)) >> 6;
  uint32_t const target = (uint32_t)(pc_k_ + 8 + off);
  uint32_t       l_fall;

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  if((cmd_ >> 28) != 0xE)
    {
      l_fall = oj_lab_alloc();
      oj_ld_cpu(OJ_RAX, OJ_CPSR);
      oj_u8(0xC1);                /* shr eax, 28 */
      oj_u8(0xE8);
      oj_u8(28);
      oj_u8(0xBA);                /* mov edx, mask */
      oj_u32(cond_flags_cross[cmd_ >> 28]);
      oj_u8(0x0F);                /* bt edx, eax */
      oj_u8(0xA3);
      oj_u8(0xC2);
      OJ_JNC(l_fall);
    }
  else
    l_fall = OJ_NO_COND;

  /* taken path */
  if(cmd_ & (1 << 24))
    {
      oj_u8(0xB8);                /* mov eax, pc_k_ + 4  (USER[15] pre-jump) */
      oj_u32(pc_k_ + 4);
      oj_st_cpu(OJ_RAX, OJ_U(14));
    }
  oj_charge_extra(SCYCLE + NCYCLE);
  oj_tail(target);
  /* backward-edge threading: a static target already emitted inside this
   * same block becomes a direct in-block jump; the register invariants
   * (rbx/rbp/r11/r12-r15 pinned by the prologue, all others transient per
   * word) hold identically at every word start.  Semantics are unchanged:
   * the branch's own tail already ran FIQ/rst/budget/FSM for each pass,
   * and the poll exits carry USER[15] = target. */
  if((target < pc_k_) && (target >= s_oj_block_pc))
    {
      uint32_t const tw = ((target - s_oj_block_pc) >> 2);
      uint32_t const at = s_oj_word_off[tw];
      /* PREFIT: the in-block loop has no per-word budget check
       * (elided by the block's guard proving the block fits in
       * the remaining budget ONCE) -- but a loop iterates, so
       * the cumulative charge is unbounded.  Restore ONE check
       * per backedge pass: the branch's own tail already charged
       * this iteration; if the cumulative r12d reached the
       * budget, exit at the branch with USER[15]=target exactly
       * like the elided per-word check would have. */
      if(g_jit_budbatch)
        {
          uint32_t const l_be_ok = oj_lab_alloc();
          oj_u8(0x45);          /* cmp r12d, r13d */
          oj_u8(0x39);
          oj_u8(0xEC);
          OJ_JL(l_be_ok);
          oj_u8(0xB8);          /* mov eax, target */
          oj_u32(target);
          oj_st_cpu(OJ_RAX, OJ_U15);
          oj_jmp_abs(g_jit_tramp_xbudget);
          oj_lab_here(l_be_ok);
        }
      oj_u8(0xE9);              /* jmp rel32 (backward, offset known) */
      oj_u32(at - (s_oj_len + 4));
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

  return 2;                       /* block ends after this word */
}

/* TIGHT_SDT_LDI / TIGHT_SDT_LDR — inline DRAM fast path only; any address
 * outside [0, RAM_SIZE) exits to a C-step of this exact word, which then
 * runs the full handler (ROM/MMIO windows, aborts, data aborts) verbatim. */
static int oj_emit_sdt_load(uint32_t const cmd_,
                            uint32_t const aux_,
                            uint32_t const pc_k_,
                            uint32_t const cls_)
{
  uint32_t l_skip;
  uint32_t l_slow;
  uint32_t l_norot;
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

  /* esi = base (USER[rn]); edi/eax = offset */
  oj_ld_cpu(OJ_RSI, OJ_U(rn));

  if(cls_ == ARM_CLS_TIGHT_SDT_LDI)
    {
      oj_u8(0xBF);                /* mov edi, imm12 */
      oj_u32(cmd_ & 0x0FFF);
    }
  else
    {
      oj_ld_cpu(OJ_RAX, OJ_U(cmd_ & 0xF));      /* eax = USER[rm] */
      oj_shift_static(aux_ & 0x3F, ((aux_ >> 8) & 7));
      oj_u8(0x89);                /* mov edi, eax */
      oj_u8(0xC7);
    }

  if(!(cmd_ & (1 << 23)))
    {
      oj_u8(0xF7);                /* neg edi */
      oj_u8(0xDF);
    }

  if(cmd_ & (1 << 24))
    {
      oj_u8(0x01);                /* add esi, edi   (pre-index) */
      oj_u8(0xFE);
      oj_u8(0x89);                /* mov edx, esi   (tbas = new base) */
      oj_u8(0xF2);
    }
  else
    {
      oj_u8(0x89);                /* mov edx, esi   (tbas = old base) */
      oj_u8(0xF2);
      oj_u8(0x01);                /* add esi, edi */
      oj_u8(0xFE);
    }

  /* range check on edx = tbas */
  if(is_byte)
    {
      oj_rip_cmp32(OJ_RDX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);       /* jae slow */

      /* val = DRAM[tbas ^ 3] */
      oj_u8(0x89);                /* mov ecx, edx */
      oj_u8(0xD1);
      oj_u8(0x83);                /* xor ecx, 3 */
      oj_u8(0xF1);
      oj_u8(3);
      oj_rip_ld64(OJ_RDI, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x0F);                /* movzx eax, byte [rdi + rcx] */
      oj_u8(0xB6);
      oj_u8(0x04);
      oj_u8(0x0F);
    }
  else
    {
      oj_u8(0x89);                /* mov eax, edx */
      oj_u8(0xD0);
      oj_u8(0x83);                /* and eax, ~3u */
      oj_u8(0xE0);
      oj_u8(0xFC);
      oj_rip_cmp32(OJ_RAX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);       /* jae slow */

      /* eax = *(uint32_t *)&DRAM[tbas & ~3] */
      oj_rip_ld64(OJ_RDI, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x8B);                /* mov eax, [rdi + rax] */
      oj_u8(0x04);
      oj_u8(0x07);

      /* rotate by (tbas & 3) * 8 (ARM unaligned-word semantics) */
      oj_u8(0x83);                /* and edx, 3 */
      oj_u8(0xE2);
      oj_u8(3);
      OJ_JZ(l_norot);
      oj_u8(0xC1);                /* shl edx, 3 */
      oj_u8(0xE2);
      oj_u8(3);
      oj_u8(0x89);                /* mov ecx, edx */
      oj_u8(0xD1);
      oj_u8(0xD3);                /* ror eax, cl */
      oj_u8(0xC8);
      oj_lab_here(l_norot);
    }

  /* writeback: (bit21 || !bit24) holds for every tight class */
  if((cmd_ & (1 << 21)) || !(cmd_ & (1 << 24)))
    oj_st_cpu(OJ_RSI, OJ_U(rn));  /* USER[rn] = new base */
  oj_st_cpu(OJ_RAX, OJ_U(rd));    /* USER[rd] = val */

  oj_charge_extra(NCYCLE + ICYCLE);
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);               /* continue block: jump over the slow stub */
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

/* PC-relative literal load (LDR rd, [pc, #imm] et al.): ARM_CLS_SDT_IMM
 * words with rn == 15, P=1, W=0, rd != 15.  The address is compile-time
 * static (pc_k_ + 8 +/- imm12, verified by differential probe), so the
 * DRAM read collapses to a single disp32 load; the emitted RAM_SIZE gate
 * mirrors the generic load's runtime bound (out-of-window -> C-step).
 * DRAM contents stay runtime-fresh per read, so baking the address into
 * the block creates no invalidation hazard.  Cost: NCYCLE+ICYCLE extra
 * plus the tail's SCYCLE -- the same 4-cycle total as the C tight load. */
static int oj_emit_sdt_literal(uint32_t const cmd_,
                               uint32_t const pc_k_)
{
  uint32_t l_skip;
  uint32_t l_slow;
  uint32_t addr;
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  /* gate down to the plain literal shape */
  if((cmd_ & (1 << 20)) == 0)                       /* bit20: loads only */
    return 0;
  if(((cmd_ >> 16) & 0xF) != 0xF)                   /* rn == pc */
    return 0;
  if(((cmd_ >> 12) & 0xF) == 0xF)                   /* rd != pc */
    return 0;
  if(!(cmd_ & (1 << 24)) || (cmd_ & (1 << 21)))     /* P=1, W=0 */
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

  if(is_byte)
    {
      /* byte: bounds on the raw address, DRAM[addr ^ 3] */
      oj_u8(0xB9);              /* mov ecx, addr */
      oj_u32(addr);
      oj_rip_cmp32(OJ_RCX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);     /* jae slow */
      oj_rip_ld64(OJ_RAX, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x0F);              /* movzx eax, byte [rax + disp32=(addr^3)] */
      oj_u8(0xB6);
      oj_u8(0x80);
      oj_u32(addr ^ 3);
    }
  else
    {
      uint32_t const aligned = (addr & ~3u);

      oj_u8(0xB8);              /* mov eax, aligned */
      oj_u32(aligned);
      oj_rip_cmp32(OJ_RAX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);     /* jae slow */
      oj_rip_ld64(OJ_RAX, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x8B);              /* mov eax, [rax + disp32=aligned] */
      oj_u8(0x80);
      oj_u32(aligned);
      if(addr & 3)
        {
          oj_u8(0xC1);          /* ror eax, (addr & 3) * 8 */
          oj_u8(0xC8);
          oj_u8((addr & 3) * 8);
        }
    }

  oj_st_cpu(OJ_RAX, OJ_U((cmd_ >> 12) & 0xF));

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

/* Invalidation probe for one aligned DRAM word held in eax: walks the
 * page's registered-block list and jumps to l_slow_ when the word falls
 * inside any live block's [table_index*4, table_index*4 + nwords*4) byte
 * range.  Scratch: ecx/rdi/r8/r9/r10; eax is consumed by the movabs
 * table loads (callers re-derive it); edx/rsi and the persistent
 * registers (rbx/rbp/r11/r12-r15) are preserved. */
/* coverage probe: eax holds an aligned guest word address.  One byte
 * load from g_jit_word_cov (kept exact by register/kill/flush_all on
 * the C side) replaces the whole page-list walk: nonzero -> a live
 * block covers the word, so the store must take the C kill path.
 * Out-of-window indices are clean (no block can cover them). */
static void oj_cov_probe(uint32_t const l_ck_,
                         uint32_t const l_slow_)
{
  uint32_t const l_in = oj_lab_alloc();

  oj_u8(0xC1);                  /* shr eax, 2  (word index) */
  oj_u8(0xE8);
  oj_u8(2);
  oj_movabs(OJ_RCX, (uint64_t)(uintptr_t)&g_jit_ram_words);
  oj_u8(0x3B);                  /* cmp eax, [rcx] (index vs limit) */
  oj_u8(0x01);
  oj_jcc(0x82, l_in);           /* jb: index inside the RAM window */
  oj_jmp(l_ck_);                /* out of window: clean */
  oj_lab_here(l_in);
  oj_movabs(OJ_RCX, (uint64_t)(uintptr_t)&g_jit_word_cov);
  oj_u8(0x48);                  /* mov rcx, [rcx]  (array base) */
  oj_u8(0x8B);
  oj_u8(0x09);
  oj_u8(0x80);                  /* cmp byte [rcx + rax], 0  (SIB) */
  oj_u8(0x3C);
  oj_u8(0x08);
  oj_u8(0x00);
  oj_jcc(0x85, l_slow_);        /* jne: covered -> C kill path */
}

static void oj_bdt_probe(uint32_t const l_ck_,
                         uint32_t const l_slow_)
{
  /* coverage-byte probe (see oj_cov_probe): eax holds the aligned guest
   * word address.  The old page-list walk is gone -- the C side keeps
   * the per-word coverage counts exact. */
  oj_cov_probe(l_ck_, l_slow_);
}

/* LDM/STM inline for the common non-banked (S=0), rn != 15, no-r15-in-list
 * shape, with the whole transfer window gated to flat DRAM.  Mirrors
 * ldm_accur/stm_accur exactly: per-element aligned word transactions
 * (the C mreadw/mwritew each apply & ~3), ascending element order, and
 * the computed writeback applied after the transfers.  Cost n+3 total
 * (n+2 extra + the tail's SCYCLE), confirmed by differential probes for
 * all four addressing modes; cond-fail costs the tail's 1 as usual.
 *
 * Windows outside [0, RAM_SIZE), plus the HIRESMODE fanout region
 * [DRAM_SIZE, RAM_SIZE) for stores, fall to the slow cstep stub, which
 * re-runs the whole word through C (CLIO pokes, soft-reset truncation,
 * data aborts and all).  In-DRAM stores never poke CLIO, so the
 * per-element g_SOFT_RESET_PENDING check in stm_accur can never fire on
 * this path and needs no emitted equivalent.
 *
 * Register plan: esi = first element address, edx = w0 = esi & ~3 (kept
 * across the probes), eax/ecx/rdi/r8/r9/r10 scratch, rdi = DRAM base for
 * the transfers. */
static int oj_emit_bdt(uint32_t const cmd_,
                       uint32_t const pc_k_)
{
  uint32_t l_skip;
  uint32_t l_slow;
  uint32_t const list = (cmd_ & 0xFFFF);
  uint32_t const rn   = ((cmd_ >> 16) & 0xF);
  int      const is_ldm = ((cmd_ & (1 << 20)) != 0);
  uint32_t       n  = 0;
  uint32_t       i;
  uint32_t       disp;

  /* ---- compile-time gates: exotic shapes stay in C ---- */
  if(cmd_ & (1 << 22))          /* S bit: usr-bank transfer */
    {
      return 0;
    }
  if(rn == 0xF)                 /* pc-relative base */
    {
      return 0;
    }
  if(list == 0)
    {
      return 0;                 /* empty list */
    }
  /* STM with pc in the list IS inlineable.  stm_accur masks the list
   * to 0x7FFF, but its count (x) comes from the full 16-bit mask -- the
   * pc slot reserves stack space and moves the base -- and the C store
   * loop still reaches i == 15 with list&1 set, storing USER[15] which
   * the cached engine has advanced to (pc_k + 4) + 8 by then.  Probed:
   * the stored value is exactly pc_k + 12, an emission-time constant. */
  /* LDM with pc in list stays inlineable: the final element load writes
   * USER[15] directly (S=0 path of ldm_accur), the word terminates the
   * block, and USER[15] is already materialized for every tail exit. */

  for(i = 0; i < 16; i++)
    n += ((list >> i) & 1);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  l_slow = oj_lab_alloc();

  /* ---- first element address: P/U mode offsets ---- */
  oj_ld_cpu(OJ_RSI, OJ_U(rn));  /* esi = base */
  if(!(cmd_ & (1 << 24)))       /* P = 0 */
    {
      if(!(cmd_ & (1 << 23)))   /* DA: first = base - 4*(n-1) */
        {
          if(n > 1)
            {
              oj_u8(0x83);      /* sub esi, 4*(n-1) */
              oj_u8(0xEE);
              oj_u8(4 * (n - 1));
            }
        }
      /* else IA: first = base */
    }
  else                          /* P = 1 */
    {
      oj_u8(0x83);
      if(cmd_ & (1 << 23))      /* IB: first = base + 4 */
        {
          oj_u8(0xC6);          /* add esi, 4 */
          oj_u8(4);
        }
      else                      /* DB: first = base - 4*n */
        {
          oj_u8(0xEE);          /* sub esi, 4*n */
          oj_u8(4 * n);
        }
    }

  /* ---- window gate: [w0, w0 + 4*(n-1)] all within RAM_SIZE ---- */
  oj_u8(0x89);                  /* mov edx, esi */
  oj_u8(0xF2);
  oj_u8(0x83);                  /* and edx, ~3u  (w0) */
  oj_u8(0xE2);
  oj_u8(0xFC);
  oj_u8(0x89);                  /* mov eax, edx */
  oj_u8(0xD0);
  if(n > 1)
    {
      oj_u8(0x83);              /* add eax, 4*(n-1)  (wN) */
      oj_u8(0xC0);
      oj_u8(4 * (n - 1));
    }
  oj_rip_cmp32(OJ_RAX, (uint64_t)(uintptr_t)&RAM_SIZE);
  oj_jcc(0x83, l_slow);         /* jae slow */

  if(!is_ldm)
    {
      /* stores: rule out the HIRESMODE fanout region exactly like the
       * single-store path does */
      uint32_t const l_nh = oj_lab_alloc();
      oj_rip_cmp8z((uint64_t)(uintptr_t)&HIRESMODE);
      oj_jcc(0x84, l_nh);
      oj_rip_ld64(OJ_RCX, (uint64_t)(uintptr_t)&DRAM_SIZE);
      oj_u8(0x3B);              /* cmp eax, [rcx] */
      oj_u8(0x01);
      oj_jcc(0x83, l_slow);     /* jae slow: wN >= DRAM_SIZE under hires */
      oj_lab_here(l_nh);

      /* Invalidation probes on EVERY word in the transfer window, not
       * just the endpoints.  The old two-point w0/wN probe let a
       * multi-register STM whose window strictly contained a block in
       * its interior overwrite compiled code without invalidating it
       * (missed self-modifying-code kill: stale block, guest-visible
       * divergence).  A bounded downward loop over [w0, wN] (n <= 16
       * words) catches any covered word; covered -> l_slow, where the C
       * cstep re-executes the whole STM through the hooked per-element
       * writes and kills exactly like a single store would.
       *
       * Register plan: edx = current probe word (starts at wN, steps
       * down by 4), r8d = w0 (loop bound), eax/ecx scratch (re-derived
       * each iteration: every movabs in the probe leaves rax holding a
       * host pointer).  r8d is free here: it is first used below by the
       * transfer loop as scratch and r8/r9/r10 are documented as
       * scratch registers for the whole emission. */
      {
        uint32_t const l_ni = oj_lab_alloc();
        uint32_t const l_w  = oj_lab_alloc();
        uint32_t const l_c  = oj_lab_alloc();
        oj_u8(0x41);        /* mov r8d, edx (save w0): REX.B + 89 D0 */
        oj_u8(0x89);
        oj_u8(0xD0);
        if(n > 1)
          {
            oj_u8(0x83);    /* add edx, 4*(n-1)  (wN) */
            oj_u8(0xC2);
            oj_u8(4 * (n - 1));
          }
        oj_lab_here(l_w);
        oj_u8(0x89);        /* mov eax, edx (probe word) */
        oj_u8(0xD0);
        oj_cov_probe(l_c,l_slow);
        oj_lab_here(l_c);
        oj_u8(0x83);        /* sub edx, 4 (next word down) */
        oj_u8(0xEA);
        oj_u8(4);
        oj_u8(0x44);        /* cmp edx, r8d */
        oj_u8(0x39);
        oj_u8(0xC2);
        oj_jcc(0x83, l_w);  /* jae: still >= w0 -> probe next */
        oj_u8(0x44);        /* mov edx, r8d (restore w0 for the loop
                               below, which addresses every element as
                               [rdi + rdx + disp]): REX.R + 89 C2 */
        oj_u8(0x89);
        oj_u8(0xC2);
        oj_lab_here(l_ni);
      }
    }
  else
    {
      /* loads read flat DRAM through mreadw; no invalidation concern */
    }

  /* ---- writeback with rn in the list.  The two engines differ:
   * ldm_accur writes USER[rn] = base_ before the transfers whenever W
   * is set (the rn load then lands last and overwrites it), while
   * stm_accur's preload fires only when the list has any register
   * BELOW rn ((list & ((1<<rn)-1)) != 0) -- without lower registers
   * there is no preload and the rn slot stores the ORIGINAL register
   * value.  The plain after-transfers writeback below is skipped for
   * the shapes handled here. */
  {
    int const wrn_in_list = ((cmd_ & (1u << 21)) &&
                             (list & (1u << rn)) &&
                             (is_ldm || (list & ((1u << rn) - 1u))));

    if(wrn_in_list)
      {
        uint32_t adj = 0;       /* |writeback - first| */
        int      sub = 0;

        if(!(cmd_ & (1 << 24)))
          {
            if(cmd_ & (1 << 23))  { adj = 4 * n; }        /* IA */
            else                  { adj = 4; sub = 1; }  /* DA */
          }
        else
          {
            if(cmd_ & (1 << 23))  { adj = 4 * (n - 1); }  /* IB */
            /* DB: writeback == first */
          }

        if(adj)
          {
            oj_u8(0x83);
            oj_u8(sub ? 0xEE : 0xC6);
            oj_u8(adj);
          }
        oj_st_cpu(OJ_RSI, OJ_U(rn));

        /* reload esi = first element address for the transfers */
        oj_ld_cpu(OJ_RSI, OJ_U(rn));
        if(!(cmd_ & (1 << 24)))       /* P = 0 */
          {
            if(!(cmd_ & (1 << 23)))   /* DA: first = base - 4*(n-1) */
              {
                if(n > 1)
                  {
                    oj_u8(0x83);      /* sub esi, 4*(n-1) */
                    oj_u8(0xEE);
                    oj_u8(4 * (n - 1));
                  }
              }
          }
        else                          /* P = 1 */
          {
            oj_u8(0x83);
            if(cmd_ & (1 << 23))      /* IB: first = base + 4 */
              {
                oj_u8(0xC6);
                oj_u8(4);
              }
            else                      /* DB: first = base - 4*n */
              {
                oj_u8(0xEE);
                oj_u8(4 * n);
              }
          }
      }
  }

  /* ---- transfers: ascending elements at w0 + 4*i ---- */
  oj_rip_ld64(OJ_RDI, (uint64_t)(uintptr_t)&DRAM);

  disp = 0;
  for(i = 0; i < 16; i++)
    {
      if(!((list >> i) & 1))
        continue;

      if(is_ldm)
        {
          oj_u8(0x8B);          /* mov eax, [rdi + rdx + disp32] */
          oj_u8(0x84);
          oj_u8(0x17);
          oj_u32(disp);
          oj_st_cpu(OJ_RAX, OJ_U(i));
        }
      else
        {
          if(i != 15)
            {
              oj_ld_cpu(OJ_RCX, OJ_U(i));
              oj_u8(0x89);          /* mov [rdi + rdx + disp32], ecx */
              oj_u8(0x8C);
              oj_u8(0x17);
              oj_u32(disp);
            }
          else
            {
              oj_u8(0xB9);           /* mov ecx, pc_k + 12 (probed:
                                       * USER[15] double-advanced by
                                       * the cached engine at store
                                       * time) */
              oj_u32(pc_k_ + 12u);
              oj_u8(0x89);          /* mov [rdi + rdx + disp32], ecx */
              oj_u8(0x8C);
              oj_u8(0x17);
              oj_u32(disp);
            }
        }
      disp += 4;
    }

  /* ---- writeback (bit21), after the transfers ----
   * STM: stm_accur's post-loop `if(opc&(1<<21)) USER[rn] = base_` is
   * unconditional on W; for the rn-in-list-with-lower-registers shape
   * it merely repeats the preload's value (idempotent), so skipping it
   * here is exact — and required, because the preload's reload left
   * esi re-derived from the written-back USER[rn], not the base.
   * LDM: ldm_accur does the opposite -- W's USER[rn] store happens
   * BEFORE the transfer loop (the rn slot's later load then
   * overwrites it); there is NO post-loop writeback, so LDM words
   * must not take this path.  esi holds the first-element address on
   * every path here. */
  if((cmd_ & (1 << 21)) &&
     (!is_ldm
      ? !(list & (1u << rn)) || !(list & ((1u << rn) - 1u))
      : !(list & (1u << rn))))
    {
      uint32_t adj = 0;         /* |writeback - first| */
      int      sub = 0;

      if(!(cmd_ & (1 << 24)))
        {
          if(cmd_ & (1 << 23))  { adj = 4 * n; }        /* IA */
          else                  { adj = 4; sub = 1; }   /* DA */
        }
      else
        {
          if(cmd_ & (1 << 23))  { adj = 4 * (n - 1); }  /* IB */
          /* DB: writeback == first */
        }

      if(adj)
        {
          oj_u8(0x83);
          oj_u8(sub ? 0xEE : 0xC6);
          oj_u8(adj);
        }
      oj_st_cpu(OJ_RSI, OJ_U(rn));
    }

  if(is_ldm && (list & 0x8000))
    {
      /* LDM {.., pc}: cost (n-1)S + N + I + (S+N) + tail S = n+6 total.
       * USER[15] already holds the loaded pc when this word actually
       * executes, so the taken-path tail must not materialize a u15
       * value of its own (the stubs would clobber the loaded pc).  The
       * cond-fail path runs the ordinary tail with USER[15] = pc_k_+4. */

      /* The window gate above jumps to l_slow on an out-of-range base;
       * this path returns before the generic slow stub below, so bind
       * l_slow with its own C-exit -- placed at the very end of the
       * word's emission, jumped over by the taken path (bug: the label
       * used to stay unbound, failing oj_resolve for every block that
       * contained an ldm-with-pc). */
      {
        uint32_t const l_go = oj_lab_alloc();
        oj_jmp(l_go);
        oj_lab_here(l_slow);
        oj_exit_cstep(pc_k_);
        oj_lab_here(l_go);
      }

      oj_charge_extra(n + 5);
      {
        uint32_t const l_nofiq = oj_lab_alloc();
        uint32_t const l_nobud = oj_lab_alloc();
        oj_u8(0x41);                    /* add r12d, 1   (SCYCLE) */
        oj_u8(0x83);
        oj_u8(0xC4);
        oj_u8(1);
        oj_u8(0x81);                    /* cmp dword [rbp], 0 */
        oj_u8(0x7D);
        oj_u8(0x00);
        oj_u32(0);
        OJ_JZ(l_nofiq);
        oj_u8(0xF6);                    /* test byte [rbx + CPSR], 0x40 */
        oj_u8(0x83);
        oj_u32(OJ_CPSR);
        oj_u8(0x40);
        OJ_JNZ(l_nofiq);
        oj_jmp_abs(g_jit_tramp_xfiq);
        oj_lab_here(l_nofiq);
        if(!g_jit_budbatch)
          {
            oj_u8(0x45);                /* cmp r12d, r13d */
            oj_u8(0x39);
            oj_u8(0xEC);
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
      return 2;                 /* block terminates at the pc load */
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

/* TIGHT_SDT_STI — store, imm offset, DRAM fast path.  Mirrors
 * arm_h_sdt_sti_t exactly: same address math, same range + HIRES gates,
 * same writeback rule, identical cost model (extra = 2*NCYCLE - SCYCLE,
 * the tail contributes the SCYCLE).
 *
 * The invalidation hook opera_mem_write8/32 would run is folded into an
 * in-block range probe: the slow path is taken only when the stored word
 * overlaps a registered block's byte range (page walk, per-entry kill on
 * overlap).  Data sitting beside code no longer invalidates anything;
 * stores that hit compiled words still invalidate exactly, mirrored by
 * the range-precise opera_arm_jit_touch on the C side.
 * Layout: jit_page_list_t { v**; uint32 n; uint32 cap; } = 16 bytes,
 * n at offset 8; indexed pages by (guest addr >> 12).  The npages bound
 * mirrors opera_arm_jit_touch's early-out.

 * Register plan:
 *   rsi = base/new base (writeback operand at the end)
 *   edx = tbas throughout (never clobbered before the store)
 *   rax/rcx/rdi = scratch
 */
static int oj_emit_sdt_store(uint32_t const cmd_,
                             uint32_t const pc_k_)
{
  uint32_t l_skip;
  uint32_t l_slow;
  uint32_t l_nh;                  /* HIRES check passed */
  uint32_t const rn = ((cmd_ >> 16) & 0xF);
  uint32_t const rd = ((cmd_ >> 12) & 0xF);
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  l_slow = oj_lab_alloc();
  l_nh   = oj_lab_alloc();

  /* esi = base (USER[rn]); edx = tbas */
  oj_ld_cpu(OJ_RSI, OJ_U(rn));

  oj_u8(0xBF);                  /* mov edi, imm12 */
  oj_u32(cmd_ & 0x0FFF);

  if(!(cmd_ & (1 << 23)))
    {
      oj_u8(0xF7);              /* neg edi */
      oj_u8(0xDF);
    }

  if(cmd_ & (1 << 24))
    {
      oj_u8(0x01);              /* add esi, edi   (pre-index) */
      oj_u8(0xFE);
      oj_u8(0x89);              /* mov edx, esi   (tbas = new base) */
      oj_u8(0xF2);
    }
  else
    {
      oj_u8(0x89);              /* mov edx, esi   (tbas = old base) */
      oj_u8(0xF2);
      oj_u8(0x01);              /* add esi, edi */
      oj_u8(0xFE);
    }

  /* range gate: byte compares tbas, word compares tbas & ~3 */
  if(is_byte)
    {
      oj_rip_cmp32(OJ_RDX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);     /* jae slow */
    }
  else
    {
      oj_u8(0x89);              /* mov eax, edx */
      oj_u8(0xD0);
      oj_u8(0x83);              /* and eax, ~3u */
      oj_u8(0xE0);
      oj_u8(0xFC);
      oj_rip_cmp32(OJ_RAX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);     /* jae slow */
    }

  /* HIRES gate: HIRESMODE && addr >= DRAM_SIZE -> slow */
  oj_rip_testb1((uint64_t)(uintptr_t)&HIRESMODE);
  oj_jcc(0x84, l_nh);           /* jz */
  {
    uint32_t const cmp_modrm_ = is_byte ? 0x10u /* cmp edx, [rcx] */
                                        : 0x00u /* cmp eax, [rcx] */;
    oj_rip_ld64(OJ_RCX, (uint64_t)(uintptr_t)&DRAM_SIZE);
    oj_u8(0x3B);                /* cmp (edx|eax), [rcx] */
    oj_u8(cmp_modrm_);
    oj_jcc(0x83, l_slow);
  }
  oj_lab_here(l_nh);

  /* invalidation probe: slow path only when the stored word overlaps a
   * registered block's [ti*4, ti*4 + nwords*4) range.  Adjacent data no
   * longer bounces through cstep + a wholesale page kill; stores to real
   * code still invalidate exactly.  Layout: jit_page_list_t = 16 bytes,
   * v at 0, n at 8; jit_block_t table_index at 8, nwords at 12, dead at
   * 16.  Page index = (tbas >> 12) which equals the aligned word's page.
   *
   *   eax = tbas & ~3 (word covered; materialized after the last movabs)
   *   edx = tbas (never clobbered before the store)
   *   rdi = page list v, r8d = walk index, r9d = n, rcx/r10 scratch
   *   (r11 stays untouched: it is a persistent block-lifetime register)
   */
  {
    uint32_t const l_ck   = oj_lab_alloc();
    uint32_t const l_walk = oj_lab_alloc();
    uint32_t const l_next = oj_lab_alloc();

    /* page = tbas >> 12 straight from edx: the low two bits never change
     * the 4KB page, and the eax form would not survive the two movabs
     * table loads that follow.  eax = tbas & ~3 is computed only once
     * rax is done being a table-pointer register. */
    oj_u8(0x89);                /* mov ecx, edx */
    oj_u8(0xD1);
    oj_u8(0xC1);                /* shr ecx, 12 */
    oj_u8(0xE9);
    oj_u8(12);
    oj_movabs(OJ_RAX, (uint64_t)(uintptr_t)&g_jit_npages);
    oj_u8(0x3B);                /* cmp ecx, [rax] */
    oj_u8(0x08);
    oj_jcc(0x83, l_ck);         /* jae: out of table == early-out */
    oj_movabs(OJ_RAX, (uint64_t)(uintptr_t)&g_jit_pages);
    oj_u8(0x48);                /* mov rax, [rax] */
    oj_u8(0x8B);
    oj_u8(0x00);
    oj_u8(0xC1);                /* shl ecx, 4 */
    oj_u8(0xE1);
    oj_u8(4);
    oj_u8(0x48);                /* mov rdi, [rax + rcx]  (page->v) */
    oj_u8(0x8B);
    oj_u8(0x3C);
    oj_u8(0x08);
    oj_u8(0x44);                /* mov r9d, [rax + rcx + 8] (page->n) */
    oj_u8(0x8B);
    oj_u8(0x4C);
    oj_u8(0x08);
    oj_u8(8);
    oj_u8(0x45);                /* test r9d, r9d */
    oj_u8(0x85);
    oj_u8(0xC9);
    oj_jcc(0x84, l_ck);         /* jz: no blocks -> fast */
    oj_u8(0x89);                /* mov eax, edx */
    oj_u8(0xD0);
    oj_u8(0x83);                /* and eax, ~3u  (rax safe past here) */
    oj_u8(0xE0);
    oj_u8(0xFC);
    oj_u8(0x45);                /* xor r8d, r8d */
    oj_u8(0x31);
    oj_u8(0xC0);
    /* loop: block = v[i]; skip dead; check eax against its range */
    oj_lab_here(l_walk);
    oj_u8(0x4E);                /* mov r10, [rdi + r8*8] (REX.WRX) */
    oj_u8(0x8B);
    oj_u8(0x14);
    oj_u8(0xC7);
    oj_u8(0x41);                /* cmp byte [r10 + 16], 0  (dead) */
    oj_u8(0x80);
    oj_u8(0x7A);
    oj_u8(16);
    oj_u8(0);
    oj_jcc(0x85, l_next);       /* jne: dead block cannot bite */
    /* r11 stays untouched (free since the carry_out store removal): this
     * whole probe must use r10/rcx/rdi/r8/r9 scratch only. */
    oj_u8(0x41);                /* mov ecx, [r10 + 8]  (table_index) */
    oj_u8(0x8B);
    oj_u8(0x4A);
    oj_u8(8);
    oj_u8(0xC1);                /* shl ecx, 2  (beg) */
    oj_u8(0xE1);
    oj_u8(2);
    oj_u8(0x39);                /* cmp eax, ecx */
    oj_u8(0xC8);
    oj_jcc(0x82, l_next);       /* jb: below block start */
    oj_u8(0x41);                /* mov ecx, [r10 + 8]  (table_index) */
    oj_u8(0x8B);
    oj_u8(0x4A);
    oj_u8(8);
    oj_u8(0x41);                /* add ecx, [r10 + 12]  (ti + nwords) */
    oj_u8(0x03);
    oj_u8(0x4A);
    oj_u8(12);
    oj_u8(0xC1);                /* shl ecx, 2  (end) */
    oj_u8(0xE1);
    oj_u8(2);
    oj_u8(0x39);                /* cmp eax, ecx */
    oj_u8(0xC8);
    oj_jcc(0x82, l_slow);       /* jb: inside block range -> invalidate */
    oj_lab_here(l_next);
    oj_u8(0x41);                /* inc r8d */
    oj_u8(0xFF);
    oj_u8(0xC0);
    oj_u8(0x45);                /* cmp r8d, r9d */
    oj_u8(0x39);
    oj_u8(0xC8);
    oj_jcc(0x82, l_walk);       /* jb: next entry */
    oj_lab_here(l_ck);
  }

  /* the store itself (rax was clobbered above) */
  if(is_byte)
    {
      oj_u8(0x89);              /* mov ecx, edx */
      oj_u8(0xD1);
      oj_u8(0x83);              /* xor ecx, 3 */
      oj_u8(0xF1);
      oj_u8(3);
      oj_ld_cpu(OJ_RAX, OJ_U(rd));
      oj_rip_ld64(OJ_RDI, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x88);              /* mov [rdi + rcx], al */
      oj_u8(0x04);
      oj_u8(0x0F);
    }
  else
    {
      oj_u8(0x89);              /* mov eax, edx (recompute aligned) */
      oj_u8(0xD0);
      oj_u8(0x83);              /* and eax, ~3u */
      oj_u8(0xE0);
      oj_u8(0xFC);
      oj_ld_cpu(OJ_RCX, OJ_U(rd));
      oj_rip_ld64(OJ_RDI, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x89);              /* mov [rdi + rax], ecx */
      oj_u8(0x0C);
      oj_u8(0x07);
    }

  /* writeback */
  if((cmd_ & (1 << 21)) || !(cmd_ & (1 << 24)))
    oj_st_cpu(OJ_RSI, OJ_U(rn));

  oj_charge_extra((uint32_t)(2 * NCYCLE - SCYCLE));
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);             /* jump over the slow stub */
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}

static int oj_emit_sdt_store_r(uint32_t const cmd_,
                               uint32_t const aux_,
                               uint32_t const pc_k_)
{
  uint32_t l_skip;
  uint32_t l_slow;
  uint32_t l_nh;                  /* HIRES check passed */
  uint32_t const rn = ((cmd_ >> 16) & 0xF);
  uint32_t const rd = ((cmd_ >> 12) & 0xF);
  int      const is_byte = ((cmd_ & (1 << 22)) != 0);

  if(oj_word_cond_is_nv(cmd_))
    {
      oj_tail(pc_k_ + 4);
      return 1;
    }

  l_skip = oj_word_cond(cmd_);
  l_slow = oj_lab_alloc();
  l_nh   = oj_lab_alloc();

  /* esi = base (USER[rn]); edx = tbas */
  oj_ld_cpu(OJ_RSI, OJ_U(rn));

  /* register-offset oper2 = ARM_SHIFT_NSC(USER[rm], aux&0x3F, (aux>>8)&7):
   * static shifts only (register-shifted offsets are SDT_RS, never this
   * class).  ARM_SHIFT_NSC leaves carry untouched, so the shift is
   * hand-rolled without any r10/carry write-back; eax is dead after the
   * mov to edi and the range gate re-derives it from edx. */
  {
    uint32_t const sh  = (aux_ & 0x3F);
    uint32_t const typ = ((aux_ >> 8) & 7);

    oj_ld_cpu(OJ_RAX, OJ_U(cmd_ & 0xF));    /* eax = USER[rm] */
    /* arm_cache_pack_shift sentinel encoding (verified at the packer):
     *   LSL #0  -> sh=0,  typ=0 (no shift)
     *   LSR #0  -> sh=32, typ=1 (must yield 0)
     *   ASR #0  -> sh=32, typ=2 (must sign-fill)
     *   ROR #0  -> typ=4  (RRX -- excluded at the classifier)
     *   ROR #sh -> typ=3, sh=1..31
     * x86 masks the C1 imm8 count to 5 bits, so a 32 count performs a
     * NO-shift; the sentinel forms are special-cased below and never
     * fed to C1 /2 or /4. */
    if((typ == 0) && (sh != 0))             /* LSL #sh */
      {
        oj_u8(0xC1);                        /* shl eax, sh */
        oj_u8(0xE0);
        oj_u8(sh);
      }
    else if(typ == 1)                       /* LSR: sh 1..31, or 32 */
      {
        if(sh == 32)
          {
            oj_u8(0x31);                    /* xor eax, eax */
            oj_u8(0xC0);
          }
        else
          {
            oj_u8(0xC1);                    /* shr eax, sh */
            oj_u8(0xE8);
            oj_u8(sh);
          }
      }
    else if(typ == 2)                       /* ASR: sh 1..31, or 32 */
      {
        if(sh == 32)
          {
            oj_u8(0xC1);                    /* sar eax, 31 */
            oj_u8(0xF8);
            oj_u8(31);
          }
        else
          {
            oj_u8(0xC1);                    /* sar eax, sh */
            oj_u8(0xF8);
            oj_u8(sh);
          }
      }
    else if((typ == 3) && (sh != 0))         /* ROR #sh, 1..31 */
      {
        oj_u8(0xC1);                        /* ror eax, sh */
        oj_u8(0xC8);
        oj_u8(sh);
      }
    /* typ==3&&sh==0 cannot arrive (packer maps it to typ=4); typ==4
     * (RRX) is excluded at the classifier: ARM_SHIFT_NSC's RRX case
     * writes carry_out, which this emitter does not reproduce. */
    oj_u8(0x89);                            /* mov edi, eax */
    oj_u8(0xC7);
  }

  if(!(cmd_ & (1 << 23)))
    {
      oj_u8(0xF7);              /* neg edi */
      oj_u8(0xDF);
    }

  if(cmd_ & (1 << 24))
    {
      oj_u8(0x01);              /* add esi, edi   (pre-index) */
      oj_u8(0xFE);
      oj_u8(0x89);              /* mov edx, esi   (tbas = new base) */
      oj_u8(0xF2);
    }
  else
    {
      oj_u8(0x89);              /* mov edx, esi   (tbas = old base) */
      oj_u8(0xF2);
      oj_u8(0x01);              /* add esi, edi */
      oj_u8(0xFE);
    }

  /* range gate: byte compares tbas, word compares tbas & ~3 */
  if(is_byte)
    {
      oj_rip_cmp32(OJ_RDX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);     /* jae slow */
    }
  else
    {
      oj_u8(0x89);              /* mov eax, edx */
      oj_u8(0xD0);
      oj_u8(0x83);              /* and eax, ~3u */
      oj_u8(0xE0);
      oj_u8(0xFC);
      oj_rip_cmp32(OJ_RAX, (uint64_t)(uintptr_t)&RAM_SIZE);
      oj_jcc(0x83, l_slow);     /* jae slow */
    }

  /* HIRES gate: HIRESMODE && addr >= DRAM_SIZE -> slow */
  oj_rip_testb1((uint64_t)(uintptr_t)&HIRESMODE);
  oj_jcc(0x84, l_nh);           /* jz */
  {
    uint32_t const cmp_modrm_ = is_byte ? 0x10u /* cmp edx, [rcx] */
                                        : 0x00u /* cmp eax, [rcx] */;
    oj_rip_ld64(OJ_RCX, (uint64_t)(uintptr_t)&DRAM_SIZE);
    oj_u8(0x3B);                /* cmp (edx|eax), [rcx] */
    oj_u8(cmp_modrm_);
    oj_jcc(0x83, l_slow);
  }
  oj_lab_here(l_nh);

  /* invalidation probe: the per-word coverage byte (kept exact by
   * register/kill/flush_all on the C side) replaces the whole page-list
   * walk -- one load + test decides whether the stored word overlaps
   * ANY live block.  Covered -> C kill path; data words beside code
   * stay fast.  eax must hold tbas & ~3 on entry (edx is untouched). */
  {
    uint32_t const l_ck = oj_lab_alloc();

    oj_u8(0x89);                /* mov eax, edx */
    oj_u8(0xD0);
    oj_u8(0x83);                /* and eax, ~3u */
    oj_u8(0xE0);
    oj_u8(0xFC);
    oj_cov_probe(l_ck, l_slow);
    oj_lab_here(l_ck);
  }

  /* the store itself (rax was clobbered above) */
  if(is_byte)
    {
      oj_u8(0x89);              /* mov ecx, edx */
      oj_u8(0xD1);
      oj_u8(0x83);              /* xor ecx, 3 */
      oj_u8(0xF1);
      oj_u8(3);
      oj_ld_cpu(OJ_RAX, OJ_U(rd));
      oj_rip_ld64(OJ_RDI, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x88);              /* mov [rdi + rcx], al */
      oj_u8(0x04);
      oj_u8(0x0F);
    }
  else
    {
      oj_u8(0x89);              /* mov eax, edx (recompute aligned) */
      oj_u8(0xD0);
      oj_u8(0x83);              /* and eax, ~3u */
      oj_u8(0xE0);
      oj_u8(0xFC);
      oj_ld_cpu(OJ_RCX, OJ_U(rd));
      oj_rip_ld64(OJ_RDI, (uint64_t)(uintptr_t)&DRAM);
      oj_u8(0x89);              /* mov [rdi + rax], ecx */
      oj_u8(0x0C);
      oj_u8(0x07);
    }

  /* writeback */
  if((cmd_ & (1 << 21)) || !(cmd_ & (1 << 24)))
    oj_st_cpu(OJ_RSI, OJ_U(rn));

  oj_charge_extra((uint32_t)(2 * NCYCLE - SCYCLE));
  oj_word_cond_close(l_skip);
  oj_tail(pc_k_ + 4);
  {
    uint32_t const l_done = oj_lab_alloc();
    oj_jmp(l_done);             /* jump over the slow stub */
    oj_lab_here(l_slow);
    oj_exit_cstep(pc_k_);
    oj_lab_here(l_done);
  }
  return 1;
}
/* ---------------------------------------------------------------------------
 * Block compile.
 * ------------------------------------------------------------------------- */





/* ---------------------------------------------------------------------------
 * Prologue: callee-saved setup + one-time poll-pointer materialization.
 * Mirrors the slice loop's per-slice locals snapshot (the pointers are bound
 * once at init and never re-pointed, so per-block reload is identical).
 * ------------------------------------------------------------------------- */
static void oj_prologue(void)
{
  /* Register pins (rbx/rbp/r11/r14/r15) and the per-activation r12d/r13d
   * initialisation live in the shared trampoline that CALLS blocks now;
   * a block body starts directly with its entry guards. */

  /* Partial poll hoisting: g_cdrom_restart_poll and g_madam_fsm_poll are
   * written only by the cdrom/madam subsystems, which run on this same
   * thread strictly between arm slices.  They are constant for the whole
   * duration of one block activation, so test them ONCE here at entry;
   * if either is already pending, abandon the block and let the
   * dispatcher C-step word 0 (re-executes the exact per-word tail order).
   * When the guards pass, per-word tails only need the FIQ test + the
   * r12d/r13d budget check.
   * FIQ is NOT hoisted: with THREADED_DSP the audio DSP worker thread can
   * raise CLIO_FIQPEND mid-slice, so the cheap dword test stays per-word. */
  {
    uint32_t const l_ng2      = oj_lab_alloc();
    uint32_t const l_ng3_fall = oj_lab_alloc();

    oj_u8(0x41);                /* cmp byte [r14], 0 */
    oj_u8(0x80);
    oj_u8(0x3E);
    oj_u8(0x00);
    OJ_JZ(l_ng2);
    oj_jmp(s_oj_lab_entry0);    /* cdrom restart pending: C-step */
    oj_lab_here(l_ng2);

    oj_u8(0x41);                /* cmp dword [r15], FSM_INPROCESS */
    oj_u8(0x81);
    oj_u8(0x3F);
    oj_u32((uint32_t)FSM_INPROCESS);
    OJ_JNZ(l_ng3_fall);         /* not in process: proceed */
    oj_jmp(s_oj_lab_entry0);    /* FSM in process: C-step */
    oj_lab_here(l_ng3_fall);

    /* FIQ hoist (OPERA_JIT_FIQHOIST + DSP threading off): in the
     * default config CLIO_FIQPEND cannot change mid-block (the only
     * async writer is the DSP worker thread; every other writer runs
     * between slices or inside C-stepped words whose own tails deliver
     * it), so test it ONCE here like rst/fsm.  Pending FIQ at entry
     * bails to C-step word 0, whose full tail delivers it at exactly
     * the USER[15] the per-word poll would have used. */
    if(g_jit_fiqhoist && !g_jit_dsp_threaded)
      {
        uint32_t const l_ng4 = oj_lab_alloc();
        oj_u8(0x81);              /* cmp dword [rbp], 0 */
        oj_u8(0x7D);
        oj_u8(0x00);
        oj_u32(0);
        OJ_JZ(l_ng4);
        oj_jmp(s_oj_lab_entry0);  /* FIQ pending at entry: C-step */
        oj_lab_here(l_ng4);
      }
  }

}


/* ---------------------------------------------------------------------------
 * Backend hooks (see opera_arm_jit_backend.h).
 * ------------------------------------------------------------------------- */

#include <sys/mman.h>

/* RWX arena.  Linux/macOS/BSD: anonymous mmap, no CFG, icache coherent
 * (x86), so this is the whole story. */
static uint8_t *ojb_alloc_arena(uint32_t const size_)
{
  void *p_ = mmap(NULL,(size_t)size_,
                  (PROT_READ | PROT_WRITE | PROT_EXEC),
                  (MAP_PRIVATE | MAP_ANONYMOUS),-1,0);
  return (p_ == MAP_FAILED) ? NULL : (uint8_t *)p_;
}

/* x86: instruction cache is hardware-coherent; nothing to do. */
static void ojb_after_commit(void *const base_, uint32_t const len_)
{
  (void)base_; (void)len_;
}

/* teardown: unmap the arena and trampoline page.  Tolerant of never-built
 * or partially built state (opera_arm_jit_destroy calls this always). */
static void ojb_shutdown(uint8_t *const arena_)
{
  if(arena_)
    munmap(arena_,(size_t)JIT_ARENA_SIZE);
  if(g_jit_tramp_page && (g_jit_tramp_page != MAP_FAILED))
    munmap(g_jit_tramp_page,4096);
  g_jit_tramp_page = NULL;
}
