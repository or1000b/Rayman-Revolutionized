# Frame rate: PS2X_FRAMERATE (2026-11-10)

Goal (user): more frames per second without changing the game's speed: game time stays real time.

## How Rayman Revolution paces its frames

- Main loop function 0x1010F8. At its end (0x101208-0x10121C) it waits on a VBlank counter at gp-0x71D8
  (0x393218): `limit = (counter == 0) ? 1 : counter; while (!(limit < counter));`. The counter is counted by the
  INTC 2 (VBlank start) handler 0x1413C8 (installed at 0x1416D4) and reset to 0 at 0x101254, after the clock update.
  So a frame lasts at least **2 VBlanks: 25 FPS in PAL**, 30 in NTSC. A frame that took longer waits for the next
  VBlank. In-game the PAL game holds a steady 25 FPS (measured in every scenario, ~57% of the game thread idle).
- The US 60 FPS patch (PCSX2 `patch=1,EE,2010121C,extended,00000000`) writes a nop over the loop's branch
  `beqz v0, 0x101208` at 0x10121C: no wait at all. The PAL ELF holds the same instruction at the same address.
- Demo playback (attract mode; flag byte gp-0x6898, set by 0x211D50 when demo data exists): the clock takes each
  frame's step from the recording (table 0x3864C8, +0x16) and the wait uses `limit = n - 1`.
- Other VBlank handlers: 0x1AA808 (RenderWare: display buffer flip on the field bit), 0x158928 (wakes the loading
  screen thread 0x158958 -> 0x215508 / 0x215600 once per VBlank, only while loading: its animation speeds up with the
  VBlank rate, nothing else).

## How the game measures time

- EE timer 0, set up by 0x14C068 (for a 1000 Hz request: BUSCLK/256 = 576 kHz; overflow interrupt INTC 9, handler
  0x14C038 counts the high part at gp-0x70F8). 0x14C1B8 returns the 64-bit tick count. Real time, not VBlanks.
- The frame clock: a struct at [gp-0x7108] (0xBD1220 at run time), made once by 0x14BAA0 -> 0x14BB50(unit, 1000)
  with the unit = the float at 0x39286C = **0.02** (one PAL field). Each frame 0x14BCD8 (called by 0x14B9A0 at
  0x10123C) measures the time since the last frame and steps by whole units:
  `n = max(1, round(ms / (unit * 1000)))` at +0x24, `+0x20 = trunc(n * unit ms)`, `+0x14 += +0x20` (game ms),
  `+0x2C = n * unit` (step in s), `+0x30 = +0x14 / 1000` (game s), `+0x28` frame counter.
- Getters: 0x14B8E0 game s, 0x14B8F0 game ms (52 callers), 0x14B900 step s (23), 0x14B910 unit s (11), 0x14B920
  frame counter (10: frame stamps "done this frame", plus 0x1029D4 "the first 16 frames"), 0x14B930 n (8).
- Physics (dynamics 0x12BA18): `dt = n * unit` (0x12BB14 / 0x12BB20).
- The unit is also used to turn durations stored as **field counts** into time (e.g. 0x1E63D0: a halfword at
  0x253F08+0x44 times 1000 times the unit, compared with a ms timer). With another unit those durations change.
- 0x14BEC8 ("wait N ms") is our real-time HLE (main.cpp): it waits real milliseconds, whatever the VBlank rate.
- Consequence: VBlank at 60 Hz with the unit left at 0.02 makes every frame count 20 ms in 16.7 ms: the game 20% fast
  (what the PCSX2 "PAL at 60 Hz" trick does). Raising the frame rate needs the unit to follow, or a rate that is a
  multiple of the unit (50).

## The setting

`PS2X_FRAMERATE` (environment): unset / `original` = the game untouched (default; run_tests references); `N`
(25..240) = N frames per second; `uncapped` = 240 (experiment).
- Generic (EeScheduler.cpp `ps2xVBlankRateOverrideHz`): VBlank at N Hz (EE cycles and host deadline); CD streaming
  and MPEG picture timing count fields of that rate.
- Rayman (main.cpp `installRaymanFramerate`): the VBlank counter handler counts 2 per VBlank (at least 1 VBlank per
  frame: N FPS); the unit becomes 1/N s (constant at 0x39286C, and the clock's +0x8 / +0xC before every update); the
  integer ms step trunc(n * 16.667) = 16 would make the ms timers 4% slow, so it is corrected to the exact time (16,
  17, 17, ...); a correction a scheduler yield skipped is applied at the next update. Demo playback stays original.
  `PS2X_RAYMAN_CLOCK_UNIT_HZ` (experiments only) sets the unit apart from the frame rate.
- The window's Game FPS is counted at guest VBlanks (the old per-host-frame sample missed flips above ~30 FPS).
- Test harness: with PS2X_FRAMERATE the script clock is guest VSyncs x 50 / N, so a scenario's ticks are 20 ms of real
  time at every rate. `PS2X_RAYMAN_TRACK="from,to[,step]"`: `[rayman-track]` lines (position, state, air, game clock).

## Measurements (rayman_physics: the same input from the same spot, sampled every 20 ms)

| | 25 (original) | 50 | 60 | 120 | uncapped (240) |
|---|---|---|---|---|---|
| game ms per real second | 1000 | 1000 | 1000 | 1000 | 1000 |
| standing jump: airtime | 0.92 s | 0.90 s | 0.90 s | 0.88 s | 0.88 s |
| standing jump: height | 3.12 | 3.24 | 3.30 | 3.25 | 3.33 |
| helicopter fall speed | -3.16 | -3.50 | -3.58 | -3.43 | -3.19 |
| walking (best 10% of 1 s windows) | 9.38 | 9.37 | 9.25 | 8.78 | stuck |
| surface swimming (best 10%) | 10.15 | 10.41 | 10.97 | 10.19 | - |
| air while diving (swim_tutorial) | -4/s | | -4/s | | |

- Time is exact at every rate. The physics differs by a few percent (more for the helicopter) already at 50, where the
  unit does not change: the engine integrates once per frame, so another step size gives another trajectory (on the
  console the same happens when the game drops from 25 to 12.5 FPS). Routes then drift apart over seconds.
- An apparent 40% slower swimming at 60 in swim_tutorial was the route (other obstacles); the best free-swimming
  windows are not slower. Control runs: unit 0.02 at 60 FPS = everything 20% fast (walking 11.0); unit 1/60 at 50 FPS
  = everything 17% slow (walking 7.8).
- Cutscenes: intro_to_first_level at 60 FPS, pictures at the same real time against the original references: name
  entry 1.9, Ubi Soft logo 1.6, opening cutscene 1.1, Rayman on the grate 4.0 (max 20). The ship cinematic (9600) shows
  a later moment: with [rayman-track] the level's game clock runs at exactly 1000 ms per second in both, but at 60 the
  level finished loading 4.3 s earlier (Rayman gets control at 244.2 s instead of 249.4 s): the loading does a fixed
  amount of work per frame. The cinematic and its sound start with the level, so they stay together.
- Not tested yet: the water-ski with Ssssam, races and mini-games (no save there).

## Performance

- 25: ~57% idle. 50: 7-21% idle, steady 50 (rare dips to 48). 60: 3-6% idle, 60 on land, 39-49 in the water.
- At 60 the biggest cost is Rayman's collision 0x20C660 (VU0 microcode on the interpreter: 18-24% of the game thread),
  then GS ~18% and VU1 ~17%. When a frame takes longer than its VBlank budget, guest time falls behind real time (slow
  motion against the music) until it catches up.
- 120 / uncapped: the host draws 60-110 FPS (guest time lags real time); at 240 Hz Rayman got stuck in the air for
  18 s (collision / ground detection with 4 ms steps); at 120 the ground state flickers once and walking is 6% slower.

## Real-time pacing (2026-11-10)

- Before: when the host could not finish a frame within its VBlank budget, guest time (EE cycles, which the game's
  clock reads through timer 0) simply fell behind wall-clock time: slow motion against the music, and once the load
  dropped the late VBlank deadlines let the guest run unthrottled until it caught up (a fast-forward).
- Now (EeScheduler.cpp, normal play only; deterministic runs unchanged; `PS2X_REALTIME_PACING=0` = old behaviour): a
  VBlank processed more than one period after its host time skips the missed time in whole VBlank periods: EE cycles
  and EE timers jump to real time (at most 250 ms of game time per skip; a longer stall is dropped like a pause) and
  the next VBlank stays on the host grid. The game sees one longer frame and steps its clock by more units, like a
  console dropping frames. The IOP already keeps real time (wallClockDeficit), so sound is not touched. `[pace]` lines.
- Measured (scenario `pacing_realtime`: not deterministic, PS2X_FRAMERATE=120 while the host draws 61-81 FPS): game
  time 1000.0 ms per wall-clock second (n alternates 1 / 2); with `PS2X_REALTIME_PACING=0` 603.7 ms (40% slow motion).

## VU0 native (2026-11-10)

- With Rayman's collision microcode translated (docs/vu1_aot.md "VU0"): at 60 FPS the collision function 0x20C660
  went from 18-24% to 7-11% of the game thread, idle on land 3-6% -> 13-25%; at 50 FPS idle 22-28% (steady 50). The
  heaviest scenes at 60 (water, the beach after the swim) still drop to 36-56 FPS, now bound by VU1 (~25%) and GS
  (~25%); with real-time pacing those drops no longer slow the game down.

## Tests

`rayman_physics` (original), `rayman_physics_50`, `rayman_physics_60`, `swim_tutorial_60fps` (scenarios.json, check
type `track` in run_tests.ps1: game_ms_per_s, top_speed, air_rate, jump_airtime, jump_height, z_speed). The helicopter
check is KNOWN-FAIL at 50 / 60 (integration step), PASS at the original rate. `pacing_realtime` (not deterministic):
game time per wall-clock second while the host is overloaded (metric game_ms_per_host_s, host_ms in [rayman-track]).

## GS thread (2026-11-11)

- The user's 60 FPS play (after the Fairy Glade fix, docs/vu1_aot.md): mostly 55-60, drops to ~40 in big busy areas.
  `[prof]` there: VU1 ~29%, GS ~28%, idle 2-4%. `PS2X_NATIVE_PROF` on `glade_explore`: GS 25.7% of the game thread =
  17.3% the GPU renderer bridge (paraLLEl-GS's CPU side `gif_transfer` 12.3%, the bring-up walk `analyzeGif` 4.1%) +
  8.4% waiting for the lock while the window thread's scanout (flush, vsync, readback, fence wait: 3-4 ms per frame)
  held it. (In run_tests another ~12% is `PS2X_VU1_KICK_HASH`, the byte-wise FNV hash of every VU1 packet for the
  `vu1_hash` check; normal play does not hash. The profiler's tables now also give each top function's module,
  hottest address and callers: a DLL without symbols, or a name folded by /OPT:ICF, no longer hides what it is.)
- Now (`gs_pgs_bridge.cpp`, default; `PS2X_GS_THREAD=0` = the old synchronous bridge): the game thread copies each
  complete GIF packet / register write into a queue (at most 8 MiB ahead, then it waits) and one GS thread runs
  paraLLEl-GS and the walk, scanouts included: the window thread queues a scanout request with the privileged
  registers of that moment and waits for that picture (250 ms at most, else the previous picture stays), without
  holding the GS frontend's lock. What the GS sends back to the guest stays synchronous: SIGNAL / FINISH / LABEL reach
  it when the game thread queues the write (the rule the deterministic test mode always used), and a GS -> host
  transfer waits for the queue. `gsthread` in the `pgs:` stats (queued KiB, `full` = waits for room, `drains`,
  `late_scanouts`).
- Measured: `glade_explore` (60 FPS) GS 26% -> 6%, idle 1-2% -> 12%, game FPS ~50 -> a steady 60 (test mode, with the
  hash); `pacing_realtime` GS 19-23% -> 5%. run_tests all PASS; the `[det]` fingerprints of boot / menu / intro are
  identical to the synchronous bridge (281 lines) and every picture matches its reference with diff 0.0. (The
  scenarios that load a memory card differ from vsync 1320 on between any two runs, also synchronous against
  synchronous: something host-dependent in the card load, not the GS.)
- VIF1 (2026-11-11, same profile without the kick hash: idle 28%, VU1 27%, `processVIF1Data` 6.5% exclusive): the
  generic per-vector UNPACK loop now has a fast path for the common case (no mask, CL >= WL, S / V2 / V3 / V4 x 32 /
  16 / 8 and V4-5; `ps2_vif1_interpreter.cpp` `unpackFast`), the same results bit for bit (`PS2X_VIF_UNPACK_CHECK=1`
  runs both and compares: 0 differences in 12.6 million UNPACKs of intro + glade_explore; run_tests' VU1 hashes are
  identical). The 16 KiB VU1 memory copy at every MSCAL / MSCNT for the bad-packet dumps is only made when a bug hunt
  asks (`PS2X_VU1_DUMP_MSCAL=1` or the RGBA / NEGQ hunts). `processVIF1Data` 6.5% -> 1.7%, idle 28% -> 33%.
  `run_tests -NoKickHash` drops the hash for profiling runs.

## 120 FPS: the graphics thread (2026-11-12 .. 15)

- Goal (user): a steady 120 with SSAA x4 and 16:9 (`run_rayman_hd_120fps.bat`). Scenario `glade_120_hd` (the Fairy
  Glade save at those settings, `fps` check 110+). Before: 84-88 game FPS; `PS2X_NATIVE_PROF` on the game thread: VU1
  43% (generated program 4ead7b35a3a6 alone 21.6%), the GS bridge's lock and worker wake-ups ~5%, idle 3%.
  `PS2X_NATIVE_PROF_CHAIN="RtlAllocateHeap,Mtx_lock"` (new) prints the call chains of chosen functions;
  `PS2X_NATIVE_PROF_THREAD=GfxThread` samples another thread.
- GS bridge staging (`gs_pgs_bridge.cpp`): packets and register writes collect in a stage and move to the GS queue
  32 KiB at a time (a scanout request, a drain and the stop flush it first). 85.6 -> 87.9.
- **Graphics thread** (`ps2_gfx_thread.h`, code `ps2_gfx_thread.inc` included by `ps2_memory.cpp`;
  `PS2X_GFX_THREAD` = unset: on in normal play, off in deterministic runs; `1` / `0` force it). Like PCSX2's MTVU, but
  for the whole graphics side: `processPendingTransfers` copies the data of the GIF and VIF1 transfers and posts them
  to one worker, which runs the VIF1 unpacks, the VU1 programs and the GIF arbiter in order; HLE stubs' GIF packets
  (`submitGifPacket`) and GS register writes (`GS::writeRegister`) are posted behind them. The game still sees the
  transfer complete at once (as before). What the game can read back waits for the worker first (`sync`): reads and
  writes of 0x10003000-0x10006FFF (GIF / VIF registers and FIFOs), VU1 code / data memory, GS -> host transfers, the
  native GIF chain paths. The window's scanout uses the display registers (PMODE, DISPFB, DISPLAY) as of the task the
  worker is running (`shadowDisplay`): a buffer flip the game made after posting its frame shows once that frame is
  drawn. On the worker the VU1 run skips the EE context's D / T bit enables and VPU_STAT (debug bits no retail game
  uses). The CPU renderer switches it off. `[gfx] posts N, the game waited ...: S syncs, F full queue` at exit.
- Races found on the way (`tools/crashstack.py`, new: runs the exe under a minimal debugger and prints the faulting
  thread's stack, for heap corruption 0xC0000374 that no in-process handler sees): `processPendingTransfers` drained
  the GIF arbiter on the game thread when a stub called it with nothing pending (the arbiter belongs to the worker);
  the VU1 self-check's packet capture pointer was a global that a VU0 self-check on the game thread set (now
  thread-local).
- Measured (`glade_120_hd` family, game FPS after tick 4500, `-NoKickHash`):

  | SSAA | without graphics thread | with |
  |---|---|---|
  | x1 | 81.6 | 119.1 |
  | x2 | 80.7 | 103.1 |
  | x4 | 87.5 | 119.9 (min 119.4, median 119.8; GPU ~29%, scanout 2.6 ms) |

  One x4 run gave 74.7 with the scanout at 19 ms and the worker waiting 52% of its time for room in the GS queue: the
  GS thread / GPU was slow then (another program on the GPU?); not reproduced.
- **Crowded places (2026-11-15, the user's run `hub_run_120`: the plain, the Fairy Glade portal and bridge).** The user
  still saw drops: 40-56 game FPS for ~25 s on the plain. There the game thread waited for the worker ~21% of its time
  (`ps2x::gfx::post`, full queue) while the worker was idle ~37% of its own: busy scenes post ~4.1 MiB of transfers per
  frame (`[gfx] ... MiB posted` at exit, new), so the 4 MiB queue let the game run at most one frame ahead and the two
  threads went in lock-step. The game thread also copied every DMA chain into a new vector (page faults on fresh memory)
  that the worker then freed (VirtualFree for the big ones).
  - DMA chain buffers are recycled (`takeChainBuffer` / `recycleChainBuffers` in `ps2_memory.cpp`, up to 64 kept).
  - The queue holds 16 MiB (~4 frames; `PS2X_GFX_QUEUE_MB`): at most ~33 ms more picture lag at 120 FPS, and only
    while the worker is the slower thread.

  | hub_run_120, game FPS (`-NoKickHash`) | all | plain | after the plain | Fairy Glade |
  |---|---|---|---|---|
  | before | 85.3 | 54.6 | 79.9 | 70.9 |
  | buffer pool | 89.3 | 56.6 | 85.8 | 71.8 |
  | pool + 16 MiB queue | 95.6 | 71.6 | 123.5 | 80.1 |
  | pool + 32 MiB queue | 93.9 | 75.4 | 116.6 | 77.0 |

  Still short of 120 there: both threads are near their limit (worker: VU1 programs ~32%, `prog_6af8ebd05633` alone
  ~21%; game thread: the chain copies, ~4 MiB per frame, and guest code). From VSync 9060 (the Fairy Glade) every replay
  of this recording has its own EE hash at the same cycle counts, also between two runs of one build: an older
  nondeterminism there, not from these changes (to look at).

## Where 120 still drops (2026-11-16)

- The user: "standing still it holds 120, when Rayman walks the FPS drops" (smooth mode, their log of 2026-11-15
  23:58: 120 at the save point, 77-113 walking around the area by the ability shop). The drop follows the view, not the
  walking: standing in a busy spot is just as slow, and those log lines had twice the primitives per frame (~90k).
- Tools: `PS2X_FPS_LOG=<ms>` (`[fps]` lines: guest vsync, game FPS in that window, longest wait between two game
  frames as the window loop saw it; test runs draw the window at 60, so 16.7 ms there is normal); `busy=` in the
  `gsthread` stats and `gfx busy=N% full=M` in the `[threads]` line (share of real time the GS thread / graphics thread
  spent working since the last line, and the game's waits for room in the graphics queue). Scenarios (all `-NoKickHash`
  for speed): `glade_walk_split` / `glade_walk_120` (Fairy Glade save: stand, walk 3 s, jump 3x, shoot 5x),
  `shop_walk_split` / `shop_walk_120` (the shop save, same moves; it loads Rayman in a busy spot that is the measuring
  window, 3900-4370; the walk then takes him through a portal), `hub_split_fps` (local fixture, a copy of
  `hub_run_120.input`; the recording drifts at logic 25, so only its first part is the user's path).
- Measured (game FPS, test mode):

  | place | smooth (logic 25) | plain 120 |
  |---|---|---|
  | Fairy Glade log (glade_walk_*): stand, walk, jump, shoot | 120 all the time | 120 all the time |
  | shop save spot (shop_walk_*), before | 86 (GS thread 91%, graphics 78%) | 74 (GS 83%, graphics 67%) |
  | same, asynchronous scanout | 89 (GS 78%, graphics 80%, game thread idle 7-9%) | 75 (game thread idle 9%) |

- **Asynchronous scanout** (`gs_pgs_bridge.cpp` `PendingReadback`; `PS2X_GS_ASYNC_SCANOUT=0` = the old way): the GS
  thread waited for the fence of every picture's readback (the GPU drawing the whole frame, 3.5 ms a frame there), so
  the CPU side of the next frame and the GPU side of this one ran one after the other. Now it records the copy and goes
  on; the window thread, which waits for that picture anyway, waits for the fence and copies the pixels (the next
  request comes from that thread after the copy, so one readback buffer is enough). The GS thread's scanout share went
  from 2-3.5 ms to ~0.5 ms a frame. `out/pgs_frame.bmp` is now written by the window thread every 5 s (it was every 300
  frames on the GS thread: a 5 MiB file every 2.5 s at 120).
- What limits it now: the game thread. `PS2X_RAYMAN_PHASES` at the spot (smooth): frame 11.0 ms, the game step 9.8 ms,
  of which `0x14B348` (sends the display lists) is 6.7 ms by the phase clock, but that clock also counts the guest's
  time slices that end inside it; the native profile shows the time spread over guest code and runtime overhead: the
  memory helpers ~21% (out-of-line copies of `ps2_runtime_macros.h` code: the nprof names `operator delete`,
  `isSpecialAddress`, `sub_0022E7B8` all point at +0x2628xx-0x2629xx), `dispatchGuestBranch` ~7% plus a neighbour
  shown as `reportMissingFunction` ~3.5%, locks ~4%, heap ~3%, vector copies ~4%. A 120 there needs about a quarter
  less game thread time per frame.

## Frame-rate bugs in the game's scripts (2026-11-15)

The physics steps once per frame, so a scripted move ends at a slightly different spot at another frame rate. Where a
level was tuned to the 25 FPS spot, that can break a script. Known and fixed:
- **Ability shop by the well of whirlpools** (CLAUDE.md issue 22): the exit run stopped inside the shop's own trigger
  zone at 60 / 120 FPS and the shop ran Rayman back in. Fix: that one zone (found by its data) is 7.2 units instead of
  8.51 when PS2X_FRAMERATE is set (main.cpp `raymanPointInGeoShopFix`); scenario `shop_exit`.
- Tools to find the next one: `PS2X_RAYMAN_RATE_SWITCH` (replay the same moment at 120 / divisor FPS),
  `PS2X_RAYMAN_AI_WATCH`, `PS2X_RAYMAN_TRACK`, and `record_for_claude.bat` for the user's recordings.
