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
- The game runs until its window is closed. For automated runs there is no exit-after-N-frames option yet; adding one is part of the test plan below.
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
    - `0x1DF7F0`: the AI behaviour-tree evaluator, wrapped by a NULL-tree guard (see open issues).
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
  - The bridge serialises every call with one mutex, reassembles split GIF packets per path (paraLLEl-GS spins forever on a piece shorter than one loop), synthesises SMODE1 (the BIOS normally writes it), and converts the libgraph stubs' DISPLAY values from pixel units (MAGH = 0, DW = width - 1) to video clocks.
- **VU1**: an interpreter (`vu/`). It costs about 29% of a frame in levels. AOT compilation like rac-rs (`vu1gen.py`) is a candidate, but a native renderer would remove most of it.

## Diagnostics already built in
- `[threads]` line every 120 ticks in `out\run.log`: per-thread state, gameFps, DMA/GIF counts, GS or pgs stats, spu2 stats, `iopcd reads/sectors/host/fail`, and `iop->ee posted/done/pending`. The pgs part includes PMODE/SMODE2/DISPFB/DISPLAY.
- `[prof] last 10s`: time split VU1/GS/IOP/idle plus the hottest EE functions.
- First bad indirect branch: a `[guest-branch:missing-target]` line, a `[guest-branch:regs]` line, and a dump of the whole 32 MiB of EE RAM to `out\crash_ee_ram.bin`.
  - Analysis tip: walk the stack in the dump and match saved registers to function prologues.
- `out\pgs_frame.bmp`: a GPU frame about every 5 s. Look at it instead of asking the user to describe the screen.
- F11 in game: one frame of GS commands to `out\gscapture.bin`.
- `[rayman-ai] NULL behaviour tree ...`: logged by the 0x1DF7F0 guard.

## Status and open issues (2026-11-02)
1. **GPU renderer (r11), waiting for a test run.** Before r11:
   - The picture was squeezed to the left (DISPLAY units).
   - After pressing Enter the game hung inside paraLLEl-GS (GS=100%, frames=0). Causes were two threads racing on it and split GIF packets.
   - Both are fixed in r11. Verify with the log and `pgs_frame.bmp`.
2. **Sound**:
   - Sound streams through UBISOUND's CD reads, which now work (`iopcd host=N fail=0`).
   - The music has not been confirmed audible yet. `peak` in the spu2 stats was still low, and there are underruns when the EE stalls.
   - Long-term plan: replace the IOP + SPU2 emulation with a native implementation of UBISOUND's command set.
3. **AI crash (guarded, root cause unknown).**
   - `0x1DF8D8` (rule loop) calls `0x1DF7F0` with a NULL tree.
   - The evaluator then walks address 0 upward as nodes until `0x1DF310` jumps through a missing entry of the 590-entry function table at 0x382E48.
   - Related code: node dispatch `0x1DF760` (by node type byte +7, table 0x3813B8); comport helpers `0x2009A0` and `0x200DD0` (array of 0x68-byte entries, tree pointer at +0x60).
   - Seen in JAIL_10 and in the SKI_60 attract demo. In the dump, the mind's comport array was all zeros while the rule table held a valid tree. The data probably changed after the bad call (initialisation order or a use-after-free?).
   - Next step: a write watch on those addresses.
4. **Performance**: levels ran 7-15 FPS on the CPU renderer (GS drain waits plus the VU1 interpreter). Menu loading looks slow mostly because of the game's own waits.

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

## Test automation plan (not built yet)
- `run_tests.bat` builds, then runs the game unattended:
  - Scripted pad input, e.g. Start at frame 300.
  - Fixed frame budget, then exit.
  - Watchdog: no new frame for 15 s → dump state and fail.
  - Checks at checkpoints: menu reached, level loaded, frame not black (pixel variance), audio peak above a threshold, FPS, comparison against golden images.
  - Writes `out\test_report.txt` with PASS/FAIL.
- Needs a deterministic mode: today time comes partly from the wall clock (the 0x14BEC8 helper and the IOP floor). In tests, time must come only from guest cycles, so the same input gives the same result.
- Small unit tests:
  - An IOP harness that loads the real IRX modules and runs UBISOUND's init RPC. It existed only in the previous assistant's sandbox; recreate it under `ps2xTest` if useful.
  - SPU2 decoder tests.
  - GIF reassembly tests.

## Housekeeping
- Initialise git early, with `.gitignore` excluding game data, and commit after every working step.
- Before editing a file, check whether it is one of the protected headers above.
