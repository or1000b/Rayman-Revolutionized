"""usage: python tools/vu1aot/vu1reach.py [out/vu1prof]

Reachable code per VU1 image: from the profiled entries, follow branches (B/BAL/IB*), JR/JALR targets from the
profile, stop after an E-bit pair's delay slot. Prints per image the reachable pairs and a hash of the reachable
bytes, then how many distinct programs (by reachable bytes) the 38 images hold."""
import collections, glob, hashlib, os, sys

folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join('out', 'vu1prof')
entries = collections.defaultdict(collections.Counter)
jr = collections.defaultdict(lambda: collections.defaultdict(set))
cycles = collections.Counter()
for path in glob.glob(os.path.join(folder, 'profile_*.txt')):
    for line in open(path):
        p = line.split()
        if not p or p[0].startswith('#'):
            continue
        if p[0] == 'code':
            cycles[p[1]] += int(dict(zip(p[2::2], p[3::2]))['cycles'])
        elif p[0] == 'entry':
            entries[p[1]][int(p[2], 16)] += int(p[3])
        elif p[0] == 'jr':
            jr[p[1]][int(p[2], 16)].add(int(p[3], 16))

def sx11(v):
    v &= 0x7ff
    return v - 0x800 if v & 0x400 else v

def reach(code, starts, jrt):
    seen = set()
    work = list(starts)
    while work:
        pc = work.pop()
        while 0 <= pc < len(code) and pc not in seen:
            seen.add(pc)
            lo = int.from_bytes(code[pc:pc + 4], 'little')
            hi = int.from_bytes(code[pc + 4:pc + 8], 'little')
            ibit = hi >> 31 & 1
            ebit = hi >> 30 & 1
            op = lo >> 25 & 0x7f
            if not ibit:
                if op in (0x20, 0x21):  # B, BAL
                    work.append(pc + 8 + sx11(lo) * 8)
                    if op == 0x21:
                        work.append(pc + 16)  # return address
                    # B: unconditional; the delay slot runs, then the target
                    seen.add(pc + 8)
                    break
                if op in (0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
                    work.append(pc + 8 + sx11(lo) * 8)
                if op in (0x24, 0x25):  # JR, JALR
                    work.extend(jrt.get(pc, ()))
                    if op == 0x25:
                        work.append(pc + 16)
                    seen.add(pc + 8)
                    break
            if ebit:
                seen.add(pc + 8)  # delay slot
                break
            pc += 8
    return seen

programs = collections.defaultdict(list)
for h in sorted(entries, key=lambda k: -cycles[k]):
    code = open(os.path.join(folder, 'code_%s.bin' % h), 'rb').read()
    pcs = reach(code, entries[h].keys(), jr[h])
    body = b''.join(code[pc:pc + 8] for pc in sorted(pcs))
    key = hashlib.sha1(repr(sorted(pcs)).encode() + body).hexdigest()[:12]
    programs[key].append(h)
    lo, hi = min(pcs), max(pcs) + 8
    print('%s %5.1f%% reachable %4d pairs range %04x-%04x program %s' % (h, 100.0 * cycles[h] / sum(cycles.values()), len(pcs), lo, hi, key))
print('\n%d images -> %d distinct reachable programs' % (len(entries), len(programs)))
for key, hs in sorted(programs.items(), key=lambda kv: -sum(cycles[h] for h in kv[1])):
    print('  %s %5.1f%% images %d' % (key, 100.0 * sum(cycles[h] for h in hs) / sum(cycles.values()), len(hs)))
