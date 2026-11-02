// Unattended runs: scripted pad input, a fixed frame budget and a hang watchdog.
// Everything is driven by environment variables so a normal run behaves exactly as before:
//   PS2X_TEST_INPUT    pad script, "tick:button[:holdTicks],..." e.g. "600:start:10,1500:cross"
//                      (tick = window frame, ~60 per second; buttons: start select cross circle square
//                      triangle up down left right l1 r1 l2 r2 l3 r3; holdTicks defaults to 8)
//   PS2X_TEST_FRAMES   stop the run after this many window frames
//   PS2X_WATCHDOG_SEC  seconds without a window frame before the native stacks of all threads are
//                      written to stderr (default 15, 0 = off). The window thread drawing a frame is
//                      the heartbeat, so a static "PRESS START" screen does not count as a hang.
//   PS2X_TEST_MODE=1   the watchdog also ends the process with exit code 3 after the dump
//   PS2X_TEST_SHOTS    window frames at which the picture shown in the window is saved as
//                      out/test_shot_<tick>.bmp, e.g. "500,1500" (works with both GS renderers)
//   PS2X_DETERMINISTIC=1  guest time comes only from guest cycles (EeScheduler, IOP, GS signals read
//                      it too). Here: the pad script, frame budget and shots count *guest VSyncs*
//                      instead of window frames, and every 60 VSyncs a "[det] vsync=N cycle=C ee=H"
//                      line (EE RAM hash) is logged. Two runs with the same input must print the same
//                      lines; the first differing line shows when they split.
// Only small extern "C" functions cross into the rest of the runtime (see CLAUDE.md, header rule).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

extern "C" void ps2xDumpCrashContext(); // ps2_runtime.cpp: last guest function, dispatch history
extern "C" void ps2xSpu2DebugDump();    // ps2xIOP iop_memory.cpp: SPU2 mixer state

namespace
{
    struct PadEvent
    {
        uint64_t tick = 0;
        uint64_t hold = 8;
        uint16_t mask = 0;
    };

    std::vector<PadEvent> g_padScript;
    std::vector<uint64_t> g_shotTicks;
    size_t g_nextShot = 0;
    std::atomic<uint64_t> g_tick{0};
    std::atomic<uint64_t> g_guestVsync{0};
    std::atomic<int64_t> g_lastTickMs{0};
    std::atomic<bool> g_exiting{false};
    uint64_t g_frameBudget = 0;
    int g_watchdogSec = 15;
    bool g_testMode = false;

    bool deterministicEnv()
    {
        const char *value = std::getenv("PS2X_DETERMINISTIC");
        return value && std::atoi(value) != 0;
    }

    // Window frames normally; guest VSyncs in deterministic mode.
    uint64_t scriptClock()
    {
        static const bool deterministic = deterministicEnv();
        return deterministic ? g_guestVsync.load(std::memory_order_relaxed) : g_tick.load(std::memory_order_relaxed);
    }

    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint16_t buttonMask(const std::string &name)
    {
        // Same bit order as Kernel/Stubs/Pad.cpp (libpad's active-low button word).
        static const char *const names[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                              "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
        for (int i = 0; i < 16; ++i)
        {
            if (name == names[i])
                return static_cast<uint16_t>(1u << i);
        }
        return 0;
    }

    void parsePadScript(const char *text)
    {
        std::string all(text);
        size_t pos = 0;
        while (pos < all.size())
        {
            size_t end = all.find(',', pos);
            if (end == std::string::npos)
                end = all.size();
            const std::string item = all.substr(pos, end - pos);
            pos = end + 1;
            const size_t c1 = item.find(':');
            if (c1 == std::string::npos)
                continue;
            const size_t c2 = item.find(':', c1 + 1);
            PadEvent ev;
            ev.tick = std::strtoull(item.substr(0, c1).c_str(), nullptr, 10);
            ev.mask = buttonMask(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
            if (c2 != std::string::npos)
                ev.hold = std::strtoull(item.substr(c2 + 1).c_str(), nullptr, 10);
            if (ev.mask == 0)
            {
                std::fprintf(stderr, "[test] unknown button in PS2X_TEST_INPUT item '%s'\n", item.c_str());
                continue;
            }
            g_padScript.push_back(ev);
        }
    }

#if defined(_WIN32)
    // Walks one suspended thread's native stack with the x64 unwind tables only. No heap, no locks
    // that another (suspended) thread could hold: symbol names are looked up after every thread runs again.
    size_t walkStack(CONTEXT ctx, DWORD64 *frames, size_t maxFrames)
    {
        size_t count = 0;
        while (count < maxFrames && ctx.Rip != 0)
        {
            frames[count++] = ctx.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!entry)
            {
                // Leaf function: the return address is on top of the stack.
                if (ctx.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void *>(ctx.Rsp), sizeof(DWORD64)))
                    break;
                ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                ctx.Rsp += sizeof(DWORD64);
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisher, nullptr);
        }
        return count;
    }

    size_t captureStack(HANDLE thread, DWORD64 *frames, size_t maxFrames)
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &ctx))
            return 0;
        return walkStack(ctx, frames, maxFrames);
    }

    // One line per frame: module!function+offset (file:line). SymInitialize must have been called.
    void printFrames(const char *prefix, const DWORD64 *frames, size_t count)
    {
        HANDLE process = GetCurrentProcess();
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
        for (size_t f = 0; f < count; ++f)
        {
            const DWORD64 addr = frames[f];
            char module[MAX_PATH] = "?";
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(addr), &mod))
            {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(mod, path, MAX_PATH);
                const char *slash = std::strrchr(path, '\\');
                std::snprintf(module, sizeof(module), "%s", slash ? slash + 1 : path);
            }
            auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
            std::memset(symBuf, 0, sizeof(symBuf));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            const bool haveSym = SymFromAddr(process, addr, &disp, sym) != FALSE;
            const bool haveLine = SymGetLineFromAddr64(process, addr, &lineDisp, &line) != FALSE;
            std::fprintf(stderr, "%s   #%02zu %s!%s+0x%llx", prefix, f, module, haveSym ? sym->Name : "?",
                         static_cast<unsigned long long>(haveSym ? disp : addr - reinterpret_cast<DWORD64>(mod)));
            if (haveLine)
                std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
            std::fprintf(stderr, "\n");
        }
    }

    struct ThreadStack
    {
        DWORD id = 0;
        std::wstring name;
        DWORD64 frames[48] = {};
        size_t count = 0;
    };

    void dumpAllStacks(const char *reason)
    {
        const DWORD self = GetCurrentThreadId();
        std::vector<DWORD> ids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te = {};
            te.dwSize = sizeof(te);
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self)
                    ids.push_back(te.th32ThreadID);
            }
            CloseHandle(snap);
        }

        std::vector<ThreadStack> stacks(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            stacks[i].id = ids[i];
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
            if (!h)
                continue;
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc)
            {
                stacks[i].name = desc;
                LocalFree(desc);
            }
            if (SuspendThread(h) != static_cast<DWORD>(-1))
            {
                stacks[i].count = captureStack(h, stacks[i].frames, 48);
                ResumeThread(h);
            }
            CloseHandle(h);
        }

        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
        std::fprintf(stderr, "[watchdog] %s: native stacks of %zu threads\n", reason, stacks.size());
        for (const ThreadStack &t : stacks)
        {
            std::fprintf(stderr, "[watchdog] thread %lu '%ls'\n", t.id, t.name.c_str());
            printFrames("[watchdog]", t.frames, t.count);
        }
        SymCleanup(process);
        std::fflush(stderr);
    }

    // First-chance crash report. The unhandled-exception filter in main.cpp can be replaced by DLLs loaded
    // later (overlays hook it), so a vectored handler reports the first few faults, with their stacks.
    LONG CALLBACK crashVectoredHandler(PEXCEPTION_POINTERS info)
    {
        static std::atomic<int> reported{0};
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        const bool fatalKind = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                               code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                               code == EXCEPTION_PRIV_INSTRUCTION;
        // Any module: a fault inside a DLL (C runtime, driver) called from our code counts too. Drivers can
        // raise and handle their own first-chance faults, so only the first few are reported.
        if (!fatalKind || reported.fetch_add(1) >= 5)
            return EXCEPTION_CONTINUE_SEARCH;
        const auto addr = reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress);
        std::fprintf(stderr, "\n[crash] first-chance exception 0x%08lx at 0x%llx, thread %lu", code,
                     static_cast<unsigned long long>(addr), GetCurrentThreadId());
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        {
            const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
            std::fprintf(stderr, " %s address 0x%llx", kind == 0 ? "reading" : (kind == 1 ? "writing" : "executing"),
                         static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
        }
        std::fprintf(stderr, "\n");
        DWORD64 frames[64] = {};
        const size_t count = walkStack(*info->ContextRecord, frames, 64);
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
        printFrames("[crash]", frames, count);
        SymCleanup(process);
        std::fflush(stderr);
        ps2xDumpCrashContext();
        std::fflush(stdout);
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
#else
    void dumpAllStacks(const char *reason)
    {
        std::fprintf(stderr, "[watchdog] %s (native stack dump is Windows-only)\n", reason);
        std::fflush(stderr);
    }
#endif

    void watchdogLoop()
    {
        bool dumped = false;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const int64_t idleMs = nowMs() - g_lastTickMs.load(std::memory_order_relaxed);
            const bool exiting = g_exiting.load(std::memory_order_relaxed);
            // While shutting down, give the threads 10 s to finish before calling it a hang too.
            const int64_t limitMs = exiting ? 10000 : static_cast<int64_t>(g_watchdogSec) * 1000;
            if (idleMs < limitMs)
            {
                dumped = false;
                continue;
            }
            if (!dumped)
            {
                char reason[128];
                std::snprintf(reason, sizeof(reason), "%s: no window frame for %lld ms at tick %llu",
                              exiting ? "HANG during shutdown" : "HANG", static_cast<long long>(idleMs),
                              static_cast<unsigned long long>(g_tick.load()));
                std::fprintf(stderr, "[test] WATCHDOG %s\n", reason);
                dumpAllStacks(reason);
                dumped = true;
            }
            if (g_testMode || exiting)
            {
                std::fprintf(stderr, "[test] exit code %d\n", exiting && !g_testMode ? 0 : 3);
                std::fflush(stderr);
                std::fflush(stdout);
#if defined(_WIN32)
                TerminateProcess(GetCurrentProcess(), exiting && !g_testMode ? 0 : 3);
#endif
                std::_Exit(exiting && !g_testMode ? 0 : 3);
            }
        }
    }
}

// Window thread, once before the main loop.
extern "C" void ps2xTestHarnessStart()
{
    if (const char *input = std::getenv("PS2X_TEST_INPUT"))
        parsePadScript(input);
    if (const char *frames = std::getenv("PS2X_TEST_FRAMES"))
        g_frameBudget = std::strtoull(frames, nullptr, 10);
    if (const char *sec = std::getenv("PS2X_WATCHDOG_SEC"))
        g_watchdogSec = std::atoi(sec);
    if (const char *shots = std::getenv("PS2X_TEST_SHOTS"))
    {
        for (const char *p = shots; *p;)
        {
            char *end = nullptr;
            const uint64_t tick = std::strtoull(p, &end, 10);
            if (end == p)
                break;
            g_shotTicks.push_back(tick);
            p = (*end == ',') ? end + 1 : end;
        }
        std::sort(g_shotTicks.begin(), g_shotTicks.end());
    }
    if (const char *mode = std::getenv("PS2X_TEST_MODE"))
        g_testMode = std::atoi(mode) != 0;
    g_lastTickMs.store(nowMs());
    if (g_testMode || !g_padScript.empty() || g_frameBudget != 0 || deterministicEnv())
        std::fprintf(stderr, "[test] harness: %zu pad events, frame budget %llu, watchdog %d s, test mode %d, deterministic %d\n",
                     g_padScript.size(), static_cast<unsigned long long>(g_frameBudget), g_watchdogSec, g_testMode ? 1 : 0,
                     deterministicEnv() ? 1 : 0);
    if (g_watchdogSec > 0)
        std::thread(watchdogLoop).detach();
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, crashVectoredHandler);
#endif
}

// Window thread, once per drawn frame. Returns true when the frame budget is used up.
extern "C" bool ps2xTestHarnessTick(uint64_t tick)
{
    g_tick.store(tick, std::memory_order_relaxed);
    g_lastTickMs.store(nowMs(), std::memory_order_relaxed);
    const uint64_t now = scriptClock();
    if (g_frameBudget != 0 && now >= g_frameBudget)
    {
        if (!g_exiting.exchange(true))
            std::fprintf(stderr, "[test] frame budget reached at %s %llu (window tick %llu)\n",
                         deterministicEnv() ? "guest vsync" : "tick", static_cast<unsigned long long>(now),
                         static_cast<unsigned long long>(tick));
        return true;
    }
    return false;
}

// Window thread, while shutting down: keeps the watchdog's shutdown timer meaningful.
extern "C" void ps2xTestHarnessExiting()
{
    g_exiting.store(true);
    g_lastTickMs.store(nowMs(), std::memory_order_relaxed);
}

// EE thread (scePadRead): buttons the script holds down right now, active-high.
extern "C" uint16_t ps2xTestPadButtons(int port)
{
    if (port != 0 || g_padScript.empty())
        return 0;
    const uint64_t tick = scriptClock();
    uint16_t mask = 0;
    for (const PadEvent &ev : g_padScript)
    {
        if (tick >= ev.tick && tick < ev.tick + ev.hold)
            mask = static_cast<uint16_t>(mask | ev.mask);
    }
    return mask;
}

// Window thread, whenever a new picture goes to the window (RGBA, rowPixels per row). Saves the
// requested checkpoint pictures and logs simple numbers about them for the test report.
extern "C" void ps2xTestHarnessFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t rowPixels)
{
    if (g_nextShot >= g_shotTicks.size() || !rgba || width == 0 || height == 0)
        return;
    const uint64_t tick = scriptClock();
    if (tick < g_shotTicks[g_nextShot])
        return;
    while (g_nextShot < g_shotTicks.size() && g_shotTicks[g_nextShot] <= tick)
        ++g_nextShot;

    // Luma mean / variance: a black or flat picture is the first sign of a broken renderer.
    double sum = 0.0;
    double sumSq = 0.0;
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t *row = rgba + static_cast<size_t>(y) * rowPixels * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const double luma = 0.299 * row[x * 4] + 0.587 * row[x * 4 + 1] + 0.114 * row[x * 4 + 2];
            sum += luma;
            sumSq += luma * luma;
        }
    }
    const double count = static_cast<double>(width) * height;
    const double mean = sum / count;
    const double variance = sumSq / count - mean * mean;

    char path[96];
    std::snprintf(path, sizeof(path), "out/test_shot_%llu.bmp", static_cast<unsigned long long>(tick));
    if (FILE *f = std::fopen(path, "wb"))
    {
        const uint32_t imageBytes = width * height * 4u;
        uint8_t header[54] = {};
        const auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i)); };
        header[0] = 'B';
        header[1] = 'M';
        put32(2, 54u + imageBytes);
        put32(10, 54u);
        put32(14, 40u);
        put32(18, width);
        put32(22, static_cast<uint32_t>(-static_cast<int32_t>(height))); // top-down
        header[26] = 1;
        header[28] = 32;
        put32(34, imageBytes);
        std::fwrite(header, 1u, sizeof(header), f);
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * 4u);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = rgba + static_cast<size_t>(y) * rowPixels * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                bgra[x * 4] = row[x * 4 + 2];
                bgra[x * 4 + 1] = row[x * 4 + 1];
                bgra[x * 4 + 2] = row[x * 4];
                bgra[x * 4 + 3] = 0xFF;
            }
            std::fwrite(bgra.data(), 1u, bgra.size(), f);
        }
        std::fclose(f);
    }
    std::fprintf(stderr, "[test] shot tick=%llu file=%s size=%ux%u mean=%.1f variance=%.1f\n",
                 static_cast<unsigned long long>(tick), path, width, height, mean, variance);
}

// Test mode: real keyboard / gamepad input is ignored (Kernel/Stubs/Pad.cpp), only the script counts.
extern "C" bool ps2xTestMode()
{
    static const bool on = []()
    {
        const char *mode = std::getenv("PS2X_TEST_MODE");
        return mode && std::atoi(mode) != 0;
    }();
    return on;
}

extern "C" bool ps2xDeterministic()
{
    static const bool on = deterministicEnv();
    return on;
}

// EE thread, at every VBlank start (EeScheduler). Feeds the guest clock used in deterministic mode and
// logs the state fingerprint used to compare two runs.
extern "C" void ps2xTestOnGuestVSync(uint64_t vsync, uint64_t eeCycle, const uint8_t *rdram, uint32_t ramBytes)
{
    g_guestVsync.store(vsync, std::memory_order_relaxed);

    // PS2X_SPU2_DUMP_AT="1200,2300": full SPU2 state at those guest VSyncs (this thread also runs the IOP).
    static const std::vector<uint64_t> spu2DumpAt = []()
    {
        std::vector<uint64_t> at;
        if (const char *list = std::getenv("PS2X_SPU2_DUMP_AT"))
        {
            for (const char *p = list; *p;)
            {
                char *end = nullptr;
                const uint64_t value = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                at.push_back(value);
                p = (*end == ',') ? end + 1 : end;
            }
        }
        return at;
    }();
    if (std::find(spu2DumpAt.begin(), spu2DumpAt.end(), vsync) != spu2DumpAt.end())
    {
        std::fprintf(stderr, "[spu2-dump] guest vsync %llu\n", static_cast<unsigned long long>(vsync));
        ps2xSpu2DebugDump();
    }

    if (!ps2xDeterministic() || !rdram || (vsync % 60u) != 0u)
        return;
    // 64-bit multiply-xor over the whole of EE RAM: ~5 ms for 32 MiB, once a second.
    uint64_t hash = 0xcbf29ce484222325ull;
    const size_t words = ramBytes / sizeof(uint64_t);
    for (size_t i = 0; i < words; ++i)
    {
        uint64_t word;
        std::memcpy(&word, rdram + i * sizeof(uint64_t), sizeof(word));
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    std::fprintf(stderr, "[det] vsync=%llu cycle=%llu ee=%016llx\n", static_cast<unsigned long long>(vsync),
                 static_cast<unsigned long long>(eeCycle), static_cast<unsigned long long>(hash));
}

// Calling thread (the game thread, at start): keeps 256 KiB of stack in reserve, so after a stack overflow the
// crash handler above still has room to print the report instead of faulting itself.
extern "C" void ps2xTestThreadStackGuarantee()
{
#if defined(_WIN32)
    ULONG guarantee = 256u * 1024u;
    SetThreadStackGuarantee(&guarantee);
#endif
}
