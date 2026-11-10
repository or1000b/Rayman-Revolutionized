#!/usr/bin/env python3
"""VU1 microprogram -> C++ ahead-of-time translator (docs/vu1_aot.md).

usage: python tools/vu1aot/vu1gen.py [--prof out/vu1prof] [--keys KEY,KEY...] [--out ps2xRuntime/src/lib/vu/aot]

For each program key (tools/vu1aot/vu1reach.py) the image with the most VU1 cycles is used: its code
(code_<hash>.bin), the interpreter's own decode table for it (decode_<hash>.txt: pipelines, latencies, register reads
and writes, the pair's VF shadow) and the profiled entries / JR targets of every image that holds the program.

The output reproduces VU1Interpreter::run bit for bit and cycle for cycle (immediate VF writes, the default):
the interpreter's timing state (per-lane VF / ACC ready cycles, VI ready cycles, the pending Q, the EFU slots and
resource, the flag pipeline, the branch hazard, the pending branch and the E-bit) is simulated here, so the C++ holds
one block per (pair, timing state) with a fixed stall, and only the values move at run time. Commits happen at the
cycle the interpreter makes them (flags in issue order, then Q, then P). What the generator cannot see ahead (an
XGKICK while the previous packet is still transferring, a JR target the profile never saw, the cycle budget) makes
the native run give up ("bail"): the dispatcher undoes it and the interpreter runs the whole program.
"""
import argparse
import collections
import glob
import os
import sys

PIPE_NONE, PIPE_FMAC, PIPE_LSU, PIPE_FDIV, PIPE_EFU, PIPE_IALU, PIPE_BRANCH, PIPE_XGKICK = range(8)
MAX_NODES = 60000


def sx11(v):
    v &= 0x7ff
    return v - 0x800 if v & 0x400 else v


class Usage:
    def __init__(self, f):
        # pipeline latency vfLatency viLatency vfR0 vfR1 vfW viRead viWrite accRead accWrite waitQ waitP readsClip
        # writesClip delays reserved
        self.pipe, self.lat, self.vflat, self.vilat = (int(x) for x in f[0:4])
        self.vfreads = []
        for t in f[4:6]:
            reg, lanes = t.split(':')
            reg, lanes = int(reg), int(lanes, 16)
            if reg and lanes:
                self.vfreads.append((reg, lanes))
        reg, lanes = f[6].split(':')
        self.vfw = (int(reg), int(lanes, 16))
        self.viread = int(f[7], 16)
        self.viwrite = int(f[8], 16)
        self.accread = int(f[9], 16)
        self.accwrite = int(f[10], 16)
        self.waitq, self.waitp, self.readsclip, self.writesclip, self.delays, self.reserved = (int(x) for x in f[11:17])

    def vf_latency(self):
        return self.vflat or self.lat

    def vi_latency(self):
        return self.vilat or self.lat


class Pair:
    def __init__(self, line):
        head, up, lo = line.split('|')
        h = head.split()
        self.pc = int(h[0], 16)
        self.lower = int(h[1], 16)
        self.upper = int(h[2], 16)
        self.ibit, self.ebit, self.mbit, self.dbit, self.tbit, self.shadow, self.suppressed = (int(x) for x in h[3:10])
        self.u = Usage(up.split())
        self.l = Usage(lo.split())


def lanes_idx(lanes):
    """VU dest bits (0x8 = x) -> component indices."""
    return [c for c in range(4) if lanes & (8 >> c)]


def upper_kind(instr):
    """'fmac' (applyFmacDest*: flags), 'clip', 'plain' (no flags), 'nop', or None (reserved)."""
    op = instr & 0x3f
    if op < 0x3c:
        if 0x10 <= op <= 0x17 or op in (0x1d, 0x1f, 0x2b, 0x2f):
            return 'plain'
        if op <= 0x2f:
            return 'fmac'
        return None
    sp = (instr & 3) | ((instr >> 4) & 0x7c)
    if sp == 0x1f:
        return 'clip'
    if 0x10 <= sp <= 0x17 or sp == 0x1d:
        return 'plain'
    if sp in (0x2f, 0x30):
        return 'nop'
    if sp <= 0x0f or 0x18 <= sp <= 0x1c or sp == 0x1e or 0x20 <= sp <= 0x2a or 0x2c <= sp <= 0x2e:
        return 'fmac'
    return None


def upper_template(instr):
    """The template call (ops::fmacSse / itof / absT) for an upper instruction, or None (generic upper())."""
    op = instr & 0x3f
    dest = instr >> 21 & 0xf
    ft = instr >> 16 & 0x1f
    fs = instr >> 11 & 0x1f
    fd = instr >> 6 & 0x1f
    if op < 0x3c:
        if op <= 0x1b:
            kind = ['Add', 'Sub', 'MAdd', 'MSub', 'Max', 'Min', 'Mul'][op >> 2]
            y, bc = 'YBc', op & 3
        else:
            table = {0x1c: ('Mul', 'YQ'), 0x1d: ('Max', 'YI'), 0x1e: ('Mul', 'YI'), 0x1f: ('Min', 'YI'),
                     0x20: ('Add', 'YQ'), 0x21: ('MAdd', 'YQ'), 0x22: ('Add', 'YI'), 0x23: ('MAdd', 'YI'),
                     0x24: ('Sub', 'YQ'), 0x25: ('MSub', 'YQ'), 0x26: ('Sub', 'YI'), 0x27: ('MSub', 'YI'),
                     0x28: ('Add', 'YVec'), 0x29: ('MAdd', 'YVec'), 0x2a: ('Mul', 'YVec'), 0x2b: ('Max', 'YVec'),
                     0x2c: ('Sub', 'YVec'), 0x2d: ('MSub', 'YVec'), 0x2e: ('OpMsub', 'YCross'), 0x2f: ('Min', 'YVec')}
            if op not in table:
                return None
            kind, y = table[op]
            bc = 0
        return 'ops::fmacSse<ops::Op::%s, %s, 0x%x, %d, %d, %d, %d, false, 0x%08xu>(c, fr)' % (kind, y, dest, fs, ft, fd, bc, instr)
    sp = (instr & 3) | ((instr >> 4) & 0x7c)
    if 0x10 <= sp <= 0x13:  # ITOF0 / 4 / 12 / 15
        return 'ops::itof<%d, 0x%x, %d, %d>(c)' % (sp & 3, dest, fs, ft)
    if sp == 0x1d:  # ABS
        return 'ops::absT<0x%x, %d, %d>(c)' % (dest, fs, ft)
    if sp <= 0x0f or 0x18 <= sp <= 0x1b:
        kind = {0: 'Add', 1: 'Sub', 2: 'MAdd', 3: 'MSub', 6: 'Mul'}[sp >> 2]
        y, bc = 'YBc', sp & 3
    else:
        table = {0x1c: ('Mul', 'YQ'), 0x1e: ('Mul', 'YI'), 0x20: ('Add', 'YQ'), 0x21: ('MAdd', 'YQ'), 0x22: ('Add', 'YI'),
                 0x23: ('MAdd', 'YI'), 0x24: ('Sub', 'YQ'), 0x25: ('MSub', 'YQ'), 0x26: ('Sub', 'YI'), 0x27: ('MSub', 'YI'),
                 0x28: ('Add', 'YVec'), 0x29: ('MAdd', 'YVec'), 0x2a: ('Mul', 'YVec'), 0x2c: ('Sub', 'YVec'),
                 0x2d: ('MSub', 'YVec'), 0x2e: ('OpMula', 'YCross')}
        if sp not in table:
            return None
        kind, y = table[sp]
        bc = 0
    return 'ops::fmacSse<ops::Op::%s, %s, 0x%x, %d, %d, %d, %d, true, 0x%08xu>(c, fr)' % (kind, y, dest, fs, ft, fd, bc, instr)


def lower_class(p):
    """How the lower instruction is emitted."""
    if p.ibit:
        return 'imm'
    lo = p.lower
    if lo in (0, 0x8000033c):
        return 'nop'
    op = lo >> 25 & 0x7f
    if op == 0x11:
        return 'fcset'
    if op == 0x15:
        return 'fsset'
    if op in (0x20, 0x21):
        return 'b'
    if op in (0x24, 0x25):
        return 'jr'
    if op in (0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
        return 'bcond'
    if op == 0x40 and (lo & 0x3f) >= 0x3c:
        sp = (lo & 3) | ((lo >> 4) & 0x7c)
        if sp in (0x38, 0x39, 0x3a):
            return 'fdiv'
        if sp == 0x6c:
            return 'xgkick'
        if sp in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7a, 0x7c, 0x7d, 0x7e):
            return 'efu'
    return 'plain'


def is_branch(p):
    return lower_class(p) in ('b', 'jr', 'bcond')


# ---------------------------------------------------------------- timing state

class State:
    """The interpreter's timing state at the top of a pair's loop iteration, relative to that cycle.
    vf / vi / acc: cycles until the register (lane) is ready. q: cycles until the pending Q commits (0 = none).
    efu: per slot cycles until P commits (0 = free). efures: cycles until the EFU takes a new op.
    flags: pending flag entries in issue order: (remaining, slot, kind, writesStatus, writesClip), kind 'f' (FMAC:
    queued only when flags are live), 's' (FSSET), 'c' (CLIP / FCSET). backup: VI register whose old value branches
    read (the pair before wrote it), 0 = none. vipend: VI write queue entries still in flight (for the flush)."""
    __slots__ = ('vf', 'vi', 'acc', 'q', 'efu', 'efures', 'flags', 'backup', 'vipend')

    def __init__(self):
        self.vf = (0,) * 128
        self.vi = (0,) * 16
        self.acc = (0,) * 4
        self.q = 0
        self.efu = (0, 0)
        self.efures = 0
        self.flags = ()
        self.backup = 0
        self.vipend = ()

    def key(self):
        return (self.vf, self.vi, self.acc, self.q, self.efu, self.efures, self.flags, self.backup, self.vipend)

    def copy(self):
        s = State()
        for k in State.__slots__:
            setattr(s, k, getattr(self, k))
        return s


def tick(st):
    """One cycle passes; returns the commits that happen at the new cycle, in the interpreter's order."""
    commits = []
    flags = []
    for rem, slot, kind, ws, wc in st.flags:
        rem -= 1
        if rem <= 0:
            commits.append(('flag', slot, kind, ws, wc))
        else:
            flags.append((rem, slot, kind, ws, wc))
    st.flags = tuple(flags)
    if st.q:
        st.q -= 1
        if st.q == 0:
            commits.append(('q',))
    efu = list(st.efu)
    for i in range(2):
        if efu[i]:
            efu[i] -= 1
            if efu[i] == 0:
                commits.append(('p', i))
    st.efu = tuple(efu)
    st.efures = max(0, st.efures - 1)
    st.vf = tuple(max(0, x - 1) for x in st.vf)
    st.vi = tuple(max(0, x - 1) for x in st.vi)
    st.acc = tuple(max(0, x - 1) for x in st.acc)
    st.vipend = tuple(x - 1 for x in st.vipend if x - 1 > 0)
    return commits


def stall_for(st, p):
    """calculatePairReadyCycle - now (static part)."""
    s = 0
    for u in (p.u, p.l):
        for reg, lanes in u.vfreads:
            for c in lanes_idx(lanes):
                s = max(s, st.vf[reg * 4 + c])
        m = u.viread & 0xfffe
        for r in range(16):
            if m >> r & 1:
                s = max(s, st.vi[r])
        for c in lanes_idx(u.accread):
            s = max(s, st.acc[c])
    if p.l.pipe == PIPE_FDIV and st.q:
        s = max(s, st.q)
    if p.l.pipe == PIPE_EFU:
        s = max(s, st.efures)
    if p.l.waitq and st.q:
        s = max(s, st.q)
    if p.l.waitp:
        for x in st.efu:
            s = max(s, x)
    return s


def free_slot(used, n):
    for i in range(n):
        if i not in used:
            return i
    return None


class Gen:
    def __init__(self, key, pairs, entries, jr):
        self.key = key
        self.pairs = pairs  # pc -> Pair
        self.entries = sorted(entries)
        self.jr = jr        # site pc -> set of targets
        self.nodes = {}     # node key -> name
        self.work = []
        self.out = []
        self.bails = collections.Counter()

    # A node: (pc, state, control). control: ('', ) normal; ('b', target) unconditional pending; ('bc', target, fall)
    # conditional (taken flag in `br`); ('jr', site) JR pending (target in `jt`); ('e',) the pair is the E-bit's slot.
    def node(self, pc, st, ctl):
        k = (pc, st.key(), ctl)
        name = self.nodes.get(k)
        if name is None:
            name = 'n%d' % len(self.nodes)
            self.nodes[k] = name
            self.work.append((name, pc, st.copy(), ctl))
            if len(self.nodes) > MAX_NODES:
                raise RuntimeError('too many nodes')
        return name

    def emit(self, line):
        self.out.append(line)

    def emit_commits(self, commits, ind):
        for c in commits:
            if c[0] == 'flag':
                _, slot, kind, ws, wc = c
                if kind == 'f':
                    self.emit(ind + 'if (c.flags[%d].on) commitFlag(c, c.flags[%d]);' % (slot, slot))
                else:
                    self.emit(ind + 'commitFlag(c, c.flags[%d]);' % slot)
            elif c[0] == 'q':
                self.emit(ind + 'commitQ(c);')
            else:
                self.emit(ind + 's.p = c.pendingP[%d];' % c[1])

    def bail(self, ind, why, pc):
        self.bails[why] += 1
        self.emit(ind + 'c.bail = %s; c.bailPc = 0x%x; return false;' % (why, pc))

    def gen_pair(self, name, pc, st, ctl):
        ind = '    '
        p = self.pairs.get(pc)
        # ctl ('k', inner): an XGKICK pair whose stall (static and the wait for the previous packet) is done.
        kicked = ctl[0] == 'k'
        if kicked:
            ctl = ctl[1]
        self.emit('%s: { // pc %04x%s%s' % (name, pc, ' (' + ctl[0] + ')' if ctl[0] else '', ' (stalled)' if kicked else ''))
        if p is None or p.u.reserved or p.l.reserved:
            self.bail(ind, 'BailUnsupported', pc)
            return
        lc = lower_class(p)
        uk = upper_kind(p.upper)
        if uk is None:
            self.bail(ind, 'BailUnsupported', pc)
            return
        if ctl[0] and is_branch(p):
            self.bail(ind, 'BailUnsupported', pc)  # branch in a branch delay slot / E-bit slot
            return
        if ctl[0] in ('b', 'bc', 'jr') and p.ebit:
            self.bail(ind, 'BailUnsupported', pc)  # E-bit in a branch delay slot
            return
        if ctl[0] == 'e' and p.ebit:
            self.bail(ind, 'BailUnsupported', pc)
            return
        if p.dbit or p.tbit:
            cond = ' || '.join(x for x, y in (('s.dBitEnabled', p.dbit), ('s.tBitEnabled', p.tbit)) if y)
            self.emit(ind + 'if (%s) { c.bail = BailUnsupported; c.bailPc = 0x%x; return false; }' % (cond, pc))
        st = st.copy()
        s = 0 if kicked else stall_for(st, p)
        if not kicked:
            # Budget: the interpreter stops at the loop top (cycle >= budget) or when the stall reaches it.
            self.emit(ind + 'if (c.cycles + %d >= c.budget) { c.bail = BailBudget; c.bailPc = 0x%x; return false; }' % (s, pc))
        commits = []
        for _ in range(s):
            commits += tick(st)
        self.emit_commits(commits, ind)
        if s:
            self.emit(ind + 'if (!advance(c, %d)) { c.bail = BailKickError; c.bailPc = 0x%x; return false; }' % (s, pc))
        if lc == 'xgkick' and not kicked:
            # XGKICK waits until the previous packet has gone (calculatePairReadyCycle: cycle + 1 while PATH1 is
            # busy, re-checked every cycle). The wait is data-dependent: count it, then continue in the block made
            # for that many extra cycles (commits that land meanwhile included). Past the longest pending latency
            # every wait leaves the same, settled state.
            longest = max(list(st.vf) + list(st.vi) + list(st.acc) + [st.q, st.efures] + list(st.efu) +
                          [f[0] for f in st.flags] + list(st.vipend) + [0])
            self.emit(ind + 'uint32_t ex = 0;')
            self.emit(ind + 'while (c.kick.active) { if (c.cycles + 1 >= c.budget) { c.bail = BailBudget; c.bailPc = 0x%x; return false; }'
                      ' if (!advance(c, 1)) { c.bail = BailKickError; c.bailPc = 0x%x; return false; } ++ex; }' % (pc, pc))
            self.emit(ind + 'switch (ex) {')
            sim = st.copy()
            landed = []  # commits of the extra cycles so far, in time order (each case jumps away: no fall-through)
            for k in range(longest + 1):
                if k:
                    landed = landed + tick(sim)
                self.emit(ind + ('default: {' if k == longest else 'case %d: {' % k))
                self.emit_commits(landed, ind + '    ')
                self.emit(ind + '    goto %s; }' % self.node(pc, sim, ('k', ctl)))
            self.emit(ind + '}')
            return

        # The branch hazard: the old value of a VI register this pair writes (for the next pair's branch).
        wvi = p.l.viwrite & 0xfffe
        written_vi = (wvi & -wvi).bit_length() - 1 if wvi else 0
        next_backup = written_vi if (written_vi and p.l.delays and not p.ibit) else 0
        if next_backup:
            self.emit(ind + 'const int32_t bkOld%s = s.vi[%d]; // the value before this pair' % (name, written_vi))

        def vi_read(reg):
            if reg == 0:
                return '0'
            if st.backup == reg:
                return 'bk'
            return 's.vi[%d]' % reg

        # Execute: upper, then lower (the interpreter's order, with the pair's VF shadow).
        used = {f[1] for f in st.flags}
        new_flags = []
        upper_slot = None
        if uk in ('fmac', 'clip'):
            upper_slot = free_slot(used, 8)
            if upper_slot is None:
                self.bail(ind, 'BailFlagOverflow', pc)
                return
            used.add(upper_slot)
        shadow = p.shadow if not p.ibit else 0
        if shadow:
            self.emit(ind + '{ float shOld[4], shNew[4]; std::memcpy(shOld, s.vf[%d], 16);' % shadow)
        if uk in ('fmac', 'clip', 'plain'):
            tmpl = upper_template(p.upper)
            if tmpl:
                self.emit(ind + '%s;' % tmpl)
            else:
                self.emit(ind + 'upper(c, 0x%08xu, fr, clipQ, clipV);' % p.upper)
            if uk == 'fmac':
                self.emit(ind + 'c.flags[%d] = {fr.mac, fr.status, fr.extraSticky, 0, true, WSTATUS_%s, false, false, fr.queued};' % (upper_slot, name))
                new_flags.append([4, upper_slot, 'f', True, False])
            elif uk == 'clip':
                self.emit(ind + 'c.flags[%d] = {0, 0, 0, clipV, false, false, false, WCLIP_%s, true};' % (upper_slot, name))
                new_flags.append([4, upper_slot, 'c', False, True])
        if shadow:
            self.emit(ind + 'std::memcpy(shNew, s.vf[%d], 16); std::memcpy(s.vf[%d], shOld, 16);' % (shadow, shadow))

        br_ctl = ('',)
        lo = p.lower
        if lc == 'imm':
            self.emit(ind + '{ float im; std::memcpy(&im, &kImm%s, 4); s.i = vuNormalizeOperand(im); }' % name)
        elif lc == 'nop':
            pass
        elif lc == 'plain':
            self.emit(ind + 'ops::lower<0x%08xu>(c);' % lo)
        elif lc == 'fdiv':
            self.emit(ind + '{ uint32_t lat; fdiv(c, 0x%08xu, c.pendingQ, c.pendingQDi, lat); }' % lo)
        elif lc == 'efu':
            slot = 0 if st.efu[0] == 0 else (1 if st.efu[1] == 0 else None)
            if slot is None:
                self.bail(ind, 'BailUnsupported', pc)
                return
            self.emit(ind + '{ uint32_t lat; efu(c, 0x%08xu, c.pendingP[%d], lat); }' % (lo, slot))
        elif lc == 'xgkick':
            vis = lo >> 11 & 0xf
            self.emit(ind + 'kickStart(c, static_cast<uint32_t>(static_cast<uint16_t>(s.vi[%d])));' % vis)
        elif lc == 'fsset':
            imm12 = (((lo >> 21) & 1) << 11) | (lo & 0x7ff)
            slot = free_slot(used, 8)
            if slot is None:
                self.bail(ind, 'BailFlagOverflow', pc)
                return
            used.add(slot)
            for f in new_flags:
                if f[2] == 'f':
                    f[3] = False  # queueFsset: same-cycle entries stop writing status
            self.emit(ind + 'c.flags[%d] = {0, 0x%xu, 0, 0, false, false, true, false, true};' % (slot, imm12 & 0xfc0))
            new_flags.append([4, slot, 's', False, False])
        elif lc == 'fcset':
            imm24 = lo & 0xffffff
            slot = free_slot(used, 8)
            if slot is None:
                self.bail(ind, 'BailFlagOverflow', pc)
                return
            used.add(slot)
            for f in new_flags:
                f[4] = False  # queueFcset: same-cycle entries stop writing clip
            self.emit(ind + 'c.workingClip = 0x%xu; c.flags[%d] = {0, 0, 0, 0x%xu, false, false, false, true, true};' % (imm24, slot, imm24))
            new_flags.append([4, slot, 'c', False, True])
        elif lc == 'b':
            op = lo >> 25 & 0x7f
            target = (pc + 8 + sx11(lo) * 8) & 0x3fff
            if op == 0x21:
                vit = lo >> 16 & 0xf
                if vit:
                    self.emit(ind + 's.vi[%d] = %d;' % (vit, (pc + 16) // 8))
            br_ctl = ('b', target)
        elif lc == 'jr':
            op = lo >> 25 & 0x7f
            vis = lo >> 11 & 0xf
            self.emit(ind + 'jt = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 8u) & 0x3FFFu;' % vi_read(vis))
            if op == 0x25:
                vit = lo >> 16 & 0xf
                if vit:
                    self.emit(ind + 's.vi[%d] = %d;' % (vit, (pc + 16) // 8))
            br_ctl = ('jr', pc)
        elif lc == 'bcond':
            op = lo >> 25 & 0x7f
            vis = lo >> 11 & 0xf
            vit = lo >> 16 & 0xf
            a = 'static_cast<int16_t>(%s)' % vi_read(vis)
            b = 'static_cast<int16_t>(%s)' % vi_read(vit)
            cond = {0x28: '%s == %s' % (a, b), 0x29: '%s != %s' % (a, b), 0x2c: '%s < 0' % a, 0x2d: '%s > 0' % a,
                    0x2e: '%s <= 0' % a, 0x2f: '%s >= 0' % a}[op]
            self.emit(ind + 'br = %s;' % cond)
            br_ctl = ('bc', (pc + 8 + sx11(lo) * 8) & 0x3fff, pc + 16)
        if shadow:
            self.emit(ind + 'std::memcpy(s.vf[%d], shNew, 16); }' % shadow)
        # VI registers hold 16 bits (the queued write lands as int16_t).
        if written_vi:
            self.emit(ind + 's.vi[%d] = static_cast<int16_t>(s.vi[%d]);' % (written_vi, written_vi))
        self.emit(ind + 's.vf[0][0] = 0.0f; s.vf[0][1] = 0.0f; s.vf[0][2] = 0.0f; s.vf[0][3] = 1.0f; s.vi[0] = 0;')
        if next_backup:
            self.emit(ind + 'bk = bkOld%s;' % name)

        # Timing after the pair (markPairWrites, queues).
        vf = list(st.vf)
        if p.l.vfw[0] and p.suppressed != p.l.vfw[0]:
            for c in lanes_idx(p.l.vfw[1]):
                vf[p.l.vfw[0] * 4 + c] = p.l.vf_latency()
        if p.u.vfw[0]:
            for c in lanes_idx(p.u.vfw[1]):
                vf[p.u.vfw[0] * 4 + c] = p.u.vf_latency()
        st.vf = tuple(vf)
        vi = list(st.vi)
        for r in range(1, 16):
            if wvi >> r & 1:
                vi[r] = p.l.vi_latency()
        st.vi = tuple(vi)
        if written_vi:
            st.vipend = st.vipend + (p.l.vi_latency(),)
        acc = list(st.acc)
        for c in lanes_idx(p.u.accwrite):
            acc[c] = 1
        st.acc = tuple(acc)
        if lc == 'fdiv':
            st.q = p.l.lat
        if lc == 'efu':
            efu = list(st.efu)
            efu[slot] = p.l.lat
            st.efu = tuple(efu)
            st.efures = max(0, p.l.lat - 1)
        st.flags = st.flags + tuple(tuple(f[:1] + f[1:2] + f[2:3] + [f[3], f[4]]) for f in new_flags)
        st.backup = next_backup
        # The flag entries' write bits are static: name them for the emitted initialisers.
        for f in new_flags:
            if f[2] == 'f':
                self.out_consts.append('constexpr bool WSTATUS_%s = %s;' % (name, 'true' if f[3] else 'false'))
            elif f[2] == 'c' and uk == 'clip' and f[1] == upper_slot:
                self.out_consts.append('constexpr bool WCLIP_%s = %s;' % (name, 'true' if f[4] else 'false'))
        if lc == 'imm':
            self.out_consts.append('constexpr uint32_t kImm%s = 0x%08xu;' % (name, p.lower))

        # advanceOneCycle.
        commits = tick(st)
        self.emit_commits(commits, ind)
        self.emit(ind + 'if (!advance(c, 1)) { c.bail = BailKickError; c.bailPc = 0x%x; return false; }' % pc)

        # Where the run goes next.
        if ctl[0] == 'e':
            self.gen_end(ind, pc + 8, st)
            return
        if ctl[0] == 'b':
            self.emit(ind + 's.branchTarget = 0x%xu;' % ctl[1])
            self.emit(ind + 'goto %s;' % self.node(ctl[1], st, ('',)))
            return
        if ctl[0] == 'bc':
            t = self.node(ctl[1], st, ('',))
            f = self.node(ctl[2], st, ('',))
            self.emit(ind + 'if (br) { s.branchTarget = 0x%xu; goto %s; } else goto %s;' % (ctl[1], t, f))
            return
        if ctl[0] == 'jr':
            targets = sorted(self.jr.get(ctl[1], ()))
            self.emit(ind + 's.branchTarget = jt;')
            self.emit(ind + 'switch (jt) {')
            for t in targets:
                self.emit(ind + 'case 0x%x: goto %s;' % (t, self.node(t, st, ('',))))
            self.emit(ind + 'default: c.bail = BailJrTarget; c.bailPc = 0x%x; return false; }' % pc)
            self.bails['BailJrTarget(default)'] += 1
            return
        if p.ebit:
            if br_ctl[0]:
                self.bail(ind, 'BailUnsupported', pc)  # branch with the E-bit
                return
            self.emit(ind + 'goto %s;' % self.node(pc + 8, st, ('e',)))
            return
        if br_ctl[0] == 'b':
            self.emit(ind + 'goto %s;' % self.node(pc + 8, st, ('b', br_ctl[1])))
        elif br_ctl[0] == 'bc':
            self.emit(ind + 'goto %s;' % self.node(pc + 8, st, br_ctl))
        elif br_ctl[0] == 'jr':
            self.emit(ind + 'goto %s;' % self.node(pc + 8, st, br_ctl))
        else:
            self.emit(ind + 'goto %s;' % self.node(pc + 8, st, ('',)))

    def gen_end(self, ind, next_pc, st):
        """The program ended (after the E-bit's slot): flushPipelines, then the state the interpreter leaves."""
        # Flush: until every queue is empty. Flag entries of FMAC instructions exist only when flags are live.
        rem_live = max([f[0] for f in st.flags] + [st.q] + list(st.efu) + list(st.vipend) + [0])
        rem_dead = max([f[0] for f in st.flags if f[2] != 'f'] + [st.q] + list(st.efu) + list(st.vipend) + [0])
        # Commits during the flush, in time order (registers only; the kick reads memory, so the order between
        # them does not matter).
        sim = st.copy()
        for _ in range(rem_live):
            self.emit_commits(tick(sim), ind)
        self.emit(ind + '{ const uint32_t fl = c.flagsLive ? %du : %du;' % (rem_live, rem_dead))
        self.emit(ind + '  if (!advance(c, fl)) { c.bail = BailKickError; c.bailPc = 0x%x; return false; }' % next_pc)
        self.emit(ind + '  uint32_t extra = 0; while (c.kick.active) { if (!advance(c, 1)) { c.bail = BailKickError; c.bailPc = 0x%x; return false; } ++extra; }' % next_pc)
        self.emit(ind + '  const uint32_t total = fl + extra;')
        self.emit(ind + '  std::memset(c.vfResidue, 0, sizeof(c.vfResidue));')
        for i, r in enumerate(st.vf):
            if r:
                self.emit(ind + '  c.vfResidue[%d][%d] = %du > total ? static_cast<uint8_t>(%du - total) : 0u;' % (i // 4, i % 4, r, r))
        self.emit(ind + '}')
        self.emit(ind + 's.pc = 0x%xu; s.ebit = false; s.haltAfterDelaySlot = false;' % next_pc)
        if st.backup:
            self.emit(ind + 'c.branchBackupValid = true; c.branchBackupReg = %d; c.branchBackupValue = bk;' % st.backup)
        else:
            self.emit(ind + 'c.branchBackupValid = false; c.branchBackupReg = 0; c.branchBackupValue = 0;')
        self.emit(ind + 'return true;')

    def generate(self):
        self.out_consts = []
        starts = {}
        for e in self.entries:
            starts[e] = self.node(e, State(), ('',))
        while self.work:
            name, pc, st, ctl = self.work.pop()
            self.gen_pair(name, pc, st, ctl)
            self.emit('    }')
        return starts


def load_profile(folder):
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
    return entries, jr, cycles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--prof', default=os.path.join('out', 'vu1prof'))
    ap.add_argument('--keys', default='')
    ap.add_argument('--out', default=os.path.join('ps2xRuntime', 'src', 'lib', 'vu', 'aot'))
    args = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import vu1reach
    entries, jr, cycles = load_profile(args.prof)
    groups = collections.defaultdict(list)
    reach = {}
    for h in entries:
        code = open(os.path.join(args.prof, 'code_%s.bin' % h), 'rb').read()
        pcs, key = vu1reach.program_of(code, entries[h].keys(), jr[h])
        groups[key].append(h)
        reach[h] = pcs
    wanted = [k for k in args.keys.split(',') if k] or sorted(groups, key=lambda k: -sum(cycles[h] for h in groups[k]))
    os.makedirs(args.out, exist_ok=True)
    table = []
    for key in wanted:
        hs = sorted(groups[key], key=lambda h: -cycles[h])
        h = hs[0]
        code = open(os.path.join(args.prof, 'code_%s.bin' % h), 'rb').read()
        dec = os.path.join(args.prof, 'decode_%s.txt' % h)
        if not os.path.exists(dec):
            print('%s: no decode table for image %s, skipped' % (key, h))
            continue
        pairs = {}
        for line in open(dec):
            if line.startswith('#'):
                continue
            pr = Pair(line)
            pairs[pr.pc] = pr
        ent = set()
        jrt = collections.defaultdict(set)
        for hh in hs:
            ent |= set(entries[hh])
            for site, ts in jr[hh].items():
                jrt[site] |= ts
        g = Gen(key, pairs, ent, jrt)
        starts = g.generate()
        fn = 'prog_%s' % key
        lines = ['// Generated by tools/vu1aot/vu1gen.py from out/vu1prof/code_%s.bin. Do not edit.' % h,
                 '// Program %s: %d pairs, %d nodes, entries %s.' % (key, len(reach[h]), len(g.nodes), ' '.join('%04x' % e for e in sorted(ent))),
                 '#include "../vu1aot_ops.h"', '', 'namespace vu1aot', '{', 'namespace', '{']
        lines += ['    ' + x for x in g.out_consts]
        lines += ['}', '', 'bool %s(Ctx &c, uint32_t entry)' % fn, '{',
                  '    VU1State &s = c.s;', '    FlagResult fr;', '    bool clipQ = false;', '    uint32_t clipV = 0;',
                  '    int32_t bk = 0;', '    bool br = false;', '    uint32_t jt = 0;',
                  '    (void)clipQ; (void)clipV; (void)bk; (void)br; (void)jt;', '    switch (entry)', '    {']
        for e, n in sorted(starts.items()):
            lines.append('    case 0x%x: goto %s;' % (e, n))
        lines += ['    default: return false;', '    }']
        lines += g.out
        lines += ['}', '}', '']
        open(os.path.join(args.out, '%s.cpp' % fn), 'w').write('\n'.join(lines))
        table.append((key, fn, sorted(reach[h]), code, sorted(ent)))
        print('%s: image %s, %d pairs, %d nodes, %d lines, bail sites %s' % (key, h, len(reach[h]), len(g.nodes), len(lines), dict(g.bails)))
    tl = ['// Generated by tools/vu1aot/vu1gen.py. Do not edit. The table of translated VU1 programs.',
          '#include "../vu1aot.h"', '', 'namespace vu1aot', '{']
    for key, fn, pcs, code, ent in table:
        tl.append('bool %s(Ctx &c, uint32_t entry);' % fn)
    tl += ['namespace', '{']
    for key, fn, pcs, code, ent in table:
        tl.append('const uint32_t kPcs_%s[] = {%s};' % (key, ', '.join('0x%x' % pc for pc in pcs)))
        tl.append('const uint64_t kWords_%s[] = {%s};' % (key, ', '.join('0x%016xull' % int.from_bytes(code[pc:pc + 8], 'little') for pc in pcs)))
        tl.append('const uint32_t kEntries_%s[] = {%s};' % (key, ', '.join('0x%x' % e for e in ent)))
    tl += ['}', '']
    if table:
        tl.append('const Program kPrograms[] = {')
        for key, fn, pcs, code, ent in table:
            tl.append('    {"%s", kPcs_%s, kWords_%s, %d, kEntries_%s, %d, %s},' % (key, key, key, len(pcs), key, len(ent), fn))
        tl += ['};', 'const uint32_t kProgramCount = %d;' % len(table)]
    else:
        tl += ['const Program kPrograms[1] = {};', 'const uint32_t kProgramCount = 0;']
    tl += ['}', '']
    open(os.path.join(args.out, 'aot_programs.cpp'), 'w').write('\n'.join(tl))


if __name__ == '__main__':
    main()
