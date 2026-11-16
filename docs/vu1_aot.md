# VU1 ahead-of-time translation (plan, 2026-11-10)

Goal: run Rayman's VU1 microprograms as compiled C++ instead of the cycle-accurate interpreter
(`ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp`), which is the bottleneck (cutscene: VU1 = 75% of the time, ~15 game FPS).
Model: rac-rs (`D:\downloads\rac-rs-main\rac-rs-main`, `tools/vu/vu1gen.py`, `src/engine/gsfull/vu1aot.rs`).

## Requirement: bit for bit, cycle for cycle

The native code must reproduce the interpreter exactly: VF / VI / ACC / Q / P / I / flags, VU data memory, the
XGKICK packets (bytes and order) and the cycle count. Two reasons:

- XGKICK copies one qword from VU data memory every 2 VU cycles while the program keeps running (`progressXgkick`),
  so what the GS receives depends on the cycle at which each store lands.
- Then every existing reference stays valid: the golden pictures and the `[vu1-hash]` references taken from the
  interpreter on 2026-11-10 (`run_tests`, check type `vu1_hash`) must match exactly.

## How (rac-rs method)

- The interpreter's timing state (per-lane VF / ACC ready cycles, VI ready cycles, the flag queue, pending Q / P,
  the store queue, the pending branch, the E-bit slot, the branch hazard) depends only on the path through the code,
  not on the data. The generator simulates it at generation time: one node = one instruction pair specialised for
  that timing state; the native code carries only the values and a fixed cycle count per node.
- One data-dependent stall: an XGKICK while the previous packet is still being transferred waits until the packet's
  end (its length comes from the GIF tags). Handled as a run-time loop, with the timing state after it specialised on
  the stall length (short stalls explicitly, long ones = everything settled).
- Coverage: entries and JR / JALR targets come from the code (E-bit slots, BAL / JALR return addresses) plus a run-time
  profile (`PS2X_VU1_PROFILE=1`, `tools/vu1prof.py`). Anything not covered hands over to the interpreter ("bail").
- Timing data: the generator reads the interpreter's own decode table (`out/vu1prof/decode_<hash>.txt`, written by
  the profiler: pipeline, latencies, register reads / writes per pair), so it cannot disagree with the interpreter on
  latencies.
- Arithmetic: SSE with round toward zero and the interpreter's operand clamp for the common case; the interpreter's
  exact path (zero / denormal / overflow / cancellation, `normalizeFmacResult`) as a scalar fallback for the lanes
  that need it. MAC / status flags only when the program reads them (same rule as the interpreter's
  `macStatusUnread`).

## Integration (no change to headers included by `ps2_runtime.h`)

- `VU1Interpreter::execute` / `resume` (member functions, so they reach the private pipeline state) ask the native
  table first: a program is used when the micro memory holds exactly the code it was generated from (checked again
  after every MPG upload, via the code generation counter) and the entry is covered.
- XGKICK packets go to `PS2Memory::submitGifPacket(Path1, ...)` through one shared function with the interpreter
  (kick hash, debug dumps).
- Generated code lives in its own static library; the generator is `tools/vu1aot/` (Python).

## Validation

- `PS2X_VU1AOT=diff`: every run executes natively on a copy and on the interpreter, compares everything (registers,
  flags, data memory, packets, cycles) and reports every difference; the interpreter's result is kept.
- `run_tests`: all golden pictures and `[vu1-hash]` references must stay identical; the unit tests must not regress.

## Steps

1. References (done 2026-11-10): `vu1_hash` check, golden pictures for 5 scenarios, `shoot_minisaurus`.
2. Survey (done 2026-11-10, results below): which code images, entries, JR targets, cycle shares.
3. Diff harness + shared kick submission (done 2026-11-10).
4. Generator, program by program (the biggest cycle share first), each one validated with the diff mode and
   `run_tests` before it is switched on. First program done 2026-11-10 (see Status).

## Survey results (2026-11-10, all 5 run_tests scenarios, 18.8 billion VU1 cycles)

`PS2X_VU1_PROFILE=1` + `python tools/vu1aot/vu1prof.py` (per image) and `python tools/vu1aot/vu1reach.py` (reachable
code per image: from the profiled entries, following branches and the profiled JR / JALR targets).

- 38 micro memory images (all 16 KiB, so leftovers of older uploads make images differ), but only **10 distinct
  programs** by reachable code, ~6700 pairs in total. Every run starts at 0 (MSCAL) and continues (MSCNT) at the
  pair after an E-bit (0x0C38, 0x3B40, 0x34A8, ...). Only 5 runs used up the 65536-cycle budget.
- By share of VU1 cycles (program key = hash of the reachable pairs, see vu1reach.py):

  | program | pairs | share | images | notes |
  |---|---|---|---|---|
  | 6af8ebd05633 | 393 | 33.3% | 7 | 0000-0C48, no JR: first target |
  | 4ead7b35a3a6 | 1820 | 28.7% | 1 | heavy JR (subroutines) |
  | 9ed834b59a07 | 1603 | 11.6% | 1 | heavy JR |
  | 514a8ad12422 | 1802 | 7.4% | 1 | heavy JR |
  | c110bb6d5b85 | 592 | 6.0% | 2 | |
  | f5739731e44d | 309 | 5.4% | 4 | |
  | bd487cfeb073 | 542 | 4.1% | 2 | |
  | daa1b3b1ce9d | 538 | 3.2% | 4 | |
  | a72a64e108c3 | 45 | 0.3% | 14 | |
  | ec561d05c7cc | 540 | ~0% | 2 | |

- Other levels may use JR targets or programs the 5 scenarios never reached: they run on the interpreter until profiled.

## Status and how to use it (2026-11-10)

- Runtime: `ps2xRuntime/src/lib/vu/vu1aot.h` / `vu1aot.cpp` (value helpers: copies of the interpreter's cases, plus
  XGKICK transfer, undo log, commits), dispatch in `ps2_vu1_core.cpp` (`vu1AotDispatch`, called by `execute` /
  `resume` for VU1). A native run works on a copy of the registers with an undo log for VU data memory and buffered
  packets; on a bail it is undone and the interpreter runs the whole program. The rounding mode is the interpreter's
  (round toward zero) during native runs: without it every float differed by 1-3 ULP.
- `PS2X_VU1AOT`: unset / `1` native, `0` interpreter only, `diff` native on a copy + interpreter for real, compared
  (`[vu1aot-diff]` every difference of a run, `[vu1aot]` counters every 600 VSyncs).
- Recording new levels: `run_rayman_profile.bat` plays the game with `PS2X_VU1_PROFILE=1` (translated programs stay
  native; untranslated ones run on the interpreter and are recorded; the report is written every 600 VSyncs and by
  `ps2xVu1ProfileFlush` when the game exits). Then regenerate, rebuild, `PS2X_VU1AOT=diff` run_tests.
- Generator: `python tools/vu1aot/vu1gen.py [--keys K,...]` reads `out/vu1prof` (run the game with
  `PS2X_VU1_PROFILE=1` first) and writes `ps2xRuntime/src/lib/vu/aot/` (prog_<key>.cpp + aot_programs.cpp). That
  folder is local (derived from the game's microcode, like `src/runner`); without it the build uses
  `vu1aot_none.cpp` (empty table, interpreter only).
- XGKICK while the previous packet transfers is the normal case (program 6af8eb: every run): the generated code
  counts the wait and continues in a block made for that wait length (up to the longest pending latency).
- Program 6af8ebd05633 (33% of VU1 cycles): 428 nodes; diff mode on `shoot_minisaurus`: 103574 runs compared,
  0 differences, 0 bails. Native in run_tests: all pictures and VU1 hashes identical; intro 394 -> 349 s, swim
  197 -> 184 s, shoot 153 -> 148 s.
- The `[vu1-hash]` line at a scenario's last VSync races with the end of the run (printed at the first XGKICK at or
  after its VSync); `vu1_hash` only compares lines before the frame budget.
- All 10 programs generate (node counts ~1.15x pairs; only JR switch defaults can bail).
- Next: diff + switch on the other 9 programs; then speed (SSE for the FMAC fast path, registers in locals, fewer
  helper calls). Done: see the sections below.

## Floating point: /fp:precise for all VU code (2026-11-10)

- The runtime is built with `/fp:fast /arch:AVX2` (`ps2xRuntime/cmake/ReleaseMode.cmake`). Under it the compiler may
  fuse `acc + vs * y` into one FMA and reorder float math, and whether it does depends on the shape of the code: the
  first native helpers (a line-by-line copy of execUpper) happened to match the interpreter, the instruction
  templates (`vu1aot_ops.h`) did not (1-3 ULP in ~70% of runs).
- The PS2's VU rounds the multiply and the add separately (PCSX2: mulps then addps). So all VU sources
  (ps2_vu1_core / upper / lower, vu1aot.cpp, the generated programs) are compiled with `/fp:precise`
  (`ps2xRuntime/CMakeLists.txt`): no fusion, no reordering, the same result whatever the shape. This changed the
  interpreter's own results slightly (the VU1 hash references were renewed; pictures compared first).
- Rule for VU code: keep it in those files (or add new files to that list); do not rely on /fp:fast code matching.

## All 10 programs native (2026-11-10)

- Instruction templates (`vu1aot_ops.h`): the generated code calls `ops::fmac<...>` / `ops::lower<word>` with the
  instruction decoded at compile time (the generic `upper()` remains for ITOF / FTOI / ABS / CLIP).
- A bailed run could leave an XGKICK transfer in the static context; the next run delivered that stale packet
  (3 mismatches in the intro): the context's transfer is now cleared when a run starts, like resetScheduler.
- Diff mode, all 5 scenarios: 2.88 million runs compared, 0 differences (5 budget bails, 4 runs not idle at entry).
- Native, same build, interpreter-only references (`PS2X_VU1AOT=0`) vs native: every picture and VU1 hash
  identical; intro 403 -> 279 s, swim 206 -> 162 s, shoot 155 -> 130 s; first level 12.2 -> 19.3 game FPS.

## Speed: SSE, flag-free programs (2026-11-10)

- `[nprof]` on the first level after all 10 programs went native: the generated functions ~50% of the game thread,
  plus helpers the compiler did not inline into those huge functions (`std::numeric_limits<float>::max` 4%,
  `ops::laneMask` 1.2%, `kickCycle` 1.1%, the generic `upper()` 1.6%).
- Flag-free programs (the default; `vu1gen.py --with-flags` for the old code): when no instruction of the code image
  reads MAC / status flags, the interpreter computes none (`macStatusUnread`), so the generated code queues no FMAC
  flag entries at all (`Program::flagsFree`; the dispatcher runs such a program only when the image's flags are not
  live, else the interpreter). In Rayman's VU1 code flags are never read.
- `ops::fmacNf` (vu1aot_ops.h) does the FMAC family on 4 lanes with SSE, bit-exact by construction:
  - ADD / SUB / MUL: the float result, a result with exponent 0 keeps only its sign. In round toward zero (the
    interpreter's mode, also set around native runs) that is exactly what normalizeFmacExactResult gives: a sum of
    two floats below FLT_MIN is exact, a product below it keeps its sign, an overflow already gives +-FLT_MAX.
  - MADD / MSUB: the interpreter's fast test on 4 lanes; lanes outside it take the exact (double) result on AVX
    (zero / below FLT_MIN -> sign, above FLT_MAX -> +-FLT_MAX, else the float result). A product that may overflow
    (exponents sum > 379) and OPMSUB / OPMULA use the scalar template.
  - MAX / MINI: `_mm_max_ps` / `_mm_min_ps` are the interpreter's `(a > b) ? a : b` / `(a < b) ? a : b`.
  - Templates also for ITOF, FTOI (double product, clamp, truncate), ABS and CLIP: no generic `upper()` call left.
- Generated code: CLIP / FCSET entries commit straight to the clip register (`s.clip = ...`), VF0 / VI0 are restored
  only after a pair that may write them (none in Rayman's programs), `advance()` adds the cycles to the XGKICK credit
  at once instead of one call per cycle.
- Diff mode, all 5 scenarios: 2.92 million runs compared, 0 differences (the suite now even passes in diff mode).
  Native: every picture and VU1 hash identical; first level 20.1 -> 30.1 game FPS (the cap), intro 271 -> 217 s,
  swim 160 -> 149 s, shoot 129 s.

## Build time (2026-11-10)

- The runtime is linked with link-time code generation: every link code-generated the whole game again (~20 min).
  Now `/LTCG:INCREMENTAL` (`ps2xRuntime/cmake/ReleaseMode.cmake`, also replacing CMake's IPO `/LTCG`): a relink
  redoes only the object files that changed.
- The generated programs are compiled with `/GL-` (code generated at compile time, in parallel, not again at the link)
  and `/Z7` (line numbers for the `[nprof] exclusive by line` table of `PS2X_NATIVE_PROF`; the code is the same).

## Other levels, self-check, merged programs (2026-11-10)

- The user's first `run_rayman_profile.bat` session (~31700 VSyncs, other places than the tests): 4.4 million VU1
  runs native, ~930 on the interpreter (314 entries not generated, 266 JR targets not seen, 349 runs longer than the
  65536-cycle budget that `ps2_runtime.cpp` gives MSCAL / MSCNT; the native code bails on those and the interpreter
  redoes them, still to improve). One new code image.
- Its new image held the same code as program 9ed834b59a07 with two more pairs reached: it became an 11th program,
  and the runtime picked the bigger one even for entries it lacked (intro under PAL: 99277 runs not covered, 8465 JR
  bails). Two fixes: the generator now merges images whose code agrees on everything their entries and JR targets
  together reach (one program knows all of them; 39 images -> 9 programs; stale `prog_*.cpp` files are removed), and
  the runtime takes the first matching program that has the entry (`vu1AotSelect` keeps all matches).
- Self-check in native mode (`vu1AotDispatch`): the first 2 finished runs of every (program, entry) also run on the
  interpreter and are compared (the interpreter's result is kept); a difference switches that entry of that program
  off for the session (`[vu1aot] self-check: ... switched off`, counter `disabled`; until 2026-11-11 the whole
  program, see "Fairy Glade" below). This covers entries the tests never reach.
- PAL timing (CLAUDE.md issue 15) changed the VSync rate, so the references were renewed; the suite was profiled again
  under PAL. Diff mode, all 5 scenarios: 2.90 million runs, 0 differences, 0 not covered, 0 JR bails.

## VU0 (2026-11-10)

- At 60 FPS (docs/framerate.md) Rayman's collision on the VU0 interpreter took ~23% of the game thread in water
  (`executeVU0Microprogram` inclusive). The same translation now covers VU0.
- Profiling: `PS2X_VU0_PROFILE=1` (out/vu0prof; `run_rayman_profile.bat` sets it too). In all 9 scenarios only two code
  images: program e09ad6f75847 (510 pairs, 95.7% of VU0 cycles: the collision library, entries 0x0 / 0xF30 / 0xFD0)
  and 63f357489401 (145 pairs). 7.7 million runs, none over the 4096-cycle budget. Both read flags (FSAND / FMAND), so
  they are generated with flags (`vu1gen.py --unit 0 --with-flags`: prog0_<key>.cpp, aot_programs0.cpp,
  vu1aot::kPrograms0; code addresses masked with 0xFFF; vu0aot_none.cpp when not generated). VU0 has no XGKICK / EFU /
  MFP / WAITP (the interpreter decodes them as reserved).
- The generator now supports a branch in a taken branch's delay slot (the interpreter's inTakenDelaySlot, PCSX2
  takedelaybranch): the first target's pair runs as the second branch's delay slot. The collision code depends on it
  (0x0718 IBEQ with B 0x0438 in its slot, CLAUDE.md issue 10).
- Runtime: the dispatcher is per unit (`Vu1AotUnit` g_vu1Aot / g_vu0Aot: mode, selection, counters, self-check);
  `PS2X_VU0AOT` = unset / 0 / diff like PS2X_VU1AOT; `[vu0aot]` lines.
- Diff mode, all 9 scenarios: 6.8 million VU0 runs compared, 0 differences, 0 bails, 0 not covered. Native: run_tests
  all PASS. Collision function 0x20C660 at 60 FPS 18-24% -> 7-11% of the time; idle at 60 FPS on land 3-6% -> 13-25%,
  at 50 FPS 7-21% -> 22-28%. The heaviest scenes at 60 are now bound by VU1 (~25%) and GS (~25%).

## Fairy Glade: a resume inside a delay slot (2026-11-11)

- The user saw 6-9 FPS in the Fairy Glade (Ly's cutscene). Their log: `[vu1aot-diff] vsync 22014 program
  4ead7b35a3a6 entry 10e8: cycles native 233 interpreter 202`; the self-check then switched the whole program off
  (28.7% of VU1 cycles): 1.35 million runs on the interpreter (`disabled`), 45-60 FPS -> 15-20, then 6-9.
- Cause: 0x10E0 is `B 0x1110`, 0x10E8 its delay slot. A run the interpreter stopped at the 65536-cycle budget right
  after the branch resumes (MSCNT) at 0x10E8 with `branchPending` set; the profile recorded 0x10E8 as an entry and the
  generated entry runs from there as ordinary code (on to 0x10F0 instead of 0x1110). Generated entries assume a pair
  outside any delay slot: `vu1AotPrepare` now leaves such resumes to the interpreter (counted `not_idle`), also those
  between an E-bit pair and its slot (`ebit` / `haltAfterDelaySlot`).
- The self-check now switches off only the (program, entry) that differed; the other entries stay native.
- Mismatch snapshots: the first 4 mismatches of each unit write `out/vu1aot_mismatch_<vu1aot|vu0aot>_<n>.bin`
  (registers, working clip flag, micro memory, data memory before the run, budget, MSCAL / MSCNT).
  `PS2X_VU1AOT_REPLAY=<file> ps2EntryRunner.exe` replays one without the game: the run in diff mode (the
  `[vu1aot-diff]` line), then a trace without instrumentation: for every cycle budget B the pc where each side stops
  (native: the pair it bails at, `BailBudget`; interpreter: where run() stops), so the first `<-- differ` line shows
  where the two part (`PS2X_VU1AOT_REPLAY_TRACE=0`: the diff run only). A hand-made snapshot of this case (entry 0x10E8,
  `branchPending`, target 0x1110) now reports `not_idle 1` (interpreter jumps to 0x1110); the same without a pending
  branch: both stop at the same pcs at every budget.

## Code generation notes (2026-11-11)

- The run's cycle count is a local of the generated function (`cyc`, `advanceLocal` in vu1aot_ops.h; stored to
  c.cycles on the successful exit only: the dispatcher undoes a bailed run and does not read it). Before, every pair
  did `c.cycles += n` in memory and the next pair's budget check read it back. Effect small (glade_explore: program
  4ead7b35a3a6 15.2% -> 14.5% of the game thread, 6af8ebd05633 3.9% -> 3.6%): out-of-order execution hid the chain.
- **MSVC's size limit**: a function past it is compiled without any optimization, silently (warning C4883 is off by
  default). The first try also stored c.cycles in every bail site (4677 stores in 4ead7b35a3a6): that program then
  compiled to 2.5 MB of unoptimized code (everything on a 760 KB stack frame) and took 70% of the game thread instead
  of 15%. The generated files are now compiled with `/d2OptimizeHugeFunctions` (optimizes them anyway: 0.6 MB, ~3 min
  for that file) and `/w14883` (the warning in the build log). Profiling new levels can make a program bigger: check
  the log for C4883 and the profile for a program that suddenly costs much more.
- Tried and dropped: an inline fast path in advanceLocal for XGKICK qwords inside a tag's data (kickQword only for
  tags and tag ends). kickQword 2.0% -> 0.5%, but the programs grew by the same (4ead7b35a3a6 14.5% -> 15.3%): about
  0.3% in all, for a build of the generated files of 43 min instead of 18 (thousands of inlined sites).
- Where VU1 time goes now (glade_explore at 60 FPS, game thread): 4ead7b35a3a6 14.5%, 6af8ebd05633 3.6%, the VU0
  collision e09ad6f75847 2.8%, kickQword 2%, lowerPlain 1.5%; idle 34%. The rest is the work of each pair (VF
  registers live in Ctx memory); a real gain needs registers kept in SSE locals across a block.

## Speed at 120 FPS: checks only where they matter (2026-11-16)

- At the busy spot by the ability shop (scenario `shop_walk_split`, 120 FPS with in-between pictures) the graphics
  thread was the limit (100% busy, ~118 FPS). Its profile (`PS2X_NATIVE_PROF_THREAD=GfxThread`): the generated
  programs ~57% (6af8ebd05633 25%, 4ead7b35a3a6 20%, c110bb6d5b85 7%), kickQword 5%. The program time was spread
  over all their code (hottest address 0.3-0.6%; the by-line table put 18% on one line of 4ead7b35a3a6, which is
  MSVC's line info in a huge function, not real): the cost was the work around every pair. The compiled code had per
  pair a budget compare, a load of `c.kick.active` and `cyc` spilled to the stack.
- Now cycles are counted in the local `cyc` only, and the XGKICK transfer is brought up to the current cycle
  (`kickSync`, vu1aot.h) only where its progress can matter: before a store into data memory (SQ ISW SQI SQD ISWR),
  at an XGKICK and at the end of the run. Nothing between two such points writes data memory or looks at the transfer,
  so the same qwords are copied from the same memory. Inside a GIF tag's data the qwords go in one copy (`kickRun`).
- The cycle budget is checked exactly at the run's last pair (the E-bit slot): the count only grows, so that check
  covers every earlier pair's (a run that gets there passed them all). Taken branches check loosely
  (`cyc >= budget`) so loops end; a bail there only means the interpreter redoes the run.
- Program 6af8ebd05633: 22000 -> 13250 instructions; the generated sources are half the size, and the VU build takes
  13 min instead of ~24.
- Diff mode (shoot_minisaurus, swim_tutorial, glade_explore, shop_walk_split, intro_to_first_level): 7.95 million VU1
  and 3.87 million VU0 runs compared, 0 differences (the intro's 5 budget bails as before). The two 120 FPS
  scenarios end by the time limit in diff mode (every run twice); their `[det]` fingerprints were identical as far as
  they ran.
- Busy spot (`-NoKickHash`): smooth ~118 -> ~120 FPS (the cap), plain 120 ~116 -> ~120; graphics thread 100% ->
  92-94% busy, the game thread no longer waits for it (room waits ~2600 per second -> 0). The GS thread (~86%) is
  next.

## Speed at 120 FPS: clamped copies of the registers (2026-11-16)

- Every SSE FMAC template clamped each operand (vuNormalizeOperand on 4 lanes, ~7 vector operations) on every use,
  about half of the instruction's work. Now `Ctx::nv` / `nacc` hold the clamped value of every VF register and of ACC:
  `ops::initNv` sets them when a run starts, and every write of a register refreshes its copy before the next read.
  The templates read the copies; what they store is clamped already (both operands were; an exponent-0 result keeps
  its sign; round toward zero never overflows to infinity; MAX / MINI return an operand; ITOF gives zero or a normal
  float; ABS of a clamped value), so the result goes into both. Exceptions take `nz4v` of the result or a refresh
  from the register: the MADD / MSUB lanes that keep the float result between FLT_MIN and FLT_MAX (it can be below),
  FTOI (integers), the scalar fallbacks (OPMSUB / OPMULA / cross, a product that may overflow, `fmacSse` with flags),
  the generic `upper()`. The generator adds a refresh after lowerPlain writes of a VF register (decode table), after
  a pair with a VF shadow (the shadow's final value), and when VF0 is restored.
- First try: the refresh after LQ / LQI read the register back right after `applyDest` wrote it lane by lane: a
  store-forwarding stall on every load (the hottest lines of the programs; busy spot 120 -> 113 FPS). Now LQ, LQI,
  MOVE and MR32 write the register with one 16-byte store and set the copy from the value itself (`loadVf`,
  MOVE / MR32 copy the copy: clamping works lane by lane), and FTOI clamps its result instead of reading it back.
- Busy spot (`-NoKickHash`, 120 FPS both modes): graphics thread 92-94% -> 75-77% busy; the GS thread (~80-85%) is
  now the busiest of the three.
- Diff mode (the same 5 scenarios): 7.95 million VU1 and 3.87 million VU0 runs compared, 0 differences.
- Full run_tests (native, with the kick hash): Overall PASS, every VU1 hash, picture and `[det]` fingerprint as
  before; `glade_split_hd` / `shop_walk_split` hold 120.0 even with the kick hash on (was 110 with it).
