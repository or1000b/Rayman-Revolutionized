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
