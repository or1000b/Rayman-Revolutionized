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
- **EE memory watchpoint** `PS2X_EE_WATCH="addr[,addr..];fromVsync,toVsync"` (hex, up to 4 words; `ps2_runtime.cpp`, `dispatchGuestBranch`): `[ee-watch]` whenever a word changed since the last guest call / jump, with old -> new (hex and float), "at entry" of a call from S (the code before S wrote it) or "after return" of a call to T (T's own code did), and the guest call stack. Every guest call passes there, so unlike a wrapper around one function it sees every write. Found issue 7 in two runs.
- **EE call log** `PS2X_EE_CALL_LOG="func[:reg:bytes...][,func...];fromVsync,toVsync"` (hex; reg a0-a3, t0-t3, s0-s7, sp): `[ee-call]` at each call of those functions with a0-a3, t0-t2, f12, sp, ra and memory dumps at the chosen registers (words and floats), e.g. `12e260:a3:340:a0:340`.
- **Native sampling profiler** `PS2X_NATIVE_PROF="fromTick,toTick"` (`ps2_test_harness.cpp`, Windows): samples the GameThread's native stack ~1000 times a second between those window ticks and prints `[nprof]` tables (functions by exclusive and inclusive samples, names from the PDB). Use it before optimizing anything.
- Both need `PS2X_DETERMINISTIC=1` to land on the same VSyncs as `run_tests` (without it the timeline drifts and nothing fires).
- GPU driver resets (TDR) show up in the Windows System log as `nvlddmkm` event 153; Granite then spins forever in `vkResetCommandPool`. Check with `Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'}`.

## Status and open issues (2026-11-05, later)
1. **GPU renderer: works through the language menu (verified 2026-11-02 by run_tests).**
   - Full-width 640x512 picture; PRESS START and LANGUAGE CHOICE draw correctly, ~30 game FPS (CPU renderer: ~2.8).
   - The hang after Enter had two causes, both fixed: a paraLLEl-GS CPU-side loop (`compute_page_rect` block range underflow, patched) and the IMAGE re-tag desync in the bridge (above).
2. **Sound: works through the emulated UBISOUND (fixed 2026-11-04; peak ~3000 in the menus, was ~130).** Three IOP-emulator bugs, all in `ps2xIOP/src/emulator/`:
   - RPC handlers ran with a 2M-instruction cap. UBISOUND's bank load (1.8 MiB to SPU2 RAM by PIO) was cut off: a third of the bank loaded and an unwritten reply, so every sound started at SPU2 address 0 (silence). Now `kMaxRpcCallInstructions` (400M); an abandoned call logs `[IOP] guest call 0x... did not return` (run_tests checks for it).
   - SetAlarm / CancelAlarm were stubs, so UBISOUND's tick thread (every driver's update: voice end detection, channel release, streams, IOP->EE notifications) never ran: sounds never ended and after ~24 plays every play failed. Now real alarms (`dispatchAlarmImport`, `serviceAlarms`).
   - The IOP heap was a bump allocator (freed memory never reused) over 832 KiB only; the level start's 900 KiB load failed. Now one first-fit arena 0x10000-0x1F0000 shared by module images and AllocSysMemory (modes 0/1/2), with QueryMaxFreeMemSize / QueryTotalFreeMemSize. Module bases moved (UBISOUND is no longer at 0x45d00: read `[IOP] loaded IRX` lines).
   - Now: streams read from the disc, `iop->ee posted` > 0, no failing play / load commands.
   - **Fixed (2026-11-04): background music was silent** (user: waves and menu sounds played, no music). UBISOUND plays its streams (music, ambience) on SPU2 core 0 (24 voices with 2 KiB ring buffers, 22050 Hz, a stereo pair per stream) and the effects on core 1; core 0's output reaches the speakers through core 1's external input. Our mixer scaled that input with BVOL instead of AVOL (and the AutoDMA input with AVOL instead of BVOL); the game sets core 1 AVOL to full and BVOL to 0, so all of core 0 was muted. Now as on the hardware (PCSX2: ExtVol = AVOL, InpVol = BVOL), `iop_spu2.h` `mixCore`. Menu peak ~12000 (was ~3300); `run_tests` requires 6000.
   - **Fixed (2026-11-05): music slowed down** at start-up and after loading screens (user report). The host audio reader (`ps2xSpu2ReadSamples`, `iop_memory.cpp`) corrected the SPU2 ring's fill by changing the playback rate up to +-10%, so whenever the emulation fell behind the music played slower. Now +-0.5% (inaudible); a dry ring fades to silence and waits for 100 ms of audio before resuming: a short gap instead of slow-motion.
   - The EE side of the streams works: segments of `SOUND\VSC.BF` are pushed with play command 9 when the IOP reports a segment end (`io2e` fn 0x30000 `{0, 4, record}` -> stream `+4` -> 0x168CB8 -> callback 0x159698 -> 0x1697A0). The first level's music starts with a ~40 s segment, so the first push only comes after ~40 s.
   - Tools: `PS2X_AUDIO_WAV=out\\x.wav` records the SPU2 output (48 kHz stereo; game content, keep under out/); `PS2X_RAYMAN_MUSIC_TRACE=<from vsync>` (`[rayman-music]`: EE stream objects, pushes, IOP events); `PS2X_UBI_TRACE=1` (`[ubi-rpc]` per ee2i call, `[iop->ee]` notifications with payload), `PS2X_IOP_WATCH="addr[:reg:bytes],..."` (`[iop-watch]`), `PS2X_SPU2_DUMP_AT="700,2300"` (`[spu2-dump]`), first 40 key-ons as `[spu2-kon]`.
   - **Decided (2026-11-02): build sound the rac-rs way** (exact native port of UBISOUND + native SPU2 voice mixer, verified against the emulated driver as oracle). The oracle is now trustworthy. Plan, stages and reverse-engineering notes: `docs/ubisound_port.md`. Disassembly: `tools/irxdis.py` (capstone; output under `out/analysis/`).
3. **Fixed (2026-11-02): the AI crashes were a runtime bug in `dispatchGuestBranch`, not game data.**
   - After a call returned, `dispatchGuestBranch` treated `ctx->pc == entryPc` as "the callee returned without touching pc". With recursion (`f -> ... -> f`), a checkpoint taken just before the inner call to `f` leaves exactly that pc pending, so the *outer* call to `f` looked finished: the caller went on with garbage registers and skipped epilogues (stack not popped).
   - Rayman's AI evaluator is recursive (`0x1DF760` node dispatch -> `0x1DF3E0`/`0x1DF310` -> `0x1F2540`/`0x1F1D80` -> `0x1DF760`). Symptoms, depending on where the checkpoint fell: a node pointer of `0xB` (the next node's type byte left in `v0`) and an endless walk through zeroed low memory (host stack overflow), or `ra` loaded from the wrong stack slot (jump to `0x1fffa90`), or the old "NULL behaviour tree" (issue 4 below).
   - Fix: a thread-local "yield pending" flag in `ps2_runtime.cpp`, set when a branch hands control to the scheduler (checkpoint `K` or jump `J`) and cleared by `EeScheduler::run` before it re-enters guest code (`ps2xGuestYieldReset`). While set, every host frame returns false.
   - Found with the deterministic mode: the crash became 100% reproducible, then `[guest-recursion]`, `[rayman-ai]`, `[sched-trace]` and `[branch-trace]` pinned it down (see Diagnostics).
4. **The NULL-tree guard at 0x1DF7F0 is probably obsolete** (same bug as 3). It is still installed; it logs `[rayman-ai] NULL behaviour tree` if it ever fires. If JAIL_10 / SKI_60 run clean without it, remove it.
   - Background, kept for reference: node layout 8 bytes = 32-bit param, depth byte at +6, type byte at +7; node dispatch table 0x3813B8 (type 0 -> 0x1DF310 "function", 2 -> 0x1DF3E0); function tables 0x382E48 (590 entries) and 0x382EC0; comport helpers `0x2009A0`, `0x200DD0` (0x68-byte entries, tree pointer at +0x60).
5. **Performance**: levels ran 7-15 FPS on the CPU renderer (GS drain waits plus the VU1 interpreter). Menu loading looks slow mostly because of the game's own waits.
   - **Now the top problem (user, 2026-11-05): the intro cutscene runs at ~15 game FPS** (was ~29 while the broken SQRT.S hid most of the scene), and the music, which keeps real time, runs ahead of the picture. `[prof]` during the cutscene: VU1=75% GS=12% (GPU renderer). The VU1 interpreter is the bottleneck.
   - Agreed with the user (2026-11-05): first optimize the VU1 interpreter, then, if still too slow, translate the game's VU1 microprograms to C++ (like the EE code).
   - `[nprof]` in the cutscene (ticks 5500-6500, before any change): `VU1Interpreter::run` 15.7% exclusive, `commitReadyPipelines` 14.8%, `execUpper` 13.9%, `calculatePairReadyCycle` 9.4%, `updateFmacFlags` 4.9%, `execLower` 4.3%: about a third is pipeline bookkeeping (the interpreter is cycle-accurate: every result is queued and committed later).
   - Step 1 done: MAC / status flags are skipped when the loaded microcode never reads them (`VuSideState::macStatusUnread`, set in `rebuildDecodedCodeCache`): cutscene 13.9 -> 15.3 game FPS, pictures identical. Next candidates: `calculatePairReadyCycle` / `commitReadyPipelines` (skip when nothing is pending), the per-instruction copies in `run`.
   - Step 2 done: VF results are written at once instead of queued (readers stall on `m_vfReady` anyway; `PS2X_VU1_QUEUED_VF=1` restores the queue). `PS2X_VU1_KICK_HASH=1` (`[vu1-hash]` every 600 VSyncs: FNV hash of every XGKICK packet) was identical over 11 million packets to VSync 6600, so the output is bit-identical. Gain only ~4% (14.6 -> 15.2 game FPS). Conclusion: tuning the cycle-accurate interpreter gives single-digit percents; a real speed-up needs the microprograms translated to C++ (stage 2 of the plan). Use `PS2X_VU1_KICK_HASH` to validate any VU1 change.
6. **Fixed (2026-11-06, MOVZ / MOVN): 3D polygons look messy** (reported by the user, 2026-11-02). **Mostly fixed by the SQRT.S fix (2026-11-05, issue 7)**: the menus' background is now a real 3D scene (jungle, waterfall, rocks, water) instead of teal streaks; the goldens `boot_language_menu.bmp` / `english_main_menu.bmp` were updated (old ones kept in `out/golden_before_sqrt/`).
   - **Fixed (2026-11-06): the black wedge-shaped triangles across the menus' background.** **Cause: the recompiler translated MOVZ / MOVN as a full 128-bit copy** (`SET_GPR_VEC`, `ps2xRecomp/src/lib/special_translator.cpp`); the R5900 copies only the lower 64 bits and keeps the upper half of rd (PCSX2: `GPR.r[rd].UD[0] = GPR.r[rs].UD[0]`). 344 generated functions changed. Unit test "MOVZ/MOVN copy only the lower 64 bits". After the fix the language menu matches the user's PCSX2 state (slot 4) and the prison ship got its lightmaps (darker, pools of light on the floor, like PCSX2 slot 3; before it was flat and over-bright). Goldens `boot_language_menu`, `english_main_menu`, `intro_name_entry`, `intro_rayman_on_grate` updated; old ones in `out/golden_before_movz/`. PCSX2 slots: 3 = Rayman on the grate (first moments of control), 4 = language menu, 1 / 2 / 5 = the jungle.
     - Chain, for reference: 0x154848 builds a PATH2 packet in the scratchpad (TEXFLUSH, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, then the texture upload) whose GIF tag `0x1000000000008005` with REGS = 0xE (A+D) is kept as one 128-bit value in $s0 (`pcpyld $s0, $v0(0xE), $a3`). It then calls 0x1A97A0, which saves $s0 with `sd`, uses it with `movz $s0, $a0, $v0` and restores it with `ld`: only the lower half comes back, so the tag's REGS became a0's / v1's upper half (0x34 or 0: 797 of the 1671 such tags in EE RAM at the menu). With REGS = 0x34 / 0 the GS read the 5 A+D words as XYZF2 / packed vertices near (0, 0): haze-coloured triangles from the top-left corner drawn with ALPHA_2 = 0x89 (multiply by As = 0) -> black wedges; the texture upload setup was lost too.
     - Found with `PS2X_EE_WATCH` on the scratchpad (`0x700003e0`, writer `sq $s0` at 0x154914) and a count of the 0x8005 tags in EE RAM (PCSX2: all REGS = 0xE). Lesson: 128-bit register state survives across calls only through the upper half being preserved by every non-MMI instruction; any translation that writes 128 bits for a 64-bit instruction breaks code that keeps quadwords in callee-saved registers.
   - The notes below (lightmap passes, VRAM layout) were the investigation before the cause was found.
   - Tools added during this hunt: `PS2X_EE_WATCH` also covers the scratchpad (0x70000000+); `PS2X_GS_CAPTURE_AT=<vsync>` (CPU renderer: `out/gscapture.bin`, 40-byte header + 4 MiB VRAM, decode PSMCT32 blocks / columns); `PS2X_GS_TRACE_PIXEL="x,y[,fromVsync]"` (CPU renderer: `[gs-pixel]` per write to that frame-buffer pixel: source colour, Z, blend selectors, result or z / alpha fail); `PS2X_PGS_EXPERIMENT` bit 64 (TEST AFAIL FB_ONLY -> KEEP); `tools/gifhit.py` handles sprites.
   - **Narrowed (2026-11-06, with the user's PCSX2 state in the language menu, slot 4): the wedges are the lightmap "multiply" passes giving 0.** `PS2X_PGS_EXPERIMENT=1` (ALPHA 0x89 = Cd x As drawn as Cd) or `=4` (no context-2 passes) removes them and the menu then looks like PCSX2's (foliage silhouettes in the corners, grey-teal sky). The camera barely sways and the wedges are in every frame, so it is not a timing difference.
   - Not the clipper: every vertex with Q < 0 in the frame carries ADC = 1 (45 of 6504), so VU1's clip test works; triangles that look giant when ADC is ignored are never drawn (`tools/gifhit.py` honours ADC). Not the foliage texture either: its CLUT (EE 0x4C7A60, uploaded to CBP 0x3E40) has alpha 0x80 on all entries in the real game too.
   - The covering triangle at screen (200, 100) is a textured green surface (TBP 0x3DC0 T8, CBP 0x3E40, TEST 0x5140B, kick 429 of the capture), then three lightmap passes (context 2, TBP 0x3800 / 0x38A0 / 0x3940, 128x128 T8, CBP 0x3A00, CLAMP_2 = 5 = plain clamp, ALPHA 0x89, FRAME_2 FBP 0x100 with one channel per pass) and a haze pass (RGB 66, 126, 154, ALPHA 0x44). Lightmap UV at that pixel ~ (0.17..0.41): inside the texture, so the lightmap texels there must have alpha 0 in our VRAM.
   - VRAM layout differs from the real game: the same foliage CLUT is at block 0x3180 in PCSX2 (0x3E40 here), and PCSX2's VRAM at 0x3800 / 0x3A00 holds something else. The game packs textures / lightmaps into VRAM at run time, so a packing difference could leave lightmap UVs pointing at empty atlas space. Next: read our VRAM at 0x3800-0x3A3F (CPU renderer `PS2X_GS_RENDERER=cpu` keeps it on the CPU), find when / from where the lightmaps are uploaded, and compare the packing state in EE RAM with PCSX2's.
   - PCSX2 `GS.bin`: GS local memory starts at offset 0x1A9; with the hardware renderer the frame buffers are not in it (black), textures are.
   - Tools: `PS2X_VU1_DUMP_NEGQ=<vsync>` (VU1 snapshot at the first XGKICK with Q < 0), `PS2X_VU1_TRACE_CYCLES="from,to"` (`[vu1-trace]` per pair, with clip flags; cycles from the dump's info file), `python tools/gifhit.py out/gif_capture.bin <gs x> <gs y>`. The notes below are from before the fix: re-check them against the new picture.
   - Before the fix: diagonal streaks and a teal tint over the menus' 3D backgrounds, same on the CPU renderer, so the cause was before the GS (it was the camera / normalisation maths).
   - Ruled out (2026-11-03, `[pgs-cover]` report): texture coordinates (S/Q, T/Q in normal ranges, ~5% degenerate triangles), mipmapping (MXL 0), depth (99% of Z within 16 bits; the 16-bit RenderWare microcode is the one uploaded), texture / CLUT uploads (real data at the expected TBP / CBP), GIF packet order (FIFO instead of the priority sort changed nothing).
   - Seen: 1-10% of world vertices have negative Q (behind the camera); RenderWare's VU1 code should clip those. Giant triangles (boxes to -1792) may be those. Next: check the VU1 CLIP / clipper path.
   - Rendering technique, for reference: the world is drawn (context 0, TFX modulate), then three per-channel lightmap passes (context 1, FBMSK keeps one of R/G/B, ALPHA 0x89 = Cd * As, Z test GEQUAL without Z write) with 128x128 T8 lightmaps (uniform index 0x60, CLUT ramp alpha = index, so x0.75).
7. **Fixed (2026-11-05): black screen after the opening cutscenes** (reported 2026-11-02; scenario `intro_to_first_level`, shot 9600 now shows the prison ship cinematic). **Cause: the recompiler translated the EE's `SQRT.S fd, ft` as `sqrt(fs)`** (`ps2xRecomp/src/lib/fpu_translator.cpp`). The EE takes the operand from the ft field and the compiler leaves fs = 0, so 107 of the game's 325 square roots computed sqrt(f0). Chain, for reference:
   - The level starts with a black screen effect (AI procedure 0x205, effect 0x6E, alpha 249) that stays until the intro director's cinematic moves the camera out of its normal comport.
   - The director parks entrance marker 5 (superobject 0x977EB0, a physical object: dynamics at 0x978078 with gravity) on Rayman. Standing on the floor, it gets 75 collision contacts (detection 0x135530 from 0x12F6F0); the collision response 0x12E260 normalises a vector with a broken square root and threw the marker 60 units away (0x12EE64: `SQRT.S f6, f1`).
   - The director's command list (texts 105 / 1301 / 1410: "entrance 5") then made the marker pull Rayman to itself, out of the intro's sector; the sector change (0x101828) deactivated the director and 10 other intro actors, so the cinematic never started and the black effect stayed.
   - Reproduce the old state: `PS2X_PGS_EXPERIMENT=32` hid the black quad; the investigation tools below still work.
   - Engine facts: per-frame actor loop 0x10C0D8 over the active list (gp-0x7414; engine struct +0x18 perso, +0x2C flags, +0x34 state byte); actor frame 0x100848 -> 0x101BF0 (AI skipped when (flags & 0x1800) == 0x1800, flags & 0x10000, or flags & 0x80000 with byte +0x34 bit 7) -> AI 0x101A78 (reflex 0x1DD7B0, intelligence 0x1DD760) and mechanics 0x101B20 -> 0x12BA18(dynamics, superobject) (dynamics +0x78 previous matrix, +0xCC current matrix, +0xC flags) -> 0x12F6F0 collisions. Set position (procedure 0xC1, 0x1F8528): dynamics path 0x12D988, else global matrix + 0x14AD50 (local = inverse(parent global) x global). Superobject +0x1C parent, +0x20 local matrix, +0x24 global matrix (translation at +0x30, type byte +0x50). Main character pointer at 0x252398; its sector at [[main+4]+0x20]. Dsgvars: mind = [[sub+0xC]], dsgmem = [mind+0xC], entries (6 bytes: offset, type at +4) at [[dsgmem]+4], current values at [dsgmem+8].
   - AI script facts (Rayman 2 engine): node = 8 bytes (param, then type byte at +7, depth at +6); evaluator 0x1DF760 dispatches through 0x3813B8, results in a record at a2 (scratchpad: +0 value, +0xC kind); tables: procedures 0x382F30 (4 bytes per entry: 0x3B change comport -> 0x1F5700, 0xA6 flags -> 0x1F5910, 0xC1 set position -> 0x1F8528, 0x205 create effect -> 0x2000A0), functions 0x3828F8 (0xAA formats a text entry into a number, 0x1E85D0), conditions 0x3825C8; comport switch 0x1DFC30(mind, comport). Screen effects: records of 0x60 bytes at 0x3796D0 (0x1D46A0 new, 0x1D4FA8 / 0x1D6B50 tick, 0x1D5568 draw).
   - Tools (main.cpp): `PS2X_RAYMAN_ACTOR_TRACE="from,to"` (`[rayman-actor]`, `[rayman-sector]`, `[rayman-flags]`, `[rayman-mechanics]`); `PS2X_RAYMAN_AI_WATCH="lo,hi,from,to[,addr]"` (every AI node in [lo, hi), with 3 words at addr after each); `PS2X_RAYMAN_FADE_TRACE=1` (`[rayman-fade]`, `[rayman-effect]`, `[rayman-fx]`, `[rayman-comport]`); `PS2X_RAYMAN_EXPERIMENT_NO_ENTRANCE=<vsync>`; `PS2X_PGS_CAPTURE_VSYNC=<vsync>` + `tools/gifcap.py` (one frame of GS input); `PS2X_VU1_DUMP_RGBA`; older: `PS2X_EE_DUMP_AT`, `PS2X_PGS_COVER_FROM`, `PS2X_PGS_BIGPRIM(_MAX)`, `PS2X_PGS_WATCH_LO/HI`, `PS2X_PGS_EXPERIMENT` (bits: 1 multiply passes neutral, 2 drop (0,0) triangles, 4 drop context 1, 8 drop textured context 0, 16 base pass opaque, 32 drop untextured blended black primitives).
   - Lesson: `PS2X_RAYMAN_PROBE` (a wrapper comparing memory before / after a guest function) misses writes in big functions, because recompiled functions can return early to the dispatcher. Use `PS2X_EE_WATCH` instead.
   - The SQRT.S fix touches collisions, normalisation and camera code everywhere: re-check issue 6 (messy polygons) and anything physics-like that looked odd before.
8. **Fixed (2026-11-05): characters and everything animated drew magenta / purple** (user report: Rayman, Globox, pirates, electricity, the grate). **Cause: MMI PEXEW** (`rd = rt.w2 w1 w0 w3`) was translated as a neighbour swap (`w1 w0 w3 w2`, `PS2_PEXEW` in `ps2_runtime_macros.h`). The engine builds vertex colours at 0x17E1E0 with PEXEW + MFC1 + PPACW; G and A were lost, so colours came out (R, 0, B, garbage). Found with `tools/gifcap.py --timeline` (every character run had G = 0), `PS2X_VU1_DUMP_RGBA=0xff800080` + `tools/vudis.py` (the VU1 input already held (255, 0, 255, junk)), and a search of EE RAM for that vector. The Ubi Soft logo was magenta for the same reason (golden updated; old one in `out/golden_before_pexew/`).
   - Same review, fixed in the recompiler too (not used by Rayman): PROT3W, PEXCW, PEXEH, PEXCH, PREVH lane orders (PCSX2 semantics; unit test "MMI word / halfword permutations").
   - **Still wrong, not used by Rayman** (check before another game): the halfword multiply-accumulates PMULTH / PMADDH / PMSUBH / PHMADH / PHMSBH (sum everything into one value instead of the per-lane HI/LO results), PMTHI / PMTLO (32 bits only), PMULTUW / PDIV* HI/LO; VU0 macro VRSQRT ignores fs (computes 1/sqrt(ft)), VSQRT of a negative gives 0 instead of sqrt(|ft|), VDIV by 0 gives 0 instead of +-max. `PS2_PEXEW` in the macro header is still wrong but no longer emitted (changing that header rebuilds everything).
   - Which MMI instructions a game uses: scan its code for opcode 0x1C (see the 2026-11-05 session; Rayman: PCPYLD x320, PADDH, PSRLH, MULT1, QFSRV, PEXTLB, MADD, PAND, PNOR, PCPYUD, POR, PEXTUB, PSUBB, PCGTH, MFLO1, DIVU1, PEXEW, PSRAH, PSLLH, PMINH, PMAXH, PSUBW, PPACW, PXOR, DIV1, PPACB, PCPYH, MFHI1).
9. **Fixed (2026-11-05): CVT.W.S rounded to nearest** (`FPU_CVT_W_S` = `nearbyintf`). The EE truncates and saturates (PCSX2 CVT_W); GCC uses it for every `(int)x` (272 sites in 132 functions). Now emitted inline by the recompiler. No visible change found yet.
10. **Fixed (2026-11-06): Rayman flew away in every direction once he got control in the ship** (user report). **Cause: the VU interpreter mishandled a branch in a taken branch's delay slot** (`ps2_vu1_core.cpp` `run`). On the hardware (PCSX2: `takedelaybranch`) the first branch's target executes one instruction as the second branch's delay slot, then the second branch jumps. Rayman's sphere-triangle collision runs as VU0 microcode (`vcallms 0` in 0x20C660) that uses exactly this: `0x0718 IBEQ vi2, vi0, 0x0C58` with `B 0x0438` in its delay slot; 0x0C58 clears vi13 ("no contact"), the fall-through 0x0728 sets it. We ran 0x0728 always, so every triangle edge was a contact.
    - Effects, for recognition: 75 contacts with t = -1 ("overlapping at start") against faces 3-4 units away, pushes of 4; the first static test at VSync 12558 pushed the dynamics' previous position to (162.1, -85.4, 48.0), the ground probe (0.45 / 0.9 down) started there, no ground was found (dynamics +0xC lacked the "on ground" bit 0x800000, ground info at [dyn+0x148] stayed zero), Rayman jittered and was thrown through the walls. Now he stands at (182.44, -53.35, 39.50), exactly as in the user's PCSX2 state.
    - Found by comparing with a PCSX2 save state taken by the user at the same moment (slot 3: `tools/p2s_extract.py`): same heap addresses (main character 0x9711F4), real contacts = 2 floor hits with t = 0.5 / 0.83 against object 0x9F5900. Then `PS2X_VU0_TRACE` + `PS2X_VU0_TRACE_MATCH` on the static test and `tools/vudis.py` on our VU0 micro memory.
    - The game swaps VU0 microcode during a frame: PCSX2's state held a small "normal" program at 0 (our library has it at 0x750), so a VU0 micro memory mismatch between a snapshot and our run is not by itself a bug.
    - `run_tests` (`intro_to_first_level`, now 13000 frames): `[rayman-sector]` must not appear after VSync 10000, and the 12900 picture (`tests/golden/intro_rayman_on_grate.bmp`) shows Rayman on the grate with Murfy's hint.
    - Engine facts found on the way: dynamics (Rayman 0x9B48AC) +0x8 / +0xC flags (+0xC 0x800000 on ground, 0x1000000 moved by the collision response, 0x4000 use ground info), +0x78 previous / +0xCC current matrix, +0x148 -> ground info (contact copy: +0x34 normal), +0x1A0 position used by the ground handler 0x131D78 (called from 0x12CF34; it probes with 0x135530 from 0x1321B0 (static), 0x132734 and 0x1328CC (down)); gp-0x724C counts collision responses (0x12E260) in one mechanics step; 0x108648(superobject) returns a type byte. Positions are converted to a moving support's local frame and back every frame (0x13A4A8 / 0x13A5D0: the ship's floor).
- Tools: `python tools/vudis.py out/vu1dump0_code.bin` disassembles VU microcode (upper ; lower, flags [I E M D T]).

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
