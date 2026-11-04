# Rayman Revolution native PC port (PS2Recomp)

## Talking to the user
- Always reply in **Hebrew**, even when the user writes in English or pastes English logs.
- The user (Or) is self-taught in programming (no CS degree). Explain the *why* in plain words, briefly; skip jargon or explain it once.
- The user runs Windows with a Ryzen 7 9800X3D and an RTX 5070 Ti.
- Long-term goal (the user's words, paraphrased): not "a PS2 emulator", but the same game running the modern way: graphics on the GPU, logic on the CPU, sound through the sound card. That gives a base to later add ray tracing, high resolution, widescreen and high frame rates (e.g. 240 FPS).
  Every design decision should move toward that. Emulating PS2 hardware is acceptable only as a temporary bridge or a reference to compare against.

## What this is
- Game: **Rayman Revolution, PAL, SLES-500.44**. The ELF and disc files are in `Rayman revolution files\`. **Never commit or upload them** (copyrighted, ~4 GB).
- Tool: PS2Recomp (GPL-3.0). It statically recompiles the EE (R5900) code to C++; the runtime then high-level emulates the rest of the PS2.
  - EE code region: 0x100000-0x393bf0, entry 0x100008.
  - The upstream base is the PS2Recomp `main` branch as of 2026-10-22, commit e5636981. It was downloaded as a zip, so there was no git history at first.
- Game engines:
  - **OpenSpace** (Ubisoft) runs the game logic and AI.
  - **RenderWare 3**, the PS2 "Sky" driver from Oct 2000 (basky.c, skyinst.c, skyconv.c ...), does all rendering. The RenderWare logo shows at boot.
- Disc layout:
  - `GAME.DSC` is a plain-text index of `GAME.BF`. Per level it lists `.ato.N`, `.lm3.N`, `.lv2`, `.pt2`, `.rw3.N` with offset and size.
  - `SOUND\*.BF` holds the sound banks and streams (SNDENG.BF, BNMENG.BF, VSC.BF).
  - `MODS\` holds the IOP modules: SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, UBISOUND.
- Reference projects on this PC:
  - `D:\downloads\rac-rs-main\rac-rs-main`: a Ratchet & Clank PS2 port (GPL-3.0). It uses a native wgpu renderer for the 3D world, GS capture only for the HUD, VU1 ahead-of-time compilation, a reimplemented sound driver (989snd, no IOP emulation), and deterministic gate tests. Use it as the model for where this port is going.
  - librw (github.com/aap/librw) is an open reimplementation of RenderWare 3 (PS2 native geometry, TXD/rasters). It is the reference for the RenderWare layer.

## Build and run (Windows, MSVC + Ninja)
- `build_rayman.bat`: the full pipeline.
  1. Builds the recompiler (`out\tools`).
  2. Recompiles the ELF with `rayman.toml` into `out\rayman_recomp` (log in `out\recomp.log`).
  3. Copies the generated `sub_*.cpp` and `entry_*.cpp` into `ps2xRuntime\src\runner\` and the headers into `ps2xRuntime\include\`.
  4. Configures `out\game` and builds the `ps2EntryRunner` target.
  - Slow; only needed after changing the recompiler or `rayman.toml`.
- `rebuild_rayman.bat`: incremental build (`cmake --build out\game --target ps2EntryRunner`, output in `out\build_game.log`), then runs the game.
- `run_rayman.bat`: moves `out\run.log` to `out\run_prev.log`, then runs `out\game\ps2xRuntime\ps2EntryRunner.exe "<repo>\Rayman revolution files\SLES_500.44"` with stdout and stderr going to `out\run.log`.
- The .bat files end with `pause`. When you build yourself, call the tools directly inside a vcvars environment, e.g.:
  ```
  cmd /c "call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build out\game --target ps2EntryRunner"
  ```
  Get VSPATH from `vswhere -latest -property installationPath`.
- The game runs until its window is closed, unless `PS2X_TEST_FRAMES` is set (see Test automation).
- **Every link takes ~9 minutes** (LTCG, `/GL` + `/LTCG` from `cmake/ReleaseMode.cmake`), even for a one-line change. Turning LTCG off would recompile all generated code, so it stays. Batch changes before building.
- The exe gets a PDB (`/DEBUG:FULL /OPT:REF /OPT:NOICF`, link step only), so stack dumps show function names. The GS bridge and paraLLEl-GS are compiled with `/Z7` for line numbers.
- paraLLEl-GS is patched at configure time by `ps2xRuntime/cmake/PatchParallelGs.cmake` (idempotent string replace with a marker; fails loudly if the anchor disappears).
- The first build after the GPU renderer was added downloads Granite and paraLLEl-GS with FetchContent (git, pinned commits). This needs network access.

## Hard rules (learned the hard way)
- **Do not change headers that `ps2_runtime.h` includes.** Every generated `sub_*.cpp` includes it, so a change triggers a full rebuild of all of them (very long).
  - Affected: `gs_frontend.h`, `ps2_gif_arbiter.h`, `ps2_memory.h`, `ps2_runtime.h` itself, and the public headers of `ps2xIOP/include/ps2x/iop/`.
  - Private headers and `.cpp` files are fine.
  - No `PUBLIC` compile definitions on `ps2_runtime` either.
  - When new data must cross a module boundary, use small `extern "C"` functions declared in the `.cpp` files. Examples: `ps2xSpu2StatsLine`, `ps2xIopSetHostCdReader`, `ps2xInstallIopToEeRpcHooks`.
- `Kernel/Stubs/Helpers/Support.h` is included through `Common.h` by many stub `.cpp` files. Its globals sit in an anonymous namespace, so **every translation unit has its own copy**. The CD file map (`g_cdFilesByKey`, pseudo LBNs from 0x100000) that matters is the one in `CD.cpp`.
- EE EABI: arguments 5-8 are passed in `$8-$11` (t0-t3), not on the stack.
- The R5900 has 3-operand `mult rd, rs, rt` (rd = LO). objdump prints it as `<unknown>`; the recompiler handles it.
- Generated code (`ps2xRuntime\src\runner\sub_XXXXXXXX_0x....cpp`) carries the original MIPS instruction as a comment on every line. That makes it a usable disassembly when no MIPS objdump is available.

## Architecture as it is now
- **EE**: recompiled C++ on a cooperative scheduler (`Kernel/EeScheduler.cpp`).
  - Game-specific high-level replacements live in `ps2xRuntime/src/main.cpp`, under `installGameHle` for SLES-50044:
    - `0x14BEC8`: the engine's "wait N ms" helper, replaced by a real-time wait that advances guest time.
    - `0x1DF7F0`: the AI behaviour-tree evaluator, wrapped by a NULL-tree guard (probably obsolete, see issue 4).
    - `0x1DF760`: AI node dispatch, wrapped by a tracer that keeps the last 64 nodes and reports a node pointer below 1 MiB (`[rayman-ai] node pointer ...`, plus scheduler / branch / dispatch history).
- **IOP** (`ps2xIOP`): an R3000 interpreter that runs the game's real IRX modules.
  - EE->IOP RPC is synchronous (`IopRpcBridge::handleRpc`).
  - IOP->EE RPC is implemented in `services/iop_rpc.cpp` (sifcmd 15/16/18) and `Kernel/Syscalls/RPC.cpp` (`SifRegisterRpc` reads cfunc/cbuf/qd from `$8-$10`; queue-walk fallback).
  - Real-time pacing: a wall-clock floor in `runEeCycles`.
  - Stacks are filled with "self-pointer garbage" (`fillStackGarbage`). UBISOUND reads an uninitialised stack flag during voice init and only works with garbage there.
- **UBISOUND protocol**:
  - EE->IOP: sid `0x65653269` ("ee2i"), `function = (module<<16)|cmd`. Request is `[outSize][inSize][data]`, reply is `[result][data]`. Init is `0x00010000`.
  - IOP->EE: sid `0x696f3265` ("io2e"), fn `0x30000` notifications (12 bytes in, 4 bytes out). The EE server thread loop is at 0x165978.
  - UBISOUND reads music and banks **itself** through IOP cdvdman `sceCdRead`, using the EE's pseudo LSNs. Since r9, `iop_cdvd.cpp` asks the host reader hook first (installed from `CD.cpp`, `ps2xInstallIopCdReader`). Imported cdvdman ordinals: 6, 7, 11, 13, 37, 39.
- **SPU2**: `ps2xIOP/src/emulator/core/iop_spu2.h`.
  - Covers ADPCM, ADSR, IRQA, ENDX/NAX, ADMA, and MMIX/VMIX routing. 768 IOP cycles per sample.
  - Not covered: reverb, noise, pitch modulation. Volume sweeps jump straight to their end value.
  - Output goes to a ring buffer read by a raylib AudioStream callback (`ps2_runtime.cpp`, `startSpu2Stream`). On underrun the last sample fades out.
- **GS**: two renderers behind `gs/gs_frontend.cpp`.
  - The CPU renderer (`gs_cpu_backend.cpp`).
  - The GPU renderer, paraLLEl-GS (Vulkan compute, LGPL-3.0). It sits behind the bridge in `gs/pgs/gs_pgs_bridge.{h,cpp}`, which is its own static library so Granite headers and defines never reach generated code.
  - `PS2X_GS_RENDERER=cpu` (environment variable) forces the CPU renderer.
  - The bridge serialises every call with one mutex, reassembles split PACKED/REGLIST GIF packets per path (paraLLEl-GS spins forever on a piece shorter than one loop), synthesises SMODE1 (the BIOS normally writes it), and converts the libgraph stubs' DISPLAY values from pixel units (MAGH = 0, DW = width - 1) to video clocks.
  - Partial **IMAGE** packets are *not* kept pending: they are re-tagged to the qwords present and sent at once. The VIF (`ps2_vif1_interpreter.cpp`, DIRECT continuation) re-tags the rest of an image as a new IMAGE packet, so a pending old tag would swallow the new tag as pixels and shift the whole path (texture data parsed as GS registers, 4096x4096 prims, GPU driver reset).
  - The bridge walks every packet it forwards (`analyzeGif`) and keeps per-frame stats (kicks, prims, TEX0/CLUT loads, uploads, largest prim) plus `[pgs-gif] anomaly` lines for out-of-range values. `PS2X_PGS_TRACE=1` prints one line per frame.
- **Window**: raylib with `FLAG_WINDOW_ALWAYS_RUN`. Without it raylib parks the window thread in `glfwWaitEvents` while minimised, which also stops GS scanout.
- **VU1**: an interpreter (`vu/`). It costs about 29% of a frame in levels. AOT compilation like rac-rs (`vu1gen.py`) is a candidate, but a native renderer would remove most of it.

## Diagnostics already built in
- `[threads]` line every 120 ticks in `out\run.log`: per-thread state, gameFps, DMA/GIF counts, GS or pgs stats, spu2 stats, `iopcd reads/sectors/host/fail`, and `iop->ee posted/done/pending`. The pgs part includes PMODE/SMODE2/DISPFB/DISPLAY.
- `[prof] last 10s`: time split VU1/GS/IOP/idle plus the hottest EE functions.
- First bad indirect branch: a `[guest-branch:missing-target]` line, a `[guest-branch:regs]` line, and a dump of the whole 32 MiB of EE RAM to `out\crash_ee_ram.bin`.
  - Analysis tip: walk the stack in the dump and match saved registers to function prologues.
- `out\pgs_frame.bmp`: a GPU frame about every 5 s. Look at it instead of asking the user to describe the screen.
- F11 in game: one frame of GS commands to `out\gscapture.bin`.
- `[rayman-ai] NULL behaviour tree ...`: logged by the 0x1DF7F0 guard.
- **Hang watchdog** (`ps2_test_harness.cpp`, always on, `PS2X_WATCHDOG_SEC`, default 15): when the window thread draws no frame for that long, every thread's native stack is written to the log as `[watchdog]` lines (function names + file:line from the PDB). Read the `GameThread` and main-thread stacks first.
  - It only sees a stuck *window* thread. A stuck game thread with a live window shows as `gameFps=0` in `[threads]` lines instead.
- **Crash report** (`ps2_test_harness.cpp`, vectored exception handler): `[crash] first-chance exception ...` with the faulting thread's native stack. The game thread keeps 256 KiB of stack in reserve (`SetThreadStackGuarantee`) so this works after a stack overflow too. If a process dies with no report, read Windows' Application log (event 1000: module + offset) and resolve the offset with dbghelp and the exe's PDB.
- **Exe stack size** is 64 MiB (`/STACK`, link only). Guest calls are nested host calls.
- `[guest-recursion]` (`dispatchGuestBranch`): guest call depth 4000 reached; prints the outermost and innermost guest frames (target, caller, a0-a3, sp, ra), dumps EE RAM to `out\crash_ee_ram.bin` and stops cleanly.
- `ps2xDumpSchedulerTrace()` (EeScheduler.cpp, `[sched-trace]`): last 64 scheduler-level dispatches (thread, invocation depth, pc/ra/sp/v0/a1) = thread switches, interrupt handlers, resumes after an unwind.
- `ps2xDumpBranchTrace()` (ps2_runtime.cpp, `[branch-trace]`): last 64 `dispatchGuestBranch` calls of the thread and how each ended (K checkpoint, J jump to scheduler, M missing, T returned, F unwound).
- GPU driver resets (TDR) show up in the Windows System log as `nvlddmkm` event 153; Granite then spins forever in `vkResetCommandPool`. Check with `Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'}`.

## Status and open issues (2026-11-04)
1. **GPU renderer: works through the language menu (verified 2026-11-02 by run_tests).**
   - Full-width 640x512 picture; PRESS START and LANGUAGE CHOICE draw correctly, ~30 game FPS (CPU renderer: ~2.8).
   - The hang after Enter had two causes, both fixed: a paraLLEl-GS CPU-side loop (`compute_page_rect` block range underflow, patched) and the IMAGE re-tag desync in the bridge (above).
2. **Sound: works through the emulated UBISOUND (fixed 2026-11-04; peak ~3000 in the menus, was ~130).** Three IOP-emulator bugs, all in `ps2xIOP/src/emulator/`:
   - RPC handlers ran with a 2M-instruction cap. UBISOUND's bank load (1.8 MiB to SPU2 RAM by PIO) was cut off: a third of the bank loaded and an unwritten reply, so every sound started at SPU2 address 0 (silence). Now `kMaxRpcCallInstructions` (400M); an abandoned call logs `[IOP] guest call 0x... did not return` (run_tests checks for it).
   - SetAlarm / CancelAlarm were stubs, so UBISOUND's tick thread (every driver's update: voice end detection, channel release, streams, IOP->EE notifications) never ran: sounds never ended and after ~24 plays every play failed. Now real alarms (`dispatchAlarmImport`, `serviceAlarms`).
   - The IOP heap was a bump allocator (freed memory never reused) over 832 KiB only; the level start's 900 KiB load failed. Now one first-fit arena 0x10000-0x1F0000 shared by module images and AllocSysMemory (modes 0/1/2), with QueryMaxFreeMemSize / QueryTotalFreeMemSize. Module bases moved (UBISOUND is no longer at 0x45d00: read `[IOP] loaded IRX` lines).
   - Now: streams read from the disc, `iop->ee posted` > 0, no failing play / load commands. Not yet listened to by the user.
   - Tools: `PS2X_UBI_TRACE=1` (`[ubi-rpc]` per ee2i call), `PS2X_IOP_WATCH="addr[:reg:bytes],..."` (`[iop-watch]`), `PS2X_SPU2_DUMP_AT="700,2300"` (`[spu2-dump]`), first 40 key-ons as `[spu2-kon]`.
   - **Decided (2026-11-02): build sound the rac-rs way** (exact native port of UBISOUND + native SPU2 voice mixer, verified against the emulated driver as oracle). The oracle is now trustworthy. Plan, stages and reverse-engineering notes: `docs/ubisound_port.md`. Disassembly: `tools/irxdis.py` (capstone; output under `out/analysis/`).
3. **Fixed (2026-11-02): the AI crashes were a runtime bug in `dispatchGuestBranch`, not game data.**
   - After a call returned, `dispatchGuestBranch` treated `ctx->pc == entryPc` as "the callee returned without touching pc". With recursion (`f -> ... -> f`), a checkpoint taken just before the inner call to `f` leaves exactly that pc pending, so the *outer* call to `f` looked finished: the caller went on with garbage registers and skipped epilogues (stack not popped).
   - Rayman's AI evaluator is recursive (`0x1DF760` node dispatch -> `0x1DF3E0`/`0x1DF310` -> `0x1F2540`/`0x1F1D80` -> `0x1DF760`). Symptoms, depending on where the checkpoint fell: a node pointer of `0xB` (the next node's type byte left in `v0`) and an endless walk through zeroed low memory (host stack overflow), or `ra` loaded from the wrong stack slot (jump to `0x1fffa90`), or the old "NULL behaviour tree" (issue 4 below).
   - Fix: a thread-local "yield pending" flag in `ps2_runtime.cpp`, set when a branch hands control to the scheduler (checkpoint `K` or jump `J`) and cleared by `EeScheduler::run` before it re-enters guest code (`ps2xGuestYieldReset`). While set, every host frame returns false.
   - Found with the deterministic mode: the crash became 100% reproducible, then `[guest-recursion]`, `[rayman-ai]`, `[sched-trace]` and `[branch-trace]` pinned it down (see Diagnostics).
4. **The NULL-tree guard at 0x1DF7F0 is probably obsolete** (same bug as 3). It is still installed; it logs `[rayman-ai] NULL behaviour tree` if it ever fires. If JAIL_10 / SKI_60 run clean without it, remove it.
   - Background, kept for reference: node layout 8 bytes = 32-bit param, depth byte at +6, type byte at +7; node dispatch table 0x3813B8 (type 0 -> 0x1DF310 "function", 2 -> 0x1DF3E0); function tables 0x382E48 (590 entries) and 0x382EC0; comport helpers `0x2009A0`, `0x200DD0` (0x68-byte entries, tree pointer at +0x60).
5. **Performance**: levels ran 7-15 FPS on the CPU renderer (GS drain waits plus the VU1 interpreter). Menu loading looks slow mostly because of the game's own waits.
6. **3D polygons look messy** (reported by the user, 2026-11-02). Partly investigated.
   - Menus / cutscene: the 3D backgrounds show diagonal streaks and a teal tint (`tests/golden/boot_language_menu.bmp`, `english_main_menu.bmp`). The CPU renderer shows the same, so the cause is before the GS.
   - Ruled out (2026-11-03, `[pgs-cover]` report): texture coordinates (S/Q, T/Q in normal ranges, ~5% degenerate triangles), mipmapping (MXL 0), depth (99% of Z within 16 bits; the 16-bit RenderWare microcode is the one uploaded), texture / CLUT uploads (real data at the expected TBP / CBP), GIF packet order (FIFO instead of the priority sort changed nothing).
   - Seen: 1-10% of world vertices have negative Q (behind the camera); RenderWare's VU1 code should clip those. Giant triangles (boxes to -1792) may be those. Next: check the VU1 CLIP / clipper path.
   - Rendering technique, for reference: the world is drawn (context 0, TFX modulate), then three per-channel lightmap passes (context 1, FBMSK keeps one of R/G/B, ALPHA 0x89 = Cd * As, Z test GEQUAL without Z write) with 128x128 T8 lightmaps (uniform index 0x60, CLUT ramp alpha = index, so x0.75).
7. **Black screen after the opening cutscenes** (reported by the user, 2026-11-02). Reproduced by the scenario `intro_to_first_level` (NEW GAME, START on the name RAYMAN, logo, intro cutscene ~3400-7900, loading screen with the logo ~8100-8300, level from ~8400).
   - Not the sound (2026-11-04): with sound fixed the level still shows black. The first guess (fade-in never starts) was wrong: `[pgs-bigprim]` shows the black overlay (`type 5 abe 1 rgba XX000000 z=0xffff`) fading 0x80 -> 0 by 3 per frame, and later frames have no overlay at all, yet the picture stays black (screenshots at VSync 9100, 9600, 10200 are byte-identical, pixel values 0-3). The level logic runs (ms timer at 0xbd1234 advances, sounds play for ~40 s).
   - So the level itself is drawn black. Suspects: (a) the lightmap multiply passes (ALPHA 0x89 = Cd * As) with empty / zero-alpha lightmaps; (b) VU1 clipper garbage: many untextured strips with a vertex at GS (0,0) (box from -1792) including multiply passes, the same giant triangles as issue 6. Next: an experiment that skips the multiply passes, then the lightmap uploads of the level.
   - Not the memory card: our run never writes a save (only GetInfo / GetDir), and the user's run with a save is black too.
   - Tools: `PS2X_EE_DUMP_AT=<vsyncs>` writes `out/ee_ram_<vsync>.bin`; `PS2X_PGS_COVER_FROM=<frame>` turns on the `[pgs-cover]` / `[pgs-blend]` / `[pgs-upload]` reports (frames are scanouts: not deterministic, fewer than VSyncs in the level); with it `PS2X_PGS_BIGPRIM=1` logs every untextured primitive bigger than 500x350 with TEST / ZBUF / Z (`=2`: fans and sprites only) plus the display buffers per scanout (`[pgs-scan]`); `PS2X_PGS_WATCH_LO/HI` choose the upload watch range.

## Roadmap (agreed with the user)
1. Stabilise the game logic (crashes, hangs).
2. **Native renderer in Vulkan** (Vulkan because of ray tracing on the RTX card).
   - Hook the RenderWare 3 layer in the binary: atomic/geometry render, Im2D/Im3D, raster/texture creation, camera.
   - Decode PS2 native geometry and textures once, cache them on the GPU, and draw them ourselves.
   - Retire VU1/GS emulation step by step, keeping paraLLEl-GS as a reference for side-by-side checks.
3. **Native sound**: implement UBISOUND's ee2i commands on the PC, decode the banks, mix through the host audio API.
4. Modern features:
   - Resolution and widescreen.
   - High FPS: logic stays at its rate and rendering is interpolated.
   - Ray tracing.

## Test automation (built 2026-11-02)
- `run_tests.bat [-NoBuild] [-Only <scenario>] [-UpdateGolden]` (logic in `run_tests.ps1`, Windows PowerShell 5.1): builds, runs each scenario of `tests\scenarios.json` unattended (minimised window, no focus), checks it, writes `out\test_report.txt`, exit code 0 = no FAIL. No `pause`, so Claude can run it.
  - Per scenario: `out\tests\<name>\run.log` and the checkpoint pictures.
  - Check types: `exit_code`, `log_absent` (regex), `shot` (luma variance + mean difference to a golden picture), `fps`, `iopcd`, `audio_peak`.
  - `known_fail` on a check = expected failure with a known cause: reported as KNOWN-FAIL, does not fail the run; if it passes it shows as `FIXED?`.
  - Golden pictures: `tests\golden\` (game content, git-ignored). Refresh with `-UpdateGolden` after an intended visual change.
- Runtime side, `ps2xRuntime/src/lib/ps2_test_harness.cpp`, driven by environment variables:
  - `PS2X_TEST_INPUT="600:start:10,1300:cross:10"` pad script (tick = window frame, ~60/s; button names in the file).
  - `PS2X_TEST_FRAMES=N` stop after N window frames. `PS2X_TEST_SHOTS="590,1400"` save `out\test_shot_<tick>.bmp` (works with both renderers).
  - `PS2X_TEST_MODE=1`: the watchdog also ends the process (exit code 3); the window opens without focus.
  - To run the game by hand with these, set them in the shell first. In `cmd`, a comma ends an argument: pass scripts through environment variables, not `%1`.
- Timing: PRESS START is up by tick ~250; Start at 600 reaches the language menu by ~700.
- **Deterministic mode** (`PS2X_DETERMINISTIC=1`, on by default in run_tests): the same input gives the same run, down to the EE cycle.
  - EeScheduler: due events run one at a time in guest-cycle order; the idle wait picks the next event by cycles. The host clock only paces.
  - IOP: no wall-clock top-up (`wallClockDeficit` returns 0). GS SIGNAL / FINISH / LABEL reach the guest when the register passes the bridge, not from paraLLEl-GS's waiter thread.
  - Harness: pad script, frame budget and shots count guest VSyncs; real keyboard / gamepad input is ignored in test mode; every 60 VSyncs `[det] vsync=N cycle=C ee=<EE RAM hash>`.
  - Check type `deterministic` runs a scenario twice and reports the first differing fingerprint.
  - Use it for any "sometimes" bug: make it reproducible first, then instrument.
- Small unit tests:
  - An IOP harness that loads the real IRX modules and runs UBISOUND's init RPC. It existed only in the previous assistant's sandbox; recreate it under `ps2xTest` if useful.
  - SPU2 decoder tests.
  - GIF reassembly tests.

## Housekeeping
- Initialise git early, with `.gitignore` excluding game data, and commit after every working step.
- Before editing a file, check whether it is one of the protected headers above.
