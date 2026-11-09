#!/usr/bin/env python3
"""Summarise the VU1 survey written by PS2X_VU1_PROFILE=1 (out/vu1prof/profile_<pid>.txt, one per game run).

usage: python tools/vu1aot/vu1prof.py [out/vu1prof]

Merges every profile file: per VU1 micro memory image (FNV-1a hash of the 16 KiB) the runs (MSCAL), resumes (MSCNT),
VU cycles, runs that used up the cycle budget, the entry PCs and the JR / JALR targets. Images are listed by cycles,
the share of all VU1 cycles first, so the translation can start with the programs that matter.
"""
import collections
import glob
import os
import sys


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join('out', 'vu1prof')
    images = collections.defaultdict(lambda: {'runs': 0, 'resumes': 0, 'cycles': 0, 'budget_hits': 0, 'unfinished': 0,
                                              'entries': collections.Counter(), 'jr': collections.Counter()})
    files = sorted(glob.glob(os.path.join(folder, 'profile_*.txt')))
    for path in files:
        for line in open(path):
            p = line.split()
            if not p or p[0].startswith('#'):
                continue
            if p[0] == 'code':
                img = images[p[1]]
                fields = dict(zip(p[2::2], p[3::2]))
                for k in ('runs', 'resumes', 'cycles', 'budget_hits', 'unfinished'):
                    img[k] += int(fields[k])
            elif p[0] == 'entry':
                images[p[1]]['entries'][int(p[2], 16)] += int(p[3])
            elif p[0] == 'jr':
                images[p[1]]['jr'][(int(p[2], 16), int(p[3], 16))] += int(p[4])
    total = sum(i['cycles'] for i in images.values()) or 1
    print('%d profile files, %d code images, %d VU1 cycles' % (len(files), len(images), total))
    for h, img in sorted(images.items(), key=lambda kv: -kv[1]['cycles']):
        code = os.path.join(folder, 'code_%s.bin' % h)
        size = 0
        if os.path.exists(code):
            data = open(code, 'rb').read()
            # Last pair that is not the NOP pair (lower NOP 0x8000033c, upper NOP 0x000002ff) or zero.
            for pc in range(len(data) - 8, -8, -8):
                lo = int.from_bytes(data[pc:pc + 4], 'little')
                hi = int.from_bytes(data[pc + 4:pc + 8], 'little')
                if (lo, hi) not in ((0, 0), (0x8000033c, 0x000002ff)):
                    size = pc + 8
                    break
        print('\n%s %5.1f%% cycles %d runs %d resumes %d budget_hits %d unfinished %d code_bytes 0x%x' %
              (h, 100.0 * img['cycles'] / total, img['cycles'], img['runs'], img['resumes'], img['budget_hits'],
               img['unfinished'], size))
        print('  entries: ' + ' '.join('%04x:%d' % (pc, n) for pc, n in sorted(img['entries'].items())))
        if img['jr']:
            print('  jr: ' + ' '.join('%04x->%04x:%d' % (s, t, n) for (s, t), n in sorted(img['jr'].items())))


if __name__ == '__main__':
    main()
