"""Run a program under a minimal debugger and print the stack of the thread that crashes.

For crashes no in-process handler sees (heap corruption 0xC0000374, fail-fast 0xC0000409) on a machine without
cdb / WinDbg. Usage: python tools/crashstack.py <exe> [args...]   (the environment is passed on).
Prints, for the faulting thread, RIP and every stack slot that points into a loaded module, with its symbol
(dbghelp, PDB next to the exe): return addresses, plus some stale ones. Windows x64 only.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbghelp = ctypes.WinDLL('dbghelp', use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 0x2
EXCEPTION_DEBUG_EVENT, CREATE_PROCESS_DEBUG_EVENT, EXIT_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT = 1, 3, 5, 6
DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001
CONTEXT_FULL = 0x10000B


class STARTUPINFO(ctypes.Structure):
    _fields_ = [('cb', wt.DWORD), ('r1', wt.LPWSTR), ('desktop', wt.LPWSTR), ('title', wt.LPWSTR),
                ('x', wt.DWORD), ('y', wt.DWORD), ('xs', wt.DWORD), ('ys', wt.DWORD), ('xc', wt.DWORD),
                ('yc', wt.DWORD), ('fill', wt.DWORD), ('flags', wt.DWORD), ('show', wt.WORD), ('r2', wt.WORD),
                ('r3', ctypes.c_void_p), ('hin', wt.HANDLE), ('hout', wt.HANDLE), ('herr', wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [('hProcess', wt.HANDLE), ('hThread', wt.HANDLE), ('pid', wt.DWORD), ('tid', wt.DWORD)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_ulonglong * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_ulonglong), ('Flags', wt.ULONG),
                ('Value', ctypes.c_ulonglong), ('Address', ctypes.c_ulonglong), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]


k32.OpenThread.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
                                ctypes.POINTER(SYMBOL_INFO)]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbghelp.SymRefreshModuleList.argtypes = [wt.HANDLE]


def symbol(process, address):
    info = SYMBOL_INFO()
    info.SizeOfStruct = 88
    info.MaxNameLen = 500
    disp = ctypes.c_ulonglong(0)
    if dbghelp.SymFromAddr(process, address, ctypes.byref(disp), ctypes.byref(info)):
        return '%s+0x%x' % (info.Name.decode('latin1'), disp.value)
    return None


def dump_thread(process, tid):
    thread = k32.OpenThread(0x1FFFFF, False, tid)
    buf = ctypes.create_string_buffer(1232 + 16)
    base = ctypes.addressof(buf)
    ctx = (base + 15) & ~15
    struct.pack_into('<I', buf, ctx - base + 0x30, CONTEXT_FULL)
    if not k32.GetThreadContext(wt.HANDLE(thread), ctypes.c_void_p(ctx)):
        print('  GetThreadContext failed', ctypes.get_last_error())
        return
    raw = ctypes.string_at(ctx, 1232)
    rsp, rip = struct.unpack_from('<Q', raw, 0x98)[0], struct.unpack_from('<Q', raw, 0xF8)[0]
    print('  thread %d rip %s (0x%x)' % (tid, symbol(process, rip), rip))
    stack = ctypes.create_string_buffer(0x6000)
    got = ctypes.c_size_t(0)
    k32.ReadProcessMemory(process, rsp, stack, len(stack), ctypes.byref(got))
    shown = 0
    for off in range(0, got.value - 7, 8):
        value = struct.unpack_from('<Q', stack.raw, off)[0]
        if value < 0x10000 or value > 0x7FFFFFFFFFFF:
            continue
        name = symbol(process, value)
        if name:
            print('    rsp+0x%04x  %s' % (off, name))
            shown += 1
            if shown >= 70:
                break


def main():
    cmd = ' '.join('"%s"' % a for a in sys.argv[1:])
    si = STARTUPINFO()
    si.cb = ctypes.sizeof(si)
    pi = PROCESS_INFORMATION()
    if not k32.CreateProcessW(None, ctypes.create_unicode_buffer(cmd), None, None, True, DEBUG_ONLY_THIS_PROCESS,
                              None, None, ctypes.byref(si), ctypes.byref(pi)):
        raise ctypes.WinError(ctypes.get_last_error())
    process = pi.hProcess
    dbghelp.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES
    event = ctypes.create_string_buffer(256)
    initialized = False
    first_break = True
    while True:
        if not k32.WaitForDebugEvent(event, 0xFFFFFFFF):
            break
        code, pid, tid = struct.unpack_from('<III', event.raw, 0)
        status = DBG_CONTINUE
        if code == CREATE_PROCESS_DEBUG_EVENT:
            pass
        elif code == EXCEPTION_DEBUG_EVENT:
            exc_code, = struct.unpack_from('<I', event.raw, 16)
            first_chance, = struct.unpack_from('<I', event.raw, 16 + 152)
            if exc_code == 0x80000003 and first_break:
                first_break = False  # the loader's initial breakpoint
            elif exc_code in (0x406D1388, 0xE06D7363) and first_chance:
                status = DBG_EXCEPTION_NOT_HANDLED  # thread names, C++ exceptions: the program handles them
            else:
                if not initialized:
                    dbghelp.SymInitializeW(process, None, True)
                    initialized = True
                else:
                    dbghelp.SymRefreshModuleList(process)
                print('[crashstack] exception 0x%08X (%s chance) in thread %d' %
                      (exc_code, 'first' if first_chance else 'second', tid))
                dump_thread(process, tid)
                sys.stdout.flush()
                if not first_chance or exc_code in (0xC0000374, 0xC0000409, 0xC0000005):
                    k32.TerminateProcess(process, 3)
                status = DBG_EXCEPTION_NOT_HANDLED
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            exit_code, = struct.unpack_from('<I', event.raw, 16)
            print('[crashstack] process exited with 0x%08X' % exit_code)
            k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
            break
        k32.ContinueDebugEvent(pid, tid, status)


if __name__ == '__main__':
    main()
