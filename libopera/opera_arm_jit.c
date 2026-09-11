/* ---------------------------------------------------------------------------
 * opera_arm_jit.c — optional host block-compiling dynarec for the ARM60
 * core.  #included at the bottom of opera_arm.c when the backend-selection
 * matrix (opera_arm_jit_backend.h) provides an encoding backend for this
 * host, so it has direct access to the file-static CPU core, handlers,
 * classifier
 * and poll pointers.  Selected at runtime via engine id 3
 * (opera_arm_engine_opt_set / the opera_arm_engine core option).
 *
 * Semantics contract: blocks are straight-line runs of words from the
 * provably safe "tight" classes of the cached engine (arm_cache_decode).
 * Every word still runs the exact per-instruction tail of arm_execute_slice
 * in the exact same order: charge cycles, FIQ poll+vector, cdrom-restart
 * poll, budget check, MADAM-FSM poll.  Cond-failed words still run the whole
 * tail and charge exactly SCYCLE, matching the cached/interp engines bit for
 * bit.  Tight classes never touch g_SOFT_RESET_PENDING, so that check is
 * provably invariant and elided exactly like the cached tight tail does.
 *
 * Boundary rule: anything outside the translated set (stores, SWI, BDT, SDS,
 * UND, SPEC, MSR, any r15 operand/target, out-of-range or slow loads, or a
 * PC outside the DRAM/ROM windows) exits the block BEFORE the word and the
 * dispatcher executes exactly that word through the verbatim cached-engine
 * handlers (arm_jit_cstep) — including its full/tight tail — then
 * re-dispatches.  No host register state is ever live across a
 * side-effecting instruction.
 *
 * ENCODING BACKENDS: this file is the arch-neutral core (block discovery,
 * invalidation, the compile driver, C-step fallback, slice loop, stats).
 * The machine-code layer lives in a per-(arch,OS) backend file selected in
 * opera_arm.c and #included below — see opera_arm_jit_backend.h for the
 * contract every backend implements (emitter state, oj_* helpers,
 * oj_emit_* per-word emitters, trampoline, arena allocator, exit ABI).
 * Master kill-switch: OPERA_JIT_BACKENDS=0 compiles every backend out.
 * ------------------------------------------------------------------------- */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------------------------- exit reasons -------------------------------- */
enum
{
  JIT_X_NEXT = 0,   /* tails all clean; continue dispatch at CPU.USER[15] */
  JIT_X_FIQ,        /* arm_fiq_vector ran; run rst/budget/fsm, then re-go */
  JIT_X_RST,        /* cdrom-restart poll fired after the word -> break   */
  JIT_X_BUDGET,     /* budget exhausted after the word -> break           */
  JIT_X_FSM,        /* MADAM FSM entered FSM_INPROCESS -> break           */
  JIT_X_CSTEP       /* execute the word at CPU.USER[15] via the C path    */
};

typedef uint32_t (*jit_entry_t)(int32_t budget_remaining);

/* ---------------------------- block records ------------------------------- */
typedef struct jit_block_s jit_block_t;
struct jit_block_s
{
  void        *code;         /* arena entry point                     */
  uint32_t     table_index;  /* start word index in g_jit_table       */
  uint32_t     nwords;       /* guest words covered                   */
  uint8_t      dead;         /* invalidated by a store to its page    */
  jit_block_t *next_all;     /* intrusive list of every block         */
};

#define JIT_ARENA_SIZE  (32u << 20)
#define JIT_MAX_WORDS   64u
#define JIT_PAGE_SHIFT  12u
#define JIT_PAGE_SIZE   (1u << JIT_PAGE_SHIFT)

typedef struct
{
  jit_block_t **v;
  uint32_t      n;
  uint32_t      cap;
} jit_page_list_t;

static uint8_t    *jit_arena;
static uint8_t    *jit_arena_cur;
static jit_block_t **g_jit_table;
static uint32_t    g_jit_table_entries;
static uint32_t    g_jit_ram_words;
static jit_page_list_t *g_jit_pages;
static uint32_t    g_jit_npages;
/* nonempty count per page, mirrored from pl->n so the hot write path in
 * opera_mem.h can skip the touch call entirely on empty pages (the
 * overwhelmingly common case: code occupies a sliver of DRAM).  Kept in
 * exact lockstep with the list mutations: register/kill/flush_all. */
static uint8_t    *g_jit_page_hot;
/* per-word covering-block counts: exactly how many live blocks cover
 * each guest word (inc at registration, dec at kill, zero on flush).
 * Exact, unlike the earlier conservative bitmap: a zero count is a
 * hard guarantee no live block covers the word, so the write hooks can
 * skip the touch walk for every data word -- even ones sharing a page
 * with live code.  1KB per 4K page. */
static uint8_t    *g_jit_word_cov;
static jit_block_t *g_jit_all;
static int32_t     g_jit_cycles;   /* written by block epilogues (movabs) */
static int         g_jit_ready;

int opera_jit_hook_active = 0;     /* guards the opera_mem write hooks */

/* env-gated runtime knobs (default ON, trace-proven exact; opt-out via
 * OPERA_JIT_BUDGET_BATCH=0 / OPERA_JIT_FIQHOIST=0): see jit.md */
static int      g_jit_fiqhoist;    /* OPERA_JIT_FIQHOIST=1: entry-only FIQ */
static int      g_jit_budbatch;  /* OPERA_JIT_BUDGET_BATCH: elide the
                                   per-word budget check; stops move to
                                   block boundaries (timing-contract
                                   change, user-gated experiment) */
static int      g_jit_dsp_threaded = -1;  /* runtime DSP-thread state snapshot */

/* runtime DSP-threading state accessor (defined in opera_lr_dsp_*.ic,
 * included via libretro.c): true = worker thread can raise FIQPEND
 * mid-slice, so per-word FIQ polls are mandatory */
extern bool opera_lr_dsp_threaded_active(void);
void opera_arm_jit_flush_all(void);

/* re-snapshot the DSP threading state; flush resident blocks when it
 * moves (the FIQ-poll emission shape of new blocks depends on it).
 * Called from the core-option toggle site. */
void opera_arm_jit_dsp_thread_refresh(void);
void opera_arm_jit_dsp_thread_refresh(void)
{
  int const now_ = opera_lr_dsp_threaded_active();
  if(now_ != g_jit_dsp_threaded)
    {
      g_jit_dsp_threaded = now_;
      opera_arm_jit_flush_all();
    }
}
/* OPERA_JIT_STATS: reserved re-instrumentation hook (build with
 * -DOPERA_JIT_STATS to reintroduce block-economics counters/dumps;
 * no gated content ships in release builds). */
#ifdef OPERA_JIT_STATS
#endif

/* ---------------- encoding backend (per-arch, per-OS) ---------------------
 * Selected in opera_arm.c: exactly one of opera_arm_jit_x86_64_sysv.c (SysV),
 * opera_arm_jit_x86_64_win.c (Win64), opera_arm_jit_aarch64_posix.c (Linux/
 * macOS), opera_arm_jit_aarch64_win.c (Windows), opera_arm_jit_arm32_linux.c
 * — or none (cache/interp fallback).  The backend
 * defines the emitter state and every oj_xxx and ojb_xxx symbol that
 * the shared core below references. */
#include "opera_arm_jit_backend.h"
#if defined(OPERA_JIT_HAVE_BACKEND)
#include OPERA_JIT_BACKEND_FILE
#endif

/* guest pc (aligned) -> block-table index, or -1 outside the DRAM window.
 * The jit compiles DRAM code only: the tramp gate (which mirrors this)
 * sends ROM pcs to the interpreter every time, so ROM never gets table
 * slots, blocks, or page registrations.  (arm_jit_fetch_word_at still
 * handles ROM: the C-step path executes out single words there.) */
static int32_t arm_jit_pc_index(uint32_t const pca_)
{
  if(pca_ < RAM_SIZE)
    return (int32_t)(pca_ >> 2);

  return -1;
}

/* translate table index -> guest pc (for USER[15] constants at compile) */
static uint32_t arm_jit_index_pc(uint32_t const table_idx_)
{
  return (table_idx_ << 2);
}



/* guest pc (aligned) -> raw guest word (DRAM or resident ROM bank) */
static uint32_t arm_jit_fetch_word_at(uint32_t const pca_)
{
  uint32_t w;

  if(pca_ < RAM_SIZE)
    return *(const uint32_t *)&DRAM[pca_];

  w = (pca_ ^ 0x03000000);
  if(!(w & ~ROM1_SIZE_MASK))
    return *(const uint32_t *)&ROM[w];

  w = (pca_ ^ 0x06000000);
  if(!(w & ~ROM1_SIZE_MASK))
    return *(const uint32_t *)&ROM[w];

  return 0xBADACCE5;      /* mreadw parity: unaligned/out-of-window pc */
}

static void arm_jit_kill_block(jit_block_t *const blk_);   /* fwd: OOM path */

static void arm_jit_page_register(jit_block_t *const blk_,
                                  uint32_t      const pca_)
{
  uint32_t page_first;
  uint32_t page_last;
  uint32_t page;

  if(pca_ >= RAM_SIZE)
    return;                       /* ROM blocks: flushed wholesale on swap */

  page_first = (pca_ >> JIT_PAGE_SHIFT);
  page_last  = ((pca_ + (blk_->nwords << 2) - 1) >> JIT_PAGE_SHIFT);
  if(page_last >= g_jit_npages)
    page_last = (g_jit_npages - 1);

  for(page = page_first; page <= page_last; page++)
    {
      jit_page_list_t *const pl = &g_jit_pages[page];

      if(pl->n == pl->cap)
        {
          uint32_t const ncap = (pl->cap ? (pl->cap * 2) : 8);
          jit_block_t **nv_ = (jit_block_t **)realloc(pl->v,ncap * sizeof(*pl->v));
          if(!nv_)
            {
              /* OOM mid-registration: a live-but-unregistered block is
               * the missed-invalidation class (its words carry cov
               * counts and the page-hot gate stays off, so SMC writes
               * would never kill it) — kill it instead and let the
               * C-step path re-execute the words. */
              arm_jit_kill_block(blk_);
              return;
            }
          pl->v   = nv_;
          pl->cap = ncap;
        }

      pl->v[pl->n++] = blk_;
      g_jit_page_hot[page] = 1;
    }

  /* count coverage for every word the block spans */
  {
    uint32_t w;
    uint32_t const wfirst = (pca_ >> 2);
    uint32_t       wlast  = (wfirst + blk_->nwords - 1);
    if(wlast >= g_jit_ram_words)
      wlast = (g_jit_ram_words - 1);   /* DRAM-window clamp */
    for(w = wfirst; w <= wlast; w++)
      g_jit_word_cov[w]++;
  }
}

static void arm_jit_kill_block(jit_block_t *const blk_)
{
  uint32_t const pca = arm_jit_index_pc(blk_->table_index);

  if(blk_->dead)
    return;

  blk_->dead = 1;
  if(g_jit_table[blk_->table_index] == blk_)
    g_jit_table[blk_->table_index] = NULL;

  /* mirror register's RAM window + clamp exactly: kills only ever made
   * counts for pca < RAM_SIZE, so only those are un-counted.  ROM
   * deaths are wholesale (flush_all zeroes cov). */
  if(pca < RAM_SIZE)
    {
      uint32_t w;
      uint32_t const wfirst = (pca >> 2);
      uint32_t       wlast  = (wfirst + blk_->nwords - 1);
      if(wlast >= g_jit_ram_words)
        wlast = (g_jit_ram_words - 1);
      for(w = wfirst; w <= wlast; w++)
        g_jit_word_cov[w]--;
    }
}

void opera_arm_jit_flush_all(void);




static jit_block_t *arm_jit_compile(uint32_t const table_idx_)
{
  uint32_t     pc_k;
  uint32_t     n = 0;
  int          ended = 0;
  int          use_cstep_exit = 0;
  jit_block_t *blk;
  /* DRAM-only table: every table index is inside the single DRAM fetch
   * window, so the mid-block window guard degenerates to the word-count
   * boundary of the window itself */
  uint32_t const win_hi = g_jit_ram_words;

  n                 = 0;
  ended             = 0;
  use_cstep_exit    = 0;
  oj_reset();
  s_oj_block_pc   = arm_jit_index_pc(table_idx_);
  s_oj_lab_entry0 = oj_lab_alloc();
  oj_prologue();

  pc_k = s_oj_block_pc;

  for(;;)
    {
      uint32_t          word;
      uint32_t          cls;
      arm_cache_entry_t e;
      int               st;

      if(n >= JIT_MAX_WORDS)
        break;

      /* never cross a fetch-window boundary mid-block */
      if((table_idx_ + n) >= win_hi)
        break;

      /* headroom guard: a maximal word is ~600 bytes incl. fix-ups */
      if((s_oj_len > (OJ_BUF - 4096)) || (s_oj_nfix > (OJ_MAXFIX - 32)) ||
         (s_oj_nlab > (OJ_MAXLAB - 32)))
        break;

      s_oj_word_off[n] = s_oj_len;


      word = arm_jit_fetch_word_at(pc_k);
      memset(&e,0,sizeof(e));
      arm_cache_decode(&e,word);
      cls = e.cls;



      st = 0;
      switch(cls)
        {
        case ARM_CLS_TIGHT_DP_IMM:
        case ARM_CLS_TIGHT_DP_RI:
        case ARM_CLS_TIGHT_DP_RS:
        case ARM_CLS_TIGHT_DP_PC:
        case ARM_CLS_TIGHT_DP_PCREL:
        case ARM_CLS_TIGHT_DP_RMPC:
          st = oj_emit_dp(word,e.aux,pc_k,cls);
          break;
        case ARM_CLS_TIGHT_MUL:
          st = oj_emit_mul(word,pc_k);
          break;
        case ARM_CLS_TIGHT_SDT_LDI:
        case ARM_CLS_TIGHT_SDT_LDR:
          st = oj_emit_sdt_load(word,e.aux,pc_k,cls);
          break;
        case ARM_CLS_TIGHT_SDT_STI:
          st = oj_emit_sdt_store(word,pc_k);
          break;
        case ARM_CLS_TIGHT_SDT_STR:
          st = oj_emit_sdt_store_r(word,e.aux,pc_k);
          break;
        case ARM_CLS_TIGHT_BRANCH:
          st = oj_emit_branch(word,pc_k);
          break;
        case ARM_CLS_SDT_IMM:
          st = oj_emit_sdt_literal(word,pc_k);
          break;
        case ARM_CLS_SDT_IMM_NP:
          /* no-pc SDT immediate (P=0,W=1 writeback shape, rn/rd != 15).
           * Route to the generic load/store emitters unless it is a
           * T-suffix word (bit5: LDRT/STRT user-bank transfer needs
           * readusr/loadusr bank access -- C-step those). */
          if((word & (1u << 5)) == 0)
            st = ((word & (1u << 20))
                    ? oj_emit_sdt_load(word,0,pc_k,ARM_CLS_SDT_IMM_NP)
                    : oj_emit_sdt_store(word,pc_k));
          else
            st = 0;
          break;
        case ARM_CLS_SDT_RI_NP:
          /* no-pc SDT register-offset (P=0,W=1, rm != 15 shapes decoded
           * into the NP class).  The load emitter re-derives the shift
           * from the raw word for non-tight classes.  T-suffix C-steps. */
          if((word & (1u << 5)) == 0)
            st = ((word & (1u << 20))
                    ? oj_emit_sdt_load(word,e.aux,pc_k,ARM_CLS_SDT_RI_NP)
                    : oj_emit_sdt_store_r(word,e.aux,pc_k));
          else
            st = 0;
          break;
        case ARM_CLS_BDT:
          st = oj_emit_bdt(word,pc_k);
          break;
        default:
          st = 0;                 /* not translatable: C-step boundary */
          break;
        }

      if(st == 0)
        {
          /* end before this word, handled in C by the dispatcher */
          use_cstep_exit = 1;
          break;
        }

      n++;

      if(st == 2)                 /* branch terminator */
        {
          ended = 1;
          break;
        }

      pc_k += 4;
    }

  if(!n)
    {
      /* First word untranslatable: install a SENTINEL block so the asm
       * dispatch never re-enters the C compile path for this entry.
       * The sentinel body is the oj_exit_cstep shape (USER[15] =
       * block pc, jump to the shared X_CSTEP stub): hitting it costs
       * one asm call + the C-step -- no slow_compile epilog round trip
       * and no repeated compile attempt.  nwords = 0 keeps the cov
       * loops empty and keeps it off the page lists (nothing to
       * invalidate: no emitted guest state).  Returns NULL once; the
       * caller C-steps word 0 and the table slot is occupied after. */
      oj_reset();
      s_oj_block_pc   = arm_jit_index_pc(table_idx_);
      s_oj_lab_entry0 = oj_lab_alloc();
      oj_prologue();
      oj_lab_here(s_oj_lab_entry0);   /* bind the prologue guard target:
        * no guest word follows, so the guards fall straight into the
        * same exit */
      oj_exit_cstep(s_oj_block_pc);
      if(oj_resolve())
        {
          return NULL;
        }
      if((uint32_t)(jit_arena + JIT_ARENA_SIZE - jit_arena_cur) <=
         (s_oj_len + 64u))
        {
          opera_arm_jit_flush_all();   /* recycle like the normal path:
                                        * without this, arena-exhaustion
                                        * at a sentinel re-enters the
                                        * compile round-trip forever */
          return NULL;
        }
      {
        jit_block_t *const s_ = (jit_block_t *)calloc(1,sizeof(*s_));

        if(!s_)
          return NULL;
        memcpy(jit_arena_cur,s_oj,s_oj_len);
        oj_abs_patch(jit_arena_cur);
        ojb_after_commit(jit_arena_cur,s_oj_len);
        s_->code        = jit_arena_cur;
        s_->table_index = table_idx_;
        s_->nwords      = 0;
        s_->dead        = 0;
        jit_arena_cur  += ((s_oj_len + 15u) & ~15u);
        s_->next_all    = g_jit_all;
        g_jit_all        = s_;
        g_jit_table[table_idx_] = s_;
      }
      return NULL;             /* first word untranslatable: all-C */
    }

  if(!ended)
    {
      if(use_cstep_exit)
        {
          oj_exit_cstep(pc_k);    /* word at pc_k not yet executed */
        }
      else
        oj_exit_next(pc_k);       /* pc_k already points past the last word */
    }

  /* ------------------------- prologue guard trap -------------------------
   * A restart / FSM event was already pending at block entry -- nothing
   * has executed; C-step word 0 for exact timing.  All other exit paths
   * (X_NEXT / X_FIQ / X_BUDGET / X_FSM / X_RST / X_CSTEP) jump straight
   * into the shared trampoline's stubs from oj_exit_next/oj_exit_cstep
   * and the per-word tails. */
  oj_lab_here(s_oj_lab_entry0);
  oj_exit_cstep(s_oj_block_pc);

  if(oj_resolve())
    {
      return NULL;
    }

  /* arena space (flush-all recycling on exhaustion) */
  if((uint32_t)(jit_arena + JIT_ARENA_SIZE - jit_arena_cur) <= (s_oj_len + 64u))
    {
      opera_arm_jit_flush_all();
      if((uint32_t)(jit_arena + JIT_ARENA_SIZE - jit_arena_cur) <= (s_oj_len + 64u))
        {
          return NULL;
        }
    }

  blk = (jit_block_t *)calloc(1,sizeof(*blk));
  if(!blk)
    return NULL;

  memcpy(jit_arena_cur,s_oj,s_oj_len);
  oj_abs_patch(jit_arena_cur);
  ojb_after_commit(jit_arena_cur,s_oj_len);

  blk->code        = jit_arena_cur;
  blk->table_index = table_idx_;
  blk->nwords      = n;
  blk->dead        = 0;

  jit_arena_cur   += ((s_oj_len + 15u) & ~15u);

  blk->next_all    = g_jit_all;
  g_jit_all        = blk;

  g_jit_table[table_idx_] = blk;
  arm_jit_page_register(blk,arm_jit_index_pc(table_idx_));

  return blk;
}
/* ---------------------------------------------------------------------------
 * Invalidation.
 * opera_arm_jit_touch: word-range kill on guest-DRAM writes; called from
 * the opera_mem write inlines for every (addr < RAM_SIZE) store.  A block
 * is invalidated only when its [table_index*4, table_index*4 + nwords*4)
 * byte range overlaps the stored word -- blocks beside the store survive,
 * which keeps store-heavy store-adjacent-to-code patterns from churning
 * the table.  Dead blocks are pruned from the list as it is walked.
 * opera_arm_jit_flush_all: wholesale flush for ROM bank swaps, save-state
 * DRAM restores, the low-boot-word seeding, and arena recycling.
 * NOTE: multi-page blocks stay listed on their other pages after a range
 * kill; those lists treat the dead flag as "not there" (the probe skips
 * dead entries and flush_all resets every list simultaneously).
 * ------------------------------------------------------------------------- */
/* fast pre-check for the write hooks: nonzero iff any live block is
 * registered on the page containing addr_.  Inlined at the opera_mem
 * write sites; keeps the (cold) touch call off every DRAM store. */
int opera_arm_jit_page_hot(uint32_t const addr_)
{
  uint32_t const page = (addr_ >> JIT_PAGE_SHIFT);
  uint32_t const wi   = (addr_ >> 2);
  if(page >= g_jit_npages || !g_jit_page_hot[page])
    return 0;
  if(wi >= g_jit_ram_words)
    return 0;
  return g_jit_word_cov[wi] != 0;
}

void opera_arm_jit_touch(uint32_t const addr_)
{
  uint32_t         page;
  uint32_t         w0;
  jit_page_list_t *pl;
  uint32_t         i;

  if(!g_jit_ready)
    return;

  page = (addr_ >> JIT_PAGE_SHIFT);
  if(page >= g_jit_npages)
    return;

  if(!g_jit_page_hot[page])
    return;                     /* empty page: nothing to kill */
  pl = &g_jit_pages[page];
  w0 = (addr_ & ~3u);
  i  = 0;

  while(i < pl->n)
    {
      jit_block_t *const b   = pl->v[i];
      uint32_t      const b0 = (b->table_index << 2);
      uint32_t      const b1 = (b0 + (b->nwords << 2));

      if(b->dead || !(w0 < b1 && (w0 + 4u) > b0))
        {
          if(b->dead)             /* prune the corpse, inspect the swap-in */
            {
              pl->v[i] = pl->v[--(pl->n)];
              continue;
            }
          i++;
        }
      else
        {
          arm_jit_kill_block(b);
          pl->v[i] = pl->v[--(pl->n)];
        }
    }
}

void opera_arm_jit_flush_all(void)
{
  jit_block_t *b;
  uint32_t     i;

  if(!g_jit_ready)
    return;

  while(g_jit_all)
    {
      b         = g_jit_all;
      g_jit_all = b->next_all;
      free(b);
    }

  memset(g_jit_table,0,(size_t)g_jit_table_entries * sizeof(*g_jit_table));

  memset(g_jit_page_hot,0,(size_t)g_jit_npages);
  memset(g_jit_word_cov,0,(size_t)g_jit_ram_words);
  for(i = 0; i < g_jit_npages; i++)
    g_jit_pages[i].n = 0;

  jit_arena_cur = jit_arena;
}
/* full teardown for opera_arm_destroy: release every jit allocation so a
 * frontend init/deinit cycle does not retain the ~32MB arena + tables.
 * Safe to call before/without startup (all statics zero-init): the write
 * hooks shut off first, the backend frees the arena/trampoline mappings
 * (tolerating unbuilt state), then the C-side structures.  A later
 * engine==3 slice re-runs opera_arm_jit_startup from scratch. */
void opera_arm_jit_destroy(void);
void
opera_arm_jit_destroy(void)
{
  jit_block_t *b_;
  uint32_t     i_;

  opera_jit_hook_active = 0;      /* quiesce the opera_mem write hooks */

  while(g_jit_all)
    {
      b_        = g_jit_all;
      g_jit_all = b_->next_all;
      free(b_);
    }

  ojb_shutdown(jit_arena);

  free(g_jit_table);
  if(g_jit_pages)
    {
      for(i_ = 0; i_ < g_jit_npages; i_++)
        free(g_jit_pages[i_].v);
      free(g_jit_pages);
    }
  free(g_jit_page_hot);
  free(g_jit_word_cov);

  jit_arena           = NULL;
  jit_arena_cur       = NULL;
  g_jit_table         = NULL;
  g_jit_pages         = NULL;
  g_jit_page_hot      = NULL;
  g_jit_word_cov      = NULL;
  g_jit_table_entries = 0;
  g_jit_ram_words     = 0;
  g_jit_npages        = 0;
  g_jit_cycles        = 0;
  g_jit_ready         = 0;
  g_jit_dsp_threaded  = -1;
}


static int opera_arm_jit_startup(void)
{
  if(g_jit_ready)
    return 1;

  {
    /* one-shot env parse (before any compile can happen) */
    {
      /* default ON: trace-proven exact in the default config
       * (zero mid-block FIQPEND flips; the only async writer is
       * the DSP worker thread) and +0.98% measured.  The
       * emission keys on the runtime threading snapshot and the
       * toggle site flushes resident blocks, so a user turning
       * opera_dsp_threaded on recompiles with per-word polls.
       * OPERA_JIT_FIQHOIST=0 restores the per-word shape. */
      {
        /* default ON (user ruling 2026-08-30): stop words move to
         * block boundaries (median 3.2 words, p95 12, block-bounded);
         * audio transient placement drifts sub-sample.  Measured
         * 107.5% of cache with PNGs byte-identical.  OPT-OUT:
         * OPERA_JIT_BUDGET_BATCH=0 restores the exact-stop shape. */
        char const *bb_ = getenv("OPERA_JIT_BUDGET_BATCH");
        g_jit_budbatch = ((bb_ == NULL) || (*bb_ != '0'));
      }
      char const *fh_ = getenv("OPERA_JIT_FIQHOIST");
      g_jit_fiqhoist = ((fh_ == NULL) || (*fh_ != '0'));
    }
    g_jit_dsp_threaded = opera_lr_dsp_threaded_active();
  }

  if(opera_arm_jit_build_trampoline())
    return 0;

  jit_arena = ojb_alloc_arena(JIT_ARENA_SIZE);
  if(!jit_arena)
    return 0;

  /* rel32 reachability of the trampoline page from every possible
   * in-arena jmp site; both mappings live for the whole process
   * lifetime, so one check covers every future block.  A jmp rel32
   * sits at arena offset p with next-ip (p+5); its displacement to a
   * target t is t-(arena+p+5).  Most negative: t=tmin from p = arena
   * end - 5 (i.e. d0 = tmin-(arena+ARENA-5)); most positive:
   * t=tmax from p = 0 (d1 = tmax-arena).  The check must bound BOTH
   * extremes of BOTH targets — checking only the least-extreme
   * corner lets a mid-band placement pass and the backend patcher
   * then silently skips the overflowed patch. */
  {
    uint64_t const tmin = (uint64_t)g_jit_tramp_entry;
    uint64_t const tmax = (uint64_t)(g_jit_tramp_page + 4096u);
    int64_t  const d0   = (int64_t)(tmin -
                        (uint64_t)(jit_arena + JIT_ARENA_SIZE - 5u));
    int64_t  const d1   = (int64_t)(tmax - (uint64_t)jit_arena);

    s_oj_abs_ok = ((d0 >= -0x80000000LL) && (d0 <= 0x7FFFFFFFLL) &&
                   (d1 >= -0x80000000LL) && (d1 <= 0x7FFFFFFFLL));
  }

  /* OS-locality note (Win64): Linux mmap places the arena near the
   * .so, so the +-2GB rel32 checks usually PASS and blocks use the
   * short jump forms.  Windows VirtualAlloc has no such bias: the
   * arena can land far from the module, both checks legitimately
   * FAIL, and every trampoline jump falls back to movabs+jmp rax
   * while every data ref falls back to movabs+deref — correct but
   * measurably denser/slower.  This is the known Win64 perf cliff;
   * the fix (allocating the arena near the module, e.g. hint-based
   * VirtualAlloc) is a Windows-host bring-up task.  The stats build
   * prints the verdict so the cliff is not silent. */

  /* RIP-relative data reachability: the .bss statics the emitters
   * reference (DRAM, RAM_SIZE and friends) must sit within +-2GB of
   * BOTH arena ends; one check covers the process lifetime.  When it
   * fails, the emitters silently keep the movabs forms. */
  {
    uint64_t const g[] = {(uint64_t)(uintptr_t)&DRAM,
                          (uint64_t)(uintptr_t)&RAM_SIZE};
    uint32_t i_;
    s_oj_rip_ok = 1;
    for(i_ = 0; i_ < (sizeof(g) / sizeof(g[0])); i_++)
      {
        int64_t const e0 = (int64_t)(g[i_] - (uint64_t)jit_arena);
        int64_t const e1 = (int64_t)(g[i_] -
                              (uint64_t)(jit_arena + JIT_ARENA_SIZE - 1u));
        if((e0 < -0x80000000LL) || (e0 > 0x7FFFFFFFLL) ||
           (e1 < -0x80000000LL) || (e1 > 0x7FFFFFFFLL))
          s_oj_rip_ok = 0;
      }
  }

  g_jit_ram_words = (RAM_SIZE >> 2);
  g_jit_table_entries = g_jit_ram_words;
  g_jit_table = (jit_block_t **)calloc(g_jit_table_entries,
                                       sizeof(*g_jit_table));

  g_jit_npages = (RAM_SIZE >> JIT_PAGE_SHIFT);
  g_jit_pages  = (jit_page_list_t *)calloc(g_jit_npages,
                                           sizeof(*g_jit_pages));
  g_jit_page_hot = (uint8_t *)calloc(g_jit_npages,1);
  g_jit_word_cov = (uint8_t *)calloc(g_jit_ram_words,1);

  if(!g_jit_table || !g_jit_pages || !g_jit_page_hot ||
     !g_jit_word_cov)
    return 0;

  jit_arena_cur         = jit_arena;
  g_jit_all             = NULL;
  g_jit_ready           = 1;
  opera_jit_hook_active = 1;

  return 1;
}
/* ---------------------------------------------------------------------------
 * Single-word execution through the verbatim cached handlers, with the
 * arm_execute_slice per-word tail replicated exactly (full or tight as the
 * word's decoded class dictates; slow tight loads drop to the full tail
 * exactly like the slice loop's goto).  Used for C-step block boundaries.
 * Returns non-zero when the slice must break.
 * ------------------------------------------------------------------------- */
static int arm_jit_cstep(int32_t  *const total_io_,
                         int32_t   const budget_,
                         uint32_t *const seq_)
{
  int               cyc;
  uint32_t          word;
  int               is_tight = 0;
  arm_cache_entry_t e;
  uint32_t const    pc  = CPU.USER[15];
  uint32_t const    pca = (pc & ~3u);

  word = arm_jit_fetch_word_at(pca);


  CPU.USER[15] = (pc + 4);

  memset(&e,0,sizeof(e));
  arm_cache_decode(&e,word);


  cyc = -SCYCLE;
  switch(e.cls)
    {
    case ARM_CLS_DP_IMM:     arm_h_dp_imm(word,e.aux,&cyc);     break;
    case ARM_CLS_DP_RS:      arm_h_dp_rs(word,e.aux,&cyc);      break;
    case ARM_CLS_DP_RI:      arm_h_dp_ri(word,e.aux,&cyc);      break;
    case ARM_CLS_DP_IMM_NP:  arm_h_dp_imm_np(word,e.aux,&cyc);  break;
    case ARM_CLS_DP_RS_NP:   arm_h_dp_rs_np(word,e.aux,&cyc);   break;
    case ARM_CLS_DP_RI_NP:   arm_h_dp_ri_np(word,e.aux,&cyc);   break;
    case ARM_CLS_SDT_IMM:    arm_h_sdt_imm(word,e.aux,&cyc);    break;
    case ARM_CLS_SDT_IMM_NP: arm_h_sdt_imm_np(word,e.aux,&cyc); break;
    case ARM_CLS_SDT_RI:     arm_h_sdt_ri(word,e.aux,&cyc);     break;
    case ARM_CLS_SDT_RI_NP:  arm_h_sdt_ri_np(word,e.aux,&cyc);  break;
    case ARM_CLS_SDT_RS:     arm_h_sdt_rs(word,e.aux,&cyc);     break;
    case ARM_CLS_MUL:        arm_h_mul(word,e.aux,&cyc);        break;
    case ARM_CLS_BRANCH:     arm_h_branch(word,e.aux,&cyc);     break;
    case ARM_CLS_SPEC:       arm_h_spec(word,e.aux,&cyc);       break;
    case ARM_CLS_UND:        arm_h_und(word,e.aux,&cyc);        break;
    case ARM_CLS_SDS:       arm_h_sds(word,e.aux,&cyc);        break;
    case ARM_CLS_BDT:        arm_h_bdt(word,e.aux,&cyc);        break;
    case ARM_CLS_SWI:        arm_h_swi(word,e.aux,&cyc);        break;
    /* pc-writing DP: full handler (ICYCLE+NCYCLE charge); the mode-
     * restore family and rm==15 are excluded at the classifier */
    case ARM_CLS_TIGHT_DP_PC:
      if(word & (1u << 25))
        arm_h_dp_imm(word,e.aux,&cyc);
      else
        arm_h_dp_ri(word,e.aux,&cyc);
      break;
    /* pc-relative op1: pipeline bump around the np handlers */
    case ARM_CLS_TIGHT_DP_PCREL:
      CPU.USER[15] += 4;
      if(word & (1u << 25))
        arm_h_dp_imm_np(word,e.aux,&cyc);
      else
        arm_h_dp_ri_np(word,e.aux,&cyc);
      CPU.USER[15] -= 4;
      is_tight = 1;
      break;
    case ARM_CLS_TIGHT_DP_RMPC:
      /* op2 = pipeline pc: bump USER[15] around the full handler's
       * read (it reads post-bump), then restore -- the tight tail must
       * observe USER[15] = pc+4 exactly like every other tight word */
      CPU.USER[15] += 4;
      arm_h_dp_ri_np(word,e.aux,&cyc);
      CPU.USER[15] -= 4;
      is_tight = 1;
      break;
    case ARM_CLS_TIGHT_DP_IMM:    arm_h_dp_imm_np(word,e.aux,&cyc);
      is_tight = 1; break;
    case ARM_CLS_TIGHT_DP_RS:     arm_h_dp_rs_np(word,e.aux,&cyc);
      is_tight = 1; break;
    case ARM_CLS_TIGHT_DP_RI:     arm_h_dp_ri_np(word,e.aux,&cyc);
      is_tight = 1; break;
    case ARM_CLS_TIGHT_MUL:       arm_h_mul_np(word,e.aux,&cyc);
      is_tight = 1; break;
    case ARM_CLS_TIGHT_BRANCH:    arm_h_branch(word,e.aux,&cyc);
      is_tight = 1; break;
    case ARM_CLS_TIGHT_SDT_LDI:
      {
        int slow = 0;
        arm_h_sdt_ldi_t(word,e.aux,&cyc,&slow);
        is_tight = !slow;
      }
      break;
    case ARM_CLS_TIGHT_SDT_LDR:
      {
        int slow = 0;
        arm_h_sdt_ldr_t(word,e.aux,&cyc,&slow);
        is_tight = !slow;
      }
      break;
    case ARM_CLS_TIGHT_SDT_STI:
      {
        int slow = 0;
        arm_h_sdt_sti_t(word,e.aux,&cyc,&slow);
        is_tight = !slow;
      }
      break;
    case ARM_CLS_TIGHT_SDT_STR:
      {
        int slow = 0;
        arm_h_sdt_str_t(word,e.aux,&cyc,&slow);
        is_tight = !slow;
      }
      break;
    }

  *total_io_ -= cyc;              /* total -= cyc (cyc is negative) */

  if(!is_tight)
    {
      /* full tail, in slice order */
      if(g_SOFT_RESET_PENDING)
        g_SOFT_RESET_PENDING = false;
      else if(!ISF && arm_fiq_pending_())
        arm_fiq_vector();

      if(arm_cdrom_restart_())
        return 1;

      if(*total_io_ >= budget_)
        return 1;

      if(arm_madam_inprocess_())
        return 1;

      if((e.cls >= ARM_CLS_SDT_IMM) && (g_arm_rom_seq != *seq_))
        *seq_ = g_arm_rom_seq;

      return 0;
    }

  /* tight tail, in slice order */
  if(!ISF && arm_fiq_pending_())
    arm_fiq_vector();

  if(arm_cdrom_restart_())
    return 1;

  if(*total_io_ >= budget_)
    return 1;

  if(arm_madam_inprocess_())
    return 1;

  return 0;
}


/* ---------------------------------------------------------------------------
 * Dispatcher — structurally identical to arm_execute_slice: per-iteration pc
 * classification, JIT block call or verbatim interp for out-of-window PCs,
 * and the same per-word poll/budget/ROM-seq handling on the boundaries the
 * blocks cannot see through.  The budget/fsm/rst checks on block exits
 * mirror exactly the checks the in-block tails already performed, in order.
 * ------------------------------------------------------------------------- */
static int32_t arm_jit_slice(int32_t const budget_)
{
  int32_t  total = 0;
  uint32_t seq   = g_arm_rom_seq;

  for(;;)
    {
      uint32_t reason;

      /* machine-code dispatch: the trampoline consumes pc/dispatch/table
       * entirely in asm, calling blocks until an exit reason other than
       * NEXT (handled internally, budget-checked) breaks the run. */
      {
        reason  = (*(jit_entry_t)g_jit_tramp_entry)(budget_ - total);
        total  += g_jit_cycles;   /* cycles charged by this run */
      }


      switch(reason)
        {
      rehandle:;   /* idx<0 in TRAMP_X_COMPILE re-dispatches as INTERP */
        case TRAMP_X_INTERP:
          /* out of the JIT windows — the interpreter owns MMIO and
           * ROM-miss fetch semantics; verbatim (+ slice tail) */
          {
            int32_t const c = arm_execute_interp();
            total += c;
            /* per-word tail, exactly the cached slice loop's order:
             * soft-reset consume, then the FIQ vector (the interp path
             * owns MMIO/ROM words, whose visible effects can pend FIQ
             * mid-slice — without this the jit diverges from the cache
             * whenever an FIQ pends during out-of-window execution) */
            if(g_SOFT_RESET_PENDING)
              g_SOFT_RESET_PENDING = false;
            else if(!ISF && arm_fiq_pending_())
              arm_fiq_vector();

            if(g_arm_rom_seq != seq)
              seq = g_arm_rom_seq;

            if(*g_cdrom_restart_poll)
              return total;

            if(total >= budget_)
              return total;

            if(*g_madam_fsm_poll == FSM_INPROCESS)
              return total;

            continue;
          }

        case TRAMP_X_UNALIGNED:
          /* unaligned pc inside a fetch window: the cached slice decodes
           * the aligned word via the arm_h_* handlers (blocks bake aligned
           * USER[15] constants), so mirror that via the verbatim
           * single-word C-step. */
          if(arm_jit_cstep(&total,budget_,&seq))
            return total;
          continue;

        case TRAMP_X_COMPILE:
          {
            uint32_t const pc  = CPU.USER[15];
            int32_t  const idx = arm_jit_pc_index(pc & ~3u);
            jit_block_t      *blk;

            /* The dispatch gate lets pc values reach the compile path
             * whose window-mapped index is -1 (pc >= RAM_SIZE but the
             * raw >>2 lookup lands in the table range): here that must
             * never arm the compiler with a wild index.  Hand the pc to
             * the interpreter, which owns out-of-window fetch
             * semantics.  (Casting (uint32_t)-1 indexes half the
             * table; TRAMP_X_INTERP semantics.) */
            if(idx < 0)
              {
                reason = TRAMP_X_INTERP;
                goto rehandle;
              }

            blk = arm_jit_compile((uint32_t)idx);
            if(blk == NULL)
              {
                /* untranslatable word or arena full: C-step once */
                if(arm_jit_cstep(&total,budget_,&seq))
                  return total;
              }
            continue;
          }

        case JIT_X_NEXT:
          /* defensive: the tramp handles NEXT internally */
          if(total >= budget_)
            return total;
          continue;
        case JIT_X_FIQ:
          if(arm_cdrom_restart_())
            return total;
          if(total >= budget_)
            return total;
          if(arm_madam_inprocess_())
            return total;
          continue;
        case JIT_X_CSTEP:
          /* budget pre-check — RESTORED: the cache engine's per-word
           * loop tests the budget BEFORE the next word (its bottom-of-
           * loop break), so a slice never runs a word on an exhausted
           * budget.  Removing this made every boundary cstep overrun
           * by a full word charge (bne +5) which the 3do line-step
           * carried as debt — the f260 divergence class. */
          if(total >= budget_)
            return total;
          {
            if(arm_jit_cstep(&total,budget_,&seq))
              return total;
          }
          continue;
        default:              /* JIT_X_RST / JIT_X_BUDGET / JIT_X_FSM */
          return total;
        }
    }
}

/* entry used by opera_arm_execute_slice / opera_arm_execute */
static int32_t arm_jit_exec_slice(int32_t const budget_)
{
  if(!g_jit_ready && !opera_arm_jit_startup())
    {
      g_arm_engine = 1;           /* graceful: fall back to cached */
      return arm_execute_slice(budget_);
    }

  return arm_jit_slice(budget_);
}
