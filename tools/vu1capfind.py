#!/usr/bin/env python3
"""Which VU1 run sent a primitive of a GS capture (CLAUDE.md issue 19).

F11 writes out/gif_capture.bin (what the GS received, tools/gifhit.py numbers its GIF records "kick N") and
out/vu1cap/run_<n>.bin + run_<n>.pkt (every VU1 run of the next VSyncs: the snapshot to replay, and the PATH1 packets it
sent). This finds the run whose packet holds the bytes of kick N, so that run can be replayed:
    PS2X_VU1AOT_REPLAY=out/vu1cap/run_<n>.bin out/game/ps2xRuntime/ps2EntryRunner.exe

Usage: python tools/vu1capfind.py <kick> [out/gif_capture.bin] [out/vu1cap]
The captures are game content: they stay under out/ (never committed).
"""
import glob
import os
import struct
import sys


def capture_kick(path, wanted):
    blob = open(path, 'rb').read()
    off = 0
    kick = 0
    while off + 16 <= len(blob):
        kind, ident, size = struct.unpack_from('<IIQ', blob, off)
        off += 16
        if kind != 0:
            continue
        data = blob[off:off + size]
        off += size
        kick += 1
        if kick == wanted:
            return ident, data
    return None, None


def packets(path):
    blob = open(path, 'rb').read()
    off = 0
    while off + 4 <= len(blob):
        size = struct.unpack_from('<I', blob, off)[0]
        off += 4
        yield blob[off:off + size]
        off += size


def main():
    kick = int(sys.argv[1])
    cap = sys.argv[2] if len(sys.argv) > 2 else 'out/gif_capture.bin'
    folder = sys.argv[3] if len(sys.argv) > 3 else 'out/vu1cap'
    path, data = capture_kick(cap, kick)
    if data is None:
        print('no kick %d in %s' % (kick, cap))
        return 1
    print('kick %d: path %d, %d bytes' % (kick, path, len(data)))
    probe = data[:min(len(data), 256)]
    found = 0
    for pkt in sorted(glob.glob(os.path.join(folder, 'run_*.pkt'))):
        for i, p in enumerate(packets(pkt)):
            if p == data or probe in p:
                exact = 'the same packet' if p == data else 'inside a %d-byte packet' % len(p)
                print('%s packet %d: %s -> replay %s' % (pkt, i, exact, pkt[:-4] + '.bin'))
                found += 1
    if not found:
        print('not found in %s (was the VU1 capture running at that time?)' % folder)
    return 0


if __name__ == '__main__':
    sys.exit(main())
