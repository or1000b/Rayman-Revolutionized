#!/usr/bin/env python3
# Which primitives of a GS capture (PS2X_PGS_CAPTURE_VSYNC -> out/gif_capture.bin) cover one GS pixel, in stream order.
#   python tools/gifhit.py out/gif_capture.bin <gs x> <gs y>      (screen pixel + XYOFFSET, e.g. 1728 + x, 1792 + y)
# Triangles, strips and fans of PACKED GIF data; a triangle counts only if its kicking vertex has ADC = 0 (XYZ2 bit 111),
# as on the GS (vertices behind the camera carry ADC = 1, so tests that ignore ADC see giant false triangles).
import struct, sys
blob = open(sys.argv[1],'rb').read()
px, py = float(sys.argv[2]), float(sys.argv[3])
def inside(a, b, c):
    def s(p, q, r): return (p[0]-r[0])*(q[1]-r[1]) - (q[0]-r[0])*(p[1]-r[1])
    d1, d2, d3 = s((px,py),a,b), s((px,py),b,c), s((px,py),c,a)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))
off = 0; prim = 0; kick = 0; regs_state = {}
while off + 16 <= len(blob):
    kind, ident, size = struct.unpack_from('<IIQ', blob, off); off += 16
    if kind != 0:
        regs_state[ident] = size; continue
    data = blob[off:off+size]; off += size
    kick += 1
    p = 0
    while p + 16 <= len(data):
        lo, hi = struct.unpack_from('<QQ', data, p); p += 16
        nloop = lo & 0x7fff; pre = (lo >> 46) & 1; flg = (lo >> 58) & 3; nreg = (lo >> 60) & 0xf or 16
        if pre: prim = (lo >> 47) & 0x7ff
        if flg != 0:
            p += ((nloop * nreg + 1) & ~1) * 8 if flg == 1 else nloop * 16
            continue
        regs = [(hi >> (4*i)) & 0xf for i in range(nreg)]
        verts = []; tex0 = None
        for n in range(nloop):
            v = {}
            for r in regs:
                w = struct.unpack_from('<4I', data, p); p += 16
                if r in (4, 5): v['xy'] = ((w[0] & 0xffff) / 16, (w[1] & 0xffff) / 16, w[2]); v['adc'] = (w[3] >> 15) & 1
                if r == 2: v['stq'] = struct.unpack('<3f', struct.pack('<3I', w[0], w[1], w[2]))
                if r == 1: v['rgba'] = (w[0] & 0xff, w[1] & 0xff, w[2] & 0xff, w[3] & 0xff)
                if r == 0xE: pass
            if 'xy' in v: verts.append(v)
        t = prim & 7
        tris = []
        if t == 3: tris = [(i, i+1, i+2) for i in range(0, len(verts) - 2, 3)]
        elif t == 4: tris = [(i, i+1, i+2) for i in range(len(verts) - 2)]
        elif t == 5: tris = [(0, i, i+1) for i in range(1, len(verts) - 1)]
        for a, b, c in tris:
            A, B, C = verts[a]['xy'], verts[b]['xy'], verts[c]['xy']
            area = (B[0]-A[0])*(C[1]-A[1]) - (C[0]-A[0])*(B[1]-A[1])
            if abs(area) > 1e-6 and inside(A, B, C) and not verts[c].get('adc', 0):
                print(f'kick {kick} path {ident} prim {prim:#x} tri {a},{b},{c}: ' + ' '.join(f'({V["xy"][0]:.0f},{V["xy"][1]:.0f} z{V["xy"][2]:x} stq={tuple(round(x,4) for x in V.get("stq",()))} rgba={V.get("rgba")})' for V in (verts[a], verts[b], verts[c])))
