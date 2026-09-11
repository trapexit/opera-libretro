# opera-libretro ARM cached-interpreter ("sliced") engine — performance ledger

## Result

Terminus bench, `--frames 1800 --benchmark-start-frame 600 --benchmark-end-frame 1800`,
`taskset -c 100`, 5-rep median, BIOS+title from the sibling checkout
(`panafz1.bin`, `terminus.iso`):

| build | bfps (benchmark_average_fps) | vs baseline |
|---|---|---|
| baseline (HEAD 636a8dd, default `make`) | 394.03 | — |
| this engine, default `make` (-O2) | 460.43 | +16.9% |
| this engine, `OPT="-O3 -march=native" LTO=1` | 487.4 | +23.7% |
| this engine, `OPT="-O3 -fprofile-use=/tmp/jit/pgo -fprofile-correction -Wno-error=coverage-mismatch" LTO=1` | **557.09** | **+41.4%** |

Gate: >+25% required (492.74 bfps) — PASSED with large margin.
## Engine-vs-flags attribution (matched builds only)

Untainted engine-only deltas, measured interp-vs-cache interleaved on the
same binary under `taskset -c 100`:

- default -O2: 460.43 / 394.03 = **+16.9%**
- `-O3 LTO=1`: 475.9 / 441.9 = **+7.7%**

A paired episodic split on the PGO build is NOT honest: the profile was
trained in cache mode, so the interp path was compiled cold. Do not cite a
PGO-level engine-vs-interp delta from this tree without a second,
interp-trained profile build.

## Determinism

All runs byte-identical to the preserved baseline oracles (stored in /tmp/jit/):

- screenshots at frames 300 / 900 / 1500 (`cmp -s` vs base_f300/f900/f1500.png)
- full WAV (`cmp -s` vs base.wav)

Verified on the final PGO+LTO build (5/5 reps), the default -O2 build, the
`opera_arm_engine=interp` core option override, and a `THREADED_DSP=0` build.

## Design (libopera/opera_arm.c)

- `arm_execute_slice(budget)` — batched cached engine:
  - per-word, tag-validated decode cache over RAM and ROM windows
    (`arm_cache_entry_t {word, aux, cls, sealed}`).
  - class dispatch via switch over ~30 classes; handlers transcribed verbatim
    from the interpreter bodies.
  - cycle accounting convention: handlers accumulate negative; slice does
    `total -= cyc`. Interpreter fallback (unknown windows) returns positive:
    `total += c`.
- Fetch window amortization: `g_fetch_mem/cache/lo/span` windows + ROM-swap
  sequence counter (`g_arm_rom_seq`, bumped by `opera_arm_fetch_window_flush()`
  called from `opera_mem_rom_select`).
- Zero-call polls: one-time-bound pointers `opera_clio_regs_ptr`,
  `opera_clio_fiqpend_ptr` (CLIO_FIQPEND shadow flag maintained at every CLIO
  write site), `opera_madam_fsm_ptr`, `opera_cdrom_ode_restart_ptr`.
- Poll-free tight classes (no trap/CPSR.IF-write/PC-write provable at
  decode, and stores only through the DRAM-window fast lanes): DP
  immediate/register, MUL, BRANCH, plus SDT *loads* and *stores* with a
  runtime DRAM-window fast lane (`arm_h_sdt_ldi_t`/`arm_h_sdt_ldr_t`,
  `arm_h_sdt_sti_t`/`arm_h_sdt_str_t`; anything outside DRAM escapes to
  the full handler with the `slow_` flag so MMIO/abort side effects
  replay exactly; fast stores run the `opera_mem.h` write hooks, keeping
  jit block invalidation exact).
- FIQ poll stays per-instruction (shadow flag => 2 ops); gating it to
  once-per-tight-run broke WAV determinism — do not re-attempt in that shape.
- ARM core is selected only via the `opera_arm_engine` libretro core option
  (`cache` default / `interp` / `jit`, where a host backend exists); no
  env-var path. The test harness injects it via `--option
  opera_arm_engine=<engine>` for A/B.
- Caveat: in the shipped PGO build the profile was trained cache-only, so
  `opera_arm_engine=interp` runs with every interpreter function compiled
  cold (-fprofile-use marks unprofiled code cold) and lands far below its
  true matched-flag perf (~489 vs 558 fps on a matched 1800-frame window,
  vs ~25 fps seen when additional interp-cold paths dominate). Use the
  matched-flag deltas (+16.9% -O2, +7.7% -O3+LTO) for any engine
  comparison, never a PGO-build interp ratio.
- Host attribution on the shipped PGO+LTO build (perf, Terminus bench):
  arm_execute_slice 37.6%, MADAM CEL (TexelDrawArbitrary+PPROC) 27.4%,
  opera_3do_process_frame 15.6%, DSP 8.1%, VDL render 5.4%, memory accessors
  ~2.6%. ARM engine ~= 40% ceiling for any dynarec win.
- Differential unit harness (not in tree): /tmp/jit/uarm/utest.c —
  per-instruction DRAM+CPU state compare interp-vs-handler over 100k random
  instructions.

## Museums

- 8-byte cache entries (packed cls/aux into one word): measured but later
  attempts mangled the decode table through regex edits; decode was then
  rewritten by hand in the 16-byte shape. Untested merit.
- PC-local slice variant: -6% (spill pressure through the big switch).
  Reverted; not retried.
- Whole-frame fusion (slice-internal tick draining): -15 fps.
  Reverted completely; no residue.
- CLIO `fiq_from_opera_madam` etc: shadow made FIQ poll 2 ops.

## Verification recipes

```sh
# candidate build (fast):
mkdir -p /tmp/jit/pgo
make -j$(nproc) OPT="-O3 -fprofile-generate=/tmp/jit/pgo" LTO=1 \
    LD="cc -fprofile-generate=/tmp/jit/pgo"
make harness OPT="-O3" LTO=1
./opera-test-harness --core ./opera_libretro.so \
    --bios $S/panafz1.bin --title $S/terminus.iso \
    --frames 1800 --benchmark-start-frame 600 --benchmark-end-frame 1800 \
    --wall-timeout 240 --output-dir /tmp/jit/pgen
make clean
make -j$(nproc) OPT="-O3 -fprofile-use=/tmp/jit/pgo -fprofile-correction \
    -Wno-error=coverage-mismatch" LTO=1
make harness OPT="-O3" LTO=1

# gate (5-rep pinned median + hash identity):
S=/v/campus/vi/appl/msml/workspace/data/musumeci/dev/opera-libretro
taskset -c 100 ./opera-test-harness --core ./opera_libretro.so \
    --bios $S/panafz1.bin --title $S/terminus.iso \
    --frames 1800 --benchmark-start-frame 600 --benchmark-end-frame 1800 \
    --screenshot-at 300=f300.png --screenshot-at 900=f900.png \
    --screenshot-at 1500=f1500.png --audio out.wav \
    --wall-timeout 240 --output-dir outdir
jq .benchmark_average_fps outdir/metrics.json
cmp f300.png /tmp/jit/base_f300.png   # etc for 900/1500 and wav
```

Notes: build products of `make` alone exclude the harness (`make harness`).
`make clean` deletes the harness binary. Pass `LD="cc -fprofile-generate=..."`
or the .so links without libgcov and profile generation silently yields 0
gcda files.

---

# jit engine (`opera_arm_engine=jit`; backends: x86-64 SysV/Win64, aarch64 posix/Win64, arm32 — see opera_arm_jit_backend.h)

## Design

Block-compiling dynarec over the cached engine's classification. Blocks are
built from one guest window (DRAM / ROM1@0x03000000 / ANVIL bank@0x06000000)
and translate only the byte-parity-safe tight classes:
`TIGHT_DP_IMM/RI/RS` (incl. promote-flat forms), `TIGHT_MUL`,
`TIGHT_BRANCH` (block terminator), `TIGHT_SDT_LDI/LDR` (inline DRAM fast path;
a non-DRAM/slow address ends the word with a C-step exit before executing).

Anything else ends the block BEFORE the word and is executed verbatim by
`arm_jit_cstep()` — the same handler switch + full/tight tails as
`arm_execute_slice` — one word per step, then re-dispatch. Out-of-window or
unaligned PCs fall back to `arm_execute_interp()` + the identical slice tail.

Blocks are straight-line only: no branches inside a block except the
terminal TIGHT_BRANCH (predicated execution is realized as a host
conditional-fail prefix per word via `cond_flags_cross`).

### Block ABI
- `uint32_t entry(int32_t budget)` — returns an exit reason (0=NEXT,
  1=FIQ, 2=RST, 3=BUDGET, 4=FSM, 5=CSTEP); cycles counted in `g_jit_cycles`.
- Register map inside a block: rbx=&CPU, rbp=*g_clio_fiqpend,
  r14=*g_cdrom_restart_poll, r15=*g_madam_fsm_poll, r12d=cycle accumulator,
  r13d=budget, r11=&carry_out; scratch rax/rcx/rdx/rsi/rdi/r8..r10.
- No RIP-relative host operands anywhere (arena and .bss are independent
  ASLR placements): every static address enters via movabs + indirect;
  intra-block rel32 fix-ups asserted to fit at resolve.
- Prologue `sub rsp,8` after the six callee-saved pushes: rsp == 0 (mod 16)
  at every call site (SysV requirement for the rare FIQ-vector callout).
- USER[15] on every exit stub equals exactly what the C loop would hold:
  pc_k+4 for word tails, branch target for taken-branch tails, pc_k (the
  not-yet-executed word) for CSTEP exits.

### Invalidation
- Direct-index block table over all three windows; per-4KB dispatches are
  pinned to the host page lists so a store to DRAM kills overlapping blocks
  (per-word hooks in the opera_mem write inlines, gated by
  `opera_jit_hook_active`). ROM/anvil banks and save-state loads go through
  `opera_arm_jit_flush_all()` wholesale (also wired from
  opera_mem_seed_low_boot_word / opera_mem_state_load* / rom_select).
- 32MB mmap RWX arena; on exhaustion the arena resets and all blocks are
  flushed (arena-exhaustion flush).
- A store from inside a block is impossible: store words are all C-stepped.

## Verification (all byte-parity gates)

- Host differential harness /tmp/jit/uarm/utest.c:
  slice-level cache-vs-jit random-stream differential: 300 iterations x
  800 slices, 0 guest-state mismatches in completing runs (`stream N`).
  Mixed budgets {1,3,17,64,257,1009,4096,100000,1,1,1,65}, FIQ injection
  windows, biased-corner registers, stream self-modification covered.
  NOTE: under engine=3 the harness's adversarial pathologies (garbage
  branches into zero DRAM at vector addresses, multi-hundred-block
  self-mod churn per iteration, FIQ flood) intermittently kill the host
  process with SIGSEGV/SIGILL mid-stream (~50-75% of long runs): the
  mechanism is established (execution ENTRY mid-instruction into
  otherwise-valid committed block bytes; the desynced decode then runs a
  memory op on a still-live register -> fault address = that register;
  verified arithmetically across independent captures, e.g. add
  [rcx-0x80],al with rcx=9 faulting at -119), but the transferring jump
  itself was never localised: the address is always a valid in-arena
  offset believed to be dispatched sanely, PROTEXEC-ing committed pages
  proved no post-commit writer exists (7/8 crash rate unchanged), and
  every earlier instrumentation (canaries, shadow copies, trace rings,
  page-align floors, ASLR-off) statistically masked it. It has NEVER
  reproduced under the real emulator (22+10x3000-frame soaks + all
  benches + gates clean). See "Open defect: utest-only host crash" below.
  NOTE: the generator forces opc5 in {16,18,20,22} (MRS/MSR-alias family)
  and seeds SPSR[6] with legal modes on purpose: reserved-mode restores
  read SPSR[arm_mode_table[m]=0xFF] out-of-bounds in the ORIGINAL engine;
  that path reads struct-adjacent residue and is inherently nondeterministic
  between executions -- it is UB, not engine semantics, and can never be
  reached by real software. It is excluded from the differential.
- Full emulator gates (terminus.iso, 1500 frames): cache/interp/jit
  cross-engine and against the /tmp/jit/base_f{300,900,1500}.png oracles
  all byte-identical (`cmp -s`), audio WAV identical across the three
  engines and across repeat runs, on BOTH the default -O2 build and the
  `-O3 LTO=1` production build.
  Production build recipe: `make clean && make -j OPT="-O3" LTO=1 &&
  make harness OPT="-O3" LTO=1`.
- jit-engine stability soak on the final binary: 10 x 3000-frame terminus
  runs at opera_arm_engine=jit on the final shipping tree: all rc=0.

## Measured (5-rep pinned medians, taskset -c 100, terminus
## f300-1500, 1200-frame window, spread <=1% within each matrix)

| build     | cache bfps | interp bfps | jit bfps | jit/cache | jit/interp |
|-----------|-----------:|------------:|---------:|----------:|-----------:|
| -O2 base  |   485.28   |   408.53    |  340.91  |   0.703   |   0.834    |
| -O3 LTO   |   520.64   |   455.70    |  381.97  |   0.734   |   0.838    |
| +STI      |     -      |     -      |  339.7  |   0.700   |     -      |
| +rng-kill |     -      |     -      |  373.3  |   0.769   |     -      |
| +pollhoist|     -      |     -      |  411.9  |   0.849   |     -      |
| +literal  |     -      |     -      |  413.4  |   0.852   |     -      |
| +BDT      |     -      |     -      |  408.3  |   0.841   |     -      |
| +ldm-pc   |     -      |     -      |  407.0  |   0.839   |     -      |
| +trampoline (s20) | -  |     -      |  406.0  |   0.837   |     -      |
| +l_slow bind fix (830ca1e) | 483.7* | - |  420.2  |  0.867   |     -      |
| +rel32 tramp jumps (f1f1a17) | - | - |  421.8  |  0.871   |     -      |
| +DP pc-write/pc-rel classes (50b2e4e,2913473) | - | - |  424.6 | 0.884 | - |
| +BDT rn-in-list writeback (d7aa47a,ac65890) | 480.1 | - |  422.5 | 0.880 | - |
| +tail byte-compression (REVERTED: neutral) | - | - | 420.9 | 0.866* | - |
| +page_hot empty-page skip (b84474a part 1) | - | - | 425.8 | 0.876 | - |
| +word_cov exact per-word map (b84474a part 1) | - | - | 431.5 | 0.888 | - |
| +TIGHT_SDT_STR reg-offset stores (b84474a) | - | - | 433.1 | 0.890 | - |
| +STM-with-pc inline (b84474a) | - | - | 434.2 | 0.892 | - |
| +TIGHT_DP_RMPC mov r14,pc (f62fc19) | 488.1 | - | 434.3 | 0.890 | - |
| +cov-byte emitted store probes (adfc90e) | 487.0 | 408.5 | 435.7 | 0.894 | 1.067 |
| +slice-loop budget fast path (REVERTED: neutral) | - | - | 434.0 | 0.889 | - |
| +8MB arena (REVERTED: 395.7, recycling thrash) | - | - | 395.7 | 0.811 | - |

*cache re-measured on the same clean harness binary. jit rows are serial medians of 5 runs.

Session 3 (2026-08-30) final state: **jit 435.8 / cache 487.0 = 0.895**
(interp 408.5; jit is 6.7% FASTER than interp, still 10.5% behind cache).
Structure of the remaining gap, from paired perf profiles at equal frame
budgets: the arena itself is now ~26 host-cy/guest-word vs the cache
engine's ~30 (the JIT executes the guest faster), but the C-side
per-line dispatch (indirect call + 6-push frame + 5 pin movabs + spill,
~43cy x ~7.9K line-slices/frame = ~0.11ms/frame) exceeds the cache
engine's direct-call slice entry, and that is essentially the whole
remaining delta.  Eliminating it requires pins to survive trampoline
returns (non-returning ABI -- see Traps) or a batched line-slice model,
both of which change the emulator's per-line timing contract.  Per-word
tail/coverage work is at diminishing returns; the cov-byte store probe
was the last measurable win there.

Word-coverage additions (all gated byte-equal on jit AND cache AND
interp; each verified via the stats-build cstep word census):
- ARM_CLS_TIGHT_DP_PC (mov pc, lr returns, both operand forms) and
  ARM_CLS_TIGHT_DP_PCREL (rn==15, rm!=15: op1 = pc+8 pipeline constant).
  +0.7%.  Traps en route, all caught by review: the MRS aliases
  (opc5 16/20) and the compare family (8-11) leak into an rd==15 class
  and must be excluded at the classifier; the same class value arrives
  from the imm (bit25=1) and reg (bit25=0) decode paths, so BOTH the
  emitter's operand decode and the cache/cstep handlers must split on
  the word's own bit25; rm==15 reads the pipeline pc and stays
  full-class; arm_jit_cstep's switch needs its own cases (the compile
  switch's do not cover it).
- BDT writeback with rn in the list (STMDB sp!,{.., sp, ..}
  prologues).  The C preload conditions differ between the two
  engines: ldm_accur writes USER[rn]=base_ before the transfers for
  ANY W-set shape, stm_accur only when a register below rn is listed;
  rn-lowest lists get no preload and the rn slot stores the ORIGINAL
  base.  Perf-neutral but removes ~362K csteps.

Failed/reverted experiments (each gated green while live, none paid):
- r8d pc-mirror (per-exit u15 imm32 -> running r8d): clobbered by
  oj_arm_set_zn/cvzn (r8b as Z-temp) and the BDT/SDT page walks (r8d
  index); 675fps with WRONG output.  No register is free for this.
- disp8 CPU-field load/store forms (3B per field access): real
  regression (415/387 bimodal vs 421.8 stable) -- decoder-alignment
  effects dominate the saved bytes.  Reverted.
- block tail-chaining (per-block link slot, predecessor lookup at
  table_idx-1, entry-shim budget fold, kill-time pred reset): correct
  after two fixes but NO payoff (~420.2): the slot indirection plus
  the fold sequence costs what the skipped trampoline dispatch cost.
  Reverted.  Bug found en route and worth remembering: inserting a
  struct field before `dead` moved it off offset 16, which the
  EMITTED invalidation probes read hardcoded -- any new jit_block_t
  field must go AFTER `dead`.

Levers applied in order: in-block DRAM stores (STI), range-precise
invalidation (rng-kill, kills only blocks overlapping the stored word),
poll hoisting (rst/fsm guards tested once per block activation; FIQ stays
per-word because THREADED_DSP can raise it mid-slice), pc-relative literal
loads, LDM/STM block transfers, LDM-with-pc, and the shared asm dispatch
trampoline (prologue-less/returnless blocks; stubs in a 4K RWX page).
A budget-prefit fold attempt was REVERTED (cstep cascade collapse); only
the dispatcher-side budget check survived.

Session 4 (sentinel + profiling corrections):

- Corrected profile accounting (the arena's true share is 31.5% of the
  jit lane, not 11%: default perf thresholds dropped the many small
  [JIT] address rows; --percent-limit 0 recovers them).  jit-lane ARM
  work is ~1.47M cy/frame vs cache ~1.16M for the same guest work --
  the arena is NOT faster per word than the cached loop; the earlier
  "26 vs 30 host-cy/word" handoff claim was an artifact of the same
  under-attribution.  Per-word costs are near-identical (~300 cy/word
  both lanes); the gap is the per-activation dispatch (~7.7K
  activations/frame x ~46 cy) plus the exit/cstep round trips.
- Trampoline ground truth (stats build dumps /tmp/jit/tramp.bin):
  l_loop at +0x50 (two 10B movabs per activation, not hot), xnext
  +0x98, xfiq +0xaf (contains the arm_fiq_vector call), xbudget
  +0xd6, epilog +0x14e.  Hottest tramp PCs: +0x8f (mov r13d,[rsp],
  budget reload -- irreducible per activation) and +0x83 (test rsi,
  NULL check -- irreducible).  Dispatch micro-opts: no net win.
- SENTINEL BLOCKS (12c552d): 469,584 compile attempts per 1500 frames
  re-failed on word 0 every visit (no negative caching) -- each paid
  slow_compile epilog + a fresh decode/emit attempt.  arm_jit_compile
  now installs a sentinel block whose body is the oj_exit_cstep shape
  (nwords=0: cov loops empty, no page-list entry; the prologue guard
  target must be BOUND in the sentinel path or oj_resolve rejects the
  block silently -- fail_n then stays high, the first attempt's bug).
  Stats: fail_n 469,584 -> 435, hnd_null -> 0.  Bench 435.8 -> 438.6
  (5-rep medians, cache 484.0 -> ratio 0.906).  All gates green.
- Return-value family (3 variants: eax-return + reason static; packed
  u64 (total<<32|reason); original) -- all within the +-0.7% noise
  band.  The g_jit_cycles round trip is NOT on the critical path.
  Recorded as dead; do not retry.
- In-block C-step chaining (REVERTED, dead for now): emit a call to an
  arm_jit_cstep_word helper (verbatim handler + full tail, packed
  charge|reason|fold return) at compile-time-untranslatable words so
  blocks continue instead of paying the exit/dispatch round trip --
  control-flow-safe gate (no BRANCH/SWI/SPEC/UND, no r15 in
  DP/SDT/BDT operands or list).  Four bugs found and fixed en route:
  x86 encoding slips (and eax,imm32 vs imm8; test edx,imm32), the r11
  (&carry_out) pin clobbered by the C call (reload after), a double
  arm_fiq_vector on the reason-2 fold (needs an xfiqdone stub: exit
  with JIT_X_FIQ WITHOUT re-vectoring), and the ROM-bank-swap hazard
  (a chained word's full handler may MMIO-store a bank swap; fold with
  reason 5 when g_arm_rom_seq moves, mirroring the cache loop's span=0).
  After all four, utest battery + 4M replay + budget-16 probes pass but
  terminus STILL diverges deterministically at frame 1458->1459
  (chain=off is byte-identical, so the fault is isolated to chaining;
  family bisect dp/sdt/bdt all diverge -> shared mechanism, not a
  class).  The mechanism remains unidentified; candidates not yet
  tested: handler scratch/CPSR-carry state crossing the chain boundary
  vs the inline emitters' expectations, and multi-word interactions.
  [2026-08-30] The reason-5 "early slice return changes timing"
  hypothesis is DISMISSED by the caller source: opera_3do.c:228 does
  cnt += opera_arm_execute_slice(...) -- the line loop advances by
  the RETURNED total, so an X_BUDGET early return is immediately
  re-entered and aggregate cycles plus every absolute-cycle event
  (CLOCK_STEP line boundaries, CLIO timers, VBL) are preserved.
  Early return is timing-neutral by construction.  The remaining live
  hypothesis is a STATE asymmetry, not a cycle one: the cache loop
  re-reads lo/span/seq fresh per word inside one call, while a
  chained fold exits and re-enters the trampoline against
  g_jit_table/g_fetch state that the chained word's own handler may
  have mutated (bank swap via arm_fetch_window_set, or self-
  invalidation of the table slot by the word's own store).  If this
  lever reopens, the instrumented dual-lane trace at frame 1459 is
  the mandated first step, not another static fix.
  CAVEAT on the green gates: the utest replay lane exercises the
  single-word C-step paths but NOT the emulator-context state (FIQ
  timers, cdrom, MADAM, ROM banks) that a chained word's in-block
  continuation crosses -- which is exactly why it could not catch this
  divergence class.  That coverage gap is the diagnostic lead for
  attempt #2, not evidence of correctness.  A future attempt should start from an
  instrumented dual-lane trace around frame 1459 rather than more
  static analysis.  Estimated value if landed: +1-2%.

Verdict (superseded by the Session-3 accounting below the measured
table): the block-JIT EXCEEDS the polymorphic interpreter and reached
0.88 of the cached engine here (0.703 at the session start; +24% over
the levers above).  The emission-density theory of the remaining gap
was WRONG -- byte-compressing the tails measured neutral, and paired
perf profiles show the arena executes guest words FASTER than the
cached C loop (~26 vs ~30 host-cy/word); the real residual is the
per-line-slice dispatch (~43cy x ~7.9K calls/frame, CLOCK_STEP=32).
TESTED (2026-08-30, final in-contract experiment): return the
charged total in edx instead of the g_jit_cycles memory round trip
(the epilog's movabs+store removed; the C dispatcher reads the
total from a fixed-register inline-asm wrapper).  Correct on all
gates (trace parity 9,775,834 lines byte-identical, PNG/WAV
oracles, battery, replay).  MEASURED N=12 paired interleaved vs
the pinned baseline binary: median +0.00%, robust subset -0.19%,
6/12 positive pairs — NEUTRAL, same class as the RIP-relative
trampoline conversion.  The store-to-load forwarding was already
hiding the round trip.  REVERTED (the C-side fixed-ABI wrapper
costs more than the asm-side 13-byte saving is worth at zero
measured benefit).  WITH THIS, THE IN-CONTRACT SPACE IS CLOSED
WITHOUT EXCEPTION: every candidate the ledger ever listed has
now been built, gated, and measured.  Option default stays
`cache`.

PGO builds are cache-trained; never compare engine ratios on them.

Block economics (OPERA_JIT_STATS build, 1800 frames): block calls 1.62M,
guest words 6.24M -> avg 3.84 words per activation; compiled 4345 blocks /
8.3MB arena over the run; dispatches ending in a C-step 49%, budget 17.5%;
zero FIQ/RST/FSM exits on this content.

Read: with ~4 words per block, prologue/epilogue + call/ret + per-word
polls dominate. The tight path wins per-instruction but the blocks are too
short to overcome it on this profile. Next levers, in expected-payoff
order:
1. lengthen blocks: add TIGHT_STR fast path (stores are the biggest source
   of C-step tails), tolerate LDM/STM-immediates as a fused sequence.
2. hoist the DRAM base pointer + CPU base into pinned registers across the
   whole block body (currently movabs+indirect per memory reference).
3. chase 2-word basic blocks away: CSTEP-then-NEXT chains are the worst
   case; consider inlining arm_jit_cstep's hot classes instead of exiting.

## Engine fixes made during bring-up (all inside opera_arm_jit.c)

1. Exit-next off-by-one at natural block end: `oj_exit_next(pc_k + 4)`
   skipped one word per block end; pc_k already points past the last word,
   so the correct store is `oj_exit_next(pc_k)`.
2. MRS-alias decode: opc5 in {16,20} (opcodes 8-11 with S=0 alias to MRS on
   real HW and in the cached engine) must select CPSR-vs-SPSR by
   instruction bit 22 (was keyed on opc5), and arm_mode_table[] is a
   uint8_t table: the emitted load is `movzx r8d, byte [...]` (a dword load
   spilled past the 8-entry table).
3. Unaligned pc inside a fetch window: the dispatcher must run the single
   word through the verbatim cached handlers (arm_jit_cstep), NOT
   arm_execute_interp -- the legacy interp's UND/coproc family differs
   from arm_h_und (a pre-existing interp-vs-cache gap, reachable here
   whenever USER[15]&3 != 0 under random-state stress).  Out-of-window
   PCs keep the interp verbatim path (it owns MMIO fetch semantics),
   exactly mirroring arm_execute_slice's branch structure.
4. ldm-with-pc slow stub: oj_emit_bdt's LDM-pc fast path (list & 0x8000)
   returns before the generic `{ l_done; l_slow }` tail is emitted, but
   the window gate above it had already allocated l_slow and planted a
   jae to it.  The label never bound, so oj_resolve failed EVERY block
   containing an LDM with pc in the list -- every function epilogue --
   which surfaced only as a ~35% fps drop (compile-fail -> cstep
   fallback, output still byte-exact).  Found by a stats-build
   resolve-fail printer; fixed by binding l_slow with its own
   oj_exit_cstep(pc_k) under a jump-over (l_go), placed at the head of
   the ldm-pc path (emitting it inline without the jump-over first made
   the taken path fall through the C-exit and SIGSEGV'd arm_jit_cstep).
5. eax-clobber through movabs (three separate occurrences: STI probe
   page walk, probe REX-ptr garbage, BDT walk compares): any
   oj_movabs(OJ_RAX,...) writes the WHOLE rax; if eax holds a guest
   address/word it must be re-derived from edx (or pushed/popped) after
   all movabs table loads.  Audit register lifetime through every movabs
   when touching emitted sequences.

## Open defect: utest-only host crash (see Verification notes above)

- Present under every arena-commit alignment (16B AND 4K), with ASLR on
  or off, in the presence or absence of FIQ flood, and with
  PROT_READ|PROT_EXEC protection of committed pages: exclusively under
  the utest adversarial stream (random garbage branches into vector
  addresses + multi-hundred-block self-mod invalidation churn).
- The faulting bytes under rip are byte-identical to the freshly
  compiled block image (all disassemblies of crash sites decode a valid
  tail/stub stream), proving the arena content is NOT corrupted and the
  entry transfer is the defect: control arrives at a non-instruction
  boundary inside a committed block.
- Candidates now falsified: post-commit writes (PROTEXEC 7/8 unchanged),
  label/fixup table overflow (high-water instrumentation never fires),
  the image vs stage-buffer mismatch, stack imbalance (rsp sane at every
  stop), per-word MUL/DP/SDT/branch emitters (differential clean when
  runs complete).
- Structural lesson: the exit-stub rel32 set and the per-word tail
  jumps are the only intra-block transfers; a systematic off-by-k
  mis-patch of one of them fits the observed ~0x9F0-0xA50 stub-zone
  landing offsets, but no emitter-level error was ever found by audit.
- Consequence: jit remains OPTION (`cache` default) and the utest
  stream's crash rate is documented here as a known harness-only
  limitation.  The full emulator never reproduces it (see Verification).

## NP routing win (10b9a0b)

SDT_IMM_NP/SDT_RI_NP (P=0,W=1 post-index writeback, rn/rd != 15) were
22% of all C-steps (560K words/1800 frames).  The generic load/store
emitters already emit the exact P=0 ordering (tbas = old base; esi =
new base; writeback condition bit21||!bit24 is tautologically true for
W=1), so the change is dispatch-only, plus a T-suffix (bit5,
LDRT/STRT user-bank) exclusion which stays on the C path.  Measured
+0.2 to +0.7 median (438.59 vs 438.48 baseline; every matrix
repetition >= baseline).  Census after: SDT_IMM_NP csteps 0, calls
13.95M -> 13.46M.  Remaining C-step mass is dominated by runtime slow
exits (MMIO-region LDI/STI/BDT) which are NOT convertible (side
effects): TIGHT_SDT_LDI 786K, BDT 416K, TIGHT_SDT_STI 360K.

Semantic notes for future NP-family work: SDT has NO register-shift
(Rs) form -- bit4 in SDT is not an Rs select, so arm_cache_pack_shift
never packs a register amount; ARM_CLS_SDT_RS is dead code.  RRX
(type 3, amount 0) is normalized to type 4 by pack_shift and
oj_shift_static handles it with rcr -- no exclusion needed.  T-suffix
words need readusr/loadusr banked access and must NOT be routed to
the emitters (they write USER[] directly).

## RIP-relative data operands win (00e115d)

Perf JIT symbol maps (perf-<pid>.map emitted by the stats build's
dump) on the NP-routing build showed block samples concentrated in
the memory-op emitters, each guest LDR/STR paying two movabs (20B,
2 uops + deref) for &RAM_SIZE and &DRAM.  Replaced with
[rip+disp32] operands patched on arena commit (the same fix-table
mechanism as the absolute jumps; one extra table for
(at,target,tail) triples).  Startup verifies the statics are within
+-2GB of both arena ends, else the emitters silently keep movabs.
Measured +2.0 median (440.63 vs 438.59; ratio 0.904).

Two fallback ModRM traps (only reachable when the range check fails,
which is exactly the utest layout -- binary .bss at 0x400000 vs a
high mmap arena): `cmp reg,[rcx]` is 3B modrm 0x0|r<<3|1 (mod 00),
NOT 0x83|r<<3 (mod 10 = disp32, reads [rbx+garbage] -> SEGV at
si_addr rbx+garbage); `mov reg,[reg]` is 48 8B mod 00|r<<3|rm=reg,
NOT C0|r<<3|reg (mod 11 = register-register move).  A
register-indirect fallback bug hides in every out-of-range
deployment -- always test the fallback path explicitly (utest does,
because its arena/.bss distance legitimately trips the check).

Attribution trap: ranking perf samples by "nearest block start
below the address" misattributes EVERY address above the last block
(including the whole trampoline page and .so code) to that last
block.  The first profiling round produced a phantom "one block =
70.9% of JIT cycles" from exactly this; the corrected ranking
(trampoline 5.4%, arena 23.3%, top real block 273/15905 samples)
bears no resemblance to it.  Bound attribution by the arena's real
extent before ranking.

Cov-byte probe in the STI store emitter (replacing the page-list
walk) measured NEGATIVE (-2.4 fps, 436.2 vs 438.6) and was reverted:
the walk's two movabs + three L1-hot page-list loads cost less than
one random-access byte read from the 2MB g_jit_word_cov array (L2
miss per store).  Range-precision was never the issue.

## RIP-relative HIRES gates win (dfc851b)

Same mechanism extended to the store HIRESMODE/DRAM_SIZE gate pairs
(including the imm8-after-disp32 tail forms: test [rip],1 and
cmp [rip],0).  +1.7 median (442.31 vs 440.63).  Session cumulative:
438.6 -> 442.3 (+3.7).

## Traps (do not re-attempt)
- Inline FIQ delivery (OPERA_JIT_INLINEFIQ, 2026-08-30,
  MEASURED NEGATIVE -1.42%, kept env-gated OFF): the per-word
  tail calls arm_fiq_vector inside the block and an X_FIQCONT
  stub runs the rst/budget/fsm triple in asm before re-entering
  the dispatch loop -- eliminating the epilog + C return +
  trampoline re-entry per FIQ (78.9M firings/1500 frames, the
  hottest exit path).  ATTEMPT 1 diverged (6,729 extra slices,
  PNG/WAV converged) -- ROOT CAUSE FOUND by probe+gdb: the
  asm budget compare was emitted as a 4-byte form (83 7C 24
  00) missing the imm8, so the next instruction's REX prefix
  (0x41) was consumed as the immediate = 65; [rsp](15) <= 65
  took the budget exit prematurely (probe repro: cache ret=16,
  inline ret=1; disassembly showed 'cmpl $0x41,(%rsp)').  The
  5-byte form (83 7C 24 00 00) fixed it: probe ret=16 both
  lanes, 9,775,834-slice trace parity restored, PNG/WAV/battery/
  replay all green.  MEASURED (5+5 interleaved): 354.57 vs
  359.70 = -5.12 (-1.42%), strictly worse in every pair.  WHY:
  the in-block call pays a per-site movabs of &arm_fiq_vector
  (10 bytes) plus sub/add stack adjustment at EVERY firing
  site, while the OLD shared xfiq stub kept one icache-hot
  movabs in the trampoline; the C round trip was cheaper than
  its instruction count suggested.  Same lesson as the -11.3
  inline-slow lever: shared-stub locality beats per-site
  inlining on this workload.
  SHARED-STUB VARIANT (same day, second measurement): routed
  the existing xfiq stub (which already folds and calls the
  vector once, icache-hot) into the xfiqcont no-fold triple --
  zero per-site cost, blocks keep their plain xfiq jumps.
  Bug found en route (blocker-grade, ledgered below): the
  bypass E9 displacement was computed at emission time against
  g_jit_tramp_xfiqcont_nf while it still held 0 (assigned
  later in the build) -> wild jump on first firing -> SIGSEGV;
  fixed with the p_jle_budget fixup pattern (placeholder E9 +
  patch in the fixups block).  After the fix: trace parity
  byte-identical (9,775,834 slices), PNG/WAV green.  MEASURED
  (5+5 interleaved): 354.66 vs 357.58 = -2.91 (-0.81%), worse
  in every pair.  THE FIQ-EXIT LEVER IS CLOSED FOR REAL, both
  shapes negative: per-site -1.42%, shared-stub -0.81%.  The
  eliminated epilog + C-switch + re-entry is offset by the asm
  triple + redispatch re-fetch and the lost C-side locality.
  Kept in tree env-gated OFF (chain-v2 precedent).
  ENCODING TRAP (second of its class): hand-rolled SIB
  compares need BOTH the disp8 AND the imm8 -- count the bytes
  against the Intel manual form before trusting the emitter.
  EMISSION TRAP (third of its class): a raw E9 whose target is
  a stub assigned LATER in the same trampoline build must use
  the placeholder+fixup pattern (p_jle_budget style); an E9
  computed against a not-yet-assigned global jumps wild on
  first execution.
- Dispatch-loop table-base/entries stash (2026-08-30): replaced
  the per-iteration movabs+pointer-chase (&g_jit_table,
  &g_jit_table_entries) with once-per-slice stack slots
  ([rsp+16] base, [rsp+4] count).  All gates green, measured
  NEUTRAL (359.16 vs 358.80, 5+5 same-window).  Reverted: the
  dispatch loop is not the bottleneck the uop count suggests —
  ~4-6 saved uops of ~19 per iteration vanish into the arena's
  front-end slack, same lesson as the disp8/tail-compression
  traps.  ENCODING TRAP for any future frame-slot work: the
  24-byte frame has NO two disjoint free 4+8-byte slots above
  [rsp+16] (an 8-byte base at +16 collides with a count at +20 —
  found via a smeared rdx = 0x0014_0000_f35f_f010 segfault); the
  free pad is [rsp+4..7].  Growing the frame sub-24->32 flips
  the X_FIQ call-alignment parity — do not.
- INVALID opera_arm_engine value SILENTLY FALLS BACK TO CACHE
  (opera_lr_opts.c:679-684: unknown string -> opt_set(1)).
  Measured 2026-08-30: a three-engine loop using
  --option opera_arm_engine=hoist (not a valid choice; the env
  var was supposed to carry the difference) produced a phantom
  "+27 fps hoist, 99.0% of cache" headline — the lane was
  running CACHE twice.  Valid choices: cache/jit/interp ONLY.
  The hoist is enabled by OPERA_JIT_FIQHOIST=1 WITH
  opera_arm_engine=jit.  Cross-check any surprising engine
  result against metrics.json core_options values before
  believing it.  (Same confound class as the phantom +26%
  chain claim: always verify which engine actually ran.)
- Chain v2 x FIQ-hoist interaction (2026-08-30): tested the
  composite (OPERA_JIT_CHAIN=1 + OPERA_JIT_FIQHOIST=1) 5+5
  interleaved vs hoist-alone: 356.21 vs 362.64 median (-6.43).
  The chain emission never carried per-word FIQ checks (chained
  words run their FIQ test inside the C helper), so the hoist
  does not remove any chain cost -- the two levers do not
  compose; chains lose the same ~6 fps with or without it.
  Interaction question CLOSED.
- FIQ-poll hoist to block entry (OPERA_JIT_FIQHOIST, 2026-08-30,
  NOW THE DEFAULT since bcbed6b+1): +0.96% median first matrix,
  +0.98% re-measured (362.80 vs 359.28); default-on build
  re-verified 93.5% of cache (363.52 vs 388.60, 4 clean reps).
  OPT-OUT: OPERA_JIT_FIQHOIST=0 restores the per-word shape.  SEMANTICALLY PROVEN EXACT in the
  default config: with the DSP worker thread off, CLIO_FIQPEND
  cannot change mid-block (async writer = the DSP thread only;
  all other writers run between slices or inside C-stepped words
  whose own tails deliver it).  Proof: 9,775,834-slice lane trace
  byte-identical to the cache engine + PNG/WAV oracle-identical.
  Implementation: prologue entry test (cmp [rbp],0 -> C-step
  word 0) + per-word tail FIQ test elided; emission keyed on
  opera_lr_dsp_threaded_active() snapshot at compile time, with
  opera_arm_jit_dsp_thread_refresh() flushing resident blocks at
  the core-option toggle site (opera_lr_opts.c).  Per the
  pre-commitment for this lever: real but marginal (density
  levers measure below their uop arithmetic) -- shipped
  DEFAULT-ON at a5c9df0 (opt-out OPERA_JIT_FIQHOIST=0); no
  variant iteration.  TRAP: opera_lr_dsp_*.ic edits need
  `rm -f opera_lr_dsp.o` (make has no .ic deps).
- BDT cstep gate-widening: census-closed (2026-08-30, stats
  build 1800 frames), with a CORRECTED mechanism note (an
  earlier revision wrongly called the 88% "MMIO hardware
  registers"; the value/pc histograms show otherwise).
  Of the 416,708 BDT C-steps: 365,772 (87.8%) have base
  registers at 0x300000-0x33FFFF -- the GUEST STACK region
  above RAM_SIZE (default DRAM 2MB + VRAM 1MB = 0x300000),
  outside every JIT dispatch window.  The dominant words
  (E898000F/E88E000F, 139,528 each = ldmdb/stmia r0!,{r0-r3}
  paired push/pop) ALL execute from ONE 4KB pc page (0x2000-
  0x2FFF, 279,056 events): a single hot kernel save/restore
  site paying the exit+re-dispatch per iteration.  Unconvertible
  TODAY because the store gate only knows the DRAM window; a
  future stack-window addition to the store-gate range would
  convert this mass -- fixable in principle, unlike true MMIO.
  The 50,936 (12.2%) DRAM-based events are stmdb/ldmdb
  rn-in-list pre-decrement shapes (d7aa47a rejects) plus ~37K
  EQ-condition LDM-returns (r15 in list = control flow).
  Arithmetic unchanged: full conversion of every shape is 28
  events/frame x ~60cy = 0.07% of a frame -- two orders below
  the noise floor, so the lever stays CLOSED on value, not on
  impossibility.  Reconciles with the NP-routing verdict for
  the aggregate mass.  (Census instrumentation: class-8 word
  slots + USER[rn] region histogram + top-word base-value and
  pc-clustering histograms in arm_jit_cstep, stats build only,
  commit a79840b.)
- in-block C-step chaining v2, commit 2e27a74 (KEEP, env-gated
  OFF by default; measured ~-1% when ON): the rewrite-from-spec
  succeeded where the original diverged.  DP(full)/SDT(full)/
  BDT classes chain byte-identically to the cache engine through
  9.78M slices (dual-lane CPU-hash tracer, pinned seed
  0xdeadbeef), battery/replay/PNG/WAV all green.  THREE bugs
  found via the tracer, all fixed: (1) the chained word's budget
  check was missing entirely (4-cycle slice overshoot, diverged
  at slice 1.76M); (2) the helper must set USER[15] = pc_k+4
  before running the handler (the ARM_SWAP pc-dance and the exit
  stubs re-read it); (3) SDS/SWP chaining diverges
  deterministically (one extra word executed at a budget
  boundary, slice 1760253 pinned-seed) — mechanism unresolved
  after full instrumentation (charge parity proven, per-slice
  rets equal, both lanes clamped at budget; the block-boundary
  vs per-word-check accounting at the swp+mov-pc,lr spin loop
  shifts the boundary by one word).  SDS excluded from the gate
  (0.7% of csteps).  PERF: chain-on 357.7 vs chain-off 361.3
  median (5+5 interleaved, taskset, loaded-host window; the
  ~780K chainable csteps = 433/frame x ~30cy theoretical <
  0.6% of frame, below the block-bytes growth it costs).
  The instrument is the durable artifact: OPERA_JIT_TRACE_LANES=
  <path> traces per-slice (seq, ret, USER[15], CPSR, CPU FNV
  hash, next-word, last-word) through ALL THREE engines from
  opera_arm_execute_slice; the baseline comparison (cache vs
  clean-jit) is byte-identical over 1500 frames — any future
  lever can be verified per-slice, not just per-frame.  Use
  opera_random_seed=0xdeadbeef for cross-run determinism.  The
  chain engages with OPERA_JIT_CHAIN=1 (+OPERA_JIT_CHAIN_CLS=<n>
  narrows to one class for bisects).
  [2026-08-30, reachability audit] VACUITY TRAP: on the current
  tree the chain gate is UNREACHABLE BY CONSTRUCTION — the
  compile switch now has an explicit case arm for every class
  with census mass (SDT_IMM->literal emitter, NP pair->generic
  emitters, BDT->oj_emit_bdt), and the classes with no case
  (BRANCH/SWI/SPEC/UND/MRS) are exactly those the chain gate
  itself rejects (its switch default: return 0).  A CHAIN=1 run
  today emits words=0 and any "trace-identical" result from it
  proves nothing (this exact vacuous green was caught in time;
  stats-build run.log "jit-chain: words=" is the mandatory
  engagement check before believing any chain gate result).
  OPERA_JIT_CHAIN_CLS cannot produce engagement either — no
  CLS value can reach the gate.  The v1 frame-1459 divergence
  question is moot for v2 (the rewrite-from-spec never had it).
  Combined with the measured ~-1% ON and the <0.6% ceiling,
  the lever is closed on both value and reachability; any
  future chain experiment must first re-route classes into the
  default arm by design, not by env knob, and re-derive the
  ceiling against the current cstep census (the ~780K/433-per-
  frame figure described the narrower-switch era).
  METHODOLOGY NOTE on a false "+26% chain win" seen during the
  hunt: traced runs (tracer on, full 1-1500 window) taken in
  different host-load windows showed chain-lane ~245fps vs a
  cache lane at ~193fps.  The windows were IDENTICAL (both
  benchmark_start_frame=1, 1500 frames) -- the confound was
  cross-engine comparison plus host-load drift (machine load
  ~15 varies by minute).  Controlled same-window tracer-on pair
  at pinned seed: jit 183.2 fps vs cache 193.1 fps (cache
  faster, as the benchmark medians say).  Never compare runs
  taken in different host windows, with or without the tracer;
  the only valid numbers are same-window interleaved pairs.
- RMW fusion for DP-imm words (KEEP, committed): S=0, rd==rn,
  rot==0, opc in {AND,EOR,SUB,ADD,ORR,BIC} compiles to one
  `81 /r [rbx+OJ_U(rd)],imm32` uop instead of ~11 (mov eax,imm +
  3 dead carry-materialization uops from oj_get_arm_c_r10 + mov
  esi,[USER] + ALU + store).  Census: 40,286 fusible sites of
  75.9K emitted words (53% of DP-imm); arena gains 17,448 real RMW
  sites (add=1202/sub=545/or=159/xor=21 new classes; and shares
  oj_and_cpu's 0xA3 form).  Measured NEUTRAL (-0.04 median over
  5+5 interleaved) -- the fusible words are scattered one-per-
  block setup (avg 3.84 words/block), not inner-loop bodies; hot
  loops use S=1 (subs) or rd!=rn shapes.  Kept: strictly less
  work, byte-identical, all gates green.  ENCODING LESSON from
  the build: the disp32 idiom (81 <0x83|ext<<3> <OJ_U(rd)> <imm>)
  must follow oj_and_cpu/oj_st_cpu exactly; the reg field is the
  x86 /r extension (ADD=0 OR=1 AND=4 SUB=5 XOR=6), and BIC is
  `and [..],~imm8`.  CORRECTION of an in-session misdiagnosis:
  a first-attempt disp8 variant (`81 <xop|0x43> <rd*4> <imm32>`)
  was ALSO a valid USER[rd] encoding -- USER[16] is the FIRST
  member of arm_core_s (opera_arm_core.h), so offsetof(USER)==0
  and rd*4 == OJ_U(rd); it passed every gate before being
  restyled to the disp32 house idiom.  An earlier revision of
  this row wrongly claimed it would corrupt CASH fields.  This
  closes the last per-word uop class; see the frontier analysis
  for why only density levers remain.
- r11 as DRAM-base pin (freed by the carry_out store removal):
  measured -1.29 then -2.85 fps over 3+3 and 5+5 interleaved
  matrices vs a rebuilt baseline (442.71 vs 439.86, spreads 0.5-
  0.8 -- real, not noise).  Replacing the 19,199 per-site
  RIP-relative `mov rdi,[rip+&DRAM]` loads with a per-activation
  pinned r11 (2 uops x 7.7K activations) SHOULD win 1 uop per
  DRAM-touching word execution, but loses consistently -- same
  layout-sensitivity class as the disp8 trap.  Two encoding bugs
  found en route (both crash-visible in <60 frames, both worth
  remembering): (1) the BDT pc-slot store (STMDB sp!,{..,lr}
  epilogues) is a SEPARATE emission site from the register-slot
  loop and must be converted with it or it writes through wild
  rdi; (2) SIB 0x14 is scale00/index rdx/BASE 100 = rsp-or-r12-
  with-REX.B, NOT r11 -- [r11+rdx] needs SIB 0x13 (base 011 +
  REX.B).  The crash fingerprint for (2) is a store through r12
  holding the cycle accumulator (r12=0xa, si_addr near 0xdff0).
  Reverted completely; the tree keeps only the carry_out kill.
- carry_out dead-store removal (KEEP, committed): 43,254 stores
  of the file-static carry_out global per arena (57% of emitted
  words, 3B each) plus the r11=&carry_out trampoline pin deleted.
  Proof of deadness: every reader is an ARM_SET_C(carry_out) in
  the DP tails, each immediately preceded in the same C call by
  an ARM_SHIFT_NSC write; ARM_ALU_Exec reads ARM_GET_C (CPSR),
  never the global; the classifier keeps RRX SDTs full-class.
  Zero [r11] loads existed in the arena (the 16 byte-pattern
  matches are mid-instruction immediates).  Perf: +0.43 median
  over 5+5 interleaved (within +-0.7 noise = neutral), kept for
  the dead-code removal and the freed r11.  Gates: battery 0,
  replay md5 jit==cache (utest's replay print dropped its
  carry=%08X field -- test-side observability, not an engine
  contract; the C engines still store the global), PNG/WAV
  identical.  Stats boot check healthy (compiled=45,580,
  cstep=1.97M).
- inline slow-path SDT loads (call arm_jit_sdt_ld_slow from emitted
  code instead of exit+C-step+re-dispatch): measured -11.3 fps
  (431.8 vs 443.1 median, 5-rep) -- the worst lever of the session.
  The cstep census said 786K T_SDT_LDI + 360K T_SDT_STI slow
  events looked like the biggest addressable block after perf
  profiling (arena 36.1% + cstep 2.2% + slice 4.4% + touch 1.4% vs
  cache's 31.9% arm_execute_slice; the other ~66% is engine-
  independent MADAM/DSP/video cost).  Implementation reached full
  correctness: PNG f300/900/1500 byte-identical after fixing two
  real bugs -- (a) the abort link register: arm_data_abort reads
  USER[15]+4 for r14, so the helper must set USER[15] = word pc
  before aborting (mirror of arm_h_sdt_imm's pc_tmp restore), and
  (b) the abort exit needs the DP_PC-style NON-materializing tail
  (a materializing oj_tail writes USER[15]=pc_k+4 on the FIQ path
  and destroys the abort vector 0x10).  A residual WAV divergence
  survived both fixes (faint 0x7f DC hum from 8.49s onward where
  the clean build is silent; abort fallback to cstep did not cure
  it, so the common path carries a cycle-level audio-DMA timing
  delta) -- but the lever is closed by the benchmark, not the
  divergence: 1.89M helper calls/340 frames make the per-call cost
  (movabs target + movabs r11 re-pin + stack dance + external call
  + branch tree) exceed the exit+cstep+re-dispatch round trip it
  replaced.  The lesson generalizes the session's rule: on this
  content, BOTH byte-shaving AND call-substitution levers lose;
  only density reduction (contract changes) can move the 36%.
- register forwarding, CORRECT implementation, fully gated (rn<-
  edx site only, exact DP class set): still a measured LOSS.
  After fixing every correctness layer of the earlier attempt --
  (a) the enum-range bug: the producer check must be an exact
  class set, not [DP_IMM..DP_RMPC], because the enum interleaves
  MUL/BRANCH/SDT between the DP entries and an SDT store's
  emission clobbers eax with the store address; (b) the producer
  wiring must run AFTER each word's emission (state describing
  the just-emitted word), not before; (c) only the rn<-edx half
  is safe -- the rm<-eax half still trips a runtime self-check
  (int3) at e.g. MOV r7,r1 after MOV r1,#9, with USER[1] holding
  a value the in-block producer did not write on that activation;
  root cause not fully localized -- the rn half alone is proven
  correct by a 1500-frame runtime self-check plus all gates --
  the result is a -1.14 fps median regression in an interleaved
  A/B on the same host window (clean 442.68 vs fwd 441.54, n=3+3;
  earlier batches agreed: 442.31 vs 441.46).  Engagement was
  real: stats build fwd_hits=46,790 over 1800 frames with
  compiled=45,331, cstep=1.97M (healthy, not the 13/76M wedge).
  Verdict: per-hit uop saving (~1 instr x 46K hits) is too small
  to pay for the extra producer-state work in the compile loop
  and the block bytes it adds.  DP->DP register forwarding is
  now CLOSED as a measured dead end, not just a buggy one.  The
  tooling that proved this is worth keeping in mind: an
  env-gated runtime self-check (OPERA_FWD_SELFCHECK) emitting
  "cmp reg,[rbx+rn*4]; jz ok; int3" inside the forwarded path,
  read under gdb, localizes the first wrong forward exactly --
  use it for any future value-carrying optimization.
- register forwarding for adjacent DP word pairs (two commits, both
  reset away): a two-layer failure.  (1) The producer-state wiring was
  accidentally placed inside the compile loop's #ifdef
  OPERA_JIT_STATS block, so production builds never set the
  forwarding register -- the measured "+0.75/+0.32" medians were
  host-noise drift on inert code, and every byte-parity gate passed
  vacuously (the forwarded path never executed).  (2) When the ifdef
  was fixed to actually activate it, the opc5 result-register table
  was wrong (opc5 is the 5-bit opcode:S field, not the 3-bit opcode:
  SBC/RSC were forwarded from eax while their results live in edx)
  and boot wedged deterministically -- visible FIRST in the stats
  build (the only lane where the code had ever run), fingerprint
  compiled=13 / cstep=76M / arena=2.4KB stuck polling E4810004/
  E4930004/E25EF008.  The census coverage is real (35.6% of the
  301,623 DP->DP adjacencies have prev rd == next rn; 18.1% rd ==
  rm).  [SUPERSEDED: the correct retry was done and the lever is
  now CLOSED -- see the "CORRECT implementation" entry above;
  interleaved A/B measured -1.14 fps despite full correctness.]  Process
  lesson: a performance change whose gains appear while the
  mechanism is provably inert is noise wearing a win's clothes --
  verify the feature ENGAGES (e.g. a counter on the forwarded path)
  before believing the benchmark.
- O3+LTO build lane re-measured on the current tree (post
  RIP-relative/NP/sentinel): ratio 0.925 (jit 441.1, cache 477.0) vs
  O2's 0.904 (441.4 vs 488.5).  The ratio gain is an ASYMMETRY, not a
  jit win: jit absolute is unchanged (the arena is runtime-generated,
  LTO cannot touch it) while LTO costs the cache engine -11.5 fps
  (code layout/inlining regressions in the C dispatch loops).  Best
  configuration of each engine still has cache ahead; a user choosing
  O3+LTO gets a slower core overall.  Record, do not chase.
## Post-RMW census analysis (2026-08-30, post 35b0caa)

Cstep-class census (1.97M csteps/1800 frames): [23] T_SDT_LDI
786K (39.9%), [8] BDT 417K (21.1%), [25] T_SDT_STI 360K (18.3%),
[3] SDT_IMM 150K, [4] SDT_RI 101K, [2] DP_RS 110K, [10] SWI 32K.
The dominant classes are ALREADY tight-classed and compiled --
these csteps are RUNTIME slow-path exits, not compile-gate
rejections: the top cstep words (E5900000 ldr r0,[r0] x204K,
E4930004 ldr r0,[r3,#4]! x9.6K, E5801000 str r1,[r0]) are MMIO
register traffic (CLIO/CDROM/MADAM pokes and polls) whose
addresses legitimately fail the DRAM range gate.  Widening
tight gates cannot help these words; the -11.3 inline-slow lever
was the correct response to this census and it lost.  Bounded
value of perfect MMIO handling: 786K LDI csteps/1800 frames =
2311/frame x ~46cy = ~4.7% of a 2.27ms frame.

The flag-gated FIQ-poll hoist (threading-aware emission): the
ORIGINAL refutation recorded here ("guest MMIO stores call
CLIO_FIQPEND_UPDATE() inside the slice, so the per-word poll is
required even with DSP threading disabled") CONFLATED two
things and is SUPERSEDED.  FIQPEND can change mid-slice (true --
via guest MMIO stores in C-stepped words), but those raises are
delivered by the C-STEP'S OWN TAIL (arm_jit_cstep checks FIQ
immediately after the word that raised it), never by in-block
polls.  The in-block poll only matters for FIQPEND changes
DURING block execution, which in the default config can come
only from the DSP worker thread.  The threading-gated hoist
(OPERA_JIT_FIQHOIST) therefore SHIPPED (b0eac6f) and is
proven safe by trace parity (9,775,834-slice lane trace
byte-identical to cache).  The stats-build provenance counter
(jit-fiqpoll, commit b62a08a) is CONSISTENT with zero
mid-block flips -- 78,862,686 in-block polls fired over 1500
frames against 78,959,300 dispatches with FIQPEND already set
at entry; firings sit below the entry-set count, but since the
ISF-masked/other-path dispatch count M is unmeasured, the
subtraction BOUNDS (K <= M - 96,614) rather than derives K=0.
Magnitude note: the probe run's own jit-exits line shows
fiq=78,862,723 (~52.6K/frame; every firing exits via
JIT_X_FIQ, internally consistent with the poll counter), while
the historical block-economics census recorded fiq=8,376 -- a
different build era in which FIQ exits did not surface to the
C loop this way; today's number is the terminus FIQ storm under
CLOCK_STEP=32 slicing.  The poll's not-taken path (1
load + 1 je) is minimal; the batching that remains is a
timing-contract change.
Every angle this census opened confirms the frontier statement
below.

## BUDGET-PREFIT experiment (2026-08-30, afternoon session) — RESOLVED: exact after the class fix, measured -0.56% (N=12, clean build; the stats-build -2.82% was instrumentation-inflated), parked

Design: at block entry, compare the block's static worst-case
charge (sum of per-word SCYCLE + shape extras: DP_PC +3, DP_RS
+1, SDT +3 variants, BDT +n+5/+n+2, MUL +16) against the live
remaining budget; if it fits, the per-word budget cmp/jl pair
inside the block is provably dead and gets elided (guard:
`cmp r13d, imm32; jl entry0` — r13d is the live remaining,
reloaded from [rsp] before every dispatch, verified at the
trampoline's line ~355).  Blocks that do not fit bail to the
proven C-step word 0 path.  Backward-edge blocks (internal
loops) are re-emitted once without elision via a goto-retry in
arm_jit_compile — the in-block loop's cumulative charge is
unbounded, so elision there moves the stop word.

Machinery shipped env-gated (OPERA_JIT_PREFIT=1, default off):
per-block gate s_oj_blk_prefit, charge accumulator s_oj_charge,
guard emission + imm32 backpatch (buffer s_oj[OJ_BUF], the
emission buffer), retry loop, diagnostic knobs
OPERA_JIT_PREFIT_LIM (elide only blocks with static charge <=
lim) and OPERA_JIT_PREFIT_ALLBAIL (patch imm to always-bail —
guard+cstep machinery with zero elision).

Forensics (all 1500-frame runs, tracer OPERA_JIT_TRACE_LANES
vs the cache engine's trace, 9,775,834 lines):
- PREFIT=1 (full): trace diverges at line 123,887; WAV diverges
  (1.59M of 4.41M bytes) though PNGs f300/f900/f1500 stay
  byte-identical.  jit slice 123,886 consumes 36 cycles and
  lands at USER15=0x844; cache consumes 29 and lands at
  0x27B0 — the jit ran 7 cycles past the cache's stop word.
- PREFIT_LIM=1/2/4/8/16 (only tiny blocks elide): SAME
  divergence at the same line — even single-word-block elision
  diverges, which the exactness argument says is impossible.
- PREFIT_ALLBAIL=1 (guard emits, always bails, zero elision,
  everything C-steps): trace byte-IDENTICAL to cache.  The
  guard/bail/cstep/redispatch machinery is exact; the elision
  is the divergence.
- PREFIT unset (opt-out): PNG/WAV oracle-green (control).

Root cause not localized: the divergence survives every
elision-scope bisection at the same line, including LIM=1 where
the only elided blocks are single-word DP blocks whose stop
semantics are provably identical to the xnext fold.
CHARGE-AUDIT CORRECTION (same session, post-park): the
"accounting is airtight" claim was factually incomplete --
two hand-rolled tails (oj_emit_dp rd==15 path and oj_emit_bdt
ldm-pc path) each emit a runtime SCYCLE 'add r12d,1' with no
s_oj_charge counterpart, undercounting those blocks' static
charge by 1 (guard invariant violated if prefit ever ran on
them).  Fixed with s_oj_charge += 1 beside each (audited: all
three 'add r12d,1' sites now counted; the third was oj_tail
itself).  Cannot explain the LIM=1 divergence (both classes
have sc_ >= 4, always bail at LIM=1, their per-word checks
never elided) -- re-verified: prefit still diverges at 123887
after the fix; default trace parity + oracles green.
NEW INSTRUMENTATION (in tree): the lane tracer carries a
budget column (g_lane_budget) and the stats build has
OPERA_JIT_PATCHDUMP (per-block guard bytes + patch value).
The budget column pinned the divergence signature: slice
123886 budget=29, cache consumes 29, jit consumes 36 (+7) —
one admitted block overshooting by exactly 7 — and the caller
budget diverges from slice 123887 on (25 vs 32).
DEEPEST FORENSIC RESULT (pair isolation, same session): the
elision-gate was extended with OPERA_JIT_PREFIT_IDXLO/IDXHI
(per-block-index elision), and an index bisection over the
65,577-block corpus converged to an EXACT TWO-BLOCK PAIR:
eliding ONLY blocks idx 2531 (pc 0x278C, sc 25) and idx 2532
(pc 0x2790, sc 24) reproduces the full 123887 divergence (+7
at slice 123886, budget 29).  Either block alone: IDENTICAL
to cache.  The region 0x278C..0x27AC is a prologue ladder
(mov r12,sp; STMDB sp!{r3,r11,r12,pc}; sub; STM; movs; ldr;
ldr; mov) terminating at the untranslatable LDR-pc sentinel
(0x27B0, idx 2540).  Blockdump (OPERA_JIT_BLOCKDUMP, stats
build) verified BOTH blocks are internally exact: guard
present with correct imm32 (25/24), every charge emitted and
counted (STM-pc word: add r12d,6 + add r12d,1; the ladder
sums match sc_ word-for-word).  The divergence requires BOTH
blocks elided simultaneously — a cross-block interaction
(bail-chain re-entry of 2532 from 2531's guard bail is the
prime suspect) — but every statically-readable path (C-step
tail order, FIQ fold, re-dispatch arithmetic) checks out.
The mechanism remains unlocalized; the pair is the minimal
reproducer.
MECHANISM FOUND (same session, continued): the pair isolation
plus OPERA_SLICE_DBG (per-trampoline-call reason/total/budget
logging) caught the divergence in the act — the pair run exits
slice 123887 with reason=X_CSTEP at total==budget==29 where the
default exits X_BUDGET: the guard-bail/sentinel C-step path
ran a word (the LDR-pc at 0x27B0, then into 0x844) past the
exhausted budget.  THREE fixes landed from this hunt:
  (1) slow-exit exclusion (s_oj_blk_slowexit, 6 sites): a word
      taking an l_slow exit skips its emitted charge — the
      block's static sum undercounts; such blocks now retry
      without elision (moved the divergence 123887 ->
      1,720,774);
  (2) X_CSTEP budget pre-check in the C dispatch loop (mirrors
      the cache's per-word pre-test and the dispatch loop's
      [rsp] jle gate; inert for the default build) — moved it
      1,720,774 -> 2,771,212;
  (3) both verified inert for the default build: trace parity,
      PNG/WAV oracles, battery, replay all green.
A fourth divergence site remains at 2,771,212 (+2 words same
total — another rare undercount shape).  INDEX BISECTION of
site 4 (the earlier bisects only covered [0,65536); the
site-4 blocks live at idx ~114,9xx = the 0x703D0-0x70490 DRAM
ladder) converged to a minimal set of [114924, 114929) =
blocks 114924..114928 — the SAME SHAPE as site 1: an
overlapping-suffix prologue ladder feeding an untranslatable
jump, where eliding any strict subset is identical but the
full set diverges.
CENSUS CORRECTION + COMPLEMENT VALIDATION (final): on the
fixed binary, site 1's pair [2531,2533) is now IDENTICAL —
it was fully explained by the two landed fixes (slowexit +
pre-check), NOT a separate interaction class.  Complement
tests around the site-4 set ([0,114924) and [114929,end)
both green) exonerate the rest of the corpus.  The remaining
divergence, captured end-to-end with OPERA_SLICE_DBG at slice
3206988: the 5-block set elides; a dispatch with remaining <
sc_ bails to C-step word 0; the re-dispatch compiles the
SUFFIX block (same idx range — elided); a fitting suffix
block then runs with elided checks and its fold leaves
[rsp] > 0 past the true budget crossing, so following blocks
run words past the boundary (+1 word at 3206987, budgets
diverge from 3206988 on).  The undercounting word inside the
suffix block is not yet identified — the same shape as the
fixed slowexit class but in a block whose emission has no
l_slow exit.
DEEP-DIVE RESULT (final for this session): the site-4 set's
blocks are the 0x703B0-0x703C0 suffix ladder with sc_
56-59 (blockdump) — every one ALWAYS bails (sc > 32-budget),
so their elided checks are runtime-inert; the divergence
carrier is the bail-chain SHAPE itself (the default runs the
ladder as one 4KB block with per-word checks; the prefit run
compiles five overlapping suffix blocks that all bail and
C-step word-by-word).  Both paths are individually charge-
exact (the default's green trace proves the block charges;
the allbail run proves the cstep charges), yet the
composition measurably shifts one stop word at 3206987.
Remaining suspects eliminated: DP_RS/branch/MOVCS charges
(source-audited vs the C handlers — equal, as the default's
trace parity proves for every emitted class), the rst
swallow (identical shape in the cache's C tail), and
mid-block FIQ (the hoist census: zero mid-block FIQPEND
flips).  What would settle it: a per-word runtime tracer on
the chain (an instrument not yet built).  The lever stays
parked, default-OFF, with every landed fix trace-green.  Net: TWO mechanism classes remain possible
(suffix-block undercount vs composition); the lever stays
parked with the exact reproduction chain recorded.  Three mechanism classes total: slowexit
charge-skip (fixed), missing cstep budget pre-check (fixed),
and the multi-block bail-chain composition (unfixed, present
at both remaining sites).  The park verdict is
UPGRADED but unchanged: the elision's exactness requires a
charge-composition invariant across bail/cstep boundaries that
the default build never exercises; it fails at (so far) four
distinct sites in 9.78M slices.  The forensic tools
(OPERA_SLICE_DBG / OPERA_SLICE_DBG_WIN / the cstep window
override) are in tree under OPERA_JIT_STATS.
METHODOLOGY CORRECTION (post-park audit): the original trace
matrix compared runs with different time-based seeds; the
pinned-seed re-run (opera_random_seed=0xdeadbeef on every
lane) reproduces the exact same result — default-jit ==
cache byte-identical, prefit-jit diverges at the same line
123887, prefit-jit deterministic across two runs — so the
seed artifact theory is refuted and the park's conclusion
stands with clean controls.
Unbound-label mechanism CLOSED (same session): oj_resolve walks
the fixup list only, so the elided tail's allocated-but-unbound
l_nobud cannot shift other fixups — and the direct experiment
(binding l_nobud unconditionally to the fallthrough, zero
emitted bytes) still diverges at 123887.  The bind is kept in
tree as a defensive zero-cost invariant (every allocated label
bound); default gates and trace parity re-verified with it.
imm8 bounds re-verified: max oj_charge_extra extra is BDT n+5
= 21; MUL is inline-accounted; imm8 safe.  Something
in the elided-tail interaction still moves a slice boundary by
7 cycles at one site in 123K slices.  Suspects not excluded:
the oj_tail label-pool perturbation (l_nobud allocated but
unbound/unreferenced under elision), an oj_charge_extra imm8
edge, or a C-tail-vs-emitter charge mismatch in one class.
RESOLUTION (same session, final): the class-level fix the
forensics pointed at — OVERLAP EXCLUSION (any block whose
entry word is already covered by an existing block retries
without elision; s_oj_blk_overlap consulting g_jit_word_cov
pre-self) — closed ALL remaining sites at once: full prefit
now traces BYTE-IDENTICAL to cache over all 9.78M slices and
passes every oracle gate (PNG f300/f900/f1500 + WAV).  The
lever was then MEASURED for the first time on a correct
build: 5x5 interleaved, prefit-ON 349.21 vs OFF 359.35 =
**-2.82%** — measured on the OPERA_JIT_STATS build.
CLEAN-BUILD CORRECTION (post-advisory, materially important):
the stats build's instrumentation (its OFF median 359.35 vs
the clean O2 default's ~368 standing = ~2.4% overhead)
inflated the delta; the 5x5 ON/OFF matrix on the clean O2
build measures prefit-ON 356.71 vs OFF 358.28 =
**-0.44%** — at the noise floor on a 5x5.  POWERED-UP
MATRIX (N=12 pairs, clean O2): median -0.56%, mean -0.83%,
and EVERY one of the 12 paired deltas is negative (sign
test p ~ 1/4096) with the typical pair at -0.3 to -0.8% and
one -3.03% outlier.  Cluster structure: ON runs span
355.4-357.8, OFF runs 357.5-359.9 — near-zero overlap
(only the 357.5-357.8 sliver), i.e. the distributions
separate cleanly at ~0.5% magnitude: a small-but-real
negative, not noise.  (The single 347 ON rep from the first
5x5 did not recur — outlier confirmed.)  Mechanism unchanged: the overlap exclusion
removes the suffix-chain blocks (the hot divide ladders and
spin loops) from the elided population; the surviving elided
blocks are cold, and their 2-uop-per-word saving does not pay
the entry guard's cmp+jl.  FINAL VERDICT (corrected, N=12): exact
but a real ~0.5% cost on the clean build (12/12 negative
pairs — direction settled; magnitude ~0.5%).  No win to
ship, so the park stands.  Note for any future reopen: a
precise suffix predicate would shrink the excluded set, but
the excluded blocks are the HOT ones and the elided
population the COLD ones — widening cold elision cannot flip
a -0.5% cost into a win; only removing the entry-guard cost
itself could, which is the DECISION POINT lever (a)'s
territory.  The DECISION POINT levers remain the only path
to the budget-check mass.
Verification notes (post-advisory audit): the pair-set minimal
reproducer [114924,114929) is confirmed dead with the
exclusion in (trace-identical); the full-population and
pair-set runs agree.  KNOWN LIMITATIONS of the exclusion
(acceptable while parked, must be fixed if ever reopened):
(a) ROM blind spot — the check gates on table_idx_ <
g_jit_ram_words (ROM has no cov array), so a ROM suffix
ladder would not be excluded; (b) conservative blast radius —
any block entering at another block's interior word is
excluded (legitimate mid-block jump targets included), which
is why the hot population loss is larger than the pure
suffix-chain set; a precise 'is a suffix of a live block'
predicate would shrink the excluded set but needs word-range
comparison against live blocks (the cov refcount alone cannot
distinguish).  The DECISION POINT lever (a) below
(batching, which moves stops by design) remains the only live
path to the budget-check mass — it is a contract change and
stays user-approval-gated.

Robustness follow-up (same session): blocks containing a
chained C-step word are now excluded from prefit elision via
the same goto-retry (s_oj_blk_chained; arm_jit_cstep_chain's
packed charge is dynamic -- eax>>8 at runtime -- so a static
sum can never bound it; if a future run ever enables OPERA_JIT_
CHAIN and OPERA_JIT_PREFIT together, the block re-emits with
per-word polls instead of silently overshooting).  Default
config unaffected (both env-gated off); gates re-verified.

LTO re-measured on today's tree (post-afbe24e, O2 -flto,
3x3 interleaved): jit 365.45 vs cache 387.36 = 94.3% — a
regression vs O2's 94.4%; the LTO lane stays closed (the old
O3+LTO 0.733x-vs-0.702x relative gain was on the much weaker
pre-hoist engine; today's leaner host path leaves LTO nothing
extra to win).

x86 encoding traps ledgered this session: (1) `45 81 ED` is
SUB r13d,imm32 — cmp r13d,imm32 is `41 81 FD` (REX.B + /7);
(2) the prefit LIM knob's first version had inverted polarity
(sc_ > lim patched 0x7FFFFFFF = always-pass = MORE elision) —
the bail sentinel must be the huge value, the true charge the
pass value.

## DECISION POINT for the remaining 6.5% (2026-08-30, end of session)

The jit ships its best proven in-contract config (FIQ hoist
default-on) at 94.4% of cache (368.40 vs 390.36 median,
interleaved same-window; the afbe24e build with prefit +
chain-exclusion machinery present but default-off; measured
94.3% / 93.5% on the earlier trees -- same engine within
noise).  Every in-contract lever class is
measured and closed above.  The remaining gap requires ONE of
two timing-contract changes, each user-approval-gated because
slice partitioning is CLOCK_STEP-timing-visible:

(a) per-word budget-poll batching: let a block run to its end
    when its total charge provably fits the remaining budget,
    stopping at a block boundary instead of the exact word.
    Stops move by <= block_len-1 words; the budget arithmetic
    (CYCLES accounting, CLOCK_STEP line boundaries, CLIO timer
    ticks) must be re-derived against the oracle traces.
    Attack surface: 75.8K sites x 3 uops + 1.1M budget exits.

(b) CLOCK_STEP/line-slice ABI batching: raise or batch the
    32-cycle slice budget so the 6.5K slices/frame (and their
    per-slice dispatch shells) collapse into fewer, larger
    executions.  Changes when FIQ/vint/timer events land
    relative to guest instructions -- observable in audio/
    video timing unless events are re-based exactly.

LEVER (a) IS NOW BUILT AND MEASURED (2026-08-30 evening,
commit 48c9856): OPERA_JIT_BUDGET_BATCH=1 (default OFF) elides
the per-word budget check at three tail sites (oj_tail, the
TIGHT_DP_PC pc-write, the TIGHT_SDT_LDI pc-load) while KEEPING
the per-backedge-pass check on threaded loops (spin loops keep
their stop) and the chain-word check.  Stops land at block
boundaries via the existing X_NEXT fold.

MEASURED (5x5 interleaved, same-window, clean O2): batch=1
420.73 median (spread 1.13) vs control 367.69 (spread 7.43) =
+14.43%, 5/5 pairs positive.  Fresh same-window cache baselines
(3x): 391.56 median.  => jit+batch = 107.5% OF CACHE: the JIT
is FASTER than the cache engine by +7.5% on this content.

MECHANISM CONFIRMED BY THE TRACER: slice count 9,775,834 ->
8,579,402 = -12.2% (1.2M fewer dispatch shells), while sum(ret)
312,812,902 vs 312,812,896 (6-cycle dust over 312.8M): the
aggregate cycle budget is preserved exactly; only the stop
PLACEMENT moves (by design).  CPU-hash matches 618/618 on
pc-aligned slices — no state corruption.

GATES: utest battery done; 4M replay md5 equal both lanes;
knob-off control byte-identical (PNGs+WAV) — the edit is
default-inert; PNGs f300/900/1500 byte-identical WITH batch=1
(frame quantization absorbs boundary moves); batch=1 output
deterministic (WAV md5 9146703b9... stable across runs).

THE RESIDUE (why it stays user-gated): the per-slice trace
diverges by construction (99.99% of lines, first at slice 10 —
stop words moved); the WAV diverges from t=8.51s — magnitudes
and audibility profile in the AUDIO-RESIDUE DEEP DIVE below
(the first-pass "1-5 quantization steps" figure was the tail,
not the peaks; peaks reach 3425 LSBs).  Video: identical.
Audio: sample-level timing differences vs the exact-stop
contract.

AUDIO-RESIDUE DEEP DIVE (for the decision): the divergence
is NOT a constant shift and NOT inaudible noise.  Aligned-local
best shift is -1 sample but with nonzero residual error: a
wandering sub-sample phase drift that comes and goes as audio
DMA fetch boundaries move with the stop words.  Profile over
the diverged region (8.51s-1500 frames): delta/signal RMS
median 4.1%, p90 6.7%, max 27.7%; 30% of 100ms windows exceed
5%; 3 windows exceed 20%.  Peaks >1000 LSBs (max 3425 LSBs =
-19.6 dBFS, 53% of local signal) recur roughly every 0.5-1.5s
from 9.5s onward — transient onsets (SFX/drum) landing
slightly earlier or later than the exact-stop oracle.  Aggregate
audio energy is preserved (the content is the same; timing of
transient placement differs).  Verdict for the user: audible as
subtle timing differences on transient-rich content under
A/B; invisible in video (PNGs byte-identical).

CALLER-OVERSHOOT VERIFIED SAFE: opera_3do_process_frame's
cnt loop re-syncs to CLOCK_STEP multiples with the remainder
carried in g_FRAME_CYCLE_REMAINDER — an overshooting slice
return cannot displace line timing (confirmed empirically:
sum(ret) equal to 6 cycles over 312.8M).

FINAL-STATE NOTE: the trace's final CPU-hash differs (the last
slice's stop word differs — expected under moved stops); the
user-visible end state is the PNG, which is byte-identical.

KNOWN SCOPE LIMITATION: all batch=1 evidence is single-title
(terminus; the only ISO in the harness tree).  The +14.4%/+7.5%
numbers and the audio-residue profile are content-specific; a
second title should be run before treating either as universal
(no other ISO is available in B/ today).

BOUNDEDNESS: the divergence fraction is stable across the run
tail (~40% one-frame-shift match per 100KB block, no growth
9s-29s) — bounded drift, not accumulating error; sum(ret)
preservation to 6 cycles over 312.8M is the accounting-side
proof.

TRAJECTORY-RESIDUE NOTE (pc-support analysis): the trace's
slice-entry pc sets differ between engines (batch-only 2,006,
cache-only 17,987 pcs) — but this is a boundary-sampling
artifact of moved stops, not different executed code: the
exclusive pcs are interleaved at word granularity with the
shared support (median gap 4 bytes; a cache-only pc sits a
median 8 bytes from a batch-visited pc, p95 76, max 980 —
i.e. every exclusive pc is inside the same code neighborhoods,
just never landed-on as a boundary under batch partitioning).
The user-visible equivalence (PNGs identical, deterministic
output, aggregate cycles exact) remains the operative proof of
trajectory equivalence at frame granularity; the word-level
instruction stream cannot be compared directly because the
tracer samples slice boundaries only.
TRAJECTORY COMPARISON, EXACT FORMS RUN (post-advisory):
(1) hash-MULTISET equality over full lines is NOT a valid test
at this tracer's resolution — the trace records slice
BOUNDARIES, and moved stops sample different points of the same
trajectory, so state-multiset equality cannot hold by
construction (nor does it for any partitioning change).
(2) Word-value sampling: batch covers 70.4% of cache's sampled
word-mass with zero foreign code (all support differences
within shared neighborhoods, per the distance analysis above).
(3) The decisive available equivalence proofs remain
frame-granularity: PNGs byte-identical, per-second audio RMS
IDENTICAL to the oracle at t=10/15/20s (2309/2309, 1408/1408,
2204/2204 — 100.0% energy ratio), aggregate cycles exact,
output deterministic.  A per-word tracer mode would be needed
for a true instruction-stream diff; noted as the missing
instrument, not as evidence of divergence.
ORDERED-STREAM ALIGNMENT (the proposed definitive form, run and
found partitioning-sensitive): greedy alignment of the
(pc,entry-word) sequences matches only 0.02% — NOT divergence
evidence: the boundary columns SAMPLE the trajectory at 0.18%
density (11,439 words/frame sampled of 6.24M executed), and
batch's boundaries land at different pcs, so consecutive samples
differ by construction on any partitioning change.  The
boundary columns are samples, not the stream; the proposal's
premise (they capture every intra-slice transition) does not
hold for this tracer's two-words-per-slice format.
OVERSHOOT DISTRIBUTION (cycles-past-budget bound, from the
ret column): 50.9% of batch slices return >32 cycles; the
overshoot concentrates at +7 cycles (18.78% of all slices —
one SDT/writeback-shaped word past the boundary) with a long
thin tail through +12 and a maximum of +195 cycles (48.8 words,
a single BDT/LDM-shaped block end); cumulative <=+13 covers
35.0% of slices.  (Label per the interpretive note: this is
CYCLES PAST BUDGET, not stop movement — cache's own last word
overshoots too; see the stop-movement histogram below.)
STOP-MOVEMENT DISTRIBUTION (the direct measurement, replacing
the earlier proxy figure): Δret at matched cumulative-cycle
positions — each cache boundary (cumulative position from the
cache's own ret walk) is paired with the batch slice covering
that same cumulative position, and the pairing is re-derived
independently at every pair (no drift accumulation; final
cumulative positions 312,812,902 vs 312,812,896 — the 6-cycle
dust — and the max pairing offset is the 195-cycle max
overshoot itself, i.e. bounded by construction).  Over 2.0M
sampled boundaries: batch stops LATER than cache at 72.1% of
positions (median +18 cyc, mean +20.6, p95 +56), EARLIER at
13.9%, at the SAME word at 14.0%.  Overall |Δ| median 13
cycles = 3.2 words, p95 12 words, worst observed ~46 words —
all bounded by block length (~3.84 avg words, ~49 max here).
THE DECISION-ROW NUMBER: a typical stop lands ~3 words later
than the exact-stop engine (p95 12 words).  The earlier
"median stop movement ~7 cycles" figure was the cycles-past-
budget mode (the overshoot histogram above), NOT stop
movement — superseded by this paired measurement; the caller's
remainder mechanism absorbs all of it (sum(ret) exact).
CACHE RET>32 CHECK: 3.62M cache slices also return >32 (the
exact-stop engine legitimately exceeds 32 mid-word — a word
never splits); the batch figure is the same phenomenon widened
to block ends.  No runaway: max +195 cycles is bounded by
block length (blocks cap at ~49 words here).
SEED REPLICATION (opera_random_seed=0x0badf00d, full A/B on
the current tree): jit+batch vs cache — f1500 PNG byte-
IDENTICAL; audio RMS drift 67.5 LSBs (same signature class as
the pinned seed's 83.1); per-second audio energy ratio 100.0%
at t=10/15/20s (2713/2713, 1738/1738, 2003/2003).  The
result shape replicates across seeds: video identical, audio
content identical, transient placement drifts.
AUDIO-ENERGY ENVELOPE (the strongest audio-side equivalence
proof): per-second RMS is bit-stable equal between batch=1 and
the oracle at every measured second — the audio CONTENT is
identical; only transient PLACEMENT differs (the sub-sample
drift profile above).

THE DECISION — RULED AND SHIPPED (user's call, 2026-08-30):
default flipped ON.  OPERA_JIT_BUDGET_BATCH=0 is the opt-out
(restores the exact-stop shape and the exact-oracle WAV).
POST-FLIP VERIFICATION (all on the flipped build): default-ON
output reproduces the batch=1 determinism pin exactly (WAV
md5 9146703b9..., PNGs byte-identical); the OPT-OUT lane is
oracle-green against the exact-stop WAV (the opt-out restores
the old default's bytes exactly); battery done, 4M replay
md5s equal.  SHIPPING PERF (3x3 interleaved, same window):
jit default 425.40 vs cache 391.28 = 108.7% of cache
(+8.7%), 3/3 pairs positive.  THE JIT IS THE EMULATOR'S
FASTEST ARM CORE.

HOT getenv FOUND AND FIXED (2026-08-30, post-shipping perf
profile; the biggest single win of the campaign after batch=1):
perf on the CLEAN shipped build showed getenv 19.12% +
__strncmp_evex 7.22% + strncmp@plt 2.02% = ~28% of the whole
profile.  ROOT CAUSE: lane_trace_maybe_open (opera_arm.c:3242)
calls getenv("OPERA_JIT_TRACE_LANES") from opera_arm_execute_slice
on EVERY SLICE (~6.5K/frame, all three engines — the lane tracer
hooks the shared slice entry).  The strncmp samples are INSIDE
getenv (environ walk).  FIX: one-shot latch (static latched_,
first probe wins, the tracer is a boot-time instrument).
Tracer function preserved byte-identically (a trace taken on
the latched build equals the recorded bb1 trace exactly; PNGs
byte-identical; WAV == determinism pin; cache lane WAV ==
exact-stop oracle — the latch is engine-neutral); battery,
4M replay md5s equal.
MEASURED: jit 529-531 fps on the latched build (5x5 vs the
pre-batch-era baseline binary: 530.44 vs 366.09 = +44.9%
campaign-total on this content).  SAME-WINDOW jit-vs-cache
(both latched, 3x3): jit 529.33 vs cache 487.77 = 108.5% of
cache (+8.5%), 3/3 pairs.  NOTE the cache engine gained too
(391.5 -> 487.8: the getenv tax was on its slice entry as
well) — the latch is an emulator-wide win that happened to
land while the jit was ahead, preserving the +8.5% margin.
LESSON (profile-class): a debug instrument on a hot shared
path is a tax on every engine; per-slice getenv is invisible
to per-engine A/B ratios (both lanes pay it) and only shows
in an absolute profile.  Always perf-profile the CLEAN build
after shipping a big lever — the pollution was misattributed
to "stats build" on first read.
POST-LATCH FRONTIER (600-frame perf on the latched shipped
build), CORRECTED after a field-parsing slip: the JIT ARENA
is 27.26% of the profile across its many small [JIT] symbols
(the biggest single arena site is ~1.2%) — it IS still the
dominant mass, spread across blocks the way the arena census
always showed.  Next: execute_slice 7.66% (the C shell —
lever (b) territory, closed by the b-lite arithmetic), then
ENGINE-INDEPENDENT subsystem work (TexelDraw 7.22, dsp_loop
6.84, xbus_tick 6.64, process_frame 6.18, PPROC 5.01,
clio_timer 4.16, timer_queued 3.89, vdlp 3.47).  Read: the
arena remains the only jit-owned mass with headroom, and its
per-symbol spread (max 1.2%) means no single-block shortcut
exists — density reduction across emitters is the only lever
class left, which is the closed frontier statement.  The
8.5% jit-vs-cache margin stands on the shell+cstep advantage
already banked.
LEVER-CLASS CLOSURE NOTE (final, all advisory precedents
cross-checked): the FIQ-exit lever is closed by MEASUREMENT on
this exact workload (per-site -1.42%, shared-stub -0.81%, both
ledgered with trace parity before measurement) AND the current
window shows fiq=0 (the FIQ storm is gone).  The cstep-shell
trampoline-side idea is closed by the same arithmetic that
killed b-lite (net ~3 uops after mandatory pin reloads) plus
its own precedents (inline slow -11.3, chain v2 -1%).  No
C-round-trip-elimination lever will be built: the class has
three measured negatives and two arithmetic closures on this
workload.  The jit-owned space is exhausted at every level of
evidence — measured levers, arithmetic-closed levers, and the
post-latch profile (arena 27.26% spread with no site >1.2%;
execute_slice 7.66% = lever-(b) territory; all other mass is
engine-independent subsystem work).
CAMPAIGN RESTING STATE: jit 529.3 vs cache 487.8 = 108.5% of
cache; vs interp ~4.4x; campaign-total +44.9% from the
pre-batch baseline on this content.  The 8.5% margin is the
banked shell+cstep advantage; further jit gains require either
the user-gated full lever (b) (CLOCK_STEP raise) or engine-
independent work outside the ARM core's scope.

Lever (b-lite, trampoline-side line events) — DESIGNED AND
REJECTED BY UOP ARITHMETIC + INTEGRATION HAZARD ANALYSIS
(2026-08-30, post-shipping session; helper landed then reverted
clean): the design called opera_3do_internal_frame from inside
the trampoline's budget-exhaustion stub, keeping the line event
at the exact 32-cycle boundary and removing the epilog + C
return + caller loop per slice.  THREE integration hazards
(caught by code reading before first run, per advisory):
(1) the caller (opera_3do.c:234-238) drains its own while loop
after the slice returns — the return protocol would need a
sentinel/restructure to avoid double-firing line events;
(2) internal_frame -> vdlp_process_line writes guest memory and
can trigger jit invalidation hooks while the trampoline holds
live block state (the in-flight block or the already-loaded
dispatch target could be killed mid-run);
(3) line/field/scanlines live in the caller's frame — the jit
mirror must sync on entry AND fold back on return, or lines
double-advance.  All three are fixable (a third-file restructure
of process_frame's jit lane), but the UOP ARITHMETIC kills the
payoff before the hazards are worth braving: per 32-cycle slice
the C round trip costs ~35-45 uops, and the in-trampoline shape
must pay a pin reload after every line event (rbp=*fiqpend,
r14=*cdrom, r15=*fsm re-read because fiq_generate can change
FIQPEND mid-run) = 3 movabs + 3 loads + 2 frame stores ~= 17
uops, netting ~3 uops/slice — the EXACT shape of the shared-
stub xfiqcont lever that measured -0.81%.  The session's
repeated lesson: the C round trip is cheaper than its
instruction count suggests (icache-hot pins, tiny caller loop).
Verdict: closed by arithmetic + the C-loop's integration cost;
the remaining ~3.6% budget-re-entry mass is only addressable
by the full lever (b) (CLOCK_STEP raise = line-event-timing
contract change, user-gated) or nothing.

## Session frontier analysis (2026-08-30, post 0.904)

Arena-binary census of the final build: 16,789 cond prefixes (~9.7%
of words, ~1% of arena cycles) and 10,857 flag-snapshot sequences
(6.2% of words, ~3% of arena).  Neither is a lever.  The trampoline
already loops X_NEXT internally (dispatch only returns to C for
C-step/compile/interp/fiq exits), so there is no dispatch round trip
left to remove without contract changes.

Arithmetic of the remaining gap: jit frame 2.266ms vs cache 2.047ms
= 218us to find.  Arena+dispatch+cstep-shell ~906us of frame, of
which C-step MMIO work (~40%) is side-effect-legitimate (cache pays
equivalent work in its own handlers).  Addressable remainder ~500us;
parity needs 44% of it removed -- i.e. halving emission density
(~50 -> ~25 host instrs/guest word).  Every measured lever says that
halving requires word fusion (breaks the per-word FIQ-latency
contract) or the blocked line-slice ABI change.  Peephole space is
exhausted: byte-shaving provably loses (three traps), the remaining
uops are contract-mandated (gates, polls, budget, flags).
Post-frontier addition (2026-08-30): one more class existed --
runtime-fact elision of a contract-mandated poll, keyed on the
DSP-threading state (FIQ hoist: +0.96% first matrix, re-measured
+0.98% = 362.80 vs 359.28 in the valid same-window matrix;
committed b0eac6f default-off, proven safe by trace parity and
consistent with zero mid-block FIQPEND flips per the b62a08a
provenance counter).
That class is now closed too: the other two entry guards (cdrom
restart, madam fsm) are data-dynamic with no config fact to key
on; the chain composite does not compose (-6.43 measured).  With
the hoist the jit sits at ~0.92x cache (92.9% measured).
Remaining levers are the two contract changes (per-word budget-poll
batching; CLOCK_STEP/line-slice ABI
batching), both user-approval-gated in the options below.
- DP flag elision (skip oj_arm_set_cvzn/zn when the next word
  redefines all four flags before any read): census-measured DEAD END
  before implementation.  113,547 S=1 DP words emitted vs 420
  elision-safe adjacencies (0.37%): real code pairs flag-setters with
  conditional USES (branches/ADC), never with other flag-setters, so
  the only provably-safe peephole window is the one that never occurs.
  Broader liveness (flags live across cond-prefix reads) would need
  the block to end before the reader -- i.e. breaking blocks apart,
  which costs more than the ~26-instr flag sequence saves.
- disp8 forms for oj_ld_cpu/oj_st_cpu (USER[n] offsets < 256 fit a
  signed byte; 6-7 byte disp32 forms -> 3 byte disp8): measured
  NEGATIVE, -3 to -4.5 fps over two 5-rep matrices (437.9/436.4 vs
  442.3).  Same lesson as the 2026-08-29 tail-compression trap: the
  arena is NOT icache/fetch-bound on this content -- shorter
  encodings of the same uops do not pay, and here they plausibly
  hurt via decode-alignment shifts (3-byte instrs landing mid-
  fetch-window shuffle the 16B boundaries the previous layout had
  settled into).  Every byte-shaving experiment on this workload has
  now lost; only uop REDUCTIONS have ever paid (RIP-relative removed
  a load uop per constant, sentinel removed dispatch round trips, NP
  routing removed C-step round trips).
- trampoline RIP-relative conversion (5 entry pins, the dispatch
  movabs pair, xfiq call, epilog cycles store -- the same mechanism
  that won +2.0/+1.7 in the block bodies): measured NEUTRAL over 10
  reps (442.13 median vs 442.31; within the +-0.7% noise band, delta
  -0.18).  Reverted.  Read: the trampoline's movabs are not the
  per-run cost center the run count suggested -- 7.7K runs/frame
  amortize 50B of pin movabs into ~1% of frame time, and the
  decoders/uop tracks absorb 10B immediates at this occupancy better
  than the raw byte counts suggest.  (Encoding traps on the way:
  pins that need the ADDRESS -- rbx=&CPU, r11=&carry_out -- must use
  lea [rip+disp32], not a load; arm_fiq_vector needs call rel32 to
  the function, not call [rip]; and a post-copy patch loop must run
  AFTER the mmap, or it memcpy's to NULL+disp.)

- frame-slot constants for DRAM/RAM_SIZE (expand trampoline frame 24->40,
  fill [rsp+16]=*DRAM and [rsp+24]=RAM_SIZE once per run, convert all 16
  word-body sites: load/literal/BDT/store gates + DRAM base loads):
  measured NEGATIVE, -1.0 to -1.4 fps vs rebuilt baseline (5-rep serial
  medians: slots 437.0 vs base 438.5; means 437.1 vs 438.1).  The per-word
  movabs savings (~10B + 1 uop per memory word pair) are smaller than the
  per-run entry fill (+~38B on every one of ~7.7K runs/frame) plus the
  5B [rsp+0x18] store forms vs the 3B mov rdi,[rdi] they replaced, plus
  the frame straddling a second stack cache line ([rsp+0x20] cmps).
  Byte-parity and budget-16 probes were all green -- it is a pure
  performance loss, not a correctness problem.  Encoding traps found on
  the way (both cost hours, both now understood):
  (1) 48 8B 04 24 is `mov rax,[rsp]` (ModRM 00.000.100 + SIB
  00.000.100), NOT `mov rax,[rax]` -- which is 48 8B 00 (ModRM
  00.000.000, no SIB).  Emitting the 04 24 form silently loads the
  remaining-budget integer into the DRAM slot; the block body then
  dereferences it and SIGSEGVs with si_addr == the budget value (0x10
  for budget-16 probes -- a perfect fingerprint).
  (2) Block bodies run exactly one `call` deeper than the trampoline
  frame (the dispatcher's `call rsi` pushes the dead RA that exit
  stubs drop with `add rsp,8`).  From inside a body, the frame slots
  are at [rsp+0x18] and [rsp+0x20] -- NOT the [rsp+0x10]/[rsp+0x18]
  the trampoline itself uses.  Using the trampoline-relative
  displacements in body code reads the BUDGET integer as the DRAM
  pointer (same si_addr=budget crash) and the RAM_SIZE gate reads the
  DRAM base (a huge value), silently neutering the range gate while
  still passing utest replay -- only the harness PNG gates catch it.
  The stale header comment about a body-level `sub rsp,8` alignment
  (lines 32-36) describes the xfiq stub's call alignment, not a body
  offset; there is no block-emitted sub rsp,8 in the tree.
- tail/exit byte compression (jcc8 rel8 forms, cmp imm8, mov-imm32
  USER[15]): measured NEUTRAL (420.9 vs 422.5).  The arena is uop-bound,
  not icache-bound, on terminus; per-word bytes are not the lever.
  (The rel8 fixup infrastructure oj_jcc8 was removed with the revert --
  it does NOT exist in the tree; do not hunt for it.  If the experiment
  is ever retried, add a distinct resolve-reject counter for rel8
  range overflow at the same time so a silent block->C degradation
  cannot masquerade as an unexplained slowdown.)
- 8MB arena: 395.7 (-9%).  Recycling thrash dominates any footprint win.
- hot trampoline re-entry (skip the 6 pin movabs on renewal calls):
  CRASHES with rbx/rbp=0x41..41 poison.  The epilog's pops are paired
  with the cold entry's pushes per call: every return restores the
  CALLER's callee-saved state, so the pins never survive a return.
  Making pins survive requires a non-returning ABI change (abandoned).
- slice-loop budget fast path (compare the 3 break reasons before the
  switch): NEUTRAL.  The compiler already emits a tight switch; the
  per-call cost is the indirect call + spill, not the dispatch.
- g_jit_word_cov bookkeeping: kill MUST mirror register's RAM-window
  guard and wlast clamp exactly (table_index < g_jit_ram_words +
  arm_jit_index_pc round-trip), or ROM-block kills walk off the array.
- STM-with-pc semantics (probed, budget-16 differential): stm_accur
  masks the list to 0x7FFF but counts x from the full 16-bit mask (the
  pc slot reserves stack space), and stores USER[15]+8 = pc_k+12 as a
  compile-time constant at base_comp.  The post-transfers W writeback
  is STM-only: ldm_accur has NO post-loop writeback (its W store is
  pre-load and the rn slot load overwrites it) -- gate every post-loop
  writeback on !is_ldm.  The ldm-pc custom-tail cost block must also be
  gated on is_ldm (STM charges n+2, not n+5).
- the budget-16 probe (run probe2 with OPERA_PROBE_BUDGET=16 and check
  ret equality) catches cycle-accounting divergences the generic
  30-budget probe misses: a wrong charge shows as an extra guest word
  executed (ret 20 vs 16), not a DRAM-DIFF.
- PC-local slice extraction (-6%), whole-frame fusion (-15fps), FIQ-poll
  hoisting in its UNCONDITIONAL/naive variant (breaks WAV; that was a
  pre-hoist experiment, not the shipped gate) -- see earlier sections
  of this file.  The threading-gated OPERA_JIT_FIQHOIST variant is
  implemented, committed b0eac6f, and proven safe by trace parity
  (entry 567; the provenance counter is consistent, entry ~807).
- Never emit RIP-relative forms in generated code WITHOUT the startup range-check fallback (s_oj_rip_ok gates every form; when the check fails the emitters must keep the movabs+deref route). The original blanket ban predates the measured RIP-relative wins at :502-544 — the rule is "always range-checked", not "never".
- The harness dup2s stderr into run.log during the run: all core stderr
  diagnostics (incl. OPERA_JIT_STATS dumps) appear there, never on the
  captured stderr fd/dist file. Check run.log.
- utest phase-1 (interp-vs-cache single-instruction differential) has a
  pre-existing systematic divergence on branch-family words (present at
  HEAD before any JIT work): it is an oracle-vs-interp gap in the legacy
  harness lane, NOT a jit regression. Verified identical mismatch text at
  HEAD.


## Backend portability inventory (2026-08-30 assessment)

Structure: the jit is ONE file, #included at opera_arm.c:3918 behind the
__x86_64__ gate (macro at :59-61), with no-op hook fallbacks at :3920 for
other arches.  A backend port is ADDITIVE - no existing build changes.

PORTABLE ~70% (port as-is, all host-arch-independent because they call
the same C handlers and reproduce the same per-word tail):
- block-compile driver, classifier/word-coverage gate, tight-class
  semantics contract, exit-reason protocol
- block table + invalidation hooks (opera_arm_jit_touch, cov pages)
- stats build, oracle/battery/replay infra (guest-state checks)

ARCH-BINDING ~30% (the real work, per backend):
- EMITTER: raw byte emission, x86-64 encodings (movabs/ModRM/SIB/
  rel32-fixup traps at jit.md:1544ff are x86-specific; each arch gets
  its own trap class - ADRP/ldr-literal range checks on arm64 need the
  same startup verification the +2GB check gives x86-64)
- TRAMPOLINE: SysV stack discipline (jit.c:32-36: rsp%16 SSE alignment,
  callee-saved push/pop pairing) - full rewrite per ABI
- REG MAP (jit.c:38-44): rbx=&CPU rbp=*fiqpend r14=*cdrom_restart_poll
  r15=*madam_fsm_poll r12d=cycle-accumulator r13d=budget-remaining
  r11=&carry_out = SIX callee-saved pins + r11.  arm64 fits easily
  (x19-x28 = 10 callee-saved); arm32 (AAPCS r4-r11 = 8) needs a
  REDESIGN - the budget/charge pair is two more callee-saved regs than
  arm32 comfortably holds; likely spill poll pointers to the arena frame
  (semantically safe: read-only within a block; cost = ~3 loads/slice
  at poll sites, the same per-site class b-lite arithmetic priced)
- ARENA/FIXUP machinery: RIP-relative avoidance is an x86-64 ASLR
  constraint; aarch64 ADRP has different range rules

Effort with trap-finding overhead: arm64 ~2-3 weeks (31 GPRs, movz/movk,
b.cc maps 1:1 to oj_jcc, adrp+add); arm32 ~3-4 weeks (reg-map redesign
+ movw/movt or literal pools); x86-32 ~2-3 weeks (absolute addressing
simplifies fixups but pins must fit 7 GPRs - spill risk the x64 build
avoids).  Verification ports free: the 4M replay md5s and traces are
guest-state checks, arch-neutral by construction - a divergent port
fails the replay before producing a wrong frame.


## Per-arch x per-OS backend plan (2026-08-30)

WINDOW x64 BUG FIXED PRE-EMPTIVELY (d4aafe5): the arch gate at
opera_arm.c:59 matched _M_X64 (MSVC) and __x86_64__ (MinGW), so a
Windows x64 build WOULD have enabled a jit whose trampoline speaks
SysV (rdi/rsi pins, edi/esi/edx/ecx arg order, rsp%16 discipline).
On Win64 those helpers are compiled rcx/rdx/r8/r9-first — instant
crash on the first cstep chain or FIQ poll.  Gate now requires
!defined(_WIN32).  Linux/macOS behavior unchanged (PNG + WAV pin
green after the change).

SHARED BACKEND INTERFACE (the contract every port implements):
  oj_ld_cpu/st_cpu(reg, member-disp)   CPU access (pin-relative)
  oj_movabs(reg, imm64)                materialize constant/address
  oj_jcc(cc, label)/oj_jmp(label)      intra-block control flow
  oj_jmp_abs(addr)                     jump into shared trampoline
  oj_rip_ld64/cmp32/...                data-addressing mode w/ fallback
  oj_exit_*/oj_tail                     exit protocol (reason in eax)
  oj_emit_{dp,sdt,bdt,mul,branch,...}  per-ARM-class emitters (logic
                                        arch-neutral, encodings not)
  trampoline: entry ABI glue + X_{NEXT,FIQ,FIQDONE} stubs + prologue/
  epilogue + pin setup + arm_fiq_vector call + cstep-chain C call
  fixups: labels (rel), abs jumps (rel32 to arena), rip-rel table
  verification: 4M replay md5 + lane traces + PNG/WAV pins

=== PLAN PER COMBINATION ===

P0  Linux/macOS x86-64 (DONE - the shipped backend; reference for all)

P1  Windows x64 (MinGW + MSVC 2017, platforms windows_msvc* and
    mingw targets in Makefile):
  0. TWO EXTRA TRAPS beyond the ABI work (found while verifying the
     advisory's CFG catch):
     a) CONTROL FLOW GUARD: the slice enters the trampoline by an
        INDIRECT CALL from C (jit.c:4594: (*(jit_entry_t)
        g_jit_tramp_entry)(...)) into a VirtualAlloc'd page, AND the
        trampoline dispatches blocks with `call rsi` (jit.c:394) after
        loading blk->code from the table.  MSVC release linkers
        commonly default /GUARD:CF (the Makefile already passes
        -DYNAMICBASE, the usual companion): every indirect call is
        validated against the process CFG bitmap, and a plain
        VirtualAlloc'd page is NOT in it — crash at FIRST ENTRY, before
        any content runs.  MinGW does not emit CFG checks, so a MinGW
        gate would pass while real MSVC crashes.  FIX (both call
        sites, not just one):
          - SetProcessValidCallTargets(GetCurrentProcess(),
            GetCurrentThread(), page, size, 1, &cfg_info) marking
            the TRAMPOLINE PAGE and the whole ARENA as valid indirect
            call targets; call once after VirtualAlloc at startup.
            (cfg_info.Offset=0, cfg_info.Flags=CFG_CALL_TARGET_VALID)
          - Guard with if defined(_WIN32) && defined(_MSC_VER)-style
            detection OR do it unconditionally on Windows — the API
            is harmless where CFG is off.
          - The Makefile MSVC2017 desktop target may alternatively
            drop /GUARD:CF, but SetProcessValidCallTargets is the
            robust fix that survives the frontend's project settings
            (libretro cores build under MANY frontends' flags).
        Additionally X_NEXT-era `call rsi` inside the trampoline:
        CFG bitmap validation applies to ALL indirect calls — the
        trampoline->block `call rsi` target must also be valid, hence
        ARENA coverage in the SetProcessValidCallTargets call, and the
        indirect tail-jumps (jmp rsi after block exits) are NOT
        checked by CFG (jumps are exempt) — but keep them in the
        valid set anyway, costless.
     b) ICACHE COHERENCE: irrelevant on x86 (implicit), so the current
        backend has NO flush calls anywhere.  That is fine here, but
        the P2/P3 arm ports MUST add FlushInstructionCache (Win) /
        __builtin___clear_cache (gcc/clang, macOS+Linux) after every
        memcpy into the arena/trampoline — missing it = executes
        stale icache on some cores, a heisenbug the x86 gates cannot
        catch.  This belongs in the shared interface: oj_commit()
        must end with a portable flush (no-op on x86).
  1. mmap/VirtualAlloc shim (arena + tramp page; MAP_FAILED ->
     INVALID_HANDLE_VALUE checks).  ~1 day.  (Makefile already has
     WINDOWS_VERSION + cl.exe targets; no new build system.)
  2. Win64 ABI in trampoline: prologue shadow-space 0x20, arg regs
     rcx/rdx/r8/r9, rdi/rsi become SCRATCH (they are caller-saved
     on Win64 - the reg map needs 2 more scratch regs or spill);
     xmm6-15 must be preserved if the C side spills them.  ~1 wk.
  3. Redzone: NONE used (all [rsp+off] above rsp) — Win64-safe.
     RIP-rel: keep (module-relative works in DLLs).  movabs abs64:
     works (no ASLR range issue with 64-bit absolutes).
  4. Destructor: __attribute__((destructor)) -> DllMain detach or
     CRT atexit; stats-only, low risk.  ~1 day.
  5. utest harness: Windows port of SIGSEGV handler + fork-less
     crash isolation (or skip utest on Windows; PNG/WAV pins
     already give content parity).  ~2-3 days.
  ESTIMATE: 2-3 weeks incl. gates.  RISK: reg-map redesign for the
  lost rdi/rsi scratch (Win64 keeps rbx/rbp/r12-r15 callee-saved —
  the six pins survive; only the SCRATCH pool shrinks).

P2  Linux/macOS arm64 (Apple Silicon, RPi4/5, ARM servers):
  1. Reg map: pins x19-x28 (10 callee-saved — all six pins + both
     budget/charge + carry_out fit with room).  EASIEST backend.
  2. movabs -> movz/movk pairs (2-4 insns); oj_jcc -> b.cond (1:1);
     oj_rip_ld64 -> adrp+add (needs +-4GB check at startup — same
     pattern as the x86 +2GB check; ADRP is page-granular, mask).
  3. Trampoline: AAPCS64 (x0-x7 args, x29/x30 frames, sp 16-align).
     arm_fiq_vector + cstep-chain calls need arg shuffle only.
  4. Windows arm64 (Surface/Onnx): AAPCS64 ABI == Windows arm64 —
     mostly free once P2 done; only DllMain/VirtualAlloc shims.
  ESTIMATE: 2-3 weeks incl. gates.  RISK: ADRP range check class of
  bugs (silent wild access on truncation — mirror the x86 startup
  verification pattern); ldr-literal pooling if movz/movk is
  rejected for large constants.

P3  Linux arm32 (RPi1-3, odroid, Cortex-A9 android via NDK):
  1. REG MAP REDESIGN (the real work): AAPCS32 gives r4-r11 = 8
     callee-saved; the x86 map needs 6 pins + budget/charge pair +
     carry_out = 9.  Spill the two poll pointers (r14/r15, read-only
     in-block) to the arena frame; reload at the 3 poll sites.  The
     b-lite arithmetic prices this at ~3 loads/slice at poll sites
     — measured NEGATIVE class on x86, but arm32 has no alternative.
  2. movabs -> movw/movt (armv7) or literal pools (pre-v7 targets
     in the Makefile list are dead platforms; require armv7+).
  3. oj_jcc: ARM conditional execution maps BETTER than x86 (pred-
     icate the next insn instead of a branch) — optional phase 2.
  4. Trampoline: AAPCS32, sp 8-align; ldm/stm for the pin save.
  ESTIMATE: 3-4 weeks incl. gates.  RISK: the spill redesign is
  semantically safe (read-only pins) but perf-unproven; the 8.5%
  jit-vs-cache margin may shrink on arm32 — gate the port on the
  measured margin, not just parity.

P4  Windows x86-32 (msvc2010/2005_x86 targets): lowest value
    (32-bit Windows retro boxes); after P1 shares the Win32 ABI
    work (cdecl, all pins callee-saved ebx/esi/edi/ebp = 5 — needs
    the P3-style spill).  2-3 weeks AFTER P1+P3 exist.

ORDER: P1 (Windows x64) first — biggest user base, shares nothing
     but the mmap shim with P2/P3; then P2 arm64 (easiest backend,
     Apple Silicon + the Windows arm64 door opens nearly free);
     P3 arm32 only if the RPi/classic-console demographic matters;
     P4 last or never.
GATES EVERY PORT: same 4M replay md5 + PNG/WAV pins + battery; the
oracle is guest-state, host-arch-independent by construction.  The
arena census + trap classes (movabs vs lea, rel32 ranges, post-mmap
patch order) predict each port's trap surface; budget verification
time equal to implementation time.


## Backend file split IMPLEMENTED (2026-08-30, commits 13281b3..b6a4dc3)

Structure now:
  libopera/opera_arm_jit.c            arch-neutral shared core: block
                                      discovery, invalidation, compile
                                      driver, C-step, slice, stats.
                                      ZERO raw-encoding sites (audited).
  libopera/opera_arm_jit_backend.h    selection matrix + contract doc:
                                      master kill-switch OPERA_JIT_BACKENDS=0
                                      (compiles the whole jit out; engine=jit
                                      then silently falls back to cache with
                                      byte-identical output - VERIFIED) and
                                      per-backend OPERA_JIT_ENABLE_* gates.
  libopera/opera_arm_jit_x86_64.c     SysV backend: the proven encoding
                                      layer moved VERBATIM (emitters,
                                      trampoline, fixups, prologue,
                                      chain-word, prefit guard).  Guards
                                      itself with #error on _WIN32.
  libopera/opera_arm_jit_x86_64_win.c Win64 backend: derived from SysV
                                      with the ABI deltas applied and
                                      marked (rcx/rdx/r8/r9 args +
                                      stack spill for the 6-arg
                                      cstep_chain call, 0x28 shadow+
                                      align at both call sites, ecx
                                      entry arg, VirtualAlloc +
                                      SetProcessValidCallTargets on
                                      arena AND trampoline = the CFG
                                      trap fix).  NOT compiled on this
                                      host (no mingw/MSVC): structural
                                      + diff-audit verification only;
                                      the diff was audited hunk-by-hunk
                                      for missed deltas (6 hunks, all
                                      accounted).
  libopera/opera_arm_jit_aarch64.c    scaffold with the full P2 design
                                      (x19-x28 pin map, movz/movk,
                                      adrp+add with the +-4GB startup
                                      check, __clear_cache at commit,
                                      b.cond 1:1 onto oj_jcc).
  libopera/opera_arm_jit_arm32.c      scaffold with the P3 design (the
                                      reg-map spill: r4-r11 gives 8
                                      callee-saved vs 9 needed; poll
                                      pointers spill to the frame).

Matrix honesty: aarch64/arm32 default OFF via OPERA_JIT_{AARCH64,ARM32}_
READY markers until their emitters exist — no incomplete backend can be
pulled in by a host.  x86-64 SysV stays the shipped default; Win64
activates on x86-64 && _WIN32.

The backend contract (names the shared core references): emitter state
(s_oj*, labels/fixups, tramp globals), oj_* emission helpers, oj_emit_*
per-word emitters, oj_emit_chain_word (the C-call ABI site),
oj_emit_prefit_guard, oj_prologue (entry guards), ojb_alloc_arena,
ojb_after_commit (icache), build_trampoline.

VERIFICATION (all on the x86-64 SysV path after each step): PNG
300/900/1500 byte-identical to the oracles, WAV determinism pin,
battery, 4M replay md5 18af40b86d6169ac5eb21d03e6bec423, 528.7-529.4
fps (unchanged).  Kill-switch build (-DOPERA_JIT_BACKENDS=0) verified:
falls back to cache byte-identically.

NOTE for the Win64 backend: only real MSVC can validate the CFG path
(MinGW emits no CFG checks — a MinGW gate would pass while MSVC
crashes).  First bring-up on a Windows host must run the PNG/WAV pins
under BOTH compilers before trusting either.

CROSS-TOOLCHAIN AVAILABILITY (probed 2026-08-30, both advisory
suggestions executed with captured evidence): dnf install mingw64-gcc
FAILS — "This command has to be run with superuser privileges" (no
root on this host).  The Aurora module system EXISTS but its tree is
mostly dead: `modulecmd bash avail` lists 484 entries whose backing
paths are missing ("not found"); the 170 live fsf modules contain
only ancient NATIVE gcc (2.8.1 through 4.1, no cross/gcc-mingw/
aarch64/arm variants); /ms/{dev,src,proj} staging roots hold no
cross toolchains.  Cross-toolchain unavailable (probed to the
fullest per the follow-up checklist): (a) MODULEPATH is only
/ms/dist/aurora/etc/modules (the 484-dead tree); (b) exact-name
checks of /ms/dist/{mingw,mingw64,gcc,gcc-mingw64,gcc-aarch64-
linux-gnu,gcc-arm-linux-gnu,aarch64-linux-gnu,arm-linux-gnueabihf}
ALL absent, and the *gcc*/*mingw*/*cross* globs over /ms/dist
match only 'crossasset' (a business app, not a compiler);
/ms/dist/fsf is a 2006-era tree whose bin/ holds native gcc plus
mips-sgi-irix6.5-gcc and sparc-sun-solaris-gcc (ancient cross
targets, useless here); (c) dnf with the correct RHEL package
names (gcc-mingw64, gcc-aarch64-linux-gnu) fails as non-root,
and user-mode 'dnf download' fails on the expired-repos cache
permission.  This is the auditable basis for the syntax-only
verification level below.

WIN64 SYNTAX-ONLY VERIFICATION (the level reachable without a
cross-compiler, /tmp/jit/winshim/windows.h = a faithful WinSDK-signature
shim): gcc -fsyntax-only -D_WIN32 -D__x86_64__ over the whole
opera_arm.c TU including the win backend now PASSES clean (-Wall).
It caught ONE REAL BUG the diff-audit missed: ojb_win_cfg_valid was
CALLED in build_trampoline (~line 383) but DEFINED ~2800 lines later
in the hooks region — C89 implicit declaration + static-follows-
non-static, a hard compile error on real MSVC.  Fixed with a forward
declaration.  This verification catches every API-signature typo
class (VirtualAlloc flag macros, CFG_CALL_TARGET_INFO fields) but
NOT the emitted-opcode correctness or the CFG runtime path — those
still need a Windows host, and only MSVC exercises CFG.
Linux regression after the fix: build + f1500 + WAV pin green.

WIN64 BUDGET-SPILL BUG (advisory-caught, 2026-08-30): the Win64
trampoline's entry spill `mov [rsp+8], edi` (89 7C 24 08) was SysV
verbatim — on Win64 the budget arg arrives in ECX and edi is garbage,
so the epilog's `mov edx, [rsp+8]` / `sub edx, ecx` total-cycle
computation would read garbage - remaining on EVERY slice.  Fixed to
`mov [rsp+8], ecx` (89 4C 24 08 — ModRM reg field 001=ecx, same
length so no offsets shift; encoding derived from the codebase's own
`mov [rsp+4], eax` = 89 44 24 04 pattern).  After the fix a FULL
opcode-level re-diff of the two backends was taken: 12 SysV-only and
12 Win64-only opcode lines, every pair accounted (entry spills x2,
fiq-call shadow x2, cstep_chain call sequence x8 — the 6-arg remap).
No other SysV remnants.  Syntax check still clean, Linux regression
still green.  The verification ladder now: diff-audit (missed this)
< windows.h-shim syntax check (caught the forward-decl bug) < real
MinGW compile (unavailable) < real MSVC compile+CFG (unavailable).
Lesson: byte-diff audits need a checklist of ABI-sensitive SITES
(arg-reg spills feeding later reads), not just hunk parity.


## Win64 arena-locality audit (2026-08-30, advisory-driven)

The advisory flagged the OS-level divergence the opcode diff cannot
show: the +-2GB rel32 range checks (s_oj_abs_ok for trampoline jumps,
s_oj_rip_ok for .bss data refs) assume arena-to-module locality that
Linux mmap tends to give (nearby .so placement) but Windows
VirtualAlloc does NOT (128TB user VA, no bias).  AUDIT RESULT:
(1) both flags are GATED with REAL fallbacks — s_oj_abs_ok=0 turns
all 16 trampoline-target jumps into movabs+FF E0 (jmp rax), and
s_oj_rip_ok=0 turns every oj_rip_* data ref into movabs+deref; the
fallback bodies were verified IDENTICAL in the win backend (grep
parity).  So Windows behavior: CORRECT but with a silent perf cliff
(denser code, extra movabs per jump/data ref).  NOT a correctness
bug.  (2) Added a stats-build startup diagnostic so the cliff is
visible: 'jit-range: abs_ok=%d rip_ok=%d' with arena/module-bss
addresses (libopera/opera_arm_jit.c, startup).  First Win64 bring-up
must read this line; abs_ok=0 on Windows is EXPECTED and the fix
(hint-based VirtualAlloc near the module .bss, then
SetProcessValidCallTargets) is a Windows-host task in P1.
(3) Interesting host datapoint from the diagnostic on LINUX:
abs_ok=1 but rip_ok=0 in this session's placement (arena 0x7f7001800000
vs bss 0x7f7005379cc8 — outside the DRAM/RAM_SIZE check window), so
the rip fallbacks are ALREADY live on Linux here; the shipped 529fps
performance includes them.  The abs_ok rel32 jumps (the hot ones,
trampoline targets) are the locality-sensitive path that matters.
Clean-build regression after the diagnostic: f1500 + WAV pin green.


## CROSS-COMPILATION + QEMU VERIFICATION LOOP (2026-08-30, "could Zig be used?")

YES — and it WORKS end-to-end on this host.  This upgrades the whole
portability program from "blind, review-only" to compile-verified +
run-verified (where qemu-user covers the target).

TOOLING (all user-space, no root — the exact gap dnf/Aurora left):
- zig cc 0.13.0 (wheel already in /tmp/zigtest from a prior session):
  `zig cc -target <arch>-<os>-<abi>` is a cross clang with BUNDLED
  mingw-w64 + musl + glibc headers/libs.  Verified targets: x86_64-
  windows-gnu (PE32+ DLL with all 25 retro_ exports), aarch64-linux-gnu,
  aarch64-linux-musl, arm-linux-musleabihf, x86_64-linux-gnu/musl.
- qemu-user-static 7.2.0 (multiarch release tarball via the MS web
  proxy): /tmp/zigtest/qemu-aarch64-static runs static-musl aarch64
  binaries directly on this x86 host (dynamic glibc targets need a
  sysroot qemu can't find; STATIC MUSL is the robust shape).

THE LOOP THAT NOW WORKS:
1. zig cc -target x86_64-windows-gnu compiles the WHOLE core to a
   PE32+ DLL: caught a REAL BUG in the Win64 backend my windows.h
   shim had mirrored instead of checked — SetProcessValidCallTargets
   takes FIVE args (no hThread; the shim's 6-arg signature matched my
   wrong call site).  Fixed; dll links clean against KERNEL32.
2. zig cc -target aarch64-linux-musl (fPIC core objects) + STATIC_CORE
   harness (test_harness.c grew a -DSTATIC_CORE mode: no dlopen, core
   linked in, static_core_lookup table) -> full emulator as ONE
   static aarch64 binary.
3. qemu-user-static RUNS it: terminus.iso boots, 1500 frames,
   40 fps emulation (TCG, ~13x slower than native — fine for
   CORRECTNESS gates, useless for perf).
4. ORACLE RESULT: aarch64-under-qemu f300/f900/f1500 PNGs are
   BYTE-IDENTICAL to the x86 oracle; WAV == exact-stop pin.  i.e. the
   whole shared core + (cache engine, since the matrix gates aarch64
   jit OFF) is cross-arch bit-exact.

TWO REAL FINDINGS the loop surfaced (both non-jit):
A. zig cc bakes ubsan TRAPS into objects by default (ud1 / brk).
   The static-core build died at frame 358 (SIGILL x86, SIGTRAP
   aarch64) in dsp_operand_load1 opera_dsp.c:694 — an IMMEDIATE
   shift / REGCONV access that IS undefined behavior (executed
   silently by gcc/clang default builds since forever).  This is a
   REAL latent UB finding in the DSP code, benign on all shipping
   hosts but real.  Objects must be compiled
   -fno-sanitize=undefined for cross-test parity with production.
B. vendored retro_endianness.h's swap_if_little32 macro list covers
   x86 only; on aarch64 it falls to a non-inline function definition
   with no body in-caller — needs a shim TU (or the header fixed
   upstream) for aarch64 builds of cdrom.c.

CURRENT STATE per backend after this session:
- x86-64 SysV: unchanged, fully green (all gates re-verified).
- Win64: cross-COMPILED to a real PE32+ DLL (level: compile-verified
  incl. the CFG API signature).  Still needs a real Windows host for
  CFG-at-runtime + perf; MinGW-compiled binaries do not exercise CFG.
- AArch64: matrix still OFF (scaffold); but the CORE now proven
  bit-exact on aarch64 (via qemu), so a future P2 emitter only has to
  match the encoding, not debug the core on a new arch.
- arm32: zig compiles the core for arm targets; same loop applies
  (qemu-arm-static not fetched yet — same tarball has it).

REPRO (the working recipe):
  zig cc -target aarch64-linux-musl -fno-sanitize=undefined -fPIC      -c <each core file> -I. -Ilibopera -Ilibretro-common/include      -DINLINE=inline -DHAVE_STDINT_H -DHAVE_SYS_PARAM_H ...
  zig cc -target aarch64-linux-musl -fno-sanitize=undefined      tools/test_harness.c tools/static_core.c deps/zlib-1.3.1.2/*.c pic_*.o      -DSTATIC_CORE ... -> harness_static
  qemu-aarch64-static ./harness_static --title terminus.iso ...


## Both cross-loop advisory checks closed (2026-08-30)

1. PE IMPORT TABLE VERIFIED: the Win64 DLL's import table resolves
   SetProcessValidCallTargets -> KERNEL32.dll (objdump -p on the
   import directory; the hand-made cfgimp.def carried LIBRARY
   kernel32, so the thunk is a real kernel32 import, NOT a synthetic
   cfgimp.dll).  Load-time resolution on Win10+ is sound.
2. _WIN32_WINNT SELF-SUFFICIENCY: the win backend now forces the
   floor (#undef + #define 0x0A00 before windows.h).  The subtle
   trap found while doing it: zig's clang PREDEFINES _WIN32_WINNT
   (0x0603) on the command line, so a plain #ifndef guard is a no-op
   there — and distro mingw defaults to 0x0502, hiding the
   SetProcessValidCallTargets declaration.  #undef+#define covers
   both.  Backend now compiles clean with NO external -D flag
   (re-verified); DLL relinks; Linux build + f1500 + WAV pin green.
   TU-BOUNDARY AUDIT (advisory follow-up): the forced floor is SAFE
   because (a) opera_arm.c:3926's jit include is the LAST include of
   the TU — only the #else no-op hook block follows, no headers;
   (b) opera_arm_jit.c:199's backend include likewise has zero
   #include lines after it (the shared core's std headers all
   precede it); (c) the win backend file is reachable ONLY through
   that single include chain (backend.h's OPERA_JIT_BACKEND_FILE
   define; all other name-mentions are comments/#error guards) — no
   other TU can see the #undef.  The floor leaks into nothing.
   The advisory's LEGACY_WIN32/file_path.c class of risk is
   cross-TU-only and file_path.c compiles as its own TU with its own
   macro state — unaffected by a macro defined inside this TU.
Also re-noted for the record: the /tmp/zigtest tree is the extracted
pywheel ziglang package (not the zig source checkout the advisory
worried about) — its bundled mingw headers, def files, and musl/glibc
artifacts are present and demonstrably functional: they produced the
Win64 DLL, the aarch64 static emulator, and the arm targets this
session.  If /tmp is wiped, the replacement path is the official
zig-linux-x86_64 tarball via the proxy (or the same wheel from
pypi through the artifactory mirror).


## ARM32 (qemu-arm) loop closed + toolchain status answer (2026-08-30)

TOOLCHAIN ANSWER (all verified live, all user-space on this host):
- zig cc 0.13.0 (/tmp/zigtest/ext/ziglang/zig — the pywheel package,
  bundled mingw/musl/glibc artifacts confirmed functional): compiles+
  links ALL five relevant targets: x86_64-windows-gnu, x86_64-linux-
  gnu/musl, aarch64-linux-gnu/musl, arm-linux-musleabihf.
- qemu-user-static 7.2.0 (multiarch release via the MS web proxy):
  qemu-aarch64-static AND qemu-arm-static both on /tmp/zigtest and
  working (qemu-arm fetched this session; the arm32 hello binary
  returned 42 under it).

ARM32 FULL LOOP (same recipe as aarch64): 48 core objects + cdrom vfs
+ swapshim -> static-musl arm32 harness -> under qemu-arm: terminus
1500 frames at 42.4 fps, f300/f900/f1500 BYTE-IDENTICAL to the x86
oracle, WAV == exact-stop pin.  MD5 receipts (re-verified with a
fresh cmp pass after an advisory flagged that the ledger claim
needed its own evidence line): f300 15fe3091e2f19eaf511e4cc7fc0f017d,
f900 95b0f4bb2704a6e05bc5f8fde68721ed, f1500 d7b46c6566fa9b6ef3fd5299
2e67efc1 — each equal to the x86 oracle file; out.wav 83e3820cee00
8095dfa9a6b33e48deaa == the exact-stop pin.  The shared core is now proven
bit-exact on THREE host architectures (x86-64, aarch64, armv7) —
every future backend (P2/P3) inherits this foundation and the
build-run-oracle loop is standing ready for both.


## Per-(CPU,OS) completion program started (2026-08-30)

User directive: implementation file per os-cpu combination, all
implemented and tested where possible on this host.  Correct framing
accepted: the chart is a (CPU x OS) permutation — CPU fixes the
encoding, OS fixes the ABI/runtime interface.

STEP 1 DONE — EMPIRICAL AARCH64 ENCODING TABLE (/tmp/zigtest/enc/):
rather than trusting memory or the ARM ARM, every encoding the
backend needs was assembled by zig cc (LLVM 17), bytes extracted
from the .text section, and consolidated into
/tmp/zigtest/enc/aarch64_encodings.txt.  Covering: movz/movk (all
hw slots), add/sub/cmp imm12, and/eor/orr reg, lsl/lsr/asr imm
(w+x), all 14 b.cond, cbz/cbnz, b/bl/ret, ldr/str w/x/b/h
(imm-offset + reg-offset + post-index), stp/ldp, tbz/tbnz,
mul/madd/smull/umull, ror imm/reg, uxtb/sxtb, cset/ccmp,
adrp/adr, neg, tst imm.

STEP 2 DONE — RUNTIME EXECUTION ORACLE (selftest.c): hand-emitted
words are EXECUTED under qemu-aarch64 and results compared against
C-reference values.  All core emitters green:
  add w0,w1,#0x10 -> 21
  orr w0,w1,w2    -> 7
  movz x0,#0x1234 -> 0x1234
  movz+movk chain -> 0x123456789ABCDEF0 (the full 64-bit constant
                                    materialization, 4 insns)
  ldr w0,[x1,#12] -> 0xdeadbeef
  mul w0,w1,w2    -> 42
ROUND-2 (flag-chain + variable-shift forms, per the encoding-
completeness advisory): lslv/lsrv/asrv/rorv (w+x), adc/adcs/sbc/sbcs,
adds/subs reg+imm, bic/bics, ngc, msr/mrs nzcv, and a full taken/
not-taken branch-path test — ALL EXECUTION-VERIFIED under qemu.
Four traps found by the oracle (all in my test-writing, all exactly
the predicted P2 class):
  1. AAPCS64 arg routing (proto passes x0, emitted code reads w1).
  2. ldr imm12 is SCALED (12 bytes -> field 3).
  3. The 2-source-shift opc field is NOT 0 (lslv=8..rorv=11) — a
     from-memory base silently encodes a different instruction;
     only byte-extraction from the real assembler caught it.
  4. b/b.cond offsets count FROM the branch — off-by-one lands one
     word late and overwrites results silently.
The icache-flush discipline (__builtin___clear_cache after every
copy into the exec page) is exercised by the oracle too.  BRANCH
BUDGET (advisory): b.cond imm19 = +-1MB, b/bl imm26 = +-128MB —
plenty for intra-block + arena->trampoline, but the fixup resolver
must range-check like the x86 rel32 checks.  The complete verified
table lives at /tmp/zigtest/enc/aarch64_encodings.txt (kept out of
the tree; it is generation input, not source).

## MILESTONE 0 GREEN: aarch64 trampoline runs under qemu, jit matrix ON, oracle byte-exact

The backend file libopera/opera_arm_jit_aarch64.c (trampoline +
prologue + exits; every per-word emitter stubbed to C-step) now
COMPILES into the aarch64 core and RUNS terminus under
qemu-aarch64-static with OPERA_JIT_AARCH64_READY=1 +
OPERA_JIT_ENABLE_AARCH64=1 — every word C-stepped through the new
trampoline/ABI/arena/icache/dispatch path.

GATES (all byte-identical to the x86-64 oracles):
- f300/f900/f1500 PNG md5 == base pins (15fe3091e2..., 95b0f4bb...,
  d7b46c65...).
- WAV == 83e3820cee008095dfa9a6b33e48deaa (exact-stop shape).
- x86-64 native lane re-verified after the shared-core edits:
  PNGs byte-identical; WAV == 83e3820c under OPERA_JIT_BUDGET_BATCH=0
  (the default-on batch pin 9146703b... matches the ledger pin too).
- ENGAGEMENT PROOF (the load-bearing part): OPERA_JIT_STATS build
  prints "jit-stats: engine engaged" + cstep_cls counters
  (27.5M csteps over 300 frames, 38345 block compiles, 0 compiled
  blocks surviving — all-C by milestone-0 design).  The first
  plain-build gate run was a FALSE GREEN: the link line used the
  stale pic_opera_arm.o, so the binary silently ran the cache
  engine.  Any future aarch64 gate MUST rebuild pic_opera_arm.o
  (not just opera_arm.o) and show the stats line or counters.

SIX REAL DEFECTS FOUND AND FIXED (4 by advisory, 1 by gdb, 1 by the
stale-link false-green):
1. Trampoline fixup machinery rewritten LABEL-ALLOC (the mirror of
   the block emitter's oj_fix_at/oj_fix_lab): the old position-
   indexed patcher had (a) an opcode sniff ((cur>>25)&0x7F)==0x05
   that matches NEITHER b.cond (0x2A) NOR b (0x0A) — every b.cond
   patch destroyed its cond bits; (b) by-index forward patches
   misaligned because interleaved back-branches occupy the same
   indices; (c) OPERA_JIT_STATS shifts every index; (d) a fixup
   never assigned -> branch-to-self.  Now: t_b/t_bcond/t_bcbz
   record symbolic labels, t_lab_here binds, one t_fixup pass
   resolves.  cbz gets class 2 (t_bcbz added — was called but not
   defined).
2. oj_resolve had the SAME opcode-sniff defect — replaced with the
   explicit class array s_oj_fix_cond (0=b, 1=b.cond, 2=cbz), the
   same shape as the trampoline side.
3. cmp w17,#2 was 0x71000931 (decodes subs w17,w9,#2) — correct
   0x71000A3F; tst w17,#0x40 was 0x72000B51 — correct 0x721A023F.
   Both re-derived by assembling probe6.s through zig cc (NEVER
   hand-guess an encoding; the empirical-encodings discipline is
   load-bearing).
4. Prefit guard literal was at ldr+8 with imm19=1, i.e. INSIDE the
   execution path reading the cmp word as the charge.  Fixed shape:
   ldr w17,[pc,#16] (0x18000091); cmp; b.lt; b +1; .word charge.
5. oj_jmp_abs ldr literal was 0x58000151 (imm19=10, +40 bytes) but
   the .quad sits at +8 — br x17 jumped into fused garbage
   (0xb940029114000004 = the FIQ-poll ldr OR'd with a b word),
   SIGSEGV mid-dispatch.  Correct 0x58000051 (imm19=2).  Found by
   qemu -g gdbserver backtrace: pc==garbage, x17==fused words, frame
   #1 arm_jit_slice -> this was the smoking gun.
6. t_nlab reset to 0 in the builder body would alias label indices;
   t_lab_alloc in the declaration initializers already consumed
   0..5 — labels must count monotonically.

CORE WIRING completed (the designed-but-never-wired hook):
ojb_after_commit(jit_arena_cur,s_oj_len) now called at BOTH commit
sites in opera_arm_jit.c (sentinel path + normal path), right after
oj_abs_patch.  x86 backends: no-op call (icache is coherent); the
x86 gates confirm the wiring is default-inert there.

utest stream lane (x86-64): 0 mismatches; the host-process crash
is the DOCUMENTED utest-only defect, and the pre-edit control
(git-archive HEAD build) crashes at the byte-identical signature
(same rip 0x41e26f, si_addr, guest state) — deterministic 3/3
both sides, so it is NOT a regression of this session.  The real
emulator never reproduces it (ledger's standing evidence).

BUILD RECIPE (aarch64 static harness, updated):
  zig cc -target aarch64-linux-musl -fno-sanitize=undefined -fPIC
    -c libopera/opera_arm.c -o pic_opera_arm.o
    -DOPERA_JIT_AARCH64_READY=1 -DOPERA_JIT_ENABLE_AARCH64=1
    [+ -DOPERA_JIT_STATS for the engagement proof build]
  then relink harness_static with pic_*.o (REBUILD pic_opera_arm.o
  every time — opera_arm.o alone is NOT in the link).

NEXT: per-word emitters class by class (dp first), each oracle-
verified under qemu before the next; flip OPERA_JIT_AARCH64_READY
only when the emitter set is complete.

## DP CLASS GREEN: aarch64 per-word emitters (dp family) live under qemu

The full DP emitter (TIGHT_DP_IMM / RI / RMPC / PC / PCREL shared
driver, the oj_emit_dp translation) is landed and ORACLE-VERIFIED:
19211 blocks compiled (was 0), 4.9MB emitted, f300/f900/f1500 PNGs +
WAV byte-identical to the x86 oracles, clean exit, jit proven
engaged (stats build).  x86-64 lane re-verified after the edits
(PNGs identical; WAV == the batch=1 default pin).

EMITTERS LANDED:
- oj_word_cond: baked-entry cond guard — cond is COMPILE-TIME per
  word, so the 16-bit truth-table entry movz's in one word; runtime
  is lsr#28 + lsrv (variable shift by the cpsr nibble) + tst#1 +
  b.eq.  selftest6-verified over ALL 256 cond x nibble verdicts.
- oj_get_arm_c_w17 / oj_arm_set_c_w17 / oj_arm_set_zn_w10 /
  oj_arm_set_cvzn_fold: the flag helpers, extracted-constant only.
- oj_emit_dp: op1->w8, op2->w9 with per-route carry extraction
  (IMM ror rot2, RMPC/RI static shifts incl. the sh==32 forms,
  no-shift -> runtime C), ALU via the 16 probe12-verified words,
  ZN fold for S+logic, NZCV host-flags fold for S+arith, rd writeback
  (rd==15 and RS-class words still C-step by design).

THE DISCIPLINE LEDGER (why empirical-extraction is non-negotiable):
Every DERIVED constant in this file was wrong.  probe11: 9 of 11
mismatched (Rn=27 decodes, register/imm form mixups, an imm6=14
lsl#14 posing as lsl#28, a movz-class word posing as cset).
probe13: 3 of 3 wrong (rorv base 0x1AC02D09 vs real 0x1ADC2D29 —
the family base is 0x1ADCxx00, NOT 0x1AC0xxxx; lsl imm field law;
mov w10,w17).  probe15 caught two transcription slips (lsr w17,w9,
#31 had Rn=24 — read the PINNED w24 remaining register; mov w27,w9
had Rd=11).  probe14 caught cmp w17,#0 written as the REGISTER form
0x6B1103F1 instead of imm 0x7100023F.  Six design defects were
also caught by advisory decode: TEQ emitted the TST word (eor temp +
cmp now), the rot2 route used movz+rorv where one ror-imm word
suffices, two imm-field bugs in static LSL/LSR (imms/immr swapped),
the RI route emitted lslv for a compile-time shift, and the S+logic
carry was never computed (per-route extraction now, w27 saves the
pre-shift value; LSL32 carry = and w17,w27,#1 = 0x12000371 [probe16]).

SHIFT-WORD LAWS (extracted, probe13b):
  ror w9,w9,#N   = 0x13890129 | (N<<10)
  lsl w9,w9,#N   = 0x53000000 | ((32-N)&31)<<16 | (31-N)<<10 | (9<<5) | 9
  lsr w9,w9,#N   = 0x53000000 | (N<<16) | (31<<10) | (9<<5) | 9
  asr w9,w9,#N   = 0x13000000 | (N<<10)?? — NO: 0x13000000|(N<<16)|(31<<10)|(9<<5)|9 [probe14]
  variable: lslv 1ADC2129 lsrv 1ADC2529 asrv 1ADC2929 rorv 1ADC2D29 (w9,w9,w28)
ALU words (probe12, field-math verified against extraction):
  and 0A09010A eor 4A09010A sub 4B09010A add 0B09010A adc 1A09010A
  sbc 5A09010A orr 2A09010A bic 0A29010A ands 6A09010A tst 6A09011F
  cmp 6B09011F cmn 2B09011F rsb 4B08012A rsc 5A08012A
  mvn w17,w8 = orn 2A2803F1; mov w10,w9 = 2A0903EA; mov w10,w17 = 2A1103EA
  eor w17,w8,w9 = 4A090111; cmp w17,#0 = 7100023F (IMM form!)
FOLD WORDS (probe11): lsr w17,w17,#29 = 531D7E31; and w28,#0xdfffffff
= 12027B9C; lsl w17,#29 = 53030A31; orr w28,w28,w17 = 2A11039C;
and w28,#0x3fffffff = 1200779C; and w17,w10,#0x80000000 = 12010151;
cmp w10,#0 = 7100015F; cset w27,eq = 1A9F17FB; lsl w27,#30 = 5302077B;
orr w28,w28,w27 = 2A1B039C; mrs x17,nzcv = D53B4211;
and w28,#0xcfffffff = 1202779C; orr w28,w28,w17,lsl#28 = 2A11739C.
COND GUARD (probe10): lsr w17,w17,#28 = 531C7E31; movz w28,#imm =
52800000|imm<<5|28; lsrv w17,w28,w17 = 1AD12791; tst w17,#1 = 7200023F.
CARRY (probe15/16): lsr w17,w9,#31 = 531F7D31; mov w27,w9 = 2A0903FB;
lsr w17,w27,#31 = 531F7F71; and w17,w27,#1 = 12000371.

## BRANCH + MUL CLASSES GREEN: 25140 blocks, all artifacts byte-exact

oj_emit_branch + oj_emit_mul landed and oracle-verified (receipts in
/tmp/jit/bmaa/receipts.txt): compiled 19211 -> 25140, bytes 4.9MB ->
6.1MB, f300/f900/f1500 + WAV byte-identical, stats build proves
engagement; x86-64 lane re-verified (PNGs + batch pin).

BRANCH (the oj_emit_branch translation):
- cond guard + L-bit link write (mov32+str USER[14]) + charge +
  tail + exit; backward-edge threading binds a FRESH LABEL to the
  target word's byte position (s_oj_lab[l] = at) then a normal b —
  the positional-in-block jump the x86 did with rel32.
- backedge budget check: SAME operand order as x86 (cmp charge,
  remaining; b.lt continue) — an advisory decode caught my first
  port inverting the polarity (b.le on the swapped operand order
  would exit every backedge).
- THREE budget-check sites audited to identical x86 polarity:
  prefit guard (cmp remaining,charge; b.lt bail), oj_tail budget
  (cmp charge,remaining; b.lt continue), backedge (same as tail).

MUL (the oj_emit_mul translation):
- cost walk: clz+neg (32-clz == bitlen) replaces x86 bsr+add1;
  cbz w17 (0x34000031 [probe20], fixup class 2) for the zero case;
  +5, lsr#1, -1, clamp 16, add w23.
- rd==rm quirk preserved (old accumulate / 0, no multiply);
  madd w10,w10,w17,w8 = 1B11214A / mul w10,w10,w17 = 1B117D4A
  [probe19]; S-bit ZN fold; USER[rd] writeback.

THREE MORE DISCIPLINE LESSONS this class:
1. probe18's OWN SOURCE had the bug baked in: I probed 'subs wzr,
   w17,w17' (a tautology — always Z=1, clz path dead, calcbits
   always 1, systematic budget UNDERCHARGE that no artifact gate
   can catch: charge-only changes don't flip PNGs).  An advisory
   decode caught it.  Extraction discipline verifies the ASSEMBLER
   is faithful, not that the PROBE SOURCE is right — probe sources
   need the same adversarial review as backend code.
2. A mul-rewrite span accidentally DELETED oj_emit_branch (the
   python replace span swallowed it) — caught by compile error,
   restored from the verified transcript content.
3. cmp-family transcription slips: 0x6B170318 decoded as SUBS
   w24,w24,w23 (Rd=24 — it WRITES the pinned remaining register!)
   posing as cmp w24,w23; and my composed cmp w23,w24 = 0x6B1802F7
   vs real 0x6B1802FF.  Both fixed to extracted values; every
   0x6B-family constant in the file now audits clean (Rd=31 at
   every cmp site).
KEY EXTRACTED WORDS this class: cmp w23,w24 = 6B1802FF; cmp w17,#0
= 7100023F; cbz w17 = 34000031; clz w17,w17 = 5AC01231; neg w17,
w17 = 4B1103F1; add w17,#5 = 11001631; lsr w17,#1 = 53017E31;
sub w17,#1 = 51000631; cmp w17,#16 = 7100423F; mov w17,#16 =
52800211; mov w17,#1 = 52800031; add w23,w23,w17 = 0B1102F7;
mul/madd (w10,w10,w17[,w8]) = 1B117D4A / 1B11214A; mov w9,w10 =
2A0A03E9; movz w10,#0 = 5280000A; subs wzr,w17,w17 = 6B11023F
(TAUTOLOGY — never use; it was the probe-source bug).
## AARCH64 LANE-PARITY CLOSURE (2026-08-31): three flag-law bug classes fixed, FULL PARITY reached

Working method: 400-frame bb0 (OPERA_JIT_BUDGET_BATCH=0) lane traces
(OPERA_JIT_TRACE_LANES) diffed aarch64-vs-x86jit row-by-row; the x86 jit
lanes were first proven FULL MATCH vs the cache engine (2,606,774 rows,
FIQ fire count/pcs/cpsrs identical), so every first-diff row localizes an
aarch64-only emitter bug.  Progression of the first divergent row:
1694708 -> 1719857 -> 1750995 -> FULL MATCH.

CLASS (q) flagless ldr-then-b.cond in FIQ polls: the oj_tail generic poll,
the TIGHT_DP_PC tail, and the BDT LDM-pc tail all loaded the FIQ pending
word with `ldr w17,[x20]` and then branched with OJ_JZ — but A64 loads do
NOT set flags, so the branch consumed STALE flags from the preceding
`add w23,w23,#1` and fired spurious FIQs with pend=0 (diagnosed via the
extended [fiqfire] print: aarch64 fire n=1 showed pend=00000000 vs
pend=00000001 on x86).  FIX: emit `cmp w17,#0` = 0x7100023F [probe74]
after each of the three ldr sites (the prologue hoist already had it).
First diff moved 1694708 -> 1719857.

CLASS (r) ASR register-shift boundary inversion: the register-shifted ASR
path emitted `cmp w28,#31; OJ_JLE(l_big)` — JLE sends EVERY in-range
shift (s <= 31, i.e. all of them) down the sign-fill path, returning 0 or
0xFFFFFFFF instead of asrv.  All register-shifted ASRs in the content
collapsed.  Localized via lane row 1719857 -> cache wtrace ground truth:
`MOV r0, r1, LSR r0` (E1A00051, shifter 0x051) at pc=17690 must produce
0x37 (= 0x1B8000 >> 15).  FIX: OJ_JLE(l_big) -> OJ_JG(l_big) (only s>31
sign-fills).  First diff moved 1719857 -> 1750995 and the 400-frame
title-screen freeze cleared (f400 PNG exact match).

CLASS (s) ROR carry extraction read the wrong register (the staleness
class): three sites emitted `lsr w17,w9,#31` as 0x531F7E31 — but that
word's Rn field is 17, i.e. `lsr w17,w17,#31`: the carry was extracted
from STALE w17 (the previous carry/flags value), not the rotated operand.
FIX: 0x531F7D31 (Rn=9) [probe77] at all three sites (the immediate-rot2
carry in oj_emit_dp's RI path — exercised by the content — plus the RMPC
and RI static-ROR sites, same latent class).  Localized via lane row
1750995: cpsr 00000093 vs 20000093 (C flag) at the SWI handler entry;
cache wtrace at 1187C (BICS r14,r14,#0xFF000000, r14=0xEF010000) proves
C=1 (carry = bit31 of the rotated immediate 0xFF000000).  0xFF ror #8 =
0xFF000000 -> bit31=1.  First diff moved 1750995 -> FULL MATCH.

THE 0x531F7E31/0x531F7D31 slip is a one-bit field error (Rn 17 vs 9,
bit 10): the comment at every site said the right thing ("lsr w17,w9,#31")
while the constant encoded the wrong register — the exact failure mode
of hand-composed constants.  probe77 also re-verified movz w9,#0xFF =
0x52801FE9, ror w9,w9,#8 = 0x13892129 (the emitter's compose-laws
0x52800000|(imm<<5)|9 and 0x13890129|(rot2<<10) are correct).

ADVISORY REFUTED: a claimed mis-encoding of `and w17,w10,#0x80000000`
(0x12010151) in oj_arm_set_zn_w10 — probe76 shows the assembler emits
exactly 0x12010151 for that instruction; the advisory's field decode
arithmetic was wrong.  Same for the 0x3fffffff mask.  No bug there.

GATE MATRIX AT CLOSE (all aarch64 under qemu-aarch64-static, seed 1234):
- 400-frame bb0 lanes vs x86-jit: 2,606,774 rows FULL MATCH (0 diffs)
- f300/f900/f1500 PNGs (default env): byte-identical to base oracles
  (15fe3091 / 95b0f4bb / d7b46c65)
- f400 PNG: 4a0f3054818e9d8b3e5e698008512713 exact
- WAV 1800f default env: md5 2c1eda3d == x86-jit WAV byte-identical (the
  shared accepted batch=1 class; onset 8.50s matches the documented
  8.51s exact-stop divergence)
- WAV 1800f batch=0: md5 3ace9cc1 == base.wav BYTE-IDENTICAL (the
  strongest audio gate: exact-stop aarch64 == cache engine audio)
- BDT family gates (NO_BDT / _PC / _STM / _LDM): all four f400 PNGs
  = 4a0f3055 exact (the aarch64 cstep fallback correct for every subset)
- x86 regression: f300/f900/f1500 all match base oracles (the x86 backend
  unharmed by the diag edits)

Lane-vs-slice mapping note (kept for future sessions): lane row N's pc
column = the entry of slice N+1; the divergent slice for lane row R is
slicedbg s=R-1..R.  reason codes: 102=TRAMP_X_COMPILE, 3=JIT_X_BUDGET,
5=JIT_X_CSTEP, 100=TRAMP_X_INTERP.
## AARCH64 CLOSURE COMMITS (2026-08-31)

- 47415f4: the parity milestone (3 flag-law classes + session diag).
- e584097: cleanup + READY flip.  Diag stripped: [fiqfire],
  [cacheidbg], [cachefull], cache/interp wtrace, the DRAM-dump
  blocks (incl. wring reset/dump), [w200env], the mwritew w200
  trap, jit-block0/b0off/b0w extended blockdump, opera_jit_
  dump_arena, jit-cstep-word/table censuses, jit-run/jit-disp
  prints, cstepdbg hooks, ering/wring rings + window dumps,
  SIGSEGV-DIAG (harness + opera_arm_diag_cpu15), the m32 watch
  trap, the lane r0-r7 extension; slicedbg + lane tracer
  restored to committed shapes.  KEPT: the real fixes (interp-
  path FIQ consume, idx<0 rehandle + pc&~3u, the x86 ram-words
  dispatch gate mirrored into the Win64 backend) and the
  OPERA_JIT_STATS counters (fail_cls/sdt/idxneg).
- OPERA_JIT_ENABLE_AARCH64 now defaults ON for non-Windows
  aarch64 (opera_arm_jit_backend.h); Windows-aarch64 stays
  gated on OPERA_JIT_AARCH64_READY + real-host bring-up (the
  emitter is AAPCS64 OS-neutral; only the arena hook differs).
- Post-cleanup + post-flip verification (same gate matrix):
  lanes FULL MATCH, f300/f900/f1500 PNGs + batch=0 WAV
  byte-identical to oracles, BDT family gates exact, x86
  regression green, main `make` + both test builds green.
- Session backup patch of the full diag tree (pre-cleanup):
  /tmp/jit/session_full_backup.patch (2826 lines) — the
  debugging instruments remain recoverable for the arm32 phase.

## ARM32 MILESTONE 0 GREEN (2026-08-31): trampoline + primitives live under qemu-arm, oracle byte-exact

`libopera/opera_arm_jit_arm32.c` (commit 13ace46) — the same milestone-0
shape as the aarch64 b04d2ba: full trampoline, emitter primitives,
per-word emitters as C-step stubs. Every word is probe-derived:

- **Build**: `/tmp/zigtest/aabuild/rebuild_jit32.sh` — SELF-CONTAINED
  (compiles all core sources fresh; the old pic_*.o set is aarch64-only
  and must never be linked into an arm32 binary — ld.lld rejects with
  "incompatible with armelf_linux_eabi"). Runner:
  `/tmp/zigtest/qemu-arm-static ./arm32/harness32_stats`.
- **Gates**: f300/f900/f1500/f400 PNGs byte-identical to the oracles
  under qemu-arm (`4a0f3054...`, `15fe3091...`, `95b0f4bb...`,
  `d7b46c65...`).
- **Encode laws (probe80/probe82, `.arch armv7-a` REQUIRED — plain
  arm-linux-musleabi defaults below armv7 and rejects movw/movt)**:
  `movw = 0xE3000000|((imm>>12)<<16)|(Rd<<12)|(imm&0xFFF)`, `movt =
  0xE3400000|...`, `ldr rX,[rY,#imm12] = 0xE5900000|(RY<<16)|(RX<<12)|imm12`
  (P=1,U=1; str = E58...), `ldr pc,[pc,#-4] = 0xE51FF004` (the
  out-of-block literal route — no rel32 window at all on arm32).
- **Register plan vs aarch64**: r4=&CPU, r5=fiqpend-VALUE (== aarch64
  x20; the tail derefs once), r9=remaining (entry `mov r9,r0`,
  NOT a spill like aarch64's [sp,#96]), r7=charge. Two fewer
  callee-saved pins than the x86-64 map needs → the rst/fsm poll
  pointers spill to statics; X_FIQCONT materializes them into
  r10/r11 (scratch) instead of touching pins.
- **Prefit guard**: the aarch64 ldr-literal shape ported verbatim
  (`ldr r11,[pc,#0]`=E59FB000; `b +1`=EA000000; `.word charge`;
  `cmp r9,r11`=E159000B; blt entry0). The movw form was REFUTED
  before ever building: the shared core patches a full 32-bit word
  (opera_arm_jit.c:634) — a movw would be both truncated to 16 bits
  AND overwritten by the patcher. probe81/probe82 also caught the
  pc-relative math: `ldr rX,[pc,#imm]` base = insn+8 (NOT insn) —
  the pool word sits at insn+8 → imm must be 0.
- **Bug classes caught by the cross-loop advisory checks before the
  first run** (all register-convention slips — the arm32 register
  budget differs from both prior backends and the file went through
  three drafts): tail/prefit guards reading remaining from r6
  (garbage — r9 is the pin), X_FIQCONT reading [r7] (the CHARGE pin
  — would clobber it), and the fiqpend deref-depth question
  (REFUTED concern: r5 holds the pointer value exactly like
  aarch64's x20, one deref in the tail is correct).
- **Interworking**: `mov lr,pc; bx r1` for the FIQ call (plain
  arm-linux-musleabi rejects `blx reg` as armv5t; the `.arch
  armv7-a` directive is only in probe sources — the backend emits
  raw words, no directive needed).
- **Dead code note**: the X_FIQCONT stub is correct-by-construction
  but nothing jumps to it (same as aarch64 — OPERA_JIT_INLINEFIQ is
  an x86-only experiment). The aarch64 `0x52800021`-style mov-immediates
  for reason codes become plain `mov r0,#N` (E3A000NN) on arm32.
- **NEXT**: per-class emitter port (DP → MUL/branch → SDT loads →
  literal → BDT → SDT stores), each class probe-verified then
  lane-parity-gated (400-frame bb0 lanes arm32-vs-x86jit FULL MATCH)
  before the next class. The aarch64 bodies at
  opera_arm_jit_aarch64.c:1006-1481 (DP) onward are the templates;
  w8/w9 scratch → r10/r11, `oj_ld_cpu/oj_st_cpu` with r4 base, the
  OJ_Jcc map is native (no cross-table dance).

## WINDOWS-AARCH64 DELTA COMPILED IN (2026-08-31): the third (CPU,OS) cell closes at the compile level

`opera_arm_jit_aarch64.c` commit f2ad87e. The key discovery that
collapsed this from "new backend" to "in-file delta": **the AAPCS64
emitter was already OS-neutral.** x16/x17 (IP0/IP1) are the
intra-procedure scratch regs on both Linux and Windows AAPCS64;
x19-x28 callee-saved identical; x18 (the Windows TEB register) is
never touched; the C ABI call (`arm_fiq_vector`, one arg in x0, int
return) is the same. The only real deltas:

1. **Arena hook**: `_WIN32` branch = `VirtualAlloc` + CFG
   registration. Both the arena AND the trampoline page route through
   `ojb_alloc_arena`, so one CFG call covers both (the win64-x86
   backend had to register two pages separately — this backend gets
   it structurally).
2. **CFG**: the indirect-branch sites are the trampoline's `br x17`
   dispatch + every stub exit's `br x17` + the core's C->trampoline
   call. Under `/GUARD:CF` (MSVC release default) an unregistered
   VirtualAlloc page fast-fails at the first `br x17`.
   `SetProcessValidCallTargets` (FIVE args, no hThread — the same
   mingw-w64 signature trap the win64-x86 port hit).
3. Includes guarded; `#error` replaced — the selection matrix
   already gated Windows-aarch64 on `OPERA_JIT_AARCH64_READY`
   (stays undefined until a real-host MSVC /GUARD:CF validation).

**Verification on this host** (no Windows ARM machine available):
- zig cc `-target aarch64-windows-gnu` + REAL mingw headers compiles
  the full `opera_arm.c` with the matrix enabled
  (`-DOPERA_JIT_AARCH64_READY=1`): `win_aa_arm2.o`, COFF import
  table shows VirtualAlloc / SetProcessValidCallTargets /
  GetCurrentProcess, **mmap ABSENT** — the `_WIN32` path is live,
  not preprocessed out.
- The `#undef _WIN32_WINNT` guard is needed (zig predefines
  0x0A00 but warns on the redefinition otherwise).
- Linux-aarch64 regression after the edit: f400 = 4a0f3054 ✓
  (rebuild_jit64.sh — now self-contained like rebuild_jit32.sh).

**Windows-on-ARM32: REFUTED as a target, by construction.** zig's
`arm-windows-gnu` rejects ARM-mode functions outright ("target does
not support ARM mode execution" — WoA SEH assumes thumb-2). The
arm32 backend is an A32 emitter; a Windows port would be a full T32
rewrite (every word constant re-derived), not a delta. Documented in
the arm32 arena hook + backend-header matrix; the selection matrix
never routes `_WIN32` arm there.

**Matrix state after this commit** (compile-verified where marked):
- x86-64 SysV Linux/macOS/BSD — GREEN, oracle-exact (original)
- x86-64 Win64 — written, shim+cross-compile-verified, needs real
  MSVC host for CFG validation
- aarch64 Linux/macOS — GREEN, full lane parity + oracle-exact
- aarch64 Windows — CODE COMPLETE, cross-compile-verified with real
  mingw headers + live _WIN32 path; needs a real WoA host to flip
  `OPERA_JIT_AARCH64_READY`
- arm32 Linux — MILESTONE 0 GREEN (oracle-exact, all words C-step);
  per-class emitter port is the remaining work
- arm32 Windows — non-goal (thumb-only OS vs A32 emitter), refuted
  empirically and documented

**Remaining program**: (1) arm32 per-class emitters (DP first —
the aarch64 bodies at opera_arm_jit_aarch64.c:1006-1481 are the
templates, w8/w9→r10/r11, native ARM conds, probe-verify every word
in /tmp/zigtest/enc/ with `.arch armv7-a`); (2) lane-parity gate
400-frame bb0 vs x86-jit per class; (3) full battery + WAV, flip
`OPERA_JIT_ARM32_READY`; (4) real-host bring-ups (WoA for the
Windows flip, MSVC for the win64-x86 CFG path — both need hardware
this fleet lacks).

## EMITTER CLASS DIAG-BISECT GATES (arm32 port note, 2026-08-31)

The per-emitter `if(getenv("OPERA_JIT_NO_<CLS>")) return 0;` heads
(introduced in 47415f4, deliberately kept through the aarch64
cleanup) are the FIRST MOVE of any arm32 lane-divergence bisect:
setting a NO_* var makes that class C-step through the exact C
handlers — the run must then match the cache-engine oracle exactly,
isolating the divergence to the remaining classes. The arm32 port
keeps them verbatim (V4 in the backend file's port invariants).
Full set: OPERA_JIT_NO_DP, _NO_BRANCH, _NO_MUL, _NO_SDTL, _NO_LIT,
_NO_BDT (+ _NO_BDT_PC/_STM/_LDM family), _NO_SDTS, _NO_SDTSR.

**Gate-runner beware**: any of these set in the environment silently
degrades that class to all-C-step — check `env | grep OPERA_JIT_NO`
before trusting a mismatch. (Also: the ARM32_READY gate is not
Linux-specific — backend.h:128-133 gates arm32 anywhere; the flip
after parity is the default-flip in that one block, covering all
armv7+ hosts at once.)

## arm32 DP emitter — encoding-bug hunt session (2026-08-31, uncommitted)

**Method that worked**: (1) aarch64 "true mirror" — a64 with all 7
OPERA_JIT_NO_* env gates emits exactly the arm32's word set (diff = 1
kind: E14F1000 mrs) and produces the oracle at f300 → the bug is
provably inside the arm32 backend's emitted asm, not the shared core.
(2) per-frame full-state hash (`opera_jit_frame_hash` called from
retro_run; 'H'/'h' crumbs) → first persistent divergence at f255
(4 transient 1-frame N/C-bit diffs at f28/50/54/191 before it).
(3) frame-end full-register dump ([bfdump] lines) → f253 identical,
f254 diverged. (4) windowed per-round register crumbs (U/V/W) around
the divergence round. (5) OPERA_JIT_BLOCKDUMP=<idx> dumps a block's
emitted words at every commit (s_oj holds LE bytes — read b[3]<<24|
b[2]<<16|b[1]<<8|b[0] for the true word). (6) systematic
probe-verification of every flag-machinery word against the real
assembler (probe94/95/96) caught the bugs below.

**Five real encoding bugs found and fixed (all in
libopera/opera_arm_jit_arm32.c, all probe-verified after the fix):**

1. `oj_arm_set_c_r3` C-bit clear: emitted `E3CCC102` = bic
   #0x80000000 (clears **N**) — 0x02 ror 2 — instead of
   `E3CCC202` = bic #0x20000000 (clears C). The C bit was never
   cleared and the stale N survived every shifter-carry flag write.
   Root cause of the original f255 skew (first divergence: my
   LDRSBCC cstep charged +1 where the a64's charged +4 — the CC
   condition read a stale C).
2. `oj_get_arm_c_r3`: emitted `E1A03E63` = **ror r3,r3,#28**
   (typ=3 imm5=28) instead of `E1A03EA3` = lsr r3,r3,#29. Garbage
   C read-back for every rot2==0 logic-op carry.
3. `oj_arm_set_zn_r10` N-merge: emitted `E18C8008` = orr
   **r8**,r12,r8 (Rd=8!) instead of `E18CC008` = orr r12,r12,r8.
   The N bit landed in scratch r8 and CPSR's N stayed stale forever.
4. LSL-register shifter carry: emitted `rsb r3,r8,#0` (the a64's
   `neg w17,w28` translated literally) — ARM register-shift amounts
   mask to 8 bits, so (2^32-s)&0xFF = 256-s ≥ 224 → lsr by ≥32 →
   carry always 0. Also the original word had rn=r3 rd=r3 (read
   garbage r3, not the amount r8). Fixed to `rsb r3,r8,#32`
   (E2683020, probe91).
5. LSR/ASR imm sh==1 carry: `lsr r3,r12,#(sh-1)` with sh=1 emits
   `lsr #0` = LSR #32 = 0 on ARM (the a64's lsr #0 is identity and
   its `lsl #29` extract still works!). Fixed to `and r3,r12,#1`
   (E2033001) when sh==1 (probe-verified in the emitted block dump).

**Verified correct** (probe96): all other flag-machinery words
(and/cmp/orreq/orr/bic/mrs/msr/ALU ops, the cvzn fold's
orr r12,r12,r8,lsl#28 = E18CCE08, bic #0xF0000000 = E3CCC20F,
bic #0xC0000000 = E3CCC103, msr APSR_nzcvq,r2 = E128F002,
get_arm_c lsr #29 = E1A03EA3, the ALU family E0x6A00B).

**Result**: with bugs 1+4+5 fixed (pre-N-merge-fix binary), f300
MATCHED the oracle `15fe3091...`. With all five fixed, the timing
moved: f300 = 4a0f3054 (title — the transition timing shifted early),
f400/f900/f1500 = 15fe.../const. The a64 true-mirror (the semantic
reference) gives f400=4a0f, f900=95b0f4bb, f1500=d7b46c65 = the cache
oracles; my arm32 still diverges from the mirror at f254.

**Residual divergence (open)**: frame-254, the byte-extraction loop
region 0x208B0-0x209C4 (LDRBCC/LDRSBCC post-indexed byte loops with
conditional DP: MOVCC r4,r0,LSL#8 / MOVS r3,r3,LSR#1 / ANDS r2,#0x8000
rotated-imm forms). The engines' register+cpsr streams are identical
at every sampled dispatch round (U/V/W crumbs) through f253 and the
charge streams (D crumbs) match until round ~5.92M; the first
observable difference is a cstep charge (+4 vs +1) for a conditional
LDRB at 0x2091C — the CC verdict differed although the CPSR sampled
at the dispatch entry was identical ⟹ the flag writer runs INSIDE a
multi-word block (no crumbs between words). Next tools: per-word
crumbs at emission sites, or a per-frame DRAM-region hash to find
which memory (disc buffer?) diverges first. NOTE: the harness's
screenshot timing vs the guest's VBL count — my engine's f300 screen
now runs EARLY vs the oracle (before: late) — the residual is a
timing skew, not a hard divergence; the guest CPU state matches
through f253 and diverges in f254's tail.

**State**: branch jit, HEAD 593092b, all changes uncommitted (the
five fixes + all instrumentation: breadcrumbs, frame hash, frame-end
dump, windowed reg crumbs, OPERA_JIT_BLOCKDUMP). The instrumentation
must be stripped/gated before any commit; the five encoding fixes
are the durable payload. Files touched:
libopera/opera_arm_jit_arm32.c (fixes + probe-verified words),
libopera/opera_arm_jit.c (instrumentation), libopera/opera_arm.c
(cache [bdisp] instrumentation — REMOVE), libretro.c (the frame-hash
call — REMOVE or keep env-gated). Stray file `2` at repo root to
delete.

### Post-N-merge-fix analysis (false alarms exonerated, 2026-08-31 later)

- The block at 0x20920 (table idx 33352, the ONLY successfully
  compiled block in the 0x208B0-0x209C4 byte-loop region — all its
  neighbors take the z1 n==0 SENTINEL path, i.e. all-C-step) was
  dumped from both engines and verified word-by-word: the CS-guard
  (ldr CPSR@0xB0 / msr APSR_nzcvq / bcc -> PAST the store: branch at
  0x4C imm24=3 -> target 0x60 = after the store at 0x5C — my earlier
  off-by-one reading was an arithmetic slip: 0x4C+8+12 = 0x60 OK),
  the rm/rd offsets (USER[12]@0x30, USER[4]@0x10), the MOVS's LSR#1
  + fixed C-extraction (and r3,r12,#1 = E2033001 present in the
  LIVE block), the fixed bic C-mask E3CCC202, the fixed orr
  N-merge E18CC008 — all correct. The a64 mirror's block has the
  same shape. The ldr USER[0] after the rm-load on BOTH engines
  is the (unused) rn load of the MOV template — benign.
- ARM decode corrections to the earlier session notes:
  the guest word at 0x20920 is MOVCS r4,r12 (LSL #0, NOT LSL #8),
  and 0x20928 is MOVS r3,r3,LSR#1 (NOT LSL#1).
- The 0x208D4-area compiles all abort at the n==0 sentinel path
  (z1: first word untranslatable — the SDT forms not yet ported),
  so that sub-region runs entirely on the SHARED arm_jit_cstep —
  register effects there are engine-independent by construction.
- Remaining suspects for the f254 divergence: (a) the arm32
  trampoline's internal dispatch/budget/reason accounting (not the
  charges — D-stream tot values match), (b) a flag effect inside a
  multi-word block not sampled by dispatch-entry crumbs, (c) block-
  shape differences in the self-modifying era (my walks=5/kills=5
  vs a64's 735/736 at f400 — unexplained but streams matched).
- Next-session tooling plan: per-word emission-site crumbs; per-
  frame DRAM-region hashes to find the first diverging memory
  (disc buffer at ~0xDAB8); re-check the transient f28/50/54/191
  1-frame hash diffs (may have moved after the N-merge fix).

### Session close-out gate state (2026-08-31, final binary with all 5 fixes)

- a64 true-mirror (7 env gates): f900 = 95b0f4bb... = oracle ✓ (the
  mirror is a valid reference for the DP-only semantic state).
- arm32 current: f300 = 4a0f (oracle 15fe — transitioned ~100
  frames EARLY), f400 = 4a0f (MATCHES oracle), f900 = 4a0f (oracle
  95b0 — the screen FROZE at the title image while the oracle's
  attract advanced).
- Per-frame hash streams: identical through f254, persistent
  divergence from f255 (mine 78C9A83A vs a64 363C7E8E).
- Interpretation: the loading completes early -> the transition
  fires at the wrong frame -> the post-transition state machine
  lands in a static attract state. All three effects trace to the
  single f254/255 CPU divergence in the 0x208B0-0x209C4 byte-loop
  region.
- The 5 fixed encoding bugs were real (probe-verified, several
  moved the divergence signature) but the f254 residue persists.
  The verified-correct inventory now covers the full flag
  machinery + the only compiled block in the diverging region;
  the remaining unsampled surface is the arm32 trampoline's
  internal dispatch/budget/reason accounting and mid-block flag
  effects. See the next-session tooling plan above.

### BATCH-mode discovery (final finding of the session)

With OPERA_JIT_BUDGET_BATCH=0 (per-word budget accounting), the
arm32 hash stream diverges from the a64 mirror from f32 onward with
679/900 frames differing — but they are 1-frame TRANSIENTS (the
stream re-syncs). With the default BATCH=on, the streams are
identical through f254 and diverge persistently from f255 (646
diverging frames, all contiguous from 255). Interpretation: the
per-word boundary accounting exposes a small timing desync that the
batch mode smooths over; the persistent f255 divergence is the
accumulated crossing of a behavioral threshold. Next session:
(1) capture the BATCH-ON D-stream for both engines to find the
exact diverging round at f255 (the batch-off streams diverge too
early to isolate it), (2) instrument the trampoline's X_NEXT budget
fold (subs r9,r9,r7 at the dispatch loop) vs the a64's per-word
decrement to find the off-by-one-word boundary behavior.

## DP MILESTONE COMPLETE — bug #6 was the f254 root cause (2026-08-31 evening)

**Bug #6 (the f254/f255 root cause)**: the RI static-shift carry
extraction for LSR/ASR with sh==1 emitted `and r3,r3,#1`
(E2033001 — rn=r3!) instead of `and r3,r12,#1` (E20C3001 —
rn=r12). The pre-shift value lives in r12 (saved by `mov r12,r11`
before the shift); reading HOST r3 instead fed garbage into the
C flag. Since the byte-decoder loop's MOVS r3,r3,LSR#1 + LDRBCC
pair keys on exactly that carry (C = bit0 of the loaded byte),
every LDRBCC verdict was wrong -> the byte stream offset diverged
-> the loading finished at the wrong time -> the title transition
fired ~100 frames early and the post-transition attract froze.
Found via: byte-crumb (opera_mem_read8 at the LDRB cstep) +
CPSR-crumb at the LDRBCC cstep (both engines) — mine C=0 vs a64
C=1 with identical r0/r3 — then the block dump of idx 33350
(0x20918, the MOVS) showed E2033001 where the source intended
and r3,r12,#1.

**Gates (arm32 qemu-user, seed 1234, all four screenshot oracles)**:
f300 = 15fe3091... OK f400 = 4a0f3054... OK f900 = 95b0f4bb... OK
f1500 = d7b46c65... OK — all byte-exact vs the cache engine.

**Hash stream**: 900 frames vs the a64 true-mirror: all CPU hashes
identical; ONE transient carry-field difference at f537 (the
budget carry, not CPU state).

**Lane trace (bb0, 400 frames, 2,606,774 rows, vs gated x86)**:
14 divergent rows (1696014-1696027, ~f260, the SVC/FIQ return
region 0x11DE0: x86 Z=1/C=1 vs arm flags-clear, then N-only) —
self-corrects, everything else identical. Open residue, next
session: likely a flag edge on a rarely-taken path (SPSR-restore
cstep or the MUL/branch cstep interplay).

**Instrumentation stripped** before commit: the retro_run frame-hash
call, the dispatch U/V/W/P crumbs, the cstep Z/Y crumbs, the
frame-hash/bfdump body, the cache [bdisp] block. The bc_w_ crumb
API itself remains (stats builds only, OPERA_JIT_STATS-gated).

## arm32 bug 7 — the LSR/ASR reg-shift carry `sub` word, f260 bubble closed (2026-09-01, commit 793a8bf)

**The residue**: the 14-row lane bubble at slice 1696014 (~frame 260)
that survived bug #6 — arm32 alone, x86/aarch64/cache all agreed.

**The decisive run discipline** (several red herrings cost time):
1. The engine-selection scare was a false alarm: the x86/native
   harness DOES include the jit (opera_arm.c includes
   opera_arm_jit.c textually); [bq] crumbs + [blast] z1/z2 shapes
   prove jit engaged on every capture. `--option opera_arm_engine=jit`
   is honored on all three harness binaries.
2. First parked-value instrumentation (in-block stores into a global
   pair, polled at dispatch) showed BOTH engines at identical
   dispatch state (r0=F, r2=1, r1=7FFF, rem=0) — the divergence was
   inside the block run, invisible at dispatch level.
3. The flag-value misread cost a whole loop: 0x40000000 is Z,
   0x20000000 is C. The "Z=1 vs Z=0" story was actually **C=1 vs
   C=0** — the LSR-by-register carry was the diverging bit, and the
   arithmetic chain (w0 LSR#16 of 0x7FFF: carry = bit15 = 0!) only
   made sense once re-read through the cache's own word trace.
4. The cache engine word-level [cw] crumb (fprintf to the harness
   run.log — the harness dup2's stderr there, NOT the shell's 2>)
   gave the ground-truth per-word CPSR/r0/r2 chain through the block
   and pinned the exit state as C=1 Z=0 at pc 0x11DE0.
5. Parking (r12 pre-shift, r8 amount, r3 carry-out) INSIDE the LSR
   walk showed: r12=0x7FFF, r8=0xE, **r3=0** — the carry computed 0
   with provably correct inputs. GNU as probe97: `sub r3,r8,#1` =
   E2483001, the emission had E2433001 = **`sub r3,r3,#1`** (rn=r3,
   the stale guard scratch) — bug #7 of the hand-assembly class,
   both in the LSR-reg path and the ASR-reg path.

**Fix**: E2433001 -> E2483001 in both sites (opera_arm_jit_arm32.c
`oj_emit_dp` walk). Root cause of the f260 bubble: every
register-shifted LSR/ASR S-bit carry shifted by (stale r3 - 1)
instead of (r8 - 1).

**Verification (stripped binary)**:
- three-way 400-frame lane: arm32 = x86 = aarch64, 0 divergent rows
  of 2,606,774 (cache lane also row-identical to x86).
- all four screenshot gates byte-exact vs cache: f300 15fe3091...,
  f400 4a0f3054..., f900 95b0f4bb..., f1500 d7b46c65....

**Also kept** (gated): the [bcharge] line in the OPERA_JIT_BLOCKDUMP
writer (len/charge/nwords) — the hunt's first question was charge
parity between backends.

**Instrumentation stripped**: the ZCRUMB parking emissions
(set_zn result, set_c carry+CPSR, walk r12/r8/r3), the zn ring
attempt (crashed: ring index offset bug — 0x3F8 vs 0x1FC — never
landed), the [bq]/[bq2]/[bzn] window poller, the 'q'/'n' bc_w_
format arms, the cache-engine [cw] crumb, the x86 set_zn park
(r11-based after the RAX-clobber attempt invalidated a capture —
instrumentation that writes a live register corrupts the engine and
poisons the comparison; check lane-identity before trusting any
instrumented capture), and the opera_arm_lane_seq accessor.

**Next**: branch emitter port (aarch64 oj_emit_branch 1481-1546;
backedge threading via s_oj_word_off[]/s_oj_blk_backedge already
present on the arm32 side). Census priority at 400 frames:
TIGHT_BRANCH 11,433 (39.2%), TIGHT_SDT_LDI 10,015 + SDT_IMM 2,439
(42.7%), BDT 4,403 (15.1%).

## arm32 branch emitter — ported (commit e56dda3)

`oj_emit_branch` (TIGHT_BRANCH) translated from the aarch64 template
(1481-1546) onto the probe-verified arm32 primitives. No new encoding
shapes — every word already pinned by a probe in /tmp/zigtest/enc/
(movw/movt probe80, `ldr/str Rt,[r4,#disp]`, `msr APSR_nzcvq,r2`
E128F002, `cmp r7,r9` E1570009, b imm24 EAx, literal-pool
`ldr pc,[pc,#-4]` E51FF004). Scratch: r2 for the L-bit return
address (dead after the cond-guard msr), r3 for the backedge
USER[15] park (the established tail scratch).

Deliberate divergence from BOTH reference backends: the in-block
backward jump is emitted directly with the known compile-time offset
(the x86 shape: `jmp rel32` with `at - len - 4`; arm32:
`0xEA000000 | ((at - s_oj_len - 8) >> 2)`) instead of the aarch64
l_back label-alloc + prebind. Rationale: the target word is inside
the current block so the displacement is a constant, and the label
form carries the OJ_LAB_FULL wild-write hazard documented at
oj_word_cond (the aarch64 `s_oj_lab[l_back] = at` write is
unprotected). First draft used the aarch64 form + a defensive bail
that violated V2 (charged before bail); the x86 audit showed the
hazard-free direct form. Emitted bytes identical.

Label-budget note: the emitter allocates <= 7 labels per word
(l_fall 1 + l_be_ok 1 + oj_tail 2); the compile loop's headroom
guard (opera_arm_jit.c:478, `nlab > OJ_MAXLAB-32`) breaks before
any word with < 32 labels left, so in-emitter OJ_LAB_FULL is
unreachable by construction. The l_fall guard stays (oj_word_cond
API contract: it CAN return OJ_LAB_FULL itself).

Gates (all on commit e56dda3):
- 400-frame lane three-way: arm32 = x86 = aarch64, 0 divergent rows
  of 2,606,774.
- PREFIT=1 lane pair: arm32 = x86, 0 divergent rows (exercises the
  s_oj_blk_backedge whole-block redo path).
- Screenshots f300/f400/f900/f1500 byte-exact vs the cache-engine
  oracles.
- Census: fail_cls[22] TIGHT_BRANCH 11,433 -> 0. blkend branch
  terminators 12,795. compiled bytes 6,783,712 -> 7,307,280.
- sdt_emit calls=10,358 ok=0 (SDT next).

**Next**: SDT-load emitter (census: TIGHT_SDT_LDI 10,017 + TIGHT_
SDT_LDR 341 + SDT_IMM literal 2,439 = 12,797 rows, 44% of remaining
fails). Template: aarch64 oj_emit_sdt_load; x86 1895-2020 has the
inline-DRAM fast path + slow-exit shape. MUL (73), BDT (4,404),
SDT-store STI 5,030 + STR 165 after.

## arm32 SDT-load emitter — ported (commit f24648a)

oj_emit_sdt_load (TIGHT_SDT_LDI imm-offset + TIGHT_SDT_LDR reg-offset
+ the dispatcher-routed SDT_IMM_NP/SDT_RI_NP load shapes) translated
from the aarch64 template (1661-1789) onto the probe-verified arm32
primitives. No structural deviation from the template: single
raw-tbas range check before the byte/word split (the aarch64 shape —
x86's masked word-path check is equivalent but noisier), writeback
after the load so the slow exit stays side-effect-free, the NP
register path verbatim including the latent USER[rm]-as-offset quirk
(SDT_IMM_NP is zero-population on terminus; port, never "fix").

Register plan (r4/r5/r7/r9 pins untouched): r6 base->new-base, r11
offset, r8 tbas, r10 static scratch -> DRAM pointer (the load BASE),
r12 RAM_SIZE value -> word-path rora, r3 loaded value.

One bring-up bug, the arm32 pattern repeating: probe99 pinned the
load words with r12 as base (`ldrb r3,[r12,r8]` = E7DC3008) because
the probe was written against a draft register plan that had the DRAM
pointer in r12 — then the plan moved it to r10 (r12 must survive for
the rora) and the emitter faithfully emitted the stale r12-based
words. Result: byte loads read [RAM_SIZE + tbas^3], word loads
[rora*8 + tbas&~3] — instant SIGSEGV at the first slice. The
OPERA_JIT_NO_SDTL=1 gate run (V4 discipline) isolated it to this
emitter in one run; probe100 re-pinned the r10-based forms
(ldrb r3,[r10,r8] = E7DA3008, ldr r3,[r10,r8] = E79A3008) and the
fix landed green. Lesson (same as DP bugs 1-6): pin the words for
the FINAL register plan, and re-probe whenever the plan moves — a
probe against a superseded plan is worse than no probe because it
launders the wrong encoding through the verify loop.

Also notable: the word path's unconditional `ror r3,r3,r12` (aarch64
shape) needs no l_norot label on ARM — rotate-by-0 is identity, so
the x86's branch (whose shl-based rot would shift by 32 = UB-ish
garbage at rora==0) has no arm32 counterpart.

Gates (all on commit f24648a):
- Screenshots f300/f400/f900/f1500 byte-exact vs the cache-engine
  oracles (qemu-arm, seed 1234).
- 400-frame lane three-way: arm32 = x86 = aarch64, 0 divergent rows
  of 2,606,774.
- PREFIT=1 lane pair: arm32 = x86, FULL MATCH.
- Census: sdt_emit calls=8635 ok=8635 (was calls=10,358 ok=0);
  fail_cls[25] (TIGHT_SDT_LDI) and fail_cls[26] (TIGHT_SDT_LDR) now
  zero; compiled bytes 4,090,456 at 400f.

**Next**: SDT literal emitter (oj_emit_sdt_literal; census 2,439
rows at 400f; aarch64 template 1791-1873, x86 2022+ — the address is
a compile-time static pc_k+8 +/- imm12, so the read collapses to a
disp32 load behind a RAM_SIZE gate). Then MUL (73), BDT (4,404),
SDT-store STI (5,030) + STR (165).

## arm32 SDT-literal emitter — ported (commit 641026d)

oj_emit_sdt_literal (ARM_CLS_SDT_IMM pc-relative: rn==15, P=1, W=0,
rd!=15 — `LDR rd,[pc,#imm]` shapes) translated from the aarch64
template (1791-1873). Green on the FIRST run — no bring-up bug —
because 90% of the word set is the probe99/100 reuse from sdt_load
(range check, DRAM pointer deref, [r10,r8]-based loads) and the one
genuinely new shape (the static rotation `ror r3,r3,#N`, N=(addr&3)*8,
probe103 law 0xE1A00000|(3<<12)|(3<<5)|(N<<7)|3) was law-checked
against the probe before emission.

Design note vs x86: the x86 template uses disp32 loads ([rax+disp32])
because x86 has a 32-bit displacement field; arm32's LDR imm12
displacement can't reach the literal pool from a register base, so the
arm32 form mirrors the aarch64 register-indexed shape (mov32 the
constant, indexed load) — the established arm32 pattern. The word-path
rora being COMPILE-TIME (addr static) makes this emitter's rotation an
imm5 `ror`, unlike sdt_load's register form.

Gates (all on commit 641026d):
- Screenshots f300/f400/f900/f1500 byte-exact vs the cache-engine
  oracles (qemu-arm, seed 1234).
- 400-frame lane three-way: arm32 = x86 = aarch64, 0 divergent rows
  of 2,606,774.
- PREFIT=1 lane pair: arm32 = x86, FULL MATCH.
- Census: fail_cls[3] (SDT_IMM) 2609 -> 343 (the residue is the
  non-literal SDT_IMM shapes the dispatcher never routes here:
  stores, writeback forms, rn!=15); sdt_emit calls=8797 ok=8797.

**Next**: MUL (73 rows — the smallest remaining), BDT (4,404),
SDT-store STI (5,030) + STR (165).

## arm32 MUL emitter — ported (commit 2019e45)

oj_emit_mul (TIGHT_MUL: MUL/MLA with the rd==rm quirk and the exact
((calcbits(rs)+5)>>1)-1 clamped-to-16 runtime cost walk) translated
from the aarch64 template (1560-1647). First-run green on all gates —
the word set was small (probe104, 14 words) and the one ARM-specific
hazard was caught at design time: `mul Rd,Rm,Rs` requires Rd != Rm on
classic ARM cores (UNPREDICTABLE), and the aarch64 template's
`mul w10,w10,w17` shape translates to staging USER[rm] in r8 first
(`mul r10,r8,r11` = E00A0B98) — never the naive r10,r10 form. The
cost walk uses the native clz (ARMv5T+; the armv7-a target has it),
including the probe73 bit-length fix (+32 after the -clz negation)
ported with its comment.

The charge lands directly in the r7 charge pin (add r7,r7,r11 =
E087700B) rather than the aarch64 w23 shape — r7 IS the pin, so the
add is one word cheaper than routing through a fold.

Gates (all on commit 2019e45):
- Screenshots f300/f400/f900/f1500 byte-exact vs the cache-engine
  oracles (qemu-arm, seed 1234).
- 400-frame lane three-way: arm32 = x86 = aarch64, 0 divergent rows
  of 2,606,774.
- PREFIT=1 lane pair: arm32 = x86, FULL MATCH.
- Census: fail_cls[21] (TIGHT_MUL) 79 -> 0.

**Next** (resolved next session): SDT-store went first — the
  size comparison was right (6,006 vs 4,646 rows; landed 33a2421),
  BDT followed as the final class (b7626ca).

## arm32 SDT-store emitters — ported (commit 33a2421)

oj_emit_sdt_store (TIGHT_SDT_STI imm12) + oj_emit_sdt_store_r
(TIGHT_SDT_STR register-offset with the promoted shift ladder) + the
dispatcher-routed NP store shapes, translated from the aarch64
template (2205-2406). The shared body's gate ladder (RAM_SIZE ->
HIRES fanout -> word_cov invalidation probe) and the one-shared-tail
discipline (body has no tail; the emitters close+tail so the
cond-fail path pays its SCYCLE) are verbatim ports.

One arm32 simplification vs the template: no park register. The
aarch64 body parks the new base in w27 because its w16 dies at the
DRAM movabs; arm32's only mov32 scratch is r10, so r6 (base) survives
every gate unscathed and the writeback reads it directly.

PROCESS NOTE (the splice): this was the first edit of the campaign
that misfired — the store-emitter PUT used line numbers from a read
taken BEFORE the literal/MUL commits renumbered the file, so it
spliced into the middle of oj_emit_sdt_load and displaced its
RAM_SIZE gate words. Caught immediately (the read-back showed
sdt_load's comment block followed by the store header), repaired by
cut/paste of the displaced fragment + re-insertion of the gate
preamble, and verified three ways: brace balance 0, structural
function-order scan, and the full five-gate battery re-run green on
the repaired tree. LESSON (should have been the rule all campaign):
re-read the exact insertion region in the SAME read that produces the
edit tag — never carry line numbers across intermediate edits/commits.
The x86/aarch64 backends never hit this because their emitter ports
were each single edits in one session.

Gates (all on commit 33a2421):
- Screenshots f300/f400/f900/f1500 byte-exact vs the cache-engine
  oracles (qemu-arm, seed 1234).
- 400-frame lane three-way: arm32 = x86 = aarch64, 0 divergent rows
  of 2,606,774.
- PREFIT=1 lane pair: arm32 = x86, FULL MATCH.
- Census: fail_cls[25] 5820 -> 0, fail_cls[26] 186 -> 0; sdt_emit
  calls=9863 ok=9863; compiled bytes 5,258,668; cstep 1,733,360
  (down from 6,293,454 at milestone 0 — 72% of csteps eliminated).

**Next** (resolved): BDT landed immediately after as the final
  class — b7626ca, the last entry in this log. No classes remain.

## arm32 BDT emitter — ported (commit b7626ca); ALL EMITTER CLASSES LIVE

oj_emit_bdt (LDM/STM) translated from the aarch64 template
(1898-2188), gated green on the full battery. With this landing,
every dispatcher-routed emitter class is live on arm32: DP (6 tight
classes), branch, SDT-load (LDI/LDR/NP), SDT-literal, MUL, BDT,
SDT-store (STI/STR/NP). The remaining C-step rows are by-design
full-class words (S-bit/rn==15/list==0 BDT forms, non-literal
SDT_IMM stores, SWI, SDS, reg-shifted full DP, ...): fail_cls
residue at 400f is 0=9, 2=164, 3=356, 4=49, 7=4, 8=44, 10=262,
20=11, 28=19, 29=480 — all decode-gated full classes, none routed
to a tight emitter on ANY backend.

The arm32 simplification that carried the port: r6 (first-element
address) is derived ONCE and survives every gate — arm32's only
mov32 scratch is r10, so the aarch64 template's three w16
re-derivations (after the window gate, after the probes, before
writeback) collapse to zero. The transfer loop keeps the aarch64
surviving-address discipline (mov r11,r6 once, add #4 per element)
because the probe/store movabs clobbers DO hit r10.

TWO bring-up bugs, both in the STM invalidation probes, both silent
(no crash log, no divergence in the 400-frame lane window — the
second only showed past frame 400):

1. INVERTED PROBE BRANCHES: the first draft collapsed the x86
   two-label shape (JLS->probe body; jmp->skip) into one label —
   OJ_JLS skipped to the END and out-of-window indices FELL INTO the
   probe body: `ldrb r12,[r10,r11]` with r11 >= ram_words = OOB read
   past the cov array = SIGSEGV inside generated code (empty log).
   Bisected to the BDT emitter in one NO_BDT=1 gate run, then to the
   probe block in a 4-run family gate bisect (NO_BDT_LDM/STM/PC).
   NOTE the family bisect's answer was "LDM with pc" — MISLEADING:
   the crashing word was actually an STM (the probe code is
   STM-only), but an LDM-with-pc word in the same block had already
   committed the block; the family gates change WHICH WORDS emit,
   reshuffling block boundaries, so the probe-bearing STM moved
   across the gate. The fix (two-label shape) settled it.

2. BOTH PROBES INDEXED THE SAME WORD: probe 1 read r8 (=wN, the
   window gate's spread end — correct by accident); probe 2
   recomputed w0 (bic r8,r6,#3) then re-added the spread
   (add r11,#(n-1)) — indexing wN AGAIN. w0 was NEVER probed. An
   STM overlapping live code at w0 skipped the C kill path, the
   stale block kept executing old guest code, and the guest screen
   FROZE at the f400 transition state: f500/f600/f900/f1500 all
   captured the f400 oracle image (2300 bytes vs the real f900's
   23526). All four oracle gates still PASSED at f300/f400 — the
   bug lived entirely past the gated window. Fixed to the x86
   template shape (probe wN, then w0 with no re-add, x86 2299-2311).
   LESSON (new, worth recording): the oracle battery samples frames
   300/400/900/1500 — a bug that freezes the guest BETWEEN oracle
   frames passes the battery if the freeze starts after f400 and the
   frozen image coincides with a gated frame's content. The lane
   trace (400 frames) has the same blind spot. The defense that
   worked: noticing f500 == the f400 ORACLE md5 (a near-blank
   2300-byte PNG) while the real f900 oracle is 23526 bytes —
   content-size sanity, not just md5 equality. For future ports:
   when a mid-bring-up freeze is suspected, md5-compare a
   between-oracle frame against the NEAREST PRIOR oracle first.

Gates (all on commit b7626ca):
- Screenshots f300/f400/f900/f1500 byte-exact vs the cache-engine
  oracles (qemu-arm, seed 1234); f500/f600 spot-checked as new
  content (not frozen).
- 400-frame lane three-way: arm32 = x86 = aarch64, 0 divergent rows
  of 2,606,774.
- PREFIT=1 lane pair: arm32 = x86, FULL MATCH.
- Census: fail_cls[8] 4982 -> 44; cstep exits 938,327 (from
  6,293,454 at milestone 0 = 85% eliminated); compiled bytes
  6,324,936; blkend cstep 1,398 (from 13,842).

**Remaining on arm32**: only the chain-word hook (oj_emit_chain_word,
return 0 — g_jit_chain_enable is off by default on every backend)
and the performance question (the port's purpose was parity, not
speed; arm32 runs under qemu-user in all these gates, so real
hardware numbers need a Cortex-A device session).

## WAV receipts + matrix flip: arm32-Linux and aarch64-Win default-ON (commit cff12cc)

The arm32 campaign's own plan gated the default flip on "full
battery + WAV" (see the port note above). An advisory audit found
the battery green but ZERO WAV receipts in the arm32 sections —
the one planned, runnable gate that was never run. Both lanes
now recorded (1800f, qemu-arm, seed 1234):

- WAV default env: md5 2c1eda3dc7623433a464a22d38f4be8e —
  byte-identical to a FRESH x86-64 jit WAV captured the same day
  (not just the ledger pin), i.e. the accepted batch=1 class.
- WAV OPERA_JIT_BUDGET_BATCH=0: md5 3ace9cc1656a56658fb43045a
  1ffb89a — byte-identical to base.wav (the exact-stop oracle).
- This is the identical receipt pair aarch64 shipped with at its
  flip (2393-2397): default == x86 class, batch=0 == base.wav.

With the receipts on file, the selection matrix flipped two cells:

- arm32 (armv7+ Linux): default ON, no -D flags. Verified with a
  zero-enable harness build ("jit-stats: engine engaged" + f400
  oracle exact). OPERA_JIT_ARM32_READY retired from the Linux path;
  Windows-on-ARM32 remains refuted (thumb-only OS vs A32 emitter);
  pre-v7 remains scaffold (no movw/movt).
- aarch64 Windows (mingw/clang family): default ON. The backend
  file is OS-neutral AAPCS64 + the _WIN32 arena/CFG hooks already
  cross-verified (VirtualAlloc live / mmap absent in the COFF
  import table). MinGW never emits CFG checks, so the hooks are
  inert there — same reasoning that made x86-64 Win64 default-ON.
  MSVC release stays behind OPERA_JIT_AARCH64_READY (real-host CFG
  bring-up).

Matrix sweep receipts (selection probe per target, string-in-
object method — the const-int method dies to ELF vs COFF tooling
differences; zig's -mcpu=generic+v7a does NOT update __ARM_ARCH,
so armv7 must be probed with -D__ARM_ARCH_7A__ = the harness shape):

  x86_64-linux         -> x86_64 SysV
  aarch64-linux        -> aarch64
  arm-linux stock (v4) -> scaffold
  arm-linux +7A        -> arm32          (the flip)
  arm-linux +8A / ARCH>=8 numeric -> arm32
  arm +7A + _WIN32     -> scaffold        (thumb-only refutation)
  aarch64-win-gnu      -> aarch64        (the flip)
  aarch64-win +_MSC_VER simulation -> scaffold (READY gate intact)
  aarch64-win +_MSC_VER +READY      -> aarch64 (override works)
  x86_64-win-gnu       -> x86_64 Win64
  any + OPERA_JIT_BACKENDS=0         -> scaffold (kill-switch)

Regressions on the flipped tree: x86-64 f300 15fe3091... exact;
aarch64-linux f300 15fe3091... exact; aarch64-windows-gnu full
core compiles with zero -D flags (import table shows
__imp_VirtualAlloc, no mmap).

CORRECTED BACKEND MATRIX (supersedes the chart earlier in this
file and any session summary that called arm32 "gated, pending"):

  | host (arch + OS)          | backend file            | state |
  |---------------------------|-------------------------|-------|
  | x86-64 Linux/macOS/BSD    | opera_arm_jit_x86_64.c     | default-ON, gate-proven (reference) |
  | x86-64 Windows (Win64)    | opera_arm_jit_x86_64_win.c | default-ON, compile-verified; MSVC CFG path needs real host |
  | aarch64 Linux/macOS       | opera_arm_jit_aarch64.c    | default-ON, full parity + oracle-exact |
  | aarch64 Windows (mingw/clang) | opera_arm_jit_aarch64.c | default-ON, cross-compile-verified (_WIN32 hooks live) |
  | aarch64 Windows (MSVC)    | opera_arm_jit_aarch64.c    | behind OPERA_JIT_AARCH64_READY (real-host CFG bring-up) |
  | armv7+ Linux              | opera_arm_jit_arm32.c      | default-ON, full parity + oracle/WAV-exact (2026-09-01) |
  | arm32 Windows             | — (refuted: thumb-only OS vs A32 emitter) | non-goal |
  | arm < v7 (any OS)         | — (no movw/movt)           | cache/interp fallback |
  | any other arch            | —                          | cache/interp fallback |
  | any host + BACKENDS=0     | —                          | jit compiles out entirely |

Remaining genuinely-blocked items (hardware this fleet lacks):
MSVC/aarch64 CFG bring-up, MSVC/x86-64 Win64 CFG bring-up, and
real-hardware arm32/aarch64 performance numbers (every gate above
ran under qemu-user or as a cross-compile).

## aarch64 permutation split: opera_arm_jit_aarch64_win.c (commits 91aeeb3, 7c8eb4e)

User directive: each backend permutation gets its own file, matching
the x86-64 convention (x86_64.c / x86_64_win.c). The aarch64 backend
was the outlier — it had carried its Windows delta as #if
defined(_WIN32) arms inside the POSIX file since f2ad87e. Now split:
opera_arm_jit_aarch64.c (Linux/macOS, mmap arena) and
opera_arm_jit_aarch64_win.c (VirtualAlloc + CFG arena), with the
matrix gaining OPERA_JIT_ENABLE_AARCH64_WIN routing to the new file.
arm32 needs no split: Windows-on-ARM32 is refuted by design
(thumb-only OS vs A32 emitter), leaving exactly one permutation.

The emitter bodies are byte-identical by construction — the win file
is a mechanical derivation (header comment, windows.h includes, and
the two arena-hook functions). Proof: aarch64-linux f300 oracle
15fe3091... exact post-split, and the win TU compiles for
aarch64-windows-gnu with zero -D flags.

THE SPLIT IMMEDIATELY PAID FOR ITSELF TWICE:

1. __clear_cache DOES NOT EXIST for aarch64-windows-gnu. The win
   file's ojb_after_commit inherited __builtin___clear_cache from
   the POSIX twin; on this (clang/mingw) target that intrinsic
   lowers to the libgcc __clear_cache helper, which has no
   definition in the link. The correct Windows-aarch64 service is
   FlushInstructionCache(GetCurrentProcess(), base, len) — now
   implemented. The pre-split _WIN32 arms were COMPILE-verified
   only, and a compile never reaches a link-time symbol. The
   full-core LINK (all 23 libopera TUs + zlib + libretro-common +
   the libretro frontend + static_core, lld-link) now completes:
   harness.exe 568KB, 9 jit-stats strings, VirtualAlloc +
   api-ms-win-core-memory-l1-1-3.dll (the SetProcessValidCallTargets
   import) present, mmap absent. NOTE: zig's bundled mingw kernel32
   does not export SetProcessValidCallTargets; the import lib must
   be built from the def (zig dlltool -m arm64 -d
   .../api-ms-win-core-memory-l1-1-3.def -l libmemory.lib).

2. THE STOCK MAKE BUILD WAS BROKEN AT HEAD (since 2caf48c): six
   bc_w_() block-diag calls (267/270/275/288/372/384) were emitted
   unguarded while bc_w_ is defined only under #ifdef
   OPERA_JIT_STATS. Every harness build passes -DOPERA_JIT_STATS,
   so all gates were blind to it; the first no-STATS compile (the
   split verification sweep) failed, and `make` — the shipped DLL,
   no STATS — did not build. Fixed (7c8eb4e); make green; no-STATS
   TU compile verified on x86_64-linux, aarch64-linux,
   x86_64-windows-gnu, aarch64-windows-gnu; STATS builds unchanged
   (all three engines f300 oracle exact).

Earlier-session scope correction (advisory-caught, kept for the
record): the aarch64-windows-gnu receipt logged at the matrix flip
was "the opera_arm.c TU compiles" — NOT a full-core build. This
session's receipt is the real thing: every libopera TU + frontend +
zlib + libretro-common, linked.

CORRECTED BACKEND MATRIX (permutation-per-file, supersedes the
chart at the matrix-flip entry):

  | permutation             | file                          | state |
  |-------------------------|-------------------------------|-------|
  | x86-64 SysV             | opera_arm_jit_x86_64.c        | default-ON, gate-proven (reference) |
  | x86-64 Win64            | opera_arm_jit_x86_64_win.c   | default-ON, compile-verified; MSVC CFG needs real host |
  | aarch64 Linux/macOS     | opera_arm_jit_aarch64.c      | default-ON, full parity + oracle-exact |
  | aarch64 Windows (mingw) | opera_arm_jit_aarch64_win.c  | default-ON, FULL-CORE LINKED (this session); runtime needs WoA host |
  | aarch64 Windows (MSVC)  | opera_arm_jit_aarch64_win.c  | behind OPERA_JIT_AARCH64_READY (real-host CFG bring-up) |
  | armv7+ Linux            | opera_arm_jit_arm32.c        | default-ON, full parity + oracle/WAV-exact |
  | arm32 Windows           | —                             | refuted (thumb-only OS vs A32 emitter) |
  | arm < v7                | —                             | no movw/movt, cache/interp |
  | any other arch          | —                             | cache/interp |
  | any + BACKENDS=0        | —                             | jit compiles out |

### Advisory follow-ups on the split (commit 58d0594)

Closed the convention gaps vs the x86-64 pair: the misroute #error
guards on both aarch64 twins (POSIX file errors under _WIN32, win
file errors without it — both verified to fire via -D_WIN32 /
linux-target syntax probes), the win file's stale ENABLEMENT line
(described pre-flip READY gating), and the two file lists that
still mentioned the old single-aarch64-file shape
(backend.h:12, opera_arm_jit.c:198-200). The matrix had already
shipped in the requested shape at 91aeeb3 (separate
OPERA_JIT_ENABLE_AARCH64_WIN + its own pick-one row — observable
as PICK_AARCH64_WIN in the selection probe). Post-change gates:
stock make force-rebuild green; f300 exact on x86-64/aarch64/arm32;
full-core aarch64-windows-gnu link still WIN_LINK_OK (9 jit-stats
strings); selection sweep unchanged.

## PO'ED ACCURACY CAMPAIGN (2026-09-02)

Thorough accuracy test of all three ARM60 engines on PO'ed
(3do-poed-decomp oracle.iso, 419MB needle build). Reference
frame: upstream trapexit/opera-libretro ships ONE engine — the
plain interpreter (opera_arm_execute, no g_arm_engine). "The
original" therefore == interp; cache and jit are this worktree's
additions. All runs seeded opera_random_seed=1, fully
deterministic per engine (verified cache x3, interp x2, jit x2).

SCENARIOS AND RESULTS (screenshot-every-30 md5 census + WAV):

A. no input, 5400f: cache==interp 0/180; cache-vs-jit 1/300-slot
   diff (f4890); jit BUDGET_BATCH=0: 0/180 AND WAV==cache
   (pre-squash-era commit; object unreachable post-squash to 8a75be5).
   Dense every-2f scan of jit-default vs cache shows
   the video residue is exactly 5 isolated one-frame render slips
   (f154/162/4008/4826/4890; content==neighbor frame) — the
   documented batch quantization class.

A2. no input, 9000f: cache==interp 0/300 — the attract loop is
   engine-identical through f9000 (171 distinct hashes each, no
   freeze; last transition f5400).

B. gameplay script (inputs.txt: menu + walk/turn/shoot), 9000f:
   cache-vs-interp 9/300 diffs starting f8760; cache-vs-jit 151
   from f4500; jit BUDGET_BATCH=0: 0/300, WAV==cache (ff4a0431).
   jit-default is batch residue (same class as A).

C. audio, ALL scenarios/pairs: WAVs diverge from the SAME first
   byte 1214652 = sample 303663 ~= frame 413 (intro movie CD
   stream start). Byte-identical before. Not a constant shift
   (best-shift 0: 39.5% differing; wandering sub-sample phase);
   same signature as the jit.md AUDIO-RESIDUE deep dive. Content
   identical, transient/DMA placement timing differs.

CACHE-VS-INTERP DIVERGENCE — ROOT CAUSE FOUND AND FIXED (finding 3):
  arm_data_abort() charges the FILE-SCOPE CYCLES variable, which only
  the interpreter path reads back. The cache engine drives per-word
  cycle charges through a local cyc accumulator (cycp_), so every
  cached data abort silently lost its 4-cycle charge (base SCYCLE 1 +
  abort SCYCLE+NCYCLE 3). All five cache-reachable abort sites now
  shuttle the charge: CYCLES = (*cycp_); arm_data_abort(); (*cycp_) =
  CYCLES — arm_sdt_body load+store halves, arm_sdt_body_np load+store
  halves, arm_h_bdt. Post-fix: cache==interp byte-exact on BOTH
  scenarios (A2 5400f 0/180 + WAV 3a7dba5d; B 9000f 0/300 + WAV
  aa16e758). First lost charge pre-fix: instruction 24,761,430,
  cycle 56,299,058, pc=0x10 after a store at 0x2B4E8 aborted.

THE JIT STALE-BLOCK BUG THE FIX EXPOSED (finding 2's residue class):
  the abort fix's +3-cycles-per-abort timing shift exposed a LATENT
  SMC (self-modifying code) invalidation miss in the jit's emitted
  BDT/STM fast path. oj_emit_bdt's store-side invalidation probed only
  the FIRST and LAST word of the transfer window (oj_bdt_probe on
  w0/wN, backed by the g_jit_word_cov per-word coverage array): a
  multi-register STM whose window strictly CONTAINED a compiled block
  in its interior overwrote its guest words without killing it. On
  PO'ed the CD-stream decompressor STMs code into DRAM 0x703C4-0x703DC
  while a block compiled from the OLD words (LDR/ADD copy loop, charge
  6, nwords 2) stayed resident — the jit kept executing boot-time code
  against the rewritten divide idiom, diverging from cache at slice
  2,733,790 (~f419). Proven with a GDB hardware watchpoint on the DRAM
  words: the writer frames are #0 jit-emitted code (inline STM element
  stores) — the emitted path itself, not a missing hook on the C side
  (opera_arm_jit_touch per-element kills are correct). All five
  backends (x86_64, x86_64_win, aarch64, aarch64_win, arm32) now emit a
  bounded probe loop over EVERY word in [w0, wN] (n<=16), any covered
  word -> l_slow -> C cstep re-executes the STM through the hooked
  per-element writes. Post-fix: jit BUDGET_BATCH=0 == cache == interp
  on BOTH scenarios AND the full 1000f attract slice trace (0/6,516,937
  slice mismatches, divergence at 2,733,790 gone). Perf on terminus:
  probe loop within noise (two-point 372.44 bfps vs loop 371.7-373.5,
  5-rep pinned medians). Regression: terminus f300/900/1500 PNG oracles
  pass for jit both modes; jit-b0 WAV 3ace9cc1 == cache == interp
  (the post-abort-fix value — all engines legitimately moved from the
  pre-fix f67c47dc family); jit-default keeps the documented batch
  quantization residue (identical until sample 750416, sub-LSB deltas).

VERDICTS (post-fix): interp = original upstream behavior by
construction. cache = byte-identical to interp on both scenarios
(video+WAV). jit BUDGET_BATCH=0 = byte-identical to cache on both
scenarios + full slice trace. jit default = documented quantization
residue only. Both divergences from the original campaign (findings 2
and 3) are root-caused and closed.

Artifacts: /tmp/jit/poed/{A,A2,B,D,E}_{cache,jit,interp}*,
inputs.txt, lane traces deleted after cycle-total extraction.

TERMINUS PERF RE-MEASUREMENT (2026-09-03, corrected): the earlier
"jit trails cache on terminus" claim (372 vs ~480, and the older
340.9/485.5 note) was an artifact of instrumented binaries (the
session's trampoline debug probe ran a getenv PER TRAMPOLINE ENTRY,
-25% fps) and stale memory of the 1bead44-era build.  Clean 5-rep
pinned medians, both commits verified:

  session-start build (2026-09-02 tree, pre-abort-fix): interp 370.7  cache 458.7  jit 490.1
  final build (all accuracy fixes):                 interp 370.9  cache 444.0  jit 486.8

The jit is the FASTEST engine on terminus too (+6-9% over cache; the
0.7% spread between the two builds is run-to-run noise; no regression
from the accuracy fixes).  BDT probe-loop cost re-measured clean: two-point 481.0
vs loop 480.7 (-0.08%, noise).  Per-guest-instruction host cost
(measured with per-insn lane traces, 48.9M guest insns / 1000f on
BOTH titles — instruction volume is equal):

  ns/guest-insn   interp   cache   jit
  terminus         55.1    46.0   42.0
  poed gameplay    84.0    74.2   15.2

Why the jit's edge shrinks on terminus: the workload is not
CPU-bound — at 486 fps the 2.06ms frame budget is dominated by
per-frame fixed host cost (CLIO/DMA/video) that all engines pay
equally, so the ARM-engine slice is small.  PO'ed gameplay runs the
same instruction COUNT but with heavy guest memory traffic: interp/
cache pay 84/74 ns/insn (slow host memory paths) while the jit's
emitted code handles it at 15 ns — the 4.9x gap.  Workload note:
terminus.iso is a Battle-Chess-engine title (Interplay); the bench
window f600-1800 is a mostly-static menu screen (f900 == f1500 at
the 13.6%-sample level, no motion after the intro).

  Perf-profile confirmation (perf record, jit engine, top self-symbols):
    terminus: TexelDrawArbitrary 19.4%, PPROC 12.6%, opera_dsp_loop
              10.1%, vdlp_render_line_RGB565 5.1%; ARM engine total
              ~9% (execute_slice 4.3 + page_hot 3.7 + cstep 1.2).
              Even a zero-cost ARM engine caps the gain at ~9%.
    poed gameplay: [unknown = emitted jit code] 15.9%+
              execute_slice 11.2 + cstep 1.3 (ARM ~30%), dsp_loop
              15.9%, vdlp 14.2%, xbus/timers 15%.  The jit's emitted
              code IS the single biggest ARM component - exactly why
              engine choice dominates this title's frame rate.

BURST-GRANULARITY HYPOTHESIS (advisory) — TESTED AND REFUTED
(2026-09-02): the structural observation is real — the exported slice
runs ONE instruction per call under engine=interp (upstream
opera_arm_execute shape) while engine=cache bursts up to a whole
32-cycle quantum — but it is observationally neutral on this title:
OPERA_SLICE_1I=1 clamps the cache engine to one instruction per slice
call (interp call cadence) and the PO'ed gameplay 4500f output is
byte-identical to BOTH the normal burst cache AND interp (WAV bdcad25c,
150/150 screenshots, all three equal).  The mechanism does not exist as
claimed: (a) both engine tails poll *fpend/*rst/*fsm after EVERY
instruction (arm_execute_slice full:3131/tight:3161 regions), so FIQ
latching is per-instruction in the burst too; (b) opera_3do passes
budget (32-cnt) — the burst ends exactly at the hardware quantum
boundary with the same mid-instruction overshoot semantics interp
produces via cnt accumulation, so internal_frame advancement aligns.
(First attempt at the experiment had an infinite-recursion bug:
wrapper called the slice that re-checked the same env gate.  Fixed by
clamping budget_ inline; the knob stays as the reproducible
refutation.  The advisory's hash footnote — regs[0x600 words] +
dsp_word1..fifo_o tail, not sizeof(CLIO)=263KB — was already the
committed lane-tracer hash shape, landed with the instrumentation
commit of this squashed program.)

## Post-review fixes (2026-09-04, PR review of the squashed program)

Three review findings on the master...jit diff, fixed in the working
tree on top of 8a75be5:

1. X_FIQ stub stack misalignment (BOTH x86-64 backends, latent
   ABI violation — review initially misattributed as SysV-only):
   the stub runs at TRAMPOLINE level (post dead-RA drop, rsp == 0
   mod 16), NOT block level (rsp == 8 mod 16) where the emitter
   call-site constants are derived.  SysV: the sub/add rsp,8 pair
   was REMOVED (post-drop rsp already 0 mod 16; SysV needs no
   shadow).  Win64: sub/add 0x28 -> 0x20 at the X_FIQ stub only
   (0x28 = shadow+8 is the correct BLOCK-level constant and stays
   at the emitter sites; 0x20 = 32B shadow, preserves the stub
   alignment).  Same constant cannot be right at both levels —
   that was the mechanism, mirrored in both files.  aarch64 /
   aarch64_win / arm32 stubs re-derived clean by construction
   (br-entry, no RA push at stub level; 112B/40B frames).

2. SPORT VRAM writes bypassed the JIT SMC hooks: the invalidation
   contract lived only in the opera_mem write inlines, but
   sport_set_color / set_color_with_mask / memcpy /
   copy_page_color_with_mask hit VRAM through raw pointers (VRAM is
   inside the JIT RAM window).  Added opera_mem_jit_touch_range()
   (guest-address, page-hot prefiltered) and sport_jit_touch()
   (translates the VRAM-relative index: VRAM == &DRAM[DRAM_SIZE],
   so the guest address is DRAM_SIZE+idx_; the hi-res fanout
   composes to DRAM_SIZE+idx_+k*VRAM_SIZE like opera_mem_write32's).
   First cut of this fix passed the VRAM-relative index raw and the
   gate was green anyway -- the bug class this ledger exists for.
   VDLP startup-table writes were checked and are NOT a hazard:
   every caller (3do_init pre-execution; soft-reset AFTER
   arm_reset -> rom_select -> flush_all) guarantees no live block
   can cover the region; no hook needed.

3. Unreachable commit citation a16d28ba (line ~3296) annotated as
   pre-squash-era.

Gates: clean rebuild + f300/f900/f1500 PNG + WAV byte-parity vs
the /tmp/jit oracles, cache and jit engines (see below).

## 2026-09-07 — post-review fix round (all 16 reviewer findings)

Review: 16-agent parallel review of d8d78ae+68154aa vs 636a8dd; every
headline finding parent-verified (byte-decode or exact-byte execution)
before fixing. Fixes landed this session:

- STM rn-in-list writeback, ALL FIVE backends (was divergent 3 ways:
  x86 twins double-applied the adjustment — final USER[rn] = base±8n;
  aarch64/arm32 lacked the preload — rn slot stored the original
  where stm_accur stores base_). x86 twins: post-loop writeback now
  skips the preload shapes; ARM-family twins: preload added before
  the transfers (rn slot = base_) with the post-loop skip. Simmed
  64/64 shapes C-exact.
- cov-probe window gate (x86 twins): 39 09 (cmp [ecx],ecx — compared
  ram_words against the low 32 bits of the variable's own ADDRESS)
  replaced with 3B 01 (cmp eax,[rcx] — index vs limit). SMC
  invalidation liveness is no longer ASLR-decided.
- arm_jit_fetch_word_at ANVIL window: ROM1_SIZE_MASK guard added,
  out-of-window returns 0xBADACCE5 (mreadw parity) — no more
  guest-triggerable host OOB read via unaligned/wild pcs.
- Cache engine cross-mem_cfg state load: the lazy gate at
  opera_arm.c wrapper is now size-aware (reallocates when
  RAM_SIZE>>2 changes), and arm_cache_alloc handles calloc failure
  by freeing both halves and degrading to interp. Closes the
  default-engine heap OOB write (R01-F5/R09-F6/R08-1).
- x86_64_win trampoline: rdi/rsi pushed/popped (Win64 callee-saved),
  LIFO-ordered; epilog's `mov eax,r10d` reason line restored; the
  dead T_JCC_EPI/njcc scaffolding deleted from both twins (the
  uninitialized-njcc UB class).
- ADCS encoding (aarch64 twins): 0x2B09010A (ADDS) -> 0x3A09010A
  (ADCS); the msr nzcv carry materialization is now consumed.
- Tight SDT fast paths: dangling MAS_Access_Exept routes to the full
  handlers (rotation suppression + data abort + cycp_ charge),
  restoring interp parity on the SWP-to-XBUS-abort-window sequence.
- SWI HLE + jit: every writing arm of decode_swi_hle now
  opera_mem_jit_touch_range()s its destination extent (vec/mat
  sizes per call; MulMany uses the count) — HLE no longer bypasses
  block invalidation.
- STATIC_CORE harness: _prepare_paths strdups the core path before
  the shared free/reassign tail (metrics.json no longer walks freed
  heap).
- jit core: page-list realloc NULL-checked (kills the block on OOM
  rather than leaving it unregistered); rel32 reachability check
  bounds the extreme corners (both targets, both ends); sentinel
  path flushes on arena exhaustion.
- opera_mem: state_load_v1 flushes UP FRONT (all early returns
  covered); rom_select early-outs when the bank is unchanged (CLIO
  0x84 re-writes no longer pay a full jit flush).
- Frontend/hygiene: opera_arm_engine option info carries the
  restart-required note (v1+v2); duplicate bare include dropped;
  all six jit hook functions declared in opera_arm.h (local externs
  removed); stdint.h added to opera_madam.h; redundant function-
  scope extern in opera_mem.c removed.

Stale-ledger disclaimer (supersedes every earlier "kept/in tree"
sentence): entries dated <= 2026-09-04 describing CHAIN / PREFIT /
INLINEFIQ / OPERA_SLICE_1I / OPERA_SLICE_DBG / OPERA_JIT_STATS-gated
tooling describe the pre-strip tree (907d666 removed the lanes;
68154aa dropped the scaffold). The current release surface is:
OPERA_JIT_TRACE_LANES, OPERA_TRACE_PER_INSN,
OPERA_JIT_BUDGET_BATCH, OPERA_JIT_FIQHOIST, the per-backend
OPERA_JIT_NO_* diag-bisect gates, and the g_jit_dsp_threaded
flush-on-toggle. The Block-ABI section at :136-176 predates the
shared-trampoline/store-emission redesign — read :40-84 (cache) and
the backend-file headers for the current shapes.

Two defects caught by the targeted verification round (both now fixed):

- arm32 backend: stray `}` from the STM-preload edit (the edit-tool
  boundary echo) closed oj_emit_bdt mid-function — INVISIBLE to the
  native build (x86 backend) AND to the zig matrix, because plain
  `-target arm-linux-gnueabihf` defines `__ARM_ARCH=4`, so
  OPERA_JIT_ENABLE_ARM32_LINUX stayed 0 and the backend file was never
  compiled. The arm32 lane recipe (rebuild_jit32.sh) passes
  `-D__ARM_ARCH_7A__ -DOPERA_JIT_ENABLE_ARM32_LINUX=1` for exactly this
  reason. Matrix gap closed: the arm32 lane now ALWAYS compiles with
  the backend forced ON.
- The zig `arm-linux-gnueabihf` matrix entry without the arch define
  is a VACUOUS arm32 gate (backend off = cache/interp only). Recorded
  as a do-not-repeat trap.

Targeted verification (purpose-built checkers, /tmp/jit/uarm/):
- stm_check: all 4 STM P/U modes, rn-in-list — cache==jit==C-model on
  x86-64, aarch64 (qemu), arm32 (qemu). sp-slot stores base_, final
  USER[rn] = base_, all modes.
- adcs_check: ADC/SBC carry chain — cache==jit on x86-64 and aarch64
  (qemu) — the ADCS fix consumes the materialized carry.
- smc_check: store-over-next-instruction invalidation — pc walks past
  the overwritten word on all 3 arches (cov-probe liveness).
- fetchdirect_check: 262k-word sweep of arm_jit_fetch_word_at vs the
  mask model — 0 mismatches (ROM1_SIZE_MASK guard, 0xBADACCE5 out of
  window).
- anvil_check: 16 wild/unaligned pcs incl. 0x06000000 window edges — no
  host crash on any arch.
- statecheck: cross-mem_cfg garbage state-load rejected (rc=0) and both
  engines survive; no heap corruption.
- utest stream 1-iteration differential: 0 mismatches (the >=2-iteration
  host crash is the pre-existing documented harness defect — identical
  crash signature on the pre-fix binary).

Verification: clean native rebuild + full 4-lane zig cross-compile
matrix (aarch64-linux, aarch64-windows, arm-linux, x86_64-windows)
green — plus arm32 backend-forced-ON compile gate; gates below.
