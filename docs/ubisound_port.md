# Native UBISOUND: port plan and reverse-engineering notes

Goal (agreed with the user, 2026-11-02): sound built the way rac-rs builds it for Ratchet & Clank
(`D:\downloads\rac-rs-main\rac-rs-main`): the game's IOP sound driver is ported to native code, and a native SPU2
voice mixer plays what the driver writes. No R3000 interpreter and no SPU2 RAM emulation in the final path.

## How rac-rs does it (reference)

- `oracle/src/snd989/` (~11k lines of Rust): an exact port of the IOP module 989snd.irx. Every function cites its IOP
  module address. Split by concern: `driver.rs` (command entry, tick clock, SPU2 RAM allocator), `handler.rs` (grain
  interpreter, 64 handler slots), `voices.rs` (48-voice pool, key-on math, the SPU2 register image), `bank.rs` (SBlk
  banks), `streams.rs` (music / speech streams), `pitch.rs`, `lfo.rs`, `reverb.rs`, `rng.rs`.
- The driver's output is a list of SPU2 register writes per field (`Reg::Kon`, pitch, volume, ADSR, start address,
  stream source ...) plus the sample data the voices play (bank / stream), handed to the game side as a batch.
- `rac-rs/src/spumix.rs` (voice mixer, plays the writes 1:1 like the hardware), `spurev.rs` (reverb), `audio.rs`
  (cpal output, 48 kHz). Streams are read from the player's disc at run time.
- Timing: the driver runs on its own tick (an IOP hard timer, 240 Hz), derived from the field count, not from EE code.
- Verification: tests replay the original driver (in their emulator, the "oracle") and require the port's voice
  envelopes (ENVX of all 48 voices) to match tick for tick; key-on logs must match exactly.
- Module tables (sine, pitch, R250) are read from the player's own IRX at run time; nothing of the module is embedded.

## Our equivalent

- Oracle = our existing path: the IOP interpreter running the real UBISOUND.IRX + LIBSD.IRX, and `iop_spu2.h`.
  Its SPU2 register writes (with IOP tick timestamps) are the reference the port must reproduce.
- The port lives in `ps2xRuntime/src/lib/sound/` (private headers only; reaches the rest through `extern "C"`, see the
  header rule in CLAUDE.md).

## Stages

0. Tools: `tools/irxdis.py` (annotated IRX disassembly; output under `out/analysis/`, never committed). Done.
1. Map the module: RPC entry and command table (done, below), threads and their loops, the tick source, the
   IOP->EE notification path (sid "io2e"), LIBSD calls, CD reads / streaming, bank loading and the SPU2 allocator.
2. Record the oracle: log every ee2i command (function, sizes, payload) and every SPU2 register write with the IOP
   cycle, for the run_tests scenarios. Store as test data under out/ (game-derived, not committed).
3. Banks: parse the Ubisoft bank files (`SOUND\*.DSC` index + `*.BF` data: SNDENG, BNMENG, VSC) natively; decode
   PS-ADPCM once at load.
4. Port the command handlers, module by module ("load" first: banks; then "play"), emitting SPU2 writes. Unit tests
   compare against the recorded oracle.
5. Native voice mixer: reuse the voice / ADSR / ADPCM logic of `iop_spu2.h` as a standalone mixer fed by register
   writes and bank sample data (no SPU2 RAM image). Output through the raylib audio stream (48 kHz).
6. Streams (music, speech from the disc) and reverb.
7. Switch over: ee2i RPCs go to the native driver; the IOP interpreter is no longer needed for sound.

The SSA = 0 problem (CLAUDE.md, Sound) must be understood in stage 1/2: the oracle has to be right before it is the
reference. Bank loading ("load" module + the SPU2 allocator) is where the bank's SPU address comes from.

## Findings so far (UBISOUND.IRX, PAL SLES-500.44, module addresses = offsets from the module base)

- No symbols. .text 0x00000-0x13850 (19808 instructions, ~436 functions), .rodata 0x13850, .data 0x13e40,
  .bss 0x15d60-0x227b0. Ubisoft naming in debug strings: `tdstSampleMemory`, `fn_iNotifyTdstChannelMEMData`,
  `E_SNDITBASE_ENDREACHED`.
- Imports: libsd 4 5 7 8 9 10 11 15 17 19 22 26 27, cdvdman 6 7 11 13 37 39, sifcmd 14-21, thbase, thsemap,
  sysmem, intrman, sysclib (memcpy/memset), stdio printf.
- Entry `start` 0xebc0 -> 0xeb70 -> init 0x83b0 -> RPC server: descriptor {sid 0x65653269 "ee2i", func 0x300}
  (0xeb88), server thread setup at 0x630 (sceSifRegisterRpc at 0x694, receive buffer 0x16960).
- RPC handler 0x300: the request is either one command or a batch. Batch: function word with high half 0xFFFF, low
  half = count; each entry `{function, outSize, inSize, data}`. Reply buffer 0x15d60, 0x400 bytes.
- Dispatcher 0x218: module registry at 0x20a60 (8 slots x 16 bytes, set by 0x200 from init): entry =
  `{fourcc, module number, command count, handler table}`; picks module = function >> 16, command = function & 0xFFFF,
  calls `handler(out + 4, in[0], in + 8, in[1])`, stores the result in out[0].
- Modules (registered in init 0x83b0):
  - 1 "play" (fourcc 0x79616c70, 12 commands, table 0x17d90, set by 0x1290):
    0:0xc60 1:0xd34 2:0xda0 3:0xebc 4:0x10c0 5:0x112c 6:0x1198 7:0xf3c 8:0xfb8 9:0x103c 10:0x121c 11:0x1250
  - 2 "load" (fourcc 0x64616f6c, 10 commands, table 0x20ce0, set by 0x9eb4):
    0:0x9924 1:0x9968 2:0x9be0 3:0x9e04 4:0x9a70 5:0x9bac 6:0x9e38 7:0x9eac 8:0x99a4 9:0x9a3c
- IOP->EE notifications: sid 0x696f3265 "io2e" used at 0xcbc (to be read).
- "load" commands read so far: 2 (0x9be0) = bank from disc straight to SPU2 RAM: in `{lsn, bytes}`, allocates SPU2
  memory (allocator 0x14c0 on the heap at 0x17dc0), then loops sceCdRead into an IOP buffer + sceSdVoiceTrans(core 1,
  mode 8 = write by PIO) chunk by chunk, out `{spuAddr, bytes}`. 4 (0x9a70) = disc -> new IOP buffer, out
  `{iopAddr, bytes}`; 5 (0x9bac) frees it. 8 (0x99a4) = IOP buffer -> SPU2 by DMA, out `{spuAddr, bytes}`; 9 frees.
  Rayman loads one 1.8 MiB bank with command 2 at boot (lsn 0x1cc3da, 0x1c6250 bytes -> SPU2 0x11010).
- "play" command 2 (0xda0): in 0x40 bytes; looks up the sound class by id (0x976c, registry 0x20c80), then class
  vtable +0x10 = new instance (fixed pools: ~24 instances per class), +0x44, +0x48, +0x38 = start. Instances are only
  freed by the update tick (below) when their voice ends.
- Channel driver (vtable 0x13d30, object 0x20fe0, 24 channels of 0x2c bytes from +4): channel = {core, voice, flags,
  owner, ..., +0x24, +0x28 SSA}. 0xd774 fills a channel from a sample descriptor (+0xa, +0x14, +0x18 = SPU2 address);
  0xd56c keys it on (sceSdProcBatch: ADSR 000f/1fc0, SSA, KON). Update 0xd870 (vtable +0x34 via 0xe15c): flags
  1 allocated, 2 key-on wanted, 4 keyed on, 8 ENDX seen (sceSdGetSwitch(ENDX)), 0x20/0x40/0x80 start / ? / end
  reported to the owner (0xd9e8 -> owner vtable +0x30).
- Tick: thread 0x9344 (priority 0x6b) arms SetAlarm(USec2SysClock(200000)) with handler 0x9314 (iSignalSema), then
  per tick calls every registered driver's update (0x941c -> driver vtable +0x34). Drivers registered in 0x9480:
  0xe184 (channels), 0x129ac, 0xa7c8, 0x2338, 0xc314, 0x6008, 0x43fc, 0x79c4, 0x10780.

- Streams (from `[ubi-rpc]`, 2026-11-04): "load" command 6 registers a stream bank of `SOUND\VSC.BF` (in `{lsn, bytes}`, e.g.
  BNK_0.VSC at LSN 0x1e634b, 0x668000 bytes; `SOUND\VSC.DSC` lists the banks with offsets). "play" command 9 (0x103c: class lookup
  0x97cc, then class vtable +0x44) hands the driver one block: in `{class, instance, 0, bytes, lsn, bytes, 0x400}`, out `{class,
  instance}`. In the menu the EE sends one ~55 KB block about every 2 s. A stream can also start with "play" command 2 carrying the
  block descriptors (type word `02042256`: two channels), as at the first level's start; there the EE never sends a next block
  (CLAUDE.md, Sound: background music missing). The IOP reads 9-sector chunks with sceCdRead + sceCdCallback meanwhile.

### Oracle bugs found and fixed (2026-11-04)
1. **SSA = 0**: the IOP ran RPC handlers synchronously with a 2M-instruction cap. Load command 2 (1.8 MiB by PIO)
   needs far more, so it was cut off: a third of the bank in SPU2 RAM and an unwritten reply (`{0, 0}`), so the game
   built every sound at SPU2 address 0. Fix: `kMaxRpcCallInstructions` (400M) for RPC calls, and
   `[IOP] guest call ... did not return` when a call is still abandoned.
2. **Sounds never end, plays fail after ~24**: SetAlarm / CancelAlarm were stubs, so the tick never ran. Fix: real
   alarms in `iop_emulator.cpp` (handler in interrupt context, return value = next interval).
3. **Level sound data not loaded**: the IOP heap never reused freed blocks and had only 832 KiB, so load command 4
   (~900 KiB IOP buffer at the level start) failed. Fix: one first-fit arena for module images and AllocSysMemory.
   The real IOP has ~1.5 MiB free here; a native port must not assume less.

Tools: `PS2X_IOP_WATCH="addr[:reg:bytes],..."` logs registers (and memory at a register) when the IOP reaches an
address (`[iop-watch]`); UBISOUND's base: the `[IOP] loaded IRX id=6` line (0x45d00 before the 2026-11-04 heap change).
