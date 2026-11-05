#!/usr/bin/env python3
"""VU micro-mode disassembler (VU0 / VU1 microcode, 64-bit upper|lower pairs).

  python tools/vudis.py out/vu1dump0_code.bin [--from 0x0] [--to 0x4000]

Addresses are byte offsets in micro memory (the PC as the VU sees it * 8 = byte offset).
Each line: address, upper instruction ; lower instruction. Flags: [I] = lower word is a float
immediate loaded into I, [E] end bit, [M] [D] [T].
"""
import argparse
import struct

BC = 'xyzw'


def dest(d):
    return ''.join(c for i, c in enumerate('xyzw') if d & (8 >> i))


def upper(w):
    d = dest((w >> 21) & 0xF)
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    op = w & 0x3F
    if (op & 0x3C) == 0x3C:
        sp = ((w >> 6) & 0x1F) << 2 | (w & 3)
        bc = BC[sp & 3]
        if sp < 0x10:
            name = ['ADDA', 'SUBA', 'MADDA', 'MSUBA'][sp >> 2] + bc
            return f'{name}.{d} ACC, vf{fs}, vf{ft}{bc}'
        if 0x10 <= sp <= 0x13:
            return f'ITOF{[0, 4, 12, 15][sp & 3]}.{d} vf{ft}, vf{fs}'
        if 0x14 <= sp <= 0x17:
            return f'FTOI{[0, 4, 12, 15][sp & 3]}.{d} vf{ft}, vf{fs}'
        if 0x18 <= sp <= 0x1B:
            return f'MULA{bc}.{d} ACC, vf{fs}, vf{ft}{bc}'
        names = {0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIP', 0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi',
                 0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq', 0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA', 0x29: 'MADDA',
                 0x2A: 'MULA', 0x2C: 'SUBA', 0x2D: 'MSUBA', 0x2E: 'OPMULA', 0x2F: 'NOP'}
        n = names.get(sp, f'upper-special?{sp:#x}')
        if n == 'NOP':
            return 'NOP'
        if n == 'ABS':
            return f'ABS.{d} vf{ft}, vf{fs}'
        if n == 'CLIP':
            return f'CLIPw.xyz vf{fs}, vf{ft}w'
        if n.endswith(('q', 'i')):
            return f'{n}.{d} ACC, vf{fs}, {n[-1].upper()}'
        return f'{n}.{d} ACC, vf{fs}, vf{ft}'
    bc = BC[op & 3]
    if op < 0x18:
        name = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI'][op >> 2] + bc
        return f'{name}.{d} vf{fd}, vf{fs}, vf{ft}{bc}'
    if op < 0x1C:
        return f'MUL{bc}.{d} vf{fd}, vf{fs}, vf{ft}{bc}'
    names = {0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi',
             0x23: 'MADDi', 0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi', 0x28: 'ADD', 0x29: 'MADD',
             0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'}
    n = names.get(op, f'upper?{op:#x}')
    if n.endswith(('q', 'i')):
        return f'{n}.{d} vf{fd}, vf{fs}, {n[-1].upper()}'
    return f'{n}.{d} vf{fd}, vf{fs}, vf{ft}'


def simm(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def lower(w, pc):
    op = (w >> 25) & 0x7F
    d = dest((w >> 21) & 0xF)
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm11 = simm(w & 0x7FF, 11)
    target = lambda: f'{(pc + 8 + imm11 * 8) & 0x3FFF:#06x}'
    if op == 0x40:
        f = w & 0x3F
        if (f & 0x3C) == 0x3C:
            sp = ((w >> 6) & 0x1F) << 2 | (w & 3)
            fsf, ftf = BC[(w >> 21) & 3], BC[(w >> 23) & 3]
            table = {
                0x30: f'MOVE.{d} vf{ft}, vf{fs}', 0x31: f'MR32.{d} vf{ft}, vf{fs}',
                0x34: f'LQI.{d} vf{ft}, (vi{fs}++)', 0x35: f'SQI.{d} vf{fs}, (vi{ft}++)',
                0x36: f'LQD.{d} vf{ft}, (--vi{fs})', 0x37: f'SQD.{d} vf{fs}, (--vi{ft})',
                0x38: f'DIV Q, vf{fs}{fsf}, vf{ft}{ftf}', 0x39: f'SQRT Q, vf{ft}{ftf}', 0x3A: f'RSQRT Q, vf{fs}{fsf}, vf{ft}{ftf}',
                0x3B: 'WAITQ', 0x3C: f'MTIR vi{ft}, vf{fs}{fsf}', 0x3D: f'MFIR.{d} vf{ft}, vi{fs}',
                0x3E: f'ILWR.{d} vi{ft}, (vi{fs})', 0x3F: f'ISWR.{d} vi{ft}, (vi{fs})',
                0x40: f'RNEXT.{d} vf{ft}, R', 0x41: f'RGET.{d} vf{ft}, R', 0x42: f'RINIT R, vf{fs}{fsf}', 0x43: f'RXOR R, vf{fs}{fsf}',
                0x64: f'MFP.{d} vf{ft}, P', 0x68: f'XTOP vi{ft}', 0x69: f'XITOP vi{ft}', 0x6C: f'XGKICK vi{fs}',
                0x70: f'ESADD P, vf{fs}', 0x71: f'ERSADD P, vf{fs}', 0x72: f'ELENG P, vf{fs}', 0x73: f'ERLENG P, vf{fs}',
                0x74: f'EATANxy P, vf{fs}', 0x75: f'EATANxz P, vf{fs}', 0x76: f'ESUM P, vf{fs}', 0x78: f'ESQRT P, vf{fs}{fsf}',
                0x79: f'ERSQRT P, vf{fs}{fsf}', 0x7A: f'ERCPR P, vf{fs}{fsf}', 0x7B: 'WAITP', 0x7C: f'ESIN P, vf{fs}{fsf}',
                0x7D: f'EATAN P, vf{fs}{fsf}', 0x7E: f'EEXP P, vf{fs}{fsf}'}
            return table.get(sp, f'lower-special?{sp:#x}')
        names = {0x30: 'IADD', 0x31: 'ISUB', 0x34: 'IAND', 0x35: 'IOR'}
        if f == 0x32:
            return f'IADDI vi{ft}, vi{fs}, {simm((w >> 6) & 0x1F, 5)}'
        return f'{names.get(f, f"lower?{f:#x}")} vi{fd}, vi{fs}, vi{ft}'
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
    imm24 = w & 0xFFFFFF
    table = {
        0x00: lambda: f'LQ.{d} vf{ft}, {imm11}(vi{fs})', 0x01: lambda: f'SQ.{d} vf{fs}, {imm11}(vi{ft})',
        0x04: lambda: f'ILW.{d} vi{ft}, {imm11}(vi{fs})', 0x05: lambda: f'ISW.{d} vi{ft}, {imm11}(vi{fs})',
        0x08: lambda: f'IADDIU vi{ft}, vi{fs}, {imm15:#x}', 0x09: lambda: f'ISUBIU vi{ft}, vi{fs}, {imm15:#x}',
        0x10: lambda: f'FCEQ vi1, {imm24:#x}', 0x11: lambda: f'FCSET {imm24:#x}', 0x12: lambda: f'FCAND vi1, {imm24:#x}',
        0x13: lambda: f'FCOR vi1, {imm24:#x}', 0x14: lambda: f'FSEQ vi{ft}, {w & 0xFFF:#x}', 0x15: lambda: f'FSSET {w & 0xFFF:#x}',
        0x16: lambda: f'FSAND vi{ft}, {w & 0xFFF:#x}', 0x17: lambda: f'FSOR vi{ft}, {w & 0xFFF:#x}',
        0x18: lambda: f'FMEQ vi{ft}, vi{fs}', 0x1A: lambda: f'FMAND vi{ft}, vi{fs}', 0x1B: lambda: f'FMOR vi{ft}, vi{fs}',
        0x1C: lambda: f'FCGET vi{ft}', 0x20: lambda: f'B {target()}', 0x21: lambda: f'BAL vi{ft}, {target()}',
        0x24: lambda: f'JR vi{fs}', 0x25: lambda: f'JALR vi{ft}, vi{fs}',
        0x28: lambda: f'IBEQ vi{ft}, vi{fs}, {target()}', 0x29: lambda: f'IBNE vi{ft}, vi{fs}, {target()}',
        0x2C: lambda: f'IBLTZ vi{fs}, {target()}', 0x2D: lambda: f'IBGTZ vi{fs}, {target()}',
        0x2E: lambda: f'IBLEZ vi{fs}, {target()}', 0x2F: lambda: f'IBGEZ vi{fs}, {target()}'}
    fn = table.get(op)
    return fn() if fn else f'lower?{op:#x}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('code')
    ap.add_argument('--from', dest='start', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--to', dest='end', type=lambda s: int(s, 0), default=None)
    args = ap.parse_args()
    code = open(args.code, 'rb').read()
    end = min(args.end if args.end is not None else len(code), len(code))
    for pc in range(args.start & ~7, end, 8):
        lo, hi = struct.unpack_from('<II', code, pc)
        flags = ''.join(f for bit, f in ((31, 'I'), (30, 'E'), (29, 'M'), (28, 'D'), (27, 'T')) if hi >> bit & 1)
        up = upper(hi)
        if hi >> 31 & 1:
            low = f'LOI {struct.unpack("<f", struct.pack("<I", lo))[0]:g}'
        else:
            low = lower(lo, pc)
        print(f'{pc:#06x}: {("[" + flags + "] ") if flags else ""}{up:40s} ; {low}')


if __name__ == '__main__':
    main()
