#!/usr/bin/env python3
"""Offline analysis of one captured GS frame (out/gif_capture.bin, written by the GPU bridge when
PS2X_PGS_CAPTURE_VSYNC=<guest vsync> is set; see gs_pgs_bridge.cpp, captureRecord).

What it reports, for the bug hunt of CLAUDE.md issue 7 (dark level) and issue 6 (messy polygons):
  - every host->local upload (BITBLTBUF / TRXPOS / TRXREG / TRXDIR + IMAGE data), with value statistics;
  - every group of textured primitives (same TEX0 / context): how many, vertex colours, and the brightness of
    the texture it samples (texels looked up in the CLUT that was loaded at the TEX0 write);
  - with --sheet: a PNG with the textures as the base pass sees them (texel -> CLUT colour).

Textures and CLUTs are matched to uploads by block address (TBP0 / CBP == DBP of an upload at 0,0), which is how
RenderWare's texture cache uploads them; anything else is reported as "not uploaded in this frame".

Usage: python tools/gifcap.py [out/gif_capture.bin] [--sheet out/analysis/textures.png] [--groups N]
The capture and the sheet are game content: they stay under out/ (never committed).
"""
import argparse
import struct
import sys
import zlib

PSM_BITS = {0x00: 32, 0x01: 24, 0x02: 16, 0x0A: 16, 0x13: 8, 0x14: 4, 0x1B: 8, 0x24: 4, 0x2C: 4, 0x30: 32, 0x31: 24,
            0x32: 16, 0x3A: 16}


def tex0_fields(v):
    f = lambda lo, n: (v >> lo) & ((1 << n) - 1)
    return dict(tbp=f(0, 14), tbw=f(14, 6), psm=f(20, 6), tw=f(26, 4), th=f(30, 4), tcc=f(34, 1), tfx=f(35, 2),
                cbp=f(37, 14), cpsm=f(51, 4), csm=f(55, 1), csa=f(56, 5), cld=f(61, 3))


def clut_index_position(i):
    # CSM1, 256 entries: bits 3 and 4 of the index are swapped in the 16x16 CLUT image.
    return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


class Upload:
    def __init__(self, n, dbp, dbw, dpsm, dx, dy, w, h, order):
        self.n, self.dbp, self.dbw, self.dpsm, self.dx, self.dy, self.w, self.h = n, dbp, dbw, dpsm, dx, dy, w, h
        self.order = order  # position in the frame's event stream
        self.expect = w * h * PSM_BITS.get(dpsm, 32) // 8
        self.data = bytearray()

    def label(self):
        return f"#{self.n} dbp={self.dbp:#x} psm={self.dpsm:#x} {self.w}x{self.h}"


class Gs:
    def __init__(self):
        self.prim = 0
        self.prmodecont = 1
        self.prmode = 0
        self.rgba = (0x80, 0x80, 0x80, 0x80)
        self.tex0 = [0, 0]
        self.clut = None  # (upload, cpsm, csa) loaded by the last TEX0 with CLD != 0
        self.clut_at_tex0 = [None, None]
        self.bitbltbuf = self.trxpos = self.trxreg = 0
        self.upload = None
        self.uploads = []
        self.by_dbp = {}
        self.events = 0
        self.vcount = 0
        self.test = [0, 0]
        self.alpha = [0, 0]
        self.frame = [0, 0]
        self.zbuf = [0, 0]
        self.path = 0
        self.xyoffset = [0, 0]
        self.verts = []
        self.timeline = []
        self.groups = []  # [key, count, rgb sums, tex upload, clut]
        self.group_index = {}
        self.reg_counts = {}

    # Attributes of the drawing: PRIM itself, or PRMODE when PRMODECONT.AC = 0.
    def attrs(self):
        return self.prim if (self.prmodecont & 1) else ((self.prim & 7) | (self.prmode & 0x7F8))

    def write(self, reg, value):
        self.events += 1
        self.reg_counts[reg] = self.reg_counts.get(reg, 0) + 1
        if reg == 0x00:
            self.prim = value & 0x7FF
            self.vcount = 0
        elif reg == 0x01:
            self.rgba = (value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF)
        elif reg in (0x06, 0x07):
            c = reg - 0x06
            self.tex0[c] = value
            t = tex0_fields(value)
            if t['cld'] != 0:  # 1 = always load; 2-5 compare / CBP0 / CBP1 variants: treated as load
                self.clut = (self.by_dbp.get(t['cbp']), t['cpsm'], t['csa'], t['cbp'])
            self.clut_at_tex0[c] = self.clut
        elif reg in (0x47, 0x48):
            self.test[reg - 0x47] = value
        elif reg in (0x42, 0x43):
            self.alpha[reg - 0x42] = value
        elif reg in (0x4C, 0x4D):
            self.frame[reg - 0x4C] = value
        elif reg in (0x4E, 0x4F):
            self.zbuf[reg - 0x4E] = value
        elif reg == 0x1A:
            self.prmodecont = value
        elif reg == 0x1B:
            self.prmode = value
        elif reg == 0x50:
            self.bitbltbuf = value
        elif reg == 0x51:
            self.trxpos = value
        elif reg == 0x52:
            self.trxreg = value
        elif reg == 0x53:
            if (value & 3) == 0:
                dbp = (self.bitbltbuf >> 32) & 0x3FFF
                up = Upload(len(self.uploads), dbp, (self.bitbltbuf >> 48) & 0x3F, (self.bitbltbuf >> 56) & 0x3F,
                            (self.trxpos >> 32) & 0x7FF, (self.trxpos >> 48) & 0x7FF, self.trxreg & 0xFFF,
                            (self.trxreg >> 32) & 0xFFF, self.events)
                self.uploads.append(up)
                self.upload = up if up.expect else None
                if up.dx == 0 and up.dy == 0:
                    self.by_dbp[dbp] = up
        elif reg in (0x18, 0x19):
            self.xyoffset[reg - 0x18] = value
        elif reg in (0x04, 0x05, 0x0C, 0x0D):
            z = ((value >> 32) & 0xFFFFFF) if reg in (0x04, 0x0C) else (value >> 32)
            self.vertex(reg in (0x04, 0x05), value & 0xFFFF, (value >> 16) & 0xFFFF, z)

    def image(self, data):
        up = self.upload
        if not up:
            return
        take = min(len(data), up.expect - len(up.data))
        up.data += data[:take]
        if len(up.data) >= up.expect:
            self.upload = None

    def vertex(self, kick, x16=0, y16=0, z=0):
        self.vcount += 1
        a = self.attrs()
        ctxt = (a >> 9) & 1
        ofs = self.xyoffset[ctxt]
        self.verts = (self.verts + [((x16 - (ofs & 0xFFFF)) >> 4, (y16 - ((ofs >> 32) & 0xFFFF)) >> 4, z, self.rgba)])[-3:]
        ptype = self.prim & 7
        need = {1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}.get(ptype, 1)
        if not kick or self.vcount < need:
            return
        if ptype in (1, 3, 6):
            self.vcount = 0
        tme = (a >> 4) & 1
        self.timeline_add(a, ptype, ctxt, tme, self.verts[-need:])
        if not tme:
            return
        tex0 = self.tex0[ctxt]
        key = (ctxt, a & 0x7F8, tex0, self.test[ctxt], self.alpha[ctxt], self.frame[ctxt])
        g = self.group_index.get(key)
        if g is None:
            t = tex0_fields(tex0)
            g = dict(key=key, count=0, rgb=[0, 0, 0, 0], amin=255, amax=0, tex=self.by_dbp.get(t['tbp']), clut=self.clut_at_tex0[ctxt],
                     first=self.events)
            self.group_index[key] = g
            self.groups.append(g)
        g['count'] += 1
        for i in range(4):
            g['rgb'][i] += self.rgba[i]
        g['amin'] = min(g['amin'], self.rgba[3])
        g['amax'] = max(g['amax'], self.rgba[3])

    # Timeline: runs of consecutive primitives with the same drawing state, in stream order.
    def timeline_add(self, a, ptype, ctxt, tme, verts):
        sig = (self.path, ptype, ctxt, tme, a & 0x7F8, self.frame[ctxt], self.test[ctxt], self.alpha[ctxt], self.zbuf[ctxt],
               (self.tex0[ctxt] & 0x3FFF) if tme else -1)
        xs = [v[0] for v in verts]
        ys = [v[1] for v in verts]
        zs = [v[2] for v in verts]
        if self.timeline and self.timeline[-1]['sig'] == sig:
            s = self.timeline[-1]
        else:
            s = dict(sig=sig, count=0, x0=1 << 30, y0=1 << 30, x1=-(1 << 30), y1=-(1 << 30), z0=1 << 40, z1=-1,
                     rgba=self.rgba, uploads_before=len(self.uploads), tris=0, flat=0)
            self.timeline.append(s)
        s['count'] += 1
        if len(verts) == 3:
            # "flat": the three vertices carry the same RGB (lighting per face, or one colour for the whole mesh).
            s['tris'] += 1
            s['flat'] += int(verts[0][3][:3] == verts[1][3][:3] == verts[2][3][:3])
        s['x0'], s['x1'] = min(s['x0'], *xs), max(s['x1'], *xs)
        s['y0'], s['y1'] = min(s['y0'], *ys), max(s['y1'], *ys)
        s['z0'], s['z1'] = min(s['z0'], *zs), max(s['z1'], *zs)
        s['rgba'] = self.rgba


def run_packets(gs, data):
    off = 0
    n = len(data)
    while n - off >= 16:
        lo, regs = struct.unpack_from('<QQ', data, off)
        nloop = lo & 0x7FFF
        pre = (lo >> 46) & 1
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        off += 16
        if pre:
            gs.write(0x00, (lo >> 47) & 0x7FF)
        if flg == 0:
            for _ in range(nloop):
                for r in range(nreg):
                    if off + 16 > n:
                        return
                    qlo, qhi = struct.unpack_from('<QQ', data, off)
                    off += 16
                    desc = (regs >> (4 * r)) & 0xF
                    if desc == 0x0:
                        gs.write(0x00, qlo)
                    elif desc == 0x1:
                        gs.write(0x01, (qlo & 0xFF) | (((qlo >> 32) & 0xFF) << 8) | ((qhi & 0xFF) << 16) |
                                 (((qhi >> 32) & 0xFF) << 24))
                    elif desc in (0x4, 0x5):
                        # PACKED XYZ(F)2 -> register layout: X 0-15, Y 16-31, Z 32-63 (XYZF: Z 32-55, F 56-63).
                        xy = (qlo & 0xFFFF) | (((qlo >> 32) & 0xFFFF) << 16)
                        if desc == 0x4:
                            value = xy | (((qhi >> 4) & 0xFFFFFF) << 32) | (((qhi >> 36) & 0xFF) << 56)
                        else:
                            value = xy | ((qhi & 0xFFFFFFFF) << 32)
                        if (qhi >> 47) & 1:  # ADC: vertex without a drawing kick
                            gs.write(0x0C if desc == 0x4 else 0x0D, value)
                        else:
                            gs.write(desc, value)
                    elif desc in (0xC, 0xD, 0x6, 0x7, 0x0):
                        gs.write(desc, qlo)
                    elif desc == 0xE:
                        gs.write(qhi & 0xFF, qlo)
        elif flg == 1:
            words = nloop * nreg
            for i in range(words):
                if off + 8 > n:
                    return
                (value,) = struct.unpack_from('<Q', data, off)
                off += 8
                desc = (regs >> (4 * (i % nreg))) & 0xF
                if desc <= 0xD:
                    gs.write(desc, value)
            if words & 1:
                off += 8
        else:
            size = nloop * 16
            gs.image(data[off:off + size])
            off += size


def clut_colour(clut_upload, cpsm, index):
    if not clut_upload or not clut_upload.data:
        return None
    d = clut_upload.data
    if cpsm == 0:  # CT32
        p = clut_index_position(index) if clut_upload.w * clut_upload.h >= 256 else index
        if p * 4 + 4 > len(d):
            return None
        return d[p * 4], d[p * 4 + 1], d[p * 4 + 2], d[p * 4 + 3]
    p = clut_index_position(index) if clut_upload.w * clut_upload.h >= 256 else index
    if p * 2 + 2 > len(d):
        return None
    v = d[p * 2] | (d[p * 2 + 1] << 8)
    return (v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, 0x80 if v & 0x8000 else 0


def texels(up):
    """Texel indices / colours of an upload, row-major."""
    if up.dpsm == 0x13:
        return list(up.data)
    if up.dpsm == 0x14:
        out = []
        for b in up.data:
            out += [b & 15, b >> 4]
        return out
    return None


def texture_rgb(tex, clut):
    """Mean RGB / alpha of the texture as sampled through its CLUT, or None."""
    if not tex or not tex.data:
        return None
    idx = texels(tex)
    if idx is None:
        if tex.dpsm == 0 and len(tex.data) >= 4:
            d = tex.data
            n = len(d) // 4
            return tuple(sum(d[i::4]) / n for i in range(4))
        return None
    if not clut:
        return None
    cup, cpsm = clut[0], clut[1]
    cache = {}
    sums = [0, 0, 0, 0]
    for i in idx:
        c = cache.get(i)
        if c is None:
            c = cache[i] = clut_colour(cup, cpsm, i) or (0, 0, 0, 0)
        for k in range(4):
            sums[k] += c[k]
    return tuple(s / max(1, len(idx)) for s in sums)


def write_png(path, width, height, rgb_rows):
    raw = b''.join(b'\x00' + bytes(row) for row in rgb_rows)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def sheet(path, groups, cell=128, columns=8):
    items = [g for g in groups if g['tex'] and g['clut'] and texels(g['tex']) is not None]
    seen, unique = set(), []
    for g in items:
        k = (g['tex'].n, g['clut'][0].n if g['clut'][0] else -1)
        if k not in seen:
            seen.add(k)
            unique.append(g)
    if not unique:
        print('sheet: no CLUT textures to draw')
        return
    rows = (len(unique) + columns - 1) // columns
    W, H = columns * cell, rows * cell
    img = [bytearray(W * 3) for _ in range(H)]
    for n, g in enumerate(unique):
        tex, (cup, cpsm) = g['tex'], g['clut'][:2]
        idx = texels(tex)
        ox, oy = (n % columns) * cell, (n // columns) * cell
        for y in range(cell):
            ty = y * tex.h // cell
            for x in range(cell):
                tx = x * tex.w // cell
                i = ty * tex.w + tx
                c = clut_colour(cup, cpsm, idx[i]) if i < len(idx) else None
                if c:
                    img[oy + y][(ox + x) * 3:(ox + x) * 3 + 3] = bytes(c[:3])
    write_png(path, W, H, img)
    print(f'sheet: {len(unique)} textures -> {path}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture', nargs='?', default='out/gif_capture.bin')
    ap.add_argument('--sheet')
    ap.add_argument('--groups', type=int, default=60)
    ap.add_argument('--timeline', action='store_true', help='print every run of primitives with the same state')
    args = ap.parse_args()
    blob = open(args.capture, 'rb').read()
    gs = Gs()
    off = 0
    paths = {}
    while off + 16 <= len(blob):
        kind, ident, size = struct.unpack_from('<IIQ', blob, off)
        off += 16
        if kind == 0:
            paths[ident] = paths.get(ident, 0) + size
            gs.path = ident
            run_packets(gs, blob[off:off + size])
            off += size
        else:
            gs.write(ident, size)
    print(f'GIF bytes per path: {paths}; register writes: PRMODECONT x{gs.reg_counts.get(0x1A, 0)} '
          f'PRMODE x{gs.reg_counts.get(0x1B, 0)} TEXA x{gs.reg_counts.get(0x3B, 0)} TEXCLUT x{gs.reg_counts.get(0x1C, 0)}')
    print(f'{len(gs.uploads)} uploads:')
    for up in gs.uploads[:200]:
        d = up.data
        extra = ''
        if up.dpsm == 0 and d:
            n = len(d) // 4
            extra = ' mean RGBA ' + ' '.join(f'{sum(d[i::4]) / n:.0f}' for i in range(4))
        elif d:
            extra = f' mean byte {sum(d) / len(d):.1f} max {max(d)}'
        print(f'  {up.label()} at {up.dx},{up.dy} dbw={up.dbw} bytes {len(d)}/{up.expect}{extra}')
    print(f'{len(gs.groups)} textured groups (ctxt, attrs, TEX0); texture brightness = mean texel colour through the CLUT:')
    for g in gs.groups[:args.groups]:
        ctxt, attrs, tex0, test, alpha, frame = g['key']
        t = tex0_fields(tex0)
        rgb = '/'.join(f"{c / g['count']:.0f}" for c in g['rgb'])
        tb = texture_rgb(g['tex'], g['clut'])
        tbs = ' '.join(f'{v:.0f}' for v in tb) if tb else '-'
        tex = g['tex'].label() if g['tex'] else 'not uploaded in this frame'
        clut = g['clut']
        cl = (clut[0].label() if clut[0] else f'CBP {clut[3]:#x} not uploaded in this frame') if clut else 'no CLUT load'
        print(f"  ctxt {ctxt} abe {(attrs >> 6) & 1} fst {(attrs >> 8) & 1} prims {g['count']:5d} vertex rgb {rgb:11s} "
              f"TEX0 tbp {t['tbp']:#x} psm {t['psm']:#x} {1 << t['tw']}x{1 << t['th']} tcc {t['tcc']} tfx {t['tfx']} "
              f"cbp {t['cbp']:#x} cpsm {t['cpsm']:#x} cld {t['cld']} | vertex A {g['amin']}..{g['amax']} "
              f"TEST {test:#x} (ATE {test & 1} ATST {(test >> 1) & 7} AREF {(test >> 4) & 0xFF:#x} AFAIL {(test >> 12) & 3} "
              f"ZTE {(test >> 16) & 1} ZTST {(test >> 17) & 3}) ALPHA {alpha:#x} FBP {frame & 0x1FF:#x} FBMSK {frame >> 32:#x} | texture RGBA {tbs} | tex {tex} | clut {cl}")
    if args.timeline:
        print(f'timeline: {len(gs.timeline)} runs of primitives with the same state, in stream order:')
        for s in gs.timeline:
            path, ptype, ctxt, tme, attrs, frame, test, alpha, zbuf, tbp = s['sig']
            r, g, b, a = s['rgba']
            print(f"  [{s['uploads_before']:3d} uploads] path {path} type {ptype} ctxt {ctxt} tme {tme} iip {(attrs >> 3) & 1} abe {(attrs >> 6) & 1} "
                  f"fge {(attrs >> 5) & 1} x{s['count']:5d} box {s['x0']},{s['y0']}..{s['x1']},{s['y1']} "
                  f"Z {s['z0']:#x}..{s['z1']:#x} rgba {r:02x}{g:02x}{b:02x}{a:02x} FBP {frame & 0x1FF:#x} "
                  f"FBMSK {frame >> 32:08x} TEST {test:#x} ALPHA {alpha:#x} ZBUF {zbuf:#x} flat {s['flat']}/{s['tris']}"
                  + (f" tbp {tbp:#x}" if tme else ''))
    if args.sheet:
        sheet(args.sheet, gs.groups)


if __name__ == '__main__':
    sys.exit(main())
