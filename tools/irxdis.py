"""Annotated disassembly of a PS2 IOP module (IRX, ELF32 little-endian MIPS R3000).

Usage (capstone must be importable, e.g. PYTHONPATH=<dir with capstone>):
    python tools/irxdis.py <module.irx> <out.asm>

What it adds over a plain disassembly:
  - import stubs (jr ra / addiu zero, zero, N after a 0x41E00000 table header) are named
    "<library>:<ordinal> <function>" and every jal to them shows that name;
  - functions are split at jal targets (fn_XXXXX labels) and listed with their callers;
  - lui/addiu and lui/ori pairs that point into .rodata/.data show the string found there;
  - jump tables are not decoded (jr through a register is marked "indirect").

The output is derived from the game's module: keep it out of git (write it under out/).
Function names of the Sony libraries come from the ps2sdk export lists; names marked "?" are uncertain.
"""
import struct
import sys

import capstone

IMPORT_NAMES = {
    'libsd': {2: 'sceSdQuit', 4: 'sceSdInit', 5: 'sceSdSetParam', 6: 'sceSdGetParam', 7: 'sceSdSetSwitch',
              8: 'sceSdGetSwitch', 9: 'sceSdSetAddr', 10: 'sceSdGetAddr', 11: 'sceSdSetCoreAttr',
              12: 'sceSdGetCoreAttr', 13: 'sceSdNote2Pitch', 14: 'sceSdPitch2Note', 15: 'sceSdProcBatch',
              16: 'sceSdProcBatchEx', 17: 'sceSdVoiceTrans', 18: 'sceSdBlockTrans', 19: 'sceSdVoiceTransStatus',
              20: 'sceSdBlockTransStatus', 21: 'sceSdSetTransCallback', 22: 'sceSdSetIRQCallback',
              23: 'sceSdSetEffectAttr', 24: 'sceSdGetEffectAttr', 25: 'sceSdClearEffectWorkArea',
              26: 'sceSdSetTransIntrHandler', 27: 'sceSdSetSpu2IntrHandler'},
    'cdvdman': {4: 'sceCdInit', 5: 'sceCdStandby', 6: 'sceCdRead', 7: 'sceCdSeek', 8: 'sceCdGetError',
                9: 'sceCdGetToc', 10: 'sceCdSearchFile', 11: 'sceCdSync', 12: 'sceCdGetDiskType',
                13: 'sceCdDiskReady', 14: 'sceCdTrayReq', 15: 'sceCdStop', 37: 'sceCdCallback?',
                39: 'sceCdStInit?'},
    'sysmem': {4: 'AllocSysMemory', 5: 'FreeSysMemory', 6: 'QueryMemSize', 7: 'QueryMaxFreeMemSize',
               8: 'QueryTotalFreeMemSize'},
    'intrman': {4: 'RegisterIntrHandler', 5: 'ReleaseIntrHandler', 6: 'EnableIntr', 7: 'DisableIntr',
                8: 'CpuDisableIntr', 9: 'CpuEnableIntr', 17: 'CpuSuspendIntr', 18: 'CpuResumeIntr'},
    'loadcore': {5: 'FlushDcache?', 6: 'RegisterLibraryEntries'},
    'sifcmd': {4: 'sceSifInitCmd', 10: 'sceSifAddCmdHandler', 12: 'sceSifSendCmd', 13: 'isceSifSendCmd',
               14: 'sceSifInitRpc', 15: 'sceSifBindRpc', 16: 'sceSifCallRpc', 17: 'sceSifRegisterRpc',
               18: 'sceSifCheckStatRpc', 19: 'sceSifSetRpcQueue', 20: 'sceSifGetNextRequest',
               21: 'sceSifExecRequest', 22: 'sceSifRpcLoop'},
    'stdio': {4: 'printf'},
    'sysclib': {8: 'setjmp?', 11: 'memcmp?', 12: 'memcpy', 13: 'memmove?', 14: 'memset'},
    'thbase': {4: 'CreateThread', 5: 'DeleteThread', 6: 'StartThread', 7: 'StartThreadArgs', 8: 'ExitThread',
               14: 'ChangeThreadPriority', 18: 'ReleaseWaitThread', 20: 'GetThreadId', 22: 'iReferThreadStatus?',
               23: 'SleepThread', 24: 'WakeupThread', 25: 'iWakeupThread', 32: 'DelayThread',
               33: 'GetSystemTime', 34: 'SetAlarm', 35: 'iSetAlarm', 36: 'CancelAlarm', 37: 'iCancelAlarm',
               38: 'USec2SysClock', 39: 'SysClock2USec'},
    'thsemap': {4: 'CreateSema', 5: 'DeleteSema', 6: 'SignalSema', 7: 'iSignalSema', 8: 'WaitSema',
                9: 'PollSema'},
}


def main():
    data = open(sys.argv[1], 'rb').read()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    sections = {}
    secs = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    strtab_off = secs[shstrndx][4]
    for s in secs:
        end = data.index(b'\0', strtab_off + s[0])
        sections[data[strtab_off + s[0]:end].decode()] = s
    text = sections['.text']
    text_addr, text_off, text_size = text[3], text[4], text[5]
    code = data[text_off:text_off + text_size]

    def mem_bytes(addr, size):
        for name in ('.text', '.rodata', '.data'):
            s = sections.get(name)
            if s and s[3] <= addr < s[3] + s[5]:
                o = s[4] + addr - s[3]
                return data[o:o + size]
        return None

    def string_at(addr):
        b = mem_bytes(addr, 96)
        if not b:
            return None
        end = b.find(b'\0')
        if end < 4:
            return None
        s = b[:end]
        if all(32 <= c < 127 or c in (9, 10) for c in s):
            return s.decode()
        return None

    # Import stubs.
    names = {}
    pos = 0
    while True:
        pos = code.find(struct.pack('<I', 0x41E00000), pos)
        if pos < 0:
            break
        lib = code[pos + 12:pos + 20].rstrip(b'\0').decode('latin1')
        p = pos + 20
        while p + 8 <= len(code):
            jr, li = struct.unpack_from('<II', code, p)
            if jr != 0x03E00008:
                break
            ordinal = li & 0xFFFF
            fn = IMPORT_NAMES.get(lib, {}).get(ordinal, '')
            names[text_addr + p] = '%s:%d %s' % (lib, ordinal, fn)
            p += 8
        pos = p

    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
    insns = list(md.disasm(code, text_addr))

    # Function starts: jal targets (+ module entry at 0). Callers per target.
    callers = {}
    for ins in insns:
        if ins.mnemonic == 'jal':
            target = int(ins.op_str, 16)
            callers.setdefault(target, []).append(ins.address)
    starts = set(callers) | {text_addr}

    out = open(sys.argv[2], 'w', encoding='utf-8')
    out.write('; %s  .text %05x-%05x\n' % (sys.argv[1], text_addr, text_addr + text_size))
    hi = {}  # register -> lui value
    for ins in insns:
        a = ins.address
        if a in names:
            out.write('\n; ---- import stub %s\n' % names[a])
        elif a in starts:
            calls = callers.get(a, [])
            out.write('\n; ==== fn_%05x  (%d callers: %s)\n' % (a, len(calls), ' '.join('%05x' % c for c in calls[:12])))
            hi.clear()
        comment = ''
        ops = ins.op_str
        if ins.mnemonic == 'jal':
            target = int(ops, 16)
            comment = names.get(target, 'fn_%05x' % target)
        elif ins.mnemonic == 'lui':
            reg, imm = ops.split(', ')
            hi[reg] = int(imm, 0) << 16
        elif ins.mnemonic in ('addiu', 'ori') and ops.count(',') == 2:
            dst, src, imm = ops.split(', ')
            if src in hi:
                value = int(imm, 0)  # capstone already prints addiu immediates signed
                if ins.mnemonic == 'addiu' and 0x8000 <= value <= 0xFFFF:
                    value -= 0x10000
                full = (hi[src] + value) & 0xFFFFFFFF if ins.mnemonic == 'addiu' else hi[src] | value
                s = string_at(full)
                comment = '= 0x%x' % full + (' "%s"' % s.replace('\n', '\\n')[:70] if s else '')
        elif ins.mnemonic == 'jr' and ops != '$ra':
            comment = 'indirect'
        out.write('  %05x: %08x  %-7s %-28s%s\n' % (a, struct.unpack_from('<I', ins.bytes)[0], ins.mnemonic, ops,
                                                   ('; ' + comment) if comment else ''))
    out.close()
    print('%d instructions, %d functions, %d import stubs -> %s' % (len(insns), len(starts), len(names), sys.argv[2]))


if __name__ == '__main__':
    main()
