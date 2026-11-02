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
