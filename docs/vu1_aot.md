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
  flags, data memory, packets, cycles) and reports the first difference; the interpreter's result is kept.
- `run_tests`: all golden pictures and `[vu1-hash]` references must stay identical; the unit tests must not regress.

## Steps

1. References (done 2026-11-10): `vu1_hash` check, golden pictures for 5 scenarios, `shoot_minisaurus`.
2. Survey (done 2026-11-10, results below): which code images, entries, JR targets, cycle shares.
3. Diff harness + shared kick submission.
4. Generator, program by program (the biggest cycle share first), each one validated with the diff mode and
   `run_tests` before it is switched on.

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
