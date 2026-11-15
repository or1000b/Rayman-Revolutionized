#!/usr/bin/env python3
"""Draw groups of one captured GS frame (out/gif_capture.bin, PS2X_PGS_CAPTURE_VSYNC), in stream order.

A group = consecutive primitives with the same PRIM type / flags, TEX0, context, ALPHA, TEST and path. For each:
path, primitive type, TME / ABE / FST, context, texture (TBP0, PSM, size, CBP), the screen box (window pixels:
(XYZ - XYOFFSET) / 16), the Z range and the vertex count. Made to tell the 2D HUD (sprites / screen-space quads late
in the frame, no depth test) from the 3D world.
Usage: python tools/gifgroups.py [out/gif_capture.bin] [--all]   (--all: also groups of 1-2 vertices)
"""
import struct
import sys

PRIM_NAMES = ['point', 'line', 'lstrip', 'tri', 'tstrip', 'tfan', 'sprite', 'bad']
PATHS = {0: 'P1', 1: 'P2', 2: 'P3'}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    show_all = '--all' in sys.argv
    blob = open(args[0] if args else 'out/gif_capture.bin', 'rb').read()
    regs = {}  # GS register -> value (context 1 = index 0, 2 = index 1 for the paired ones)
    prim = 0
    groups = []
    cur = None
    vtx = []  # vertices of the primitive being built: (x, y, z)
    order = 0

    def ctx():
        return (prim >> 9) & 1

    def state_key(path):
        c = ctx()
        tex0 = regs.get(0x06 + c, 0)
        alpha = regs.get(0x42 + c, 0)
        test = regs.get(0x47 + c, 0)
        return (path, prim & 0x7, (prim >> 4) & 1, (prim >> 6) & 1, (prim >> 8) & 1, c,
                tex0 if (prim >> 4) & 1 else 0, alpha if (prim >> 6) & 1 else 0, test)

    def add_vertex(path, x, y, z, kick):
        nonlocal cur, order
        off = regs.get(0x18 + ctx(), 0)
        ox, oy = (off & 0xFFFF), ((off >> 32) & 0xFFFF)
        sx, sy = (x - ox) / 16.0, (y - oy) / 16.0
        key = state_key(path)
        if cur is None or cur['key'] != key:
            cur = {'key': key, 'n': 0, 'x0': 1e9, 'y0': 1e9, 'x1': -1e9, 'y1': -1e9, 'z0': 1 << 33, 'z1': -1,
                   'order': order}
            groups.append(cur)
        order += 1
        cur['n'] += 1
        cur['x0'], cur['x1'] = min(cur['x0'], sx), max(cur['x1'], sx)
        cur['y0'], cur['y1'] = min(cur['y0'], sy), max(cur['y1'], sy)
        cur['z0'], cur['z1'] = min(cur['z0'], z), max(cur['z1'], z)

    def write_reg(path, reg, lo, hi=None, packed=False):
        nonlocal prim
        if packed:
            if reg == 0x0:  # PRIM
                prim = lo & 0x7FF
                regs[0x00] = prim
            elif reg in (0x5, 0xD):  # XYZF2 packed / XYZ3? (A+D = 0xE handled below)
                x, y = lo & 0xFFFF, (lo >> 32) & 0xFFFF
                z = (hi >> 4) & 0xFFFFFF if reg == 0x4 else (hi & 0xFFFFFFFF)
                adc = (hi >> 47) & 1
                if not adc:
                    add_vertex(path, x, y, z, True)
            elif reg == 0x4:  # XYZF2
                x, y = lo & 0xFFFF, (lo >> 32) & 0xFFFF
                z = (hi >> 4) & 0xFFFFFF
                adc = (hi >> 47) & 1
                if not adc:
                    add_vertex(path, x, y, z, True)
            elif reg == 0xE:  # A+D
                write_reg(path, hi & 0xFF, lo)
            elif reg == 0x6 or reg == 0x7:
                regs[reg] = lo
            return
        regs[reg] = lo
        if reg == 0x00:
            prim = lo & 0x7FF
        elif reg in (0x05, 0x0C):  # XYZ2, XYZ3 (A+D or REGLIST)
            add_vertex(path, lo & 0xFFFF, (lo >> 16) & 0xFFFF, (lo >> 32) & 0xFFFFFFFF, reg == 0x05)
        elif reg in (0x04, 0x0D):  # XYZF2, XYZF3
            add_vertex(path, lo & 0xFFFF, (lo >> 16) & 0xFFFF, (lo >> 32) & 0xFFFFFF, reg == 0x04)

    off = 0
    while off + 16 <= len(blob):
        kind, ident, size = struct.unpack_from('<IIQ', blob, off)
        off += 16
        if kind != 0:
            write_reg(-1, ident & 0xFF, size)
            continue
        data = blob[off:off + size]
        off += size
        path = ident
        p = 0
        while p + 16 <= len(data):
            lo, hi = struct.unpack_from('<QQ', data, p)
            p += 16
            nloop = lo & 0x7FFF
            pre = (lo >> 46) & 1
            flg = (lo >> 58) & 3
            nreg = ((lo >> 60) & 0xF) or 16
            descs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
            if pre:
                prim = (lo >> 47) & 0x7FF
                regs[0x00] = prim
            if flg == 0:
                for _ in range(nloop):
                    for d in descs:
                        if p + 16 > len(data):
                            break
                        qlo, qhi = struct.unpack_from('<QQ', data, p)
                        p += 16
                        write_reg(path, d, qlo, qhi, packed=True)
            elif flg == 1:
                total = nloop * nreg
                for i in range(total):
                    if p + 8 > len(data):
                        break
                    v, = struct.unpack_from('<Q', data, p)
                    p += 8
                    write_reg(path, descs[i % nreg], v)
                if total & 1:
                    p += 8
            else:
                p += nloop * 16

    print('%5s %4s %-7s %3s %3s %3s %3s %-26s %-24s %-15s %6s' % ('order', 'path', 'prim', 'tme', 'abe', 'fst', 'ctx',
                                                                    'texture', 'screen box', 'z', 'verts'))
    for g in groups:
        path, ptype, tme, abe, fst, c, tex0, alpha, test = g['key']
        if g['n'] < 3 and not show_all:
            continue
        tex = '-'
        if tme:
            f = lambda lo, n: (tex0 >> lo) & ((1 << n) - 1)
            tex = 'tbp %04x psm %02x %dx%d cbp %04x' % (f(0, 14), f(20, 6), 1 << f(26, 4), 1 << f(30, 4), f(37, 14))
        ztest = (test >> 17) & 3 if (test >> 16) & 1 else 1
        box = '%4.0f,%4.0f-%4.0f,%4.0f' % (g['x0'], g['y0'], g['x1'], g['y1'])
        print('%5d %4s %-7s %3d %3d %3d %3d %-26s %-24s %7x-%-7x %6d  zt%d' % (
            g['order'], PATHS.get(path, str(path)), PRIM_NAMES[ptype], tme, abe, fst, c, tex, box, g['z0'], g['z1'],
            g['n'], ztest))


if __name__ == '__main__':
    main()
