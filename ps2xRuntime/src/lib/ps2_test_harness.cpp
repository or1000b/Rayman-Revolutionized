// Unattended runs: scripted pad input, a fixed frame budget and a hang watchdog.
// Everything is driven by environment variables so a normal run behaves exactly as before:
//   PS2X_TEST_INPUT    pad script, "tick:button[:holdTicks],..." e.g. "600:start:10,1500:cross"; analog sticks at
//                      full deflection: lleft / lright / lup / ldown (left stick), rleft / rright / rup / rdown
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
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
extern "C" void ps2xRequestGsCapture(); // gs_cpu_backend.cpp: F11 capture (CPU renderer)
extern "C" uint32_t ps2xVBlankRateOverrideHz(); // EeScheduler.cpp (PS2X_FRAMERATE)

namespace
{
    struct PadEvent
    {
        uint64_t tick = 0;
        uint64_t hold = 8;
        uint16_t mask = 0;
        int8_t axis = -1; // analog stick: 0 rx, 1 ry, 2 lx, 3 ly (full deflection), -1 = a button
        uint8_t value = 0x80;
    };

    // Analog stick directions for the pad script: "lleft" / "lright" / "lup" / "ldown" (left stick) and the same
    // with "r" (right stick), held at full deflection.
    bool stickEvent(const std::string &name, PadEvent &ev)
    {
        static const struct
        {
            const char *name;
            int8_t axis;
            uint8_t value;
        } kSticks[] = {{"rleft", 0, 0x00}, {"rright", 0, 0xFF}, {"rup", 1, 0x00}, {"rdown", 1, 0xFF},
                       {"lleft", 2, 0x00}, {"lright", 2, 0xFF}, {"lup", 3, 0x00}, {"ldown", 3, 0xFF}};
        for (const auto &s : kSticks)
        {
            if (name == s.name)
            {
                ev.axis = s.axis;
                ev.value = s.value;
                return true;
            }
        }
        // Any position (recordings, PS2X_RECORD_INPUT): "rx=NN" / "ry=NN" / "lx=NN" / "ly=NN", NN in hex.
        static const char *const kAxes[4] = {"rx=", "ry=", "lx=", "ly="};
        for (int i = 0; i < 4; ++i)
        {
            if (name.size() > 3u && name.compare(0, 3, kAxes[i]) == 0)
            {
                ev.axis = static_cast<int8_t>(i);
                ev.value = static_cast<uint8_t>(std::strtoul(name.c_str() + 3, nullptr, 16) & 0xFFu);
                return true;
            }
        }
        return false;
    }

    std::vector<PadEvent> g_padScript;
    std::vector<uint64_t> g_shotTicks;
    size_t g_nextShot = 0;
    std::atomic<uint64_t> g_tick{0};
    std::atomic<uint64_t> g_guestVsync{0};
    std::atomic<int64_t> g_lastTickMs{0};
    std::atomic<bool> g_exiting{false};
    uint64_t g_frameBudget = 0;
    // Game-specific hook at every guest VBlank (main.cpp sets it): script time (see scriptClock), VSync, EE RAM.
    using GuestVsyncHook = void (*)(uint64_t scriptTime, uint64_t vsync, const uint8_t *rdram);
    GuestVsyncHook g_guestVsyncHook = nullptr;
    int g_watchdogSec = 15;
    bool g_testMode = false;

    bool deterministicEnv()
    {
        const char *value = std::getenv("PS2X_DETERMINISTIC");
        return value && std::atoi(value) != 0;
    }

    // Window frames normally; guest VSyncs in deterministic mode. With PS2X_FRAMERATE=N the VSyncs are scaled to
    // PAL fields (x 50 / N), so a scenario's ticks mean the same real time (20 ms each) at every frame rate.
    uint64_t scriptClock()
    {
        static const bool deterministic = deterministicEnv();
        static const uint32_t rateHz = ps2xVBlankRateOverrideHz();
        if (!deterministic)
            return g_tick.load(std::memory_order_relaxed);
        const uint64_t vsync = g_guestVsync.load(std::memory_order_relaxed);
        return rateHz != 0u ? vsync * 50u / rateHz : vsync;
    }

    // The clock of the pad script: script ticks (scriptClock), or guest VSyncs with PS2X_TEST_INPUT_CLOCK=vsync
    // (recordings: the game reads the pad at most once per VSync, so a replay gives every read the recorded state).
    uint64_t inputClock()
    {
        static const bool vsyncClock = []
        {
            const char *env = std::getenv("PS2X_TEST_INPUT_CLOCK");
            return env && std::strcmp(env, "vsync") == 0;
        }();
        return vsyncClock ? g_guestVsync.load(std::memory_order_relaxed) : scriptClock();
    }

    // ---- PS2X_RECORD_INPUT=<file>: the real pad as the game reads it, as a pad script on the VSync clock (run_tests
    // -Record: deterministic like a test run, so replaying the file with PS2X_TEST_INPUT_FILE and
    // PS2X_TEST_INPUT_CLOCK=vsync does what the player did).
    struct Recorder
    {
        std::mutex mutex;
        std::string path;
        std::string out;
        size_t items = 0;
        uint64_t pressedSince[16] = {};
        bool pressed[16] = {};
        uint8_t axis[4] = {0x80, 0x80, 0x80, 0x80};
        uint64_t axisSince[4] = {};
        bool finished = false;
    };
    Recorder g_recorder;

    void recorderEmit(uint64_t tick, const char *name, uint64_t hold)
    {
        char item[48];
        std::snprintf(item, sizeof(item), "%s%llu:%s:%llu", g_recorder.items ? "," : "", static_cast<unsigned long long>(tick),
                      name, static_cast<unsigned long long>(hold ? hold : 1u));
        g_recorder.out += item;
        ++g_recorder.items;
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
            const std::string name = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            ev.mask = buttonMask(name);
            if (c2 != std::string::npos)
                ev.hold = std::strtoull(item.substr(c2 + 1).c_str(), nullptr, 10);
            if (ev.mask == 0 && !stickEvent(name, ev))
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

    // PS2X_NATIVE_PROF_THREAD=GfxThread: the thread the native profiler samples (default GameThread).
    std::wstring profThreadName()
    {
        const char *env = std::getenv("PS2X_NATIVE_PROF_THREAD");
        const std::string name = (env && *env) ? env : "GameThread";
        return std::wstring(name.begin(), name.end());
    }

    // Native sampling profiler: PS2X_NATIVE_PROF="fromTick,toTick" samples the GameThread's native stack about every
    // millisecond between those window ticks, then prints `[nprof]` tables: functions by exclusive samples (the
    // function the thread was in) and by inclusive samples (anywhere on the stack). Bug hunt / performance tool.
    void nativeProfilerLoop(uint64_t fromTick, uint64_t toTick)
    {
        DWORD gameThreadId = 0;
        while (gameThreadId == 0 && !g_exiting.load())
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te = {};
                te.dwSize = sizeof(te);
                for (BOOL ok = Thread32First(snap, &te); ok && gameThreadId == 0; ok = Thread32Next(snap, &te))
                {
                    if (te.th32OwnerProcessID != GetCurrentProcessId())
                        continue;
                    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!h)
                        continue;
                    PWSTR desc = nullptr;
                    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc)
                    {
                        if (std::wstring(desc) == profThreadName())
                            gameThreadId = te.th32ThreadID;
                        LocalFree(desc);
                    }
                    CloseHandle(h);
                }
                CloseHandle(snap);
            }
            if (gameThreadId == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, gameThreadId);
        if (!thread)
            return;
        while (g_tick.load() < fromTick && !g_exiting.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::fprintf(stderr, "[nprof] sampling GameThread %lu from tick %llu\n", gameThreadId,
                     static_cast<unsigned long long>(g_tick.load()));
        std::vector<std::vector<DWORD64>> samples;
        samples.reserve(200000);
        while (g_tick.load() < toTick && !g_exiting.load())
        {
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                DWORD64 frames[24] = {};
                const size_t n = captureStack(thread, frames, 24);
                ResumeThread(thread);
                if (n != 0)
                    samples.emplace_back(frames, frames + n);
            }
            // Spin for ~1 ms: Sleep's default resolution (~15 ms) would give too few samples.
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(1000);
            while (std::chrono::steady_clock::now() < until)
                YieldProcessor();
        }
        CloseHandle(thread);

        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
        std::unordered_map<DWORD64, std::string> names;
        auto nameOf = [&](DWORD64 addr) -> const std::string & {
            auto it = names.find(addr);
            if (it != names.end())
                return it->second;
            auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
            std::memset(symBuf, 0, sizeof(symBuf));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            std::string name = SymFromAddr(process, addr, &disp, sym) ? std::string(sym->Name) : std::string("?");
            return names.emplace(addr, std::move(name)).first->second;
        };
        std::unordered_map<std::string, uint32_t> exclusive, inclusive;
        for (const auto &s : samples)
        {
            ++exclusive[nameOf(s[0])];
            std::vector<const std::string *> seen;
            for (DWORD64 a : s)
            {
                const std::string &n = nameOf(a);
                if (std::find(seen.begin(), seen.end(), &n) == seen.end())
                {
                    seen.push_back(&n);
                    ++inclusive[n];
                }
            }
        }
        // Exclusive samples by source line (file:line of the sampled instruction; inlined code reports the line of the
        // inlined function), to see what is hot inside one big function.
        std::unordered_map<std::string, uint32_t> lines;
        {
            std::unordered_map<DWORD64, std::string> lineNames;
            for (const auto &s : samples)
            {
                auto it = lineNames.find(s[0]);
                if (it == lineNames.end())
                {
                    IMAGEHLP_LINE64 line = {};
                    line.SizeOfStruct = sizeof(line);
                    DWORD lineDisp = 0;
                    std::string where = "?";
                    if (SymGetLineFromAddr64(process, s[0], &lineDisp, &line) && line.FileName)
                    {
                        const char *file = line.FileName;
                        for (const char *p = line.FileName; *p; ++p)
                            if (*p == '\\' || *p == '/')
                                file = p + 1;
                        where = std::string(file) + ":" + std::to_string(line.LineNumber);
                    }
                    it = lineNames.emplace(s[0], where + "  " + nameOf(s[0])).first;
                }
                ++lines[it->second];
            }
        }
        // Where the top exclusive functions run and who calls them: the module of the sampled address (a DLL without
        // symbols reports its nearest export, which can be any function: the module tells what it really is), the
        // hottest address, and the first caller frames with another name.
        std::vector<std::string> callerLines;
        {
            std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> callers;
            std::unordered_map<std::string, std::unordered_map<DWORD64, uint32_t>> addresses;
            for (const auto &s : samples)
            {
                const std::string &leaf = nameOf(s[0]);
                ++addresses[leaf][s[0]];
                for (size_t i = 1; i < s.size(); ++i)
                    if (nameOf(s[i]) != leaf)
                    {
                        ++callers[leaf][nameOf(s[i])];
                        break;
                    }
            }
            std::vector<std::pair<std::string, uint32_t>> top(exclusive.begin(), exclusive.end());
            std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            for (size_t i = 0; i < top.size() && i < 10u; ++i)
            {
                const auto &adds = addresses[top[i].first];
                const auto hot = std::max_element(adds.begin(), adds.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
                IMAGEHLP_MODULE64 module = {};
                module.SizeOfStruct = sizeof(module);
                std::string where = "?";
                DWORD64 rva = 0;
                if (hot != adds.end() && SymGetModuleInfo64(process, hot->first, &module))
                {
                    where = module.ModuleName;
                    rva = hot->first - module.BaseOfImage;
                }
                char head[640];
                std::snprintf(head, sizeof(head), "%5.1f%%  %s  [%s, hottest +0x%llx: %.1f%%] called from:", 100.0 * top[i].second / std::max<size_t>(1, samples.size()),
                              top[i].first.c_str(), where.c_str(), static_cast<unsigned long long>(rva),
                              hot != adds.end() ? 100.0 * hot->second / std::max<size_t>(1, samples.size()) : 0.0);
                std::string line = head;
                std::vector<std::pair<std::string, uint32_t>> from(callers[top[i].first].begin(), callers[top[i].first].end());
                std::sort(from.begin(), from.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                for (size_t k = 0; k < from.size() && k < 4u; ++k)
                    line += " " + from[k].first + " (" + std::to_string(from[k].second * 100u / std::max<uint32_t>(1u, top[i].second)) + "%)";
                callerLines.push_back(line);
            }
        }
        // PS2X_NATIVE_PROF_CHAIN="malloc,Mtx_lock": for samples whose leaf name contains one of these, the most
        // common call chains (8 frames of other names), to find who calls a small helper that the one-level caller
        // list cannot explain (an allocation, a lock).
        std::vector<std::string> chainLines;
        if (const char *chainEnv = std::getenv("PS2X_NATIVE_PROF_CHAIN"))
        {
            std::vector<std::string> wanted;
            for (const char *p = chainEnv; *p;)
            {
                const char *end = std::strchr(p, ',');
                const size_t n = end ? static_cast<size_t>(end - p) : std::strlen(p);
                if (n)
                    wanted.emplace_back(p, n);
                p += n + (end ? 1u : 0u);
            }
            for (const std::string &want : wanted)
            {
                std::unordered_map<std::string, uint32_t> chains;
                uint32_t total = 0;
                for (const auto &s : samples)
                {
                    bool match = false;
                    // The leaf or one of the first frames (a helper called by the function in the name).
                    for (size_t i = 0; i < s.size() && i < 4u && !match; ++i)
                        match = nameOf(s[i]).find(want) != std::string::npos;
                    if (!match)
                        continue;
                    ++total;
                    std::string chain;
                    std::string last;
                    size_t frames = 0;
                    for (size_t i = 0; i < s.size() && frames < 8u; ++i)
                    {
                        const std::string &name = nameOf(s[i]);
                        if (name == last)
                            continue;
                        chain += (frames ? " < " : "") + name;
                        last = name;
                        ++frames;
                    }
                    ++chains[chain];
                }
                std::vector<std::pair<std::string, uint32_t>> rows(chains.begin(), chains.end());
                std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                char head[256];
                std::snprintf(head, sizeof(head), "chains of '%s' (%.1f%% of the samples):", want.c_str(),
                              100.0 * total / std::max<size_t>(1, samples.size()));
                chainLines.push_back(head);
                for (size_t k = 0; k < rows.size() && k < 10u; ++k)
                {
                    char pct[32];
                    std::snprintf(pct, sizeof(pct), "  %5.1f%%  ", 100.0 * rows[k].second / std::max<size_t>(1, samples.size()));
                    chainLines.push_back(pct + rows[k].first);
                }
            }
        }
        SymCleanup(process);
        auto print = [&](const char *title, const std::unordered_map<std::string, uint32_t> &table, size_t maxRows) {
            std::vector<std::pair<std::string, uint32_t>> rows(table.begin(), table.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            std::fprintf(stderr, "[nprof] %s (%zu samples):\n", title, samples.size());
            for (size_t i = 0; i < rows.size() && i < maxRows; ++i)
                std::fprintf(stderr, "[nprof]   %5.1f%%  %s\n", 100.0 * rows[i].second / std::max<size_t>(1, samples.size()),
                             rows[i].first.c_str());
        };
        print("exclusive", exclusive, 45);
        print("inclusive", inclusive, 45);
        print("exclusive by line", lines, 120);
        std::fprintf(stderr, "[nprof] top exclusive: module, hottest address, callers:\n");
        for (const std::string &line : callerLines)
            std::fprintf(stderr, "[nprof]   %s\n", line.c_str());
        for (const std::string &line : chainLines)
            std::fprintf(stderr, "[nprof]   %s\n", line.c_str());
        std::fflush(stderr);
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
    // A long script (a recording) from a file: PS2X_TEST_INPUT_FILE=<path> (same format, commas and line breaks).
    if (const char *file = std::getenv("PS2X_TEST_INPUT_FILE"))
    {
        if (FILE *f = std::fopen(file, "rb"))
        {
            std::string text;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1u, sizeof(buf), f)) > 0u)
                text.append(buf, n);
            std::fclose(f);
            for (char &c : text)
                if (c == '\n' || c == '\r')
                    c = ',';
            parsePadScript(text.c_str());
            std::fprintf(stderr, "[test] %zu pad events from %s\n", g_padScript.size(), file);
        }
        else
            std::fprintf(stderr, "[test] cannot read PS2X_TEST_INPUT_FILE %s\n", file);
    }
    if (const char *record = std::getenv("PS2X_RECORD_INPUT"))
    {
        g_recorder.path = record;
        std::fprintf(stderr, "[record] recording the pad to %s (VSync clock)\n", record);
    }
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
    if (const char *prof = std::getenv("PS2X_NATIVE_PROF"))
    {
        char *end = nullptr;
        const uint64_t from = std::strtoull(prof, &end, 10);
        const uint64_t to = (end && *end == ',') ? std::strtoull(end + 1, nullptr, 10) : from + 600u;
        std::thread(nativeProfilerLoop, from, to).detach();
    }
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
    if (!g_recorder.path.empty())
    {
        // Close what is still held and write the recording.
        std::lock_guard<std::mutex> lock(g_recorder.mutex);
        if (!g_recorder.finished)
        {
            g_recorder.finished = true;
            const uint64_t tick = g_guestVsync.load(std::memory_order_relaxed);
            static const char *const names[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                                  "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
            for (int i = 0; i < 16; ++i)
                if (g_recorder.pressed[i])
                    recorderEmit(g_recorder.pressedSince[i], names[i], tick - g_recorder.pressedSince[i]);
            static const char *const axes[4] = {"rx", "ry", "lx", "ly"};
            for (int i = 0; i < 4; ++i)
                if (g_recorder.axis[i] != 0x80u)
                {
                    char name[8];
                    std::snprintf(name, sizeof(name), "%s=%02x", axes[i], g_recorder.axis[i]);
                    recorderEmit(g_recorder.axisSince[i], name, tick - g_recorder.axisSince[i]);
                }
            if (FILE *f = std::fopen(g_recorder.path.c_str(), "wb"))
            {
                std::fwrite(g_recorder.out.data(), 1u, g_recorder.out.size(), f);
                std::fclose(f);
                std::fprintf(stderr, "[record] %zu pad events written to %s (last VSync %llu)\n", g_recorder.items,
                             g_recorder.path.c_str(), static_cast<unsigned long long>(tick));
            }
            else
                std::fprintf(stderr, "[record] cannot write %s\n", g_recorder.path.c_str());
        }
    }
}

// Pad.cpp (scePadRead, the real pad, before scripted input): with PS2X_RECORD_INPUT, what the game reads. buttons is
// libpad's active-low word, axes rx, ry, lx, ly. Sticks are kept in steps of 4 (0x80 within 8 = centre) so a resting
// stick does not fill the file.
extern "C" void ps2xTestRecordPad(int port, uint16_t buttons, const uint8_t axes[4])
{
    if (g_recorder.path.empty() || port != 0)
        return;
    std::lock_guard<std::mutex> lock(g_recorder.mutex);
    if (g_recorder.finished)
        return;
    const uint64_t tick = g_guestVsync.load(std::memory_order_relaxed);
    static const char *const names[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                          "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    for (int i = 0; i < 16; ++i)
    {
        const bool down = ((buttons >> i) & 1u) == 0u;
        if (down == g_recorder.pressed[i])
            continue;
        if (down)
            g_recorder.pressedSince[i] = tick;
        else
            recorderEmit(g_recorder.pressedSince[i], names[i], tick - g_recorder.pressedSince[i]);
        g_recorder.pressed[i] = down;
    }
    static const char *const axisNames[4] = {"rx", "ry", "lx", "ly"};
    for (int i = 0; i < 4; ++i)
    {
        int v = axes[i];
        if (v >= 0x78 && v <= 0x88)
            v = 0x80;
        else
            v = std::min(0xFF, ((v + 2) / 4) * 4);
        if (v == g_recorder.axis[i])
            continue;
        if (g_recorder.axis[i] != 0x80u)
        {
            char name[8];
            std::snprintf(name, sizeof(name), "%s=%02x", axisNames[i], g_recorder.axis[i]);
            recorderEmit(g_recorder.axisSince[i], name, tick - g_recorder.axisSince[i]);
        }
        g_recorder.axis[i] = static_cast<uint8_t>(v);
        g_recorder.axisSince[i] = tick;
    }
}

// EE thread (scePadRead): buttons the script holds down right now, active-high.
extern "C" uint16_t ps2xTestPadButtons(int port)
{
    if (port != 0 || g_padScript.empty())
        return 0;
    const uint64_t tick = inputClock();
    uint16_t mask = 0;
    for (const PadEvent &ev : g_padScript)
    {
        if (ev.axis < 0 && tick >= ev.tick && tick < ev.tick + ev.hold)
            mask = static_cast<uint16_t>(mask | ev.mask);
    }
    return mask;
}

// EE thread (scePadRead): analog sticks the script holds now (axes = rx, ry, lx, ly; 0x80 = centred).
// Returns false when no stick event is active, so the caller keeps its own values.
extern "C" bool ps2xTestPadAxes(int port, uint8_t axes[4])
{
    if (port != 0 || g_padScript.empty())
        return false;
    const uint64_t tick = inputClock();
    bool any = false;
    for (int i = 0; i < 4; ++i)
        axes[i] = 0x80;
    for (const PadEvent &ev : g_padScript)
    {
        if (ev.axis >= 0 && tick >= ev.tick && tick < ev.tick + ev.hold)
        {
            axes[ev.axis] = ev.value;
            any = true;
        }
    }
    return any;
}

// Window thread, whenever a new picture goes to the window (RGBA, rowPixels per row). Saves the
// requested checkpoint pictures and logs simple numbers about them for the test report.
static std::atomic<uint64_t> g_lastShotTick{0};

// ps2_runtime.cpp (PS2X_WINDOW_SHOTS): the tick of the last test_shot taken, so the window can save itself too.
extern "C" uint64_t ps2xTestLastShotTick()
{
    return g_lastShotTick.load(std::memory_order_relaxed);
}

extern "C" void ps2xTestHarnessFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t rowPixels)
{
    if (g_nextShot >= g_shotTicks.size() || !rgba || width == 0 || height == 0)
        return;
    const uint64_t tick = scriptClock();
    if (tick < g_shotTicks[g_nextShot])
        return;
    g_lastShotTick.store(tick, std::memory_order_relaxed);
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

// Last guest VSync seen by ps2xTestOnGuestVSync (for tools that pick a moment by guest time, e.g. GS captures).
extern "C" uint64_t ps2xGuestVsync()
{
    return g_guestVsync.load(std::memory_order_relaxed);
}

// F11 (window thread): EE RAM at the next guest VSync (ps2xTestOnGuestVSync).
static std::atomic<bool> g_eeDumpRequest{false};
extern "C" void ps2xRequestEeDump()
{
    g_eeDumpRequest.store(true);
}

// EE thread, at every VBlank start (EeScheduler). Feeds the guest clock used in deterministic mode and
// logs the state fingerprint used to compare two runs.
extern "C" void ps2xSetGuestVsyncHook(void (*fn)(uint64_t scriptTime, uint64_t vsync, const uint8_t *rdram))
{
    g_guestVsyncHook = fn;
}

extern "C" void ps2xTestOnGuestVSync(uint64_t vsync, uint64_t eeCycle, const uint8_t *rdram, uint32_t ramBytes)
{
    g_guestVsync.store(vsync, std::memory_order_relaxed);
    if (g_guestVsyncHook && rdram)
        g_guestVsyncHook(scriptClock(), vsync, rdram);

    const auto vsyncList = [](const char *name)
    {
        std::vector<uint64_t> at;
        if (const char *list = std::getenv(name))
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
    };
    // PS2X_SPU2_DUMP_AT="1200,2300": full SPU2 state at those guest VSyncs (this thread also runs the IOP).
    static const std::vector<uint64_t> spu2DumpAt = vsyncList("PS2X_SPU2_DUMP_AT");
    if (std::find(spu2DumpAt.begin(), spu2DumpAt.end(), vsync) != spu2DumpAt.end())
    {
        std::fprintf(stderr, "[spu2-dump] guest vsync %llu\n", static_cast<unsigned long long>(vsync));
        ps2xSpu2DebugDump();
    }
    // PS2X_EE_DUMP_AT="7150,9000": EE RAM (32 MiB) to out/ee_ram_<vsync>.bin at those guest VSyncs. In deterministic
    // mode two dumps of the same VSync from two runs are identical, so state can be diffed across runs and times.
    static const std::vector<uint64_t> eeDumpAt = vsyncList("PS2X_EE_DUMP_AT");
    // F11 (ps2xRequestEeDump): EE RAM to out/gif_capture_ee_ram.bin at the next guest VSync, next to the GS / VU1
    // captures, to compare the game's state (camera, objects) with a PCSX2 save state at the same spot
    // (tools/p2s_extract.py: eeMemory.bin; CLAUDE.md issue 19).
    if (rdram && g_eeDumpRequest.exchange(false))
    {
        if (FILE *f = std::fopen("out/gif_capture_ee_ram.bin", "wb"))
        {
            std::fwrite(rdram, 1u, ramBytes, f);
            std::fclose(f);
            std::fprintf(stderr, "[ee-dump] F11: guest vsync %llu -> out/gif_capture_ee_ram.bin\n", static_cast<unsigned long long>(vsync));
        }
    }
    if (rdram && std::find(eeDumpAt.begin(), eeDumpAt.end(), vsync) != eeDumpAt.end())
    {
        char path[64];
        std::snprintf(path, sizeof(path), "out/ee_ram_%llu.bin", static_cast<unsigned long long>(vsync));
        if (FILE *f = std::fopen(path, "wb"))
        {
            std::fwrite(rdram, 1u, ramBytes, f);
            std::fclose(f);
            std::fprintf(stderr, "[ee-dump] guest vsync %llu -> %s\n", static_cast<unsigned long long>(vsync), path);
        }
    }

    // PS2X_GS_CAPTURE_AT="1400": the CPU renderer's GS capture (out/gscapture.bin: 40-byte header, then the whole
    // 4 MiB of GS local memory, then one frame of commands) at that guest VSync, as F11 does. Needs
    // PS2X_GS_RENDERER=cpu (the GPU renderer keeps VRAM on the GPU).
    static const std::vector<uint64_t> gsCaptureAt = vsyncList("PS2X_GS_CAPTURE_AT");
    if (std::find(gsCaptureAt.begin(), gsCaptureAt.end(), vsync) != gsCaptureAt.end())
    {
        ps2xRequestGsCapture();
        std::fprintf(stderr, "[gs] capture requested at guest vsync %llu\n", static_cast<unsigned long long>(vsync));
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
