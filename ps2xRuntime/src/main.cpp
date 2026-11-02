#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "games_database.h"
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
// Rayman bring-up: report crashes in out\run.log. Declared by hand so <windows.h> (which clashes
// with raylib) stays out of this file.
extern "C"
{
    struct Ps2xExceptionRecord
    {
        unsigned long ExceptionCode;
        unsigned long ExceptionFlags;
        Ps2xExceptionRecord *ExceptionRecord;
        void *ExceptionAddress;
        unsigned long NumberParameters;
        unsigned long long ExceptionInformation[15];
    };
    struct Ps2xExceptionPointers
    {
        Ps2xExceptionRecord *ExceptionRecord;
        void *ContextRecord;
    };
    typedef long(__stdcall *Ps2xTopLevelFilter)(Ps2xExceptionPointers *);
    __declspec(dllimport) Ps2xTopLevelFilter __stdcall SetUnhandledExceptionFilter(Ps2xTopLevelFilter filter);
    __declspec(dllimport) void *__stdcall GetModuleHandleA(const char *name);
    __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long framesToSkip, unsigned long framesToCapture,
                                                                             void **backTrace, unsigned long *backTraceHash);
    void ps2xDumpCrashContext(); // ps2_runtime.cpp
}

namespace
{
    long __stdcall ps2xCrashFilter(Ps2xExceptionPointers *info)
    {
        static volatile long s_entered = 0;
        if (s_entered++)
            return 0; // EXCEPTION_CONTINUE_SEARCH
        const auto base = reinterpret_cast<unsigned long long>(GetModuleHandleA(nullptr));
        if (info && info->ExceptionRecord)
        {
            const Ps2xExceptionRecord *rec = info->ExceptionRecord;
            const auto addr = reinterpret_cast<unsigned long long>(rec->ExceptionAddress);
            std::fprintf(stderr, "\n[crash] exception 0x%08lx at 0x%llx (exe+0x%llx)", rec->ExceptionCode, addr, addr - base);
            if (rec->ExceptionCode == 0xC0000005ul && rec->NumberParameters >= 2)
                std::fprintf(stderr, " %s address 0x%llx", rec->ExceptionInformation[0] == 0 ? "reading" : (rec->ExceptionInformation[0] == 1 ? "writing" : "executing"),
                             rec->ExceptionInformation[1]);
            std::fprintf(stderr, "\n");
        }
        void *frames[32] = {};
        const unsigned short count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
        std::fprintf(stderr, "[crash] host stack (exe offsets):");
        for (unsigned short i = 0; i < count; ++i)
        {
            const auto f = reinterpret_cast<unsigned long long>(frames[i]);
            if (f >= base && f < base + 0x40000000ull)
                std::fprintf(stderr, " +0x%llx", f - base);
            else
                std::fprintf(stderr, " 0x%llx", f);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        ps2xDumpCrashContext();
        std::fflush(stdout);
        std::fflush(stderr);
        return 0; // let Windows terminate the process
    }
}
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    // ------------------------------------------------------------------
    // Rayman Revolution (SLES-500.44) game-specific HLE
    // ------------------------------------------------------------------
    // 0x14BEC8 is the engine's "wait N milliseconds" helper (a0 = timer object,
    // a1 = ms). It spins on EE timer 0 (via 0x14C1B8) until the time has passed.
    // In the recompiled build every spin iteration costs far more host time than
    // the guest cycles it accounts for, so a 300 ms wait took many seconds and the
    // game looked frozen on the PRESS START screen (pad init waits 300 ms per port
    // and then 16 x 15 ms). Replace it with a real-time wait that also advances
    // guest time (EE timers + IOP) by the same amount.
    constexpr uint32_t kRaymanWaitMsAddr = 0x0014BEC8u;

    void raymanWaitMsHle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t ms = std::min<uint32_t>(getRegU32(ctx, 5), 5000u);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

        constexpr uint64_t kCyclesPerMs = EeScheduler::kEeClockHz / 1000u;
        constexpr uint64_t kChunk = kCyclesPerMs / 2u; // 0.5 ms of guest time per step
        uint64_t remaining = static_cast<uint64_t>(ms) * kCyclesPerMs;
        EeScheduler &scheduler = runtime->eeScheduler();
        while (remaining > 0u)
        {
            const uint64_t step = std::min<uint64_t>(remaining, kChunk);
            scheduler.accountCycles(static_cast<uint32_t>(step));
            remaining -= step;
        }
        std::this_thread::sleep_until(deadline);

        setReturnU32(ctx, 0u);
        ctx->pc = getRegU32(ctx, 31);
    }

    // 0x1DF7F0 evaluates one AI behaviour tree (a0 = actor context, a1 = first node, a2 = result
    // record in scratchpad). The game crashed in two runs when an actor's rule handed it a NULL tree:
    // the evaluator then walks address 0 upward as nodes until it jumps through a missing table entry.
    // Guard: a tree below the game heap is treated as "no rule fired" (result flags 0, type 0x14, as the
    // function itself writes on exit), and the first few cases are logged for the bug hunt.
    constexpr uint32_t kRaymanEvalTreeAddr = 0x001DF7F0u;
    PS2Runtime::RecompiledFunction g_raymanEvalTree = nullptr;

    uint32_t readGuest32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0u;
        const uint32_t physical = address & 0x01FFFFFFu;
        if (physical <= PS2_RAM_SIZE - 4u)
            std::memcpy(&value, rdram + physical, sizeof(value));
        return value;
    }

    void raymanEvalTreeGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t node = getRegU32(ctx, 5);
        if (node >= 0x00100000u && node < 0x02000000u)
        {
            g_raymanEvalTree(rdram, ctx, runtime);
            return;
        }

        static uint32_t s_reports = 0u;
        if (s_reports < 8u)
        {
            ++s_reports;
            const uint32_t mind = getRegU32(ctx, 17); // caller (0x1DF8D8) keeps the actor's mind in s1
            const uint32_t rules = readGuest32(rdram, mind + 0x08u);
            const uint32_t comports = readGuest32(rdram, mind + 0x10u);
            const uint32_t rulesBase = readGuest32(rdram, rules);
            std::cerr << std::hex << "[rayman-ai] NULL behaviour tree: node=0x" << node
                      << " ra=0x" << getRegU32(ctx, 31) << " a0=0x" << getRegU32(ctx, 4)
                      << " ctx=0x" << getRegU32(ctx, 6) << " rule=" << getRegU32(ctx, 16)
                      << " mind=0x" << mind << " [mind+0]=0x" << readGuest32(rdram, mind)
                      << " [mind+8]=0x" << rules << " [mind+10]=0x" << comports
                      << " comportHdr=0x" << readGuest32(rdram, comports) << ",0x" << readGuest32(rdram, comports + 4u)
                      << " rulesBase=0x" << rulesBase << " rule0=0x" << readGuest32(rdram, rulesBase)
                      << std::dec << std::endl;
        }

        const uint32_t record = getRegU32(ctx, 6) & 0x01FFFFFFu;
        if (getRegU32(ctx, 6) >= 0x70000000u && getRegU32(ctx, 6) < 0x70004000u)
        {
            uint8_t *scratch = runtime->memory().getScratchpad();
            if (scratch)
            {
                const uint32_t offset = getRegU32(ctx, 6) & 0x3FFFu;
                const uint32_t zero = 0u;
                std::memcpy(scratch + offset, &zero, sizeof(zero));
                scratch[offset + 0x0Cu] = 0x14u;
            }
        }
        else if (record <= PS2_RAM_SIZE - 0x10u)
        {
            const uint32_t zero = 0u;
            std::memcpy(rdram + record, &zero, sizeof(zero));
            rdram[record + 0x0Cu] = 0x14u;
        }
        setReturnU32(ctx, 0u);
        ctx->pc = getRegU32(ctx, 31);
    }

    // 0x1DF760 dispatches one AI node (a0 = actor context, a1 = node pointer, node type byte at a1+7) and returns the
    // next node. Bug hunt: a node pointer in low memory (< 1 MiB, no game data lives there) means a tree was read from
    // memory that was still zero. Keep the last nodes dispatched and print them, with their bytes, when that happens.
    constexpr uint32_t kRaymanNodeDispatchAddr = 0x001DF760u;
    extern "C" void ps2xDumpSchedulerTrace(); // Kernel/EeScheduler.cpp
    extern "C" void ps2xDumpBranchTrace();    // ps2_runtime.cpp
    PS2Runtime::RecompiledFunction g_raymanNodeDispatch = nullptr;

    struct RaymanNodeTrace
    {
        uint32_t ra, actor, node, type;
    };
    RaymanNodeTrace g_raymanNodeRing[64];
    uint32_t g_raymanNodeRingPos = 0u;

    void raymanNodeDispatchTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t node = getRegU32(ctx, 5);
        if (node < 0x00100000u)
        {
            static bool s_reported = false;
            if (!s_reported)
            {
                s_reported = true;
                std::fprintf(stderr, "[rayman-ai] node pointer 0x%x (low memory) from ra=0x%x actor=0x%x; last nodes, oldest first:\n",
                             node, getRegU32(ctx, 31), getRegU32(ctx, 4));
                for (uint32_t i = 0; i < 64u; ++i)
                {
                    const RaymanNodeTrace &t = g_raymanNodeRing[(g_raymanNodeRingPos + i) % 64u];
                    if (t.node == 0u && t.ra == 0u)
                        continue;
                    std::fprintf(stderr, "[rayman-ai]   ra=0x%06x actor=0x%x node=0x%08x type=%u bytes[-8..+8]=%08x %08x | %08x %08x\n",
                                 t.ra, t.actor, t.node, t.type, readGuest32(rdram, t.node - 8u), readGuest32(rdram, t.node - 4u),
                                 readGuest32(rdram, t.node), readGuest32(rdram, t.node + 4u));
                }
                ps2xDumpSchedulerTrace();
                ps2xDumpBranchTrace();
                ps2xDumpCrashContext();
            }
        }
        RaymanNodeTrace &slot = g_raymanNodeRing[g_raymanNodeRingPos++ % 64u];
        slot.ra = getRegU32(ctx, 31);
        slot.actor = getRegU32(ctx, 4);
        slot.node = node;
        slot.type = node >= 0x00100000u ? (readGuest32(rdram, node + 4u) >> 24) : 0xFFu;
        g_raymanNodeDispatch(rdram, ctx, runtime);
    }

    void installGameHle(PS2Runtime &runtime, const std::string &gameId)
    {
        if (gameId != "SLES-50044")
            return;

        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram)
            return;
        uint32_t entryWord = 0u;
        uint32_t timerCallWord = 0u;
        std::memcpy(&entryWord, rdram + kRaymanWaitMsAddr, sizeof(entryWord));
        std::memcpy(&timerCallWord, rdram + kRaymanWaitMsAddr + 0x20u, sizeof(timerCallWord));
        // addiu sp,sp,-0x50 / jal 0x14C1B8
        if (entryWord != 0x27BDFFB0u || timerCallWord != 0x0C05306Eu)
        {
            std::cerr << "[hle] Rayman wait helper not found at 0x14BEC8, skipping" << std::endl;
            return;
        }
        if (runtime.registerFunction(kRaymanWaitMsAddr, &raymanWaitMsHle))
            std::cerr << "[hle] Rayman: real-time wait helper installed at 0x14BEC8" << std::endl;
        else
            std::cerr << "[hle] Rayman: failed to install wait helper" << std::endl;

        uint32_t evalEntryWord = 0u;
        std::memcpy(&evalEntryWord, rdram + kRaymanEvalTreeAddr, sizeof(evalEntryWord));
        // addiu sp,sp,-0x50
        if (evalEntryWord == 0x27BDFFB0u && runtime.hasFunction(kRaymanEvalTreeAddr))
        {
            g_raymanEvalTree = runtime.lookupFunction(kRaymanEvalTreeAddr);
            if (g_raymanEvalTree && runtime.registerFunction(kRaymanEvalTreeAddr, &raymanEvalTreeGuard))
                std::cerr << "[hle] Rayman: AI tree NULL guard installed at 0x1DF7F0" << std::endl;
        }

        uint32_t dispatchEntryWord = 0u;
        std::memcpy(&dispatchEntryWord, rdram + kRaymanNodeDispatchAddr, sizeof(dispatchEntryWord));
        // addiu sp,sp,-0x10
        if (dispatchEntryWord == 0x27BDFFF0u && runtime.hasFunction(kRaymanNodeDispatchAddr))
        {
            g_raymanNodeDispatch = runtime.lookupFunction(kRaymanNodeDispatchAddr);
            if (g_raymanNodeDispatch && runtime.registerFunction(kRaymanNodeDispatchAddr, &raymanNodeDispatchTrace))
                std::cerr << "[hle] Rayman: AI node trace installed at 0x1DF760" << std::endl;
        }
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    setupTerminateLogger();
#if defined(_WIN32)
    SetUnhandledExceptionFilter(ps2xCrashFilter);
#endif

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        installGameHle(runtime, normalizedId);

        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
