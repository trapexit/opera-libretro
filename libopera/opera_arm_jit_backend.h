/* ---------------------------------------------------------------------------
 * opera_arm_jit_backend.h — encoding-backend contract + selection matrix
 * for the ARM60 block-JIT.
 *
 * THE SPLIT:
 *   opera_arm_jit.c        arch-neutral core (this file's includer):
 *                          block discovery, invalidation, the compile
 *                          driver, C-step fallback, slice loop, stats.
 *   opera_arm_jit_<f>.c    one encoding backend per (arch, OS):
 *     opera_arm_jit_x86_64_sysv.c   x86-64, SysV ABI   (Linux, macOS, BSD)
 *     opera_arm_jit_x86_64_win.c     x86-64, Win64 ABI (Windows, MSVC/MinGW)
 *     opera_arm_jit_aarch64_posix.c   AArch64, AAPCS64  (Linux, macOS)
 *     opera_arm_jit_aarch64_win.c   AArch64, AAPCS64  (Windows, MinGW/MSVC)
 *     opera_arm_jit_arm32_linux.c   Armv7-A, AAPCS32  (Linux; Windows-on-
 *                                  ARM32 is thumb-only — a T32 rewrite,
 *                                  never routed here)
 *
 * The backend is #included INTO the jit TU (inside opera_arm.c), so it
 * sees the file-static CPU core, handlers, classifier and poll pointers
 * exactly like the original single-file implementation did.  It defines
 * every symbol below with EXACTLY these names; the shared core references
 * them directly, and only one backend is ever present in a translation
 * unit, so no collision is possible.
 *
 * SELECTION (computed in opera_arm.c before it #includes the core):
 *   OPERA_JIT_BACKENDS      master kill-switch.  Define to 0 to compile
 *                           EVERY backend out: OPERA_ARM_JIT_ENABLED is
 *                           then not defined and the core falls back to
 *                           cache/interp exactly like an unsupported
 *                           host.  Default (undefined or 1): matrix on.
 *   OPERA_JIT_ENABLE_*      per-backend overrides.  Each defaults ON for
 *                           its (arch, OS) combination and OFF elsewhere;
 *                           define explicitly to force (e.g. a distro
 *                           that ships the jit off: -DOPERA_JIT_BACKENDS=0).
 *
 * BACKEND CONTRACT (what each backend file must define):
 *   Emitter state:  s_oj[OJ_BUF], s_oj_len, OJ_MAXFIX/OJ_MAXLAB,
 *                  label+fixup tables, s_oj_block_pc, trampoline page
 *                  globals.
 *   Emission:      oj_u8/oj_u32, oj_reset, oj_lab_alloc/here, oj_jcc,
 *                  oj_jmp, oj_jmp_abs, oj_movabs, oj_ld_cpu/oj_st_cpu,
 *                  oj_prologue, oj_tail, oj_exit_next, oj_exit_cstep,
 *                  oj_resolve, oj_abs_patch (commit-side), the
 *                  oj_word_cond / oj_word_cond_close /
 *                  oj_word_cond_is_nv conditional-guard trio (the
 *                  emitters call these around every conditional word:
 *                  open the skip label, close it on every exit path,
 *                  and route NV cond-0xF words to the tail before the
 *                  guard — on armv7 the inverse branch for NV is a
 *                  HINT, not a branch), the whole
 *                  oj_emit_{dp,mul,branch,sdt_load,sdt_literal,bdt,
 *                  sdt_store,sdt_store_r} family,
 *                  s_oj_abs_ok / s_oj_rip_ok range flags.
 *   Trampoline:    opera_arm_jit_build_trampoline() — builds the
 *                  dispatch loop + exit stubs, sets g_jit_tramp_entry
 *                  and the g_jit_tramp_x* stub addresses.
 *   Arena:         ojb_alloc_arena(size) — RWX mapping with the target
 *                  OS's executable-memory requirements (CFG
 *                  SetProcessValidCallTargets on Win64; icache flush is
 *                  a commit-time concern on ARM, see ojb_after_commit);
 *                  ojb_shutdown(arena) — teardown counterpart (unmaps the
 *                  arena AND the trampoline page; NULL/never-built
 *                  tolerant; called by opera_arm_jit_destroy).
 *   Commit:        ojb_after_commit(base, len) — instruction-cache
 *                  coherence after copying a block to the arena
 *                  (__clear_cache on ARM; no-op on x86).
 *   Exit ABI:      block entry uint32_t f(int32_t budget_remaining);
 *                  exit reason returned per JIT_X_*; cycles written to
 *                  g_jit_cycles by the shared epilogue.
 *
 * Verified on: x86-64 SysV (the original gate-proven backend);
 * aarch64 (Linux/macOS full lane parity + oracle byte-exact,
 * 2026-08-31; Windows-gnu via cross-compile with live _WIN32
 * arena/CFG hooks, 2026-09-01); arm32 Linux (ALL emitter classes
 * live, full lane parity + oracle PNG/WAV byte-exact, 2026-09-01)
 * — both default ON per the matrix below.  x86-64 Win64 and
 * MSVC-aarch64 are compile-verified only (real-host CFG validation
 * pending; MSVC stays behind OPERA_JIT_AARCH64_READY).
 * ------------------------------------------------------------------------- */

#ifndef OPERA_ARM_JIT_BACKEND_H
#define OPERA_ARM_JIT_BACKEND_H

/* ------------------------- master kill-switch ------------------------------
 * OPERA_JIT_BACKENDS=0 removes every backend (and thus the whole jit,
 * exactly like a host with no supported arch). */
#if defined(OPERA_JIT_BACKENDS) && (OPERA_JIT_BACKENDS == 0)
#  undef OPERA_JIT_HAVE_BACKEND
#  undef OPERA_JIT_BACKEND_FILE
#else

/* ------------------------- selection matrix --------------------------------
 * Defaults: each backend ON only on its (arch, OS), OFF elsewhere.
 * Explicit -DOPERA_JIT_ENABLE_<name>=0 disables even the native
 * backend; =1 on a backend whose (arch, OS) does NOT match the host
 * is an #error below (never a silent wrong-arch compile).
 *
 * x86-64 non-Windows (SysV) : opera_arm_jit_x86_64_sysv.c  (SHIPPED default)
 * x86-64 Windows (Win64)    : opera_arm_jit_x86_64_win.c
 * aarch64 non-Windows       : opera_arm_jit_aarch64_posix.c
 * aarch64 Windows           : opera_arm_jit_aarch64_win.c
 * arm32 (armv7+)            : opera_arm_jit_arm32_linux.c
 *
 * NOTE on x86-64 Windows: MinGW defines __x86_64__ AND _WIN32; MSVC
 * defines _M_X64 AND _WIN32.  Both route to the Win64 backend.  The
 * SysV backend requires x86-64 AND NOT _WIN32. */

#  if (defined(__x86_64__) || defined(__amd64__) || defined(_M_X64))
#    if defined(_WIN32) || defined(_WIN64)
#      if !defined(OPERA_JIT_ENABLE_X86_64_WIN)
#        define OPERA_JIT_ENABLE_X86_64_WIN 1
#      endif
#    else
#      if !defined(OPERA_JIT_ENABLE_X86_64_SYSV)
#        define OPERA_JIT_ENABLE_X86_64_SYSV 1
#      endif
#    endif
#  elif defined(__aarch64__) || defined(_M_ARM64)
#    if defined(_WIN32) || defined(_WIN64)
       /* Windows-aarch64: opera_arm_jit_aarch64_win.c — the AAPCS64
        * emitter is byte-identical to the POSIX twin; only the arena
        * hooks differ (VirtualAlloc + CFG, cross-compile-verified
        * against real mingw headers: VirtualAlloc/CFG live in the
        * import table, mmap absent).  Enabled by default for the
        * GNU/clang family (MinGW never emits CFG checks — the hooks
        * are inert there); MSVC release builds still need the
        * real-host CFG bring-up and stay behind
        * OPERA_JIT_AARCH64_READY. */
#      if !defined(_MSC_VER) || defined(OPERA_JIT_AARCH64_READY)
#        if !defined(OPERA_JIT_ENABLE_AARCH64_WIN)
#          define OPERA_JIT_ENABLE_AARCH64_WIN 1
#        endif
#      endif
#    else
       /* Linux/macOS aarch64: opera_arm_jit_aarch64_posix.c — complete and
        * verified to full lane parity vs the x86-64 jit + oracle
        * PNG/WAV byte-exact (qemu-aarch64, 2026-08-31).  Default ON. */
#      if !defined(OPERA_JIT_ENABLE_AARCH64_POSIX)
#        define OPERA_JIT_ENABLE_AARCH64_POSIX 1
#      endif
#    endif
#    if !defined(OPERA_JIT_ENABLE_AARCH64_POSIX) && \
        !defined(OPERA_JIT_ENABLE_AARCH64_WIN)
     /* scaffold only — cache/interp fallback (MSVC pre-READY) */
#    endif
#  elif defined(__arm__) || defined(_M_ARM)
       /* armv7+ (movw/movt) non-Windows: complete + verified to full
        * lane parity vs the x86-64 jit + oracle PNG/WAV byte-exact
        * (qemu-arm, 2026-09-01) — default ON.  Windows-on-ARM32 is
        * refuted by design (thumb-only OS vs A32 emitter — see the
        * arena hook comment in opera_arm_jit_arm32_linux.c); pre-v7 has
        * no movw/movt and stays on cache/interp. */
#    if !defined(_WIN32) && !defined(_WIN64) && \
        (defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_8A__) || \
         defined(__ARM_ARCH) && (__ARM_ARCH >= 7))
#      if !defined(OPERA_JIT_ENABLE_ARM32_LINUX)
#        define OPERA_JIT_ENABLE_ARM32_LINUX 1
#      endif
#    endif
#  endif

/* default OFF for every backend the arch chain did not enable above (runs
 * AFTER the chain so the explicit-override and native paths win) */
#  if !defined(OPERA_JIT_ENABLE_X86_64_SYSV)
#    define OPERA_JIT_ENABLE_X86_64_SYSV 0
#  endif
#  if !defined(OPERA_JIT_ENABLE_X86_64_WIN)
#    define OPERA_JIT_ENABLE_X86_64_WIN 0
#  endif
#  if !defined(OPERA_JIT_ENABLE_AARCH64_POSIX)
#    define OPERA_JIT_ENABLE_AARCH64_POSIX 0
#  endif
#  if !defined(OPERA_JIT_ENABLE_AARCH64_WIN)
#    define OPERA_JIT_ENABLE_AARCH64_WIN 0
#  endif
#  if !defined(OPERA_JIT_ENABLE_ARM32_LINUX)
#    define OPERA_JIT_ENABLE_ARM32_LINUX 0
#  endif

/* ------------------------- pick exactly one -------------------------------- */
#  if OPERA_JIT_ENABLE_X86_64_SYSV
#    define OPERA_JIT_HAVE_BACKEND 1
#    define OPERA_JIT_BACKEND_FILE "opera_arm_jit_x86_64_sysv.c"
#  elif OPERA_JIT_ENABLE_X86_64_WIN
#    define OPERA_JIT_HAVE_BACKEND 1
#    define OPERA_JIT_BACKEND_FILE "opera_arm_jit_x86_64_win.c"
#  elif OPERA_JIT_ENABLE_AARCH64_POSIX
#    define OPERA_JIT_HAVE_BACKEND 1
#    define OPERA_JIT_BACKEND_FILE "opera_arm_jit_aarch64_posix.c"
#  elif OPERA_JIT_ENABLE_AARCH64_WIN
#    define OPERA_JIT_HAVE_BACKEND 1
#    define OPERA_JIT_BACKEND_FILE "opera_arm_jit_aarch64_win.c"
#  elif OPERA_JIT_ENABLE_ARM32_LINUX
#    define OPERA_JIT_HAVE_BACKEND 1
#    define OPERA_JIT_BACKEND_FILE "opera_arm_jit_arm32_linux.c"
#  else
#    undef OPERA_JIT_HAVE_BACKEND
#    undef OPERA_JIT_BACKEND_FILE
#  endif

/* an explicitly enabled backend that does not match this (arch, OS) is a
 * build error, never a silent wrong-arch codegen compile */
#  if OPERA_JIT_ENABLE_X86_64_SYSV && \
      !((defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)) && \
        !defined(_WIN32) && !defined(_WIN64))
#    error "OPERA_JIT_ENABLE_X86_64_SYSV=1 but the host is not x86-64 SysV"
#  endif
#  if OPERA_JIT_ENABLE_X86_64_WIN && \
      !((defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)) && \
        (defined(_WIN32) || defined(_WIN64)))
#    error "OPERA_JIT_ENABLE_X86_64_WIN=1 but the host is not x86-64 Windows"
#  endif
#  if OPERA_JIT_ENABLE_AARCH64_POSIX && \
      !((defined(__aarch64__) || defined(_M_ARM64)) && \
        !defined(_WIN32) && !defined(_WIN64))
#    error "OPERA_JIT_ENABLE_AARCH64_POSIX=1 but the host is not POSIX aarch64"
#  endif
#  if OPERA_JIT_ENABLE_AARCH64_WIN && \
      !((defined(__aarch64__) || defined(_M_ARM64)) && \
        (defined(_WIN32) || defined(_WIN64)))
#    error "OPERA_JIT_ENABLE_AARCH64_WIN=1 but the host is not Windows aarch64"
#  endif
#  if OPERA_JIT_ENABLE_ARM32_LINUX && \
      !(defined(__arm__) && !defined(_WIN32) && !defined(_WIN64) && \
        (defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_8A__) || \
         defined(__ARM_ARCH) && (__ARM_ARCH >= 7)))
#    error "OPERA_JIT_ENABLE_ARM32_LINUX=1 but the host is not POSIX armv7+"
#  endif

#endif /* !OPERA_JIT_BACKENDS==0 */

#endif /* OPERA_ARM_JIT_BACKEND_H */
