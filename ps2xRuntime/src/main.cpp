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
#include <cmath>
#include <thread>
#include <array>

extern "C" uint64_t ps2xGuestVsync(); // ps2_test_harness.cpp
extern "C" void ps2xSetGameStatsLine(int (*fn)(const uint8_t *rdram, char *buffer, int size)); // ps2_runtime.cpp
void ps2xVu1ProfileFlush(); // ps2_vu1_core.cpp
int ps2xVu1AotReplay(const char *path); // ps2_vu1_core.cpp: PS2X_VU1AOT_REPLAY
extern "C" uint32_t ps2xVBlankRateOverrideHz(); // EeScheduler.cpp (PS2X_FRAMERATE)
extern "C" bool ps2xGuestYieldPending();        // ps2_runtime.cpp
extern "C" void ps2xSetGuestVsyncHook(void (*fn)(uint64_t scriptTime, uint64_t vsync, const uint8_t *rdram)); // ps2_test_harness.cpp

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

    // ------------------------------------------------------------------
    // PS2X_FRAMERATE=N (EeScheduler.cpp: VBlank at N Hz): Rayman at N frames per second, game time kept real time.
    // - Frame cap. The main loop (0x1010F8) ends waiting (0x101208-0x10121C) until the VBlank-start counter gp-0x71D8
    //   (counted by the INTC 2 handler 0x1413C8, reset after every frame) passes max(1, its value at the check): at
    //   least two VBlanks per frame, 25 FPS in PAL. (The US 60 FPS patch nops that loop's branch, 0x10121C.) The
    //   handler now counts 2 per VBlank: at least one VBlank per frame, so N FPS.
    // - Game time comes from EE timer 0 (0x14C1B8). The frame clock (0x14BCD8, struct at [gp-0x7108]) takes the time
    //   since the last frame, rounds it to whole units (n at +0x24; unit +0xC ms = 1000 * the constant 0.02 at
    //   0x39286C, read once when the clock is made) and steps n units: +0x20 = trunc(n * unit ms), +0x14 += that (ms),
    //   +0x2C = n * unit s, +0x30 = +0x14 / 1000 s. With 20 ms units a 16.7 ms frame would count 20 ms (the game 20%
    //   fast), so the unit becomes 1/N s; and trunc(16.667) = 16 would make the ms timers 4% slow, so the ms step is
    //   made exact (16, 17, 17, ...).
    // - Demo playback (gp-0x6898 set: n comes from the recording, in 20 ms units): all original.
    constexpr uint32_t kRaymanVBlankHandlerAddr = 0x001413C8u;
    constexpr uint32_t kRaymanFrameClockAddr = 0x0014BCD8u;
    constexpr uint32_t kRaymanClockUnitAddr = 0x0039286Cu;
    constexpr uint32_t kRaymanFrameWaitBranchAddr = 0x0010121Cu;
    PS2Runtime::RecompiledFunction g_raymanVBlankHandler = nullptr;
    PS2Runtime::RecompiledFunction g_raymanFrameClock = nullptr;
    uint32_t g_raymanFrameRate = 0u;
    uint32_t g_raymanUnitHz = 0u; // clock unit = 1 / this (the frame rate; PS2X_RAYMAN_CLOCK_UNIT_HZ for experiments)
    float g_raymanOriginalUnit = 0.02f;
    int64_t g_raymanClockLastFrame = -1; // frame counter (+0x28) of the last update corrected
    uint64_t g_raymanClockRemainder = 0; // ms * N not yet counted

    uint32_t raymanRam32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, rdram + (address & 0x01FFFFFCu), sizeof(value));
        return value;
    }

    void raymanRamWrite32(uint8_t *rdram, uint32_t address, uint32_t value)
    {
        std::memcpy(rdram + (address & 0x01FFFFFCu), &value, sizeof(value));
    }

    void raymanRamWriteFloat(uint8_t *rdram, uint32_t address, float value)
    {
        std::memcpy(rdram + (address & 0x01FFFFFCu), &value, sizeof(value));
    }

    bool raymanDemoPlayback(const uint8_t *rdram, uint32_t gp)
    {
        return rdram[(gp - 0x6898u) & 0x01FFFFFFu] != 0u;
    }

    void raymanVBlankCounterHle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        g_raymanVBlankHandler(rdram, ctx, runtime); // counter++, ei, return 0
        if (raymanDemoPlayback(rdram, gp))
            return;
        // PS2X_RAYMAN_RATE_SWITCH="fromVsync:divisor" (experiments, CLAUDE.md issue 22): from that guest VSync the game
        // runs at N / divisor FPS (a frame every divisor VBlanks) with the matching clock unit. A replay recorded at
        // 120 FPS can then show the same moment at 24 FPS.
        static const auto rateSwitch = []()
        {
            std::pair<uint64_t, uint32_t> s{0u, 1u};
            if (const char *text = std::getenv("PS2X_RAYMAN_RATE_SWITCH"))
            {
                unsigned long long from = 0u;
                unsigned divisor = 1u;
                if (std::sscanf(text, "%llu:%u", &from, &divisor) == 2 && divisor >= 2u && divisor <= 10u)
                    s = {from, divisor};
            }
            return s;
        }();
        if (rateSwitch.second > 1u && ps2xGuestVsync() >= rateSwitch.first)
        {
            static uint64_t phase = 0u;
            if (phase == 0u)
            {
                g_raymanUnitHz = g_raymanFrameRate / rateSwitch.second;
                g_raymanClockRemainder = 0u;
                raymanRamWriteFloat(rdram, kRaymanClockUnitAddr, 1.0f / static_cast<float>(g_raymanUnitHz));
                std::fprintf(stderr, "[hle] Rayman: VSync %llu: now %u FPS (PS2X_RAYMAN_RATE_SWITCH)\n",
                             static_cast<unsigned long long>(ps2xGuestVsync()), g_raymanUnitHz);
            }
            // Two counts every divisor-th VBlank (the frame may end), none in between (undo the handler's one).
            const uint32_t counter = raymanRam32(rdram, gp - 0x71D8u);
            raymanRamWrite32(rdram, gp - 0x71D8u, (phase++ % rateSwitch.second) == 0u ? counter + 1u : counter - 1u);
            return;
        }
        raymanRamWrite32(rdram, gp - 0x71D8u, raymanRam32(rdram, gp - 0x71D8u) + 1u);
    }

    // The update just made (frame counter one past the last one corrected): the exact ms step instead of trunc(n * unit).
    void raymanClockExactStep(uint8_t *rdram, uint32_t clock)
    {
        const uint32_t frame = raymanRam32(rdram, clock + 0x28u);
        if (g_raymanClockLastFrame >= 0 && frame == static_cast<uint32_t>(g_raymanClockLastFrame) + 1u)
        {
            const uint32_t n = raymanRam32(rdram, clock + 0x24u);
            const uint32_t gameStep = raymanRam32(rdram, clock + 0x20u);
            g_raymanClockRemainder += static_cast<uint64_t>(n) * 1000u;
            const uint32_t exactStep = static_cast<uint32_t>(g_raymanClockRemainder / g_raymanUnitHz);
            g_raymanClockRemainder %= g_raymanUnitHz;
            const uint32_t ms = raymanRam32(rdram, clock + 0x14u) - gameStep + exactStep;
            raymanRamWrite32(rdram, clock + 0x20u, exactStep);
            raymanRamWrite32(rdram, clock + 0x14u, ms);
            raymanRamWriteFloat(rdram, clock + 0x30u,
                                static_cast<float>(ms) / static_cast<float>(raymanRam32(rdram, clock + 0x10u)));
        }
        g_raymanClockLastFrame = frame;
    }

    void raymanFrameClockHle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t clock = getRegU32(ctx, 4);
        const bool demo = raymanDemoPlayback(rdram, getRegU32(ctx, 28));
        if (clock != 0u)
        {
            if (demo)
                g_raymanClockLastFrame = -1;
            else
                raymanClockExactStep(rdram, clock); // an update a scheduler yield left without its correction
            // The unit, as the clock's maker (0x14BBA8) sets it: +0x8 seconds, +0xC = +0x10 (1000) * that in ms.
            const float unit = demo ? g_raymanOriginalUnit : 1.0f / static_cast<float>(g_raymanUnitHz);
            raymanRamWriteFloat(rdram, clock + 0x08u, unit);
            raymanRamWriteFloat(rdram, clock + 0x0Cu, static_cast<float>(raymanRam32(rdram, clock + 0x10u)) * unit);
        }
        g_raymanFrameClock(rdram, ctx, runtime);
        if (clock != 0u && !demo && !ps2xGuestYieldPending())
            raymanClockExactStep(rdram, clock);
    }

    // ------------------------------------------------------------------
    // The ability shop by the well of whirlpools at high frame rates (CLAUDE.md issue 22). Leaving the shop (Triangle)
    // starts a scripted run out of the gorge; where it stops depends on the frame rate (the physics steps once per
    // frame). Right after it, the shop's trigger (perso 0x8E76FC, condition 0xB "any of my detection zones holds the
    // main actor", 0x207420 -> 0x1152E0 -> 0x1143C8 -> 0x210940) tests its one zone: a sphere of local radius 3.404,
    // 8.51 units in the world (the trigger's matrix scales by 2.5), centre (198.49, 62.77, 59.52). At 25 FPS the run
    // stops 1.3 units outside it, at 120 FPS 0.22 inside, and the shop runs Rayman back in. With the frame rate
    // changed, the sphere gets a world radius of 7.2 units: the run's stop is outside it again at every rate, with
    // about the original margin; walking up to the shop opens it ~1.3 units later. The sphere is found by its data
    // (one sphere, exact radius and centre: it appears once in memory), on its first test after the level loads.
    // 0x210940(a0 = point in the zone owner's space, a1 = geometric object): +0 vertex count, +2 element count (int16),
    // +4 vertices (3 floats each), +8 element types (int16; 7 = spheres), +0xC elements; a sphere element: +0 spheres
    // (12 bytes: radius, material, int16 centre vertex), +4 count (int16).
    constexpr uint32_t kRaymanPointInGeoAddr = 0x00210940u;
    constexpr uint32_t kRaymanShopZoneRadiusBits = 0x4059DC48u; // 3.4041
    constexpr uint32_t kRaymanShopZoneCentreBits[3] = {0xC07B8C54u, 0xC1077E87u, 0x40B7930Cu};
    constexpr float kRaymanShopZoneRadius = 7.2f / 2.5f;
    PS2Runtime::RecompiledFunction g_raymanPointInGeo = nullptr;

    bool raymanGuestDataPointer(uint32_t address)
    {
        return address >= 0x00100000u && address < 0x01FFFFF0u;
    }

    void raymanPointInGeoShopFix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t geo = getRegU32(ctx, 5);
        if (raymanGuestDataPointer(geo) && (geo & 3u) == 0u && raymanRam32(rdram, geo) == 0x00010001u) // 1 vertex, 1 element
        {
            const uint32_t verts = raymanRam32(rdram, geo + 4u), types = raymanRam32(rdram, geo + 8u);
            const uint32_t elements = raymanRam32(rdram, geo + 0xCu);
            if (raymanGuestDataPointer(verts) && raymanGuestDataPointer(types) && raymanGuestDataPointer(elements) &&
                (raymanRam32(rdram, types) & 0xFFFFu) == 7u && raymanRam32(rdram, verts) == kRaymanShopZoneCentreBits[0] &&
                raymanRam32(rdram, verts + 4u) == kRaymanShopZoneCentreBits[1] &&
                raymanRam32(rdram, verts + 8u) == kRaymanShopZoneCentreBits[2])
            {
                const uint32_t element = raymanRam32(rdram, elements);
                const uint32_t sphere = raymanGuestDataPointer(element) ? raymanRam32(rdram, element) : 0u;
                if (raymanGuestDataPointer(sphere) && (raymanRam32(rdram, element + 4u) & 0xFFFFu) == 1u &&
                    raymanRam32(rdram, sphere) == kRaymanShopZoneRadiusBits && (raymanRam32(rdram, sphere + 8u) & 0xFFFFu) == 0u)
                {
                    raymanRamWriteFloat(rdram, sphere, kRaymanShopZoneRadius);
                    std::fprintf(stderr, "[hle] Rayman: shop trigger zone at 0x%x: radius 8.51 -> 7.2 units (high frame rate, CLAUDE.md issue 22)\n", sphere);
                }
            }
        }
        g_raymanPointInGeo(rdram, ctx, runtime);
    }

    void installRaymanShopZoneFix(PS2Runtime &runtime, uint8_t *rdram)
    {
        if (raymanRam32(rdram, kRaymanPointInGeoAddr) != 0x27BDFF30u || !runtime.hasFunction(kRaymanPointInGeoAddr)) // addiu sp,sp,-0xd0
        {
            std::cerr << "[hle] Rayman: zone test 0x210940 not found, shop exit fix off (CLAUDE.md issue 22)" << std::endl;
            return;
        }
        g_raymanPointInGeo = runtime.lookupFunction(kRaymanPointInGeoAddr);
        if (!g_raymanPointInGeo || !runtime.registerFunction(kRaymanPointInGeoAddr, &raymanPointInGeoShopFix))
            std::cerr << "[hle] Rayman: failed to install the shop exit fix" << std::endl;
    }

    void installRaymanFramerate(PS2Runtime &runtime, uint8_t *rdram)
    {
        g_raymanFrameRate = ps2xVBlankRateOverrideHz();
        if (g_raymanFrameRate == 0u)
            return;
        g_raymanUnitHz = g_raymanFrameRate;
        if (const char *unitHz = std::getenv("PS2X_RAYMAN_CLOCK_UNIT_HZ")) // experiments: separate unit and frame rate
            if (const long hz = std::strtol(unitHz, nullptr, 10); hz >= 10 && hz <= 1000)
                g_raymanUnitHz = static_cast<uint32_t>(hz);
        const bool codeMatches = raymanRam32(rdram, kRaymanVBlankHandlerAddr) == 0x8F828E28u &&      // lw v0,-0x71d8(gp)
                                 raymanRam32(rdram, kRaymanVBlankHandlerAddr + 4u) == 0x24420001u && // addiu v0,v0,1
                                 raymanRam32(rdram, kRaymanVBlankHandlerAddr + 8u) == 0xAF828E28u && // sw v0,-0x71d8(gp)
                                 raymanRam32(rdram, kRaymanFrameWaitBranchAddr) == 0x1040FFFAu &&    // beqz v0,0x101208
                                 raymanRam32(rdram, kRaymanFrameClockAddr) == 0x27BDFFA0u &&         // addiu sp,sp,-0x60
                                 runtime.hasFunction(kRaymanVBlankHandlerAddr) && runtime.hasFunction(kRaymanFrameClockAddr);
        if (!codeMatches)
        {
            std::cerr << "[hle] Rayman: PS2X_FRAMERATE=" << g_raymanFrameRate
                      << ": frame cap / clock code not found, only the VBlank rate changes (game time may run fast)" << std::endl;
            return;
        }
        std::memcpy(&g_raymanOriginalUnit, rdram + kRaymanClockUnitAddr, sizeof(g_raymanOriginalUnit));
        raymanRamWriteFloat(rdram, kRaymanClockUnitAddr, 1.0f / static_cast<float>(g_raymanUnitHz));
        g_raymanVBlankHandler = runtime.lookupFunction(kRaymanVBlankHandlerAddr);
        g_raymanFrameClock = runtime.lookupFunction(kRaymanFrameClockAddr);
        if (g_raymanVBlankHandler && g_raymanFrameClock &&
            runtime.registerFunction(kRaymanVBlankHandlerAddr, &raymanVBlankCounterHle) &&
            runtime.registerFunction(kRaymanFrameClockAddr, &raymanFrameClockHle))
            std::cerr << "[hle] Rayman: " << g_raymanFrameRate << " FPS (VBlank " << g_raymanFrameRate
                      << " Hz, frame cap 1 VBlank, clock unit 1/" << g_raymanUnitHz << " s, exact ms step; was "
                      << g_raymanOriginalUnit << " s)" << std::endl;
        else
        {
            std::cerr << "[hle] Rayman: failed to install the frame rate change" << std::endl;
            return;
        }
        installRaymanShopZoneFix(runtime, rdram);
    }

    // ------------------------------------------------------------------
    // PS2X_WIDESCREEN=16:9 (any w:h wider than 4:3; 1 = 16:9): the 3D view widened to that aspect, as the PCSX2
    // widescreen patch for this ELF (SLES-50044 55EDA5A0, ElHecht & ICUP321) does; the window then shows the frame at
    // that aspect (ps2_runtime.cpp HostDisplayAspect), so 2D (HUD, menus, videos) is stretched as in PCSX2.
    // - Projection: RenderWare's RwCameraSetViewWindow 0x18C660 (viewWindow at +0x68, recipViewWindow +0x70 / +0x74;
    //   then marks the camera's frame dirty, 0x18E670) gets recipViewWindow.x scaled by f = (4/3) / aspect (0.75 for
    //   16:9; the patch's mul.s at 0x18C6A4): x is squeezed, the 4:3 frame holds the wider view.
    // - Culling: the game's camera planes (0x117FB8) take tanf(fov x / 2) at 0x118140 for the side planes (+0x8C -
    //   +0xA4); the patch divides that half angle by f (div.s at 0x1180EC), so objects at the new edges stay. Here
    //   tanf (0x217DA0) does it for the call returning to 0x118148.
    constexpr uint32_t kRaymanSetViewWindowAddr = 0x0018C660u;
    constexpr uint32_t kRaymanTanfAddr = 0x00217DA0u;
    constexpr uint32_t kRaymanSidePlaneTanReturn = 0x00118148u;
    PS2Runtime::RecompiledFunction g_raymanSetViewWindow = nullptr;
    PS2Runtime::RecompiledFunction g_raymanTanf = nullptr;
    float g_raymanWideFactor = 1.0f;

    void raymanSetViewWindowWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t camera = getRegU32(ctx, 4);
        g_raymanSetViewWindow(rdram, ctx, runtime);
        float recipX = 0.0f;
        std::memcpy(&recipX, rdram + ((camera + 0x70u) & 0x01FFFFFCu), sizeof(recipX));
        raymanRamWriteFloat(rdram, camera + 0x70u, recipX * g_raymanWideFactor);
    }

    void raymanTanfWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == kRaymanSidePlaneTanReturn)
            ctx->f[12] = ctx->f[12] / g_raymanWideFactor;
        g_raymanTanf(rdram, ctx, runtime);
    }

    void installRaymanWidescreen(PS2Runtime &runtime, uint8_t *rdram)
    {
        const char *env = std::getenv("PS2X_WIDESCREEN");
        if (!env || !*env || std::strcmp(env, "0") == 0)
            return;
        float w = 16.0f, h = 9.0f;
        if (std::strcmp(env, "1") != 0 && (std::sscanf(env, "%f:%f", &w, &h) != 2 || w <= 0.0f || h <= 0.0f))
        {
            std::cerr << "[hle] Rayman: PS2X_WIDESCREEN=" << env << " not understood (use e.g. 16:9)" << std::endl;
            return;
        }
        const float factor = (4.0f / 3.0f) / (w / h);
        if (!(factor > 0.2f && factor < 1.0f))
        {
            std::cerr << "[hle] Rayman: PS2X_WIDESCREEN=" << env << " is not wider than 4:3, ignored" << std::endl;
            return;
        }
        const bool codeMatches = raymanRam32(rdram, kRaymanSetViewWindowAddr) == 0x27BDFFE0u &&         // addiu sp,sp,-0x20
                                 raymanRam32(rdram, kRaymanSetViewWindowAddr + 0x38u) == 0x46010043u && // div.s f1,f0,f1
                                 raymanRam32(rdram, kRaymanSetViewWindowAddr + 0x4Cu) == 0xE6010070u && // swc1 f1,0x70(s0)
                                 raymanRam32(rdram, kRaymanSidePlaneTanReturn - 8u) == 0x0C085F68u &&   // jal 0x217DA0
                                 raymanRam32(rdram, 0x001180D8u) == 0x46166302u &&                      // mul.s f12,f12,f22
                                 runtime.hasFunction(kRaymanSetViewWindowAddr) && runtime.hasFunction(kRaymanTanfAddr);
        if (!codeMatches)
        {
            std::cerr << "[hle] Rayman: PS2X_WIDESCREEN: camera code not found, the view stays 4:3" << std::endl;
            return;
        }
        g_raymanWideFactor = factor;
        g_raymanSetViewWindow = runtime.lookupFunction(kRaymanSetViewWindowAddr);
        g_raymanTanf = runtime.lookupFunction(kRaymanTanfAddr);
        if (g_raymanSetViewWindow && g_raymanTanf &&
            runtime.registerFunction(kRaymanSetViewWindowAddr, &raymanSetViewWindowWide) &&
            runtime.registerFunction(kRaymanTanfAddr, &raymanTanfWide))
            std::cerr << "[hle] Rayman: widescreen " << w << ":" << h << " (view x scale " << factor << ")" << std::endl;
        else
            std::cerr << "[hle] Rayman: failed to install widescreen" << std::endl;
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
        if (ps2IsScratchpadAddress(address))
        {
            if (const uint8_t *scratch = ps2GetScratchpadHostPtr())
                std::memcpy(&value, scratch + (ps2ScratchpadOffset(address) & ~3u), sizeof(value));
            return value;
        }
        const uint32_t physical = address & 0x01FFFFFFu;
        if (physical <= PS2_RAM_SIZE - 4u)
            std::memcpy(&value, rdram + physical, sizeof(value));
        return value;
    }

    // EE side of UBISOUND's ADPCM streams (sndeetrans.c). Stream object: +0x0 flags (1 active, 4 playing, 0x20 fade done,
    // 0x40 stopped), +0x4 "event from the IOP" (handled by 0x168CB8: segment-end callback +0x2C(+0x28), +0x1C next
    // segment -> +0x18), +0xC / +0x10 IOP class / instance, +0x18 current segment (flags at +0x14, bit 5 = streamed).
    // 0x169320 fn_bIsPlayingTdstAdpcmData(stream), 0x1697A0 fn_u32PushNextBlockResourceMemTdstAdpcmData(stream, fade,
    // block) sends play command 9. PS2X_RAYMAN_MUSIC_TRACE=<from vsync> logs every change of an active stream's state from
    // that guest VSync on (0 = from the start), every push and every IOP event.
    constexpr uint32_t kRaymanAdpcmIsPlayingAddr = 0x00169320u, kRaymanAdpcmPushAddr = 0x001697A0u,
                       kRaymanAdpcmEventAddr = 0x00168CB8u;
    PS2Runtime::RecompiledFunction g_raymanAdpcmIsPlaying = nullptr, g_raymanAdpcmPush = nullptr, g_raymanAdpcmEvent = nullptr;

    void logAdpcmStream(const char *what, const uint8_t *rdram, uint32_t stream, uint32_t ra, uint32_t result)
    {
        const uint32_t segment = readGuest32(rdram, stream + 0x18u);
        std::fprintf(stderr,
                     "[rayman-music] %s vsync %llu stream 0x%x flags=%x event=%x class/inst=%x/%x seg=0x%x (flags %x) next=0x%x "
                     "cb=0x%x(0x%x) -> %x ra=0x%x\n",
                     what, static_cast<unsigned long long>(ps2xGuestVsync()), stream, readGuest32(rdram, stream),
                     readGuest32(rdram, stream + 4u), readGuest32(rdram, stream + 0xCu), readGuest32(rdram, stream + 0x10u),
                     segment, segment ? readGuest32(rdram, segment + 0x14u) : 0u, readGuest32(rdram, stream + 0x1Cu),
                     readGuest32(rdram, stream + 0x2Cu), readGuest32(rdram, stream + 0x28u), result, ra);
    }

    void raymanAdpcmIsPlayingTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        struct Last
        {
            uint32_t stream = 0, flags = 0, event = 0, segment = 0, result = 0;
        };
        static Last s_last[16];
        static uint32_t s_logs = 0u;
        const uint32_t stream = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        g_raymanAdpcmIsPlaying(rdram, ctx, runtime);
        const uint32_t result = getRegU32(ctx, 2);
        static const uint64_t fromVsync = std::strtoull(std::getenv("PS2X_RAYMAN_MUSIC_TRACE"), nullptr, 10);
        Last &last = s_last[(stream >> 4) & 15u];
        const Last now{stream, readGuest32(rdram, stream), readGuest32(rdram, stream + 4u), readGuest32(rdram, stream + 0x18u), result};
        if ((now.flags & 1u) != 0u && ps2xGuestVsync() >= fromVsync &&
            (now.stream != last.stream || now.flags != last.flags || now.event != last.event || now.segment != last.segment ||
             now.result != last.result) && s_logs < 2000u)
        {
            ++s_logs;
            last = now;
            logAdpcmStream("isPlaying", rdram, stream, ra, result);
        }
    }

    void raymanAdpcmPushTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        const uint32_t stream = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        g_raymanAdpcmPush(rdram, ctx, runtime);
        if (s_logs++ < 1000u)
            logAdpcmStream("push", rdram, stream, ra, getRegU32(ctx, 2));
    }

    void raymanAdpcmEventTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        static const uint64_t fromVsync = std::strtoull(std::getenv("PS2X_RAYMAN_MUSIC_TRACE"), nullptr, 10);
        const uint32_t stream = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        const bool log = ps2xGuestVsync() >= fromVsync && s_logs++ < 1000u;
        if (log)
            logAdpcmStream("event before", rdram, stream, ra, 0u);
        g_raymanAdpcmEvent(rdram, ctx, runtime);
        if (log)
            logAdpcmStream("event after ", rdram, stream, ra, 0u);
    }

    // 0x155698 builds the full-screen fade quad (vertex array at 0x36FFC0): f12 = alpha (0-255), f13 = grey level,
    // a2 = viewport. PS2X_RAYMAN_FADE_TRACE=1 logs every change of (caller, alpha, grey, viewport) with the guest
    // VSync (bug hunt: CLAUDE.md issue 7, a black quad at alpha 249 over the first level).
    constexpr uint32_t kRaymanFadeQuadAddr = 0x00155698u;
    PS2Runtime::RecompiledFunction g_raymanFadeQuad = nullptr;

    void raymanFadeQuadTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        struct Last
        {
            uint32_t ra = 0, a2 = 0;
            float alpha = -1.0f, grey = -1.0f;
        };
        static Last s_last[8];
        static uint32_t s_logs = 0u;
        const uint32_t ra = getRegU32(ctx, 31), a2 = getRegU32(ctx, 6);
        const float alpha = ctx->f[12], grey = ctx->f[13];
        Last &last = s_last[(ra >> 2) & 7u];
        if ((last.ra != ra || last.a2 != a2 || last.alpha != alpha || last.grey != grey) && s_logs < 4000u)
        {
            ++s_logs;
            last = Last{ra, a2, alpha, grey};
            std::fprintf(stderr, "[rayman-fade] vsync %llu ra=0x%x alpha=%g grey=%g viewport=0x%x a0=0x%x a1=0x%x s0=0x%x s2=0x%x\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), ra, alpha, grey, a2, getRegU32(ctx, 4),
                         getRegU32(ctx, 5), getRegU32(ctx, 16), getRegU32(ctx, 18));
        }
        g_raymanFadeQuad(rdram, ctx, runtime);
    }

    // Screen-effect records (0x60 bytes each, array at 0x3796D0): +0x0 type (low byte) | state (0xF000) | flags,
    // +0xC start time, +0x4C duration, +0x58 alpha. 0x1D46A0 creates one (a0 = type word, f12 -> +0x4C,
    // f13 -> +0x40, f14 -> +0x44), 0x1D6B50 updates record a0 once per frame (gp-0x69F4 = frame time).
    // Traced with PS2X_RAYMAN_FADE_TRACE: every creation, and every update of record 0 that changes it.
    constexpr uint32_t kRaymanEffectNewAddr = 0x001D46A0u, kRaymanEffectUpdateAddr = 0x001D6B50u;
    constexpr uint32_t kRaymanEffectRecords = 0x003796D0u;
    PS2Runtime::RecompiledFunction g_raymanEffectNew = nullptr, g_raymanEffectUpdate = nullptr;

    float readGuestF32(const uint8_t *rdram, uint32_t address)
    {
        const uint32_t bits = readGuest32(rdram, address);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void logEffectRecord(const char *what, const uint8_t *rdram, R5900Context *ctx, uint32_t index)
    {
        const uint32_t rec = kRaymanEffectRecords + index * 0x60u, gp = getRegU32(ctx, 28);
        std::fprintf(stderr,
                     "[rayman-effect] %s vsync %llu rec %u type=%08x +4=%x start=%g dur=%g alpha=%g +40=%g +44=%g +48=%g "
                     "+50=%x | frame time=%g gp-0x69FC=%g\n",
                     what, static_cast<unsigned long long>(ps2xGuestVsync()), index, readGuest32(rdram, rec),
                     readGuest32(rdram, rec + 4u), readGuestF32(rdram, rec + 0xCu), readGuestF32(rdram, rec + 0x4Cu),
                     readGuestF32(rdram, rec + 0x58u), readGuestF32(rdram, rec + 0x40u), readGuestF32(rdram, rec + 0x44u),
                     readGuestF32(rdram, rec + 0x48u), readGuest32(rdram, rec + 0x50u), readGuestF32(rdram, gp - 0x69F4u),
                     readGuestF32(rdram, gp - 0x69FCu));
    }

    void raymanEffectNewTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        const uint32_t type = getRegU32(ctx, 4);
        const bool log = (type & 0xFFu) != 0x03u && s_logs < 200u; // 0x03: particles (menus, many per frame)
        if (log)
        {
            ++s_logs;
            std::fprintf(stderr, "[rayman-effect] new vsync %llu ra=0x%x type=%08x f12=%g f13=%g f14=%g a1=0x%x a2=0x%x a3=0x%x\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), getRegU32(ctx, 31), type, ctx->f[12],
                         ctx->f[13], ctx->f[14], getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7));
        }
        g_raymanEffectNew(rdram, ctx, runtime);
        if (log)
            std::fprintf(stderr, "[rayman-effect]   -> v0=0x%x count gp-0x6A28=%u\n", getRegU32(ctx, 2),
                         readGuest32(rdram, getRegU32(ctx, 28) - 0x6A28u));
    }

    // 0x1D4FA8: per-frame effect manager (frame time, expiry, compaction, then 0x1D6B50 for each record).
    constexpr uint32_t kRaymanEffectTickAddr = 0x001D4FA8u;
    PS2Runtime::RecompiledFunction g_raymanEffectTick = nullptr;

    // 0x2000A0: the AI procedure "create screen effect" (a0 = actor context, a1 = node; it evaluates its arguments
    // with 0x1DF760 and calls 0x1DA3F0(effect id, pos, pos, a3, f12 = value)). Traced with PS2X_RAYMAN_FADE_TRACE: which
    // actor asks for which effect (first 120 distinct actor / id / value combinations).
    constexpr uint32_t kRaymanFxProcAddr = 0x002000A0u, kRaymanFxDispatchAddr = 0x001DA3F0u;
    PS2Runtime::RecompiledFunction g_raymanFxProc = nullptr, g_raymanFxDispatch = nullptr;
    uint32_t g_raymanFxActor = 0u, g_raymanFxNode = 0u;

    // 0x1DFC30(mind, comport): switches an AI mind to another comport (mind+8 = current, +0xC = previous). Called by
    // 0x1F5588 (procedure 0x3B "change comport", a no-op when the mind already runs the target) and 0x1DFC00.
    constexpr uint32_t kRaymanSetComportAddr = 0x001DFC30u, kRaymanChangeComportAddr = 0x001F5588u;
    PS2Runtime::RecompiledFunction g_raymanSetComport = nullptr, g_raymanChangeComport = nullptr;

    void raymanSetComportTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        const uint32_t mind = getRegU32(ctx, 4), comport = getRegU32(ctx, 5);
        if (ps2xGuestVsync() >= 2880u && s_logs < 600u)
        {
            ++s_logs;
            std::fprintf(stderr, "[rayman-comport] vsync %llu set mind 0x%x comport 0x%x -> 0x%x ra=0x%x\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), mind, readGuest32(rdram, mind + 8u), comport,
                         getRegU32(ctx, 31));
        }
        g_raymanSetComport(rdram, ctx, runtime);
    }

    void raymanChangeComportTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        const uint32_t mind = getRegU32(ctx, 4), comport = getRegU32(ctx, 5);
        if (ps2xGuestVsync() >= 2880u && readGuest32(rdram, mind + 8u) == comport && s_logs < 60u)
        {
            ++s_logs;
            std::fprintf(stderr, "[rayman-comport] vsync %llu change mind 0x%x to its current comport 0x%x (no-op) ra=0x%x\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), mind, comport, getRegU32(ctx, 31));
        }
        g_raymanChangeComport(rdram, ctx, runtime);
    }

    void raymanFxProcTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_raymanFxActor = getRegU32(ctx, 4);
        g_raymanFxNode = getRegU32(ctx, 5);
        g_raymanFxProc(rdram, ctx, runtime);
        g_raymanFxActor = g_raymanFxNode = 0u;
    }

    void raymanFxDispatchTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        struct Seen
        {
            uint32_t actor, id;
            float value;
        };
        static Seen s_seen[120];
        static uint32_t s_count = 0u;
        const uint32_t id = getRegU32(ctx, 4);
        const float value = ctx->f[12];
        bool known = false;
        for (uint32_t i = 0; i < s_count && !known; ++i)
            known = s_seen[i].actor == g_raymanFxActor && s_seen[i].id == id && s_seen[i].value == value;
        if (!known && s_count < 120u)
        {
            s_seen[s_count++] = Seen{g_raymanFxActor, id, value};
            std::fprintf(stderr, "[rayman-fx] vsync %llu effect id 0x%x value %g a3=0x%x ra=0x%x from AI actor 0x%x node 0x%x\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), id, value, getRegU32(ctx, 7), getRegU32(ctx, 31),
                         g_raymanFxActor, g_raymanFxNode);
        }
        g_raymanFxDispatch(rdram, ctx, runtime);
    }

    void raymanEffectTickTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0u, s_logs = 0u, s_lastState = 0xFFFFFFFFu;
        const uint32_t gp = getRegU32(ctx, 28);
        const auto state = [&]()
        {
            return readGuest32(rdram, gp - 0x6A28u) | (static_cast<uint32_t>(rdram[(gp - 0x69E8u) & 0x01FFFFFFu]) << 16) |
                   (static_cast<uint32_t>(rdram[(gp - 0x690Bu) & 0x01FFFFFFu]) << 20) |
                   (static_cast<uint32_t>(rdram[(gp - 0x6A08u) & 0x01FFFFFFu]) << 24);
        };
        const uint32_t before = state();
        g_raymanEffectTick(rdram, ctx, runtime);
        ++s_calls;
        const uint32_t after = state();
        const uint64_t vs = ps2xGuestVsync();
        if (((vs >= 2880u && after != s_lastState) || (vs >= 8340u && vs <= 8380u) || (s_calls % 500u) == 0u) && s_logs < 300u)
        {
            ++s_logs;
            s_lastState = after;
            std::fprintf(stderr, "[rayman-effect] tick #%u vsync %llu count %u->%u flags 69E8=%u 690B=%u 6A08=%u (before %08x)\n",
                         s_calls, static_cast<unsigned long long>(ps2xGuestVsync()), before & 0xFFFFu, after & 0xFFFFu,
                         (after >> 16) & 0xFu, (after >> 20) & 0xFu, after >> 24, before);
            logEffectRecord("  rec0", rdram, ctx, 0u);
        }
    }

    void raymanEffectUpdateTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u, s_calls = 0u;
        const uint32_t index = getRegU32(ctx, 4);
        const uint32_t rec = kRaymanEffectRecords + index * 0x60u;
        const uint32_t typeBefore = readGuest32(rdram, rec), alphaBefore = readGuest32(rdram, rec + 0x58u);
        const uint64_t vs = ps2xGuestVsync();
        const bool fade = (typeBefore & 0xFFu) == 0x11u || (vs >= 8340u && vs <= 8380u); // 0x11: the full-screen fade (drawn by 0x1D5568)
        const bool early = fade && s_calls < 6u;
        if (early)
            logEffectRecord("update before:", rdram, ctx, index);
        g_raymanEffectUpdate(rdram, ctx, runtime);
        if (!fade)
            return;
        ++s_calls;
        const bool changed = readGuest32(rdram, rec) != typeBefore || readGuest32(rdram, rec + 0x58u) != alphaBefore;
        if ((early || changed || (vs >= 8340u && vs <= 8380u) || (s_calls % 600u) == 0u) && s_logs < 400u)
        {
            ++s_logs;
            std::fprintf(stderr, "[rayman-effect] update of rec %u: type before %08x\n", index, typeBefore);
            logEffectRecord(changed ? "update changed:" : "update after: ", rdram, ctx, index);
        }
    }

    // 0x101BF0(perso): one actor's frame. flags = ((perso+4)->+4)->+0x2C; the AI step 0x101A78 (reflex, then intelligence,
    // each N frames: (perso+4)->+0 bytes +0xF2 period / +0xF3 counter) is skipped when (flags & 0x1800) == 0x1800, when
    // flags & 0x10000, or when flags & 0x80000 with bit 7 of byte +0x34. PS2X_RAYMAN_ACTOR_TRACE="fromVsync,toVsync" logs every
    // actor passing through, with those fields (bug hunt: CLAUDE.md issue 7, Rayman's AI does not run).
    constexpr uint32_t kRaymanActorFrameAddr = 0x00101BF0u;
    PS2Runtime::RecompiledFunction g_raymanActorFrame = nullptr;

    // 0x101828: sector change. When the main character's current sector ([[0x252398]+4]+0x20 -> +0) differs from the
    // previous one (gp-0x74C0), the active-actor lists are swapped, the new list is built from the new sector's activity
    // (0x1016B0) and every actor left in the old list is deactivated (0x10BA78). Logged with PS2X_RAYMAN_ACTOR_TRACE.
    constexpr uint32_t kRaymanSectorChangeAddr = 0x00101828u;
    PS2Runtime::RecompiledFunction g_raymanSectorChange = nullptr;

    void raymanSectorChangeTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t before = readGuest32(rdram, gp - 0x74C0u);
        g_raymanSectorChange(rdram, ctx, runtime);
        const uint32_t after = readGuest32(rdram, gp - 0x74C0u);
        if (before != after && s_logs < 200u)
        {
            ++s_logs;
            const uint32_t main = readGuest32(rdram, 0x00252398u), sub = readGuest32(rdram, main + 4u);
            std::fprintf(stderr, "[rayman-sector] vsync %llu main character 0x%x sector 0x%x -> 0x%x at %.2f %.2f %.2f\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), main, before, after, readGuestF32(rdram, sub + 0x74u),
                         readGuestF32(rdram, sub + 0x78u), readGuestF32(rdram, sub + 0x7Cu));
        }
    }

    // 0x214358(perso): the entrance trigger byte ([perso+4]+0x2A) read by AI function 0x2F. In the first level the
    // director turns a code from the global perso 0x96BBD8 (105) into "entrance 5"; the marker actor then teleports Rayman
    // there, out of the intro's sector, and the intro actors are deactivated (CLAUDE.md issue 7).
    // PS2X_RAYMAN_EXPERIMENT_NO_ENTRANCE=<from vsync> makes the trigger read 0 from that guest VSync on (experiment only:
    // does the intro then play? Earlier VSyncs matter: the menus use the same byte).
    constexpr uint32_t kRaymanEntranceByteAddr = 0x00214358u;
    PS2Runtime::RecompiledFunction g_raymanEntranceByte = nullptr;

    void raymanEntranceByteExperiment(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_raymanEntranceByte(rdram, ctx, runtime);
        static const uint64_t fromVsync = std::strtoull(std::getenv("PS2X_RAYMAN_EXPERIMENT_NO_ENTRANCE"), nullptr, 10);
        if (getRegU32(ctx, 2) != 0u && ps2xGuestVsync() >= fromVsync)
        {
            static uint32_t s_logs = 0u;
            if (s_logs++ < 20u)
                std::fprintf(stderr, "[rayman-experiment] vsync %llu entrance trigger %u of perso 0x%x read as 0\n",
                             static_cast<unsigned long long>(ps2xGuestVsync()), getRegU32(ctx, 2), getRegU32(ctx, 4));
            setReturnU32(ctx, 0u);
        }
    }

    // 0x101B20(perso): the mechanics (dynamics) step of one actor: [sub+8] = dynamics, handler from the table at
    // gp-0x74A8 indexed by 0x12B698(type), called with the frame time. With PS2X_RAYMAN_ACTOR_TRACE, every step that
    // moves an actor by more than 10 units is logged (CLAUDE.md issue 7: the entrance marker jumps 60 units at 8346).
    constexpr uint32_t kRaymanMechanicsAddr = 0x00101B20u;
    PS2Runtime::RecompiledFunction g_raymanMechanics = nullptr;

    void raymanMechanicsTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        const uint32_t perso = getRegU32(ctx, 4), sub = readGuest32(rdram, perso + 4u);
        const float x0 = readGuestF32(rdram, sub + 0x74u), y0 = readGuestF32(rdram, sub + 0x78u), z0 = readGuestF32(rdram, sub + 0x7Cu);
        const uint32_t dyn = readGuest32(rdram, sub + 8u);
        g_raymanMechanics(rdram, ctx, runtime);
        const float x1 = readGuestF32(rdram, sub + 0x74u), y1 = readGuestF32(rdram, sub + 0x78u), z1 = readGuestF32(rdram, sub + 0x7Cu);
        const float moved = std::fabs(x1 - x0) + std::fabs(y1 - y0) + std::fabs(z1 - z0);
        if (moved > 10.0f && s_logs < 100u)
        {
            ++s_logs;
            const uint32_t mech = dyn ? readGuest32(rdram, dyn + 0x1Cu) : 0u;
            std::fprintf(stderr,
                         "[rayman-mechanics] vsync %llu perso 0x%x sub 0x%x moved %.2f %.2f %.2f -> %.2f %.2f %.2f | dynamics 0x%x mech 0x%x "
                         "words %08x %08x %08x %08x\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), perso, sub, x0, y0, z0, x1, y1, z1, dyn, mech,
                         mech ? readGuest32(rdram, mech) : 0u, mech ? readGuest32(rdram, mech + 4u) : 0u,
                         mech ? readGuest32(rdram, mech + 8u) : 0u, mech ? readGuest32(rdram, mech + 0xCu) : 0u);
        }
    }

    // Generic write probe: PS2X_RAYMAN_PROBE="addr:func1,func2,..." (hex) wraps up to 8 guest functions and logs every
    // call after which the 3 words at addr differ from before: which function writes a value (the recompiled code has
    // no data watchpoints). Bug hunt tool.
    struct RaymanProbe
    {
        uint32_t addr = 0u;
        uint32_t funcs[8] = {};
        PS2Runtime::RecompiledFunction originals[8] = {};
        uint32_t count = 0u, logs = 0u;
    } g_raymanProbe;

    float bitsToFloat(uint32_t bits)
    {
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    template <int N>
    void raymanProbeTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto &p = g_raymanProbe;
        static bool s_first = true;
        if (s_first)
        {
            s_first = false;
            std::fprintf(stderr, "[rayman-probe] first call of 0x%x at vsync %llu, @%x = %08x\n", p.funcs[N],
                         static_cast<unsigned long long>(ps2xGuestVsync()), p.addr, readGuest32(rdram, p.addr));
        }
        const uint32_t a0 = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
        const uint32_t w0 = readGuest32(rdram, p.addr), w1 = readGuest32(rdram, p.addr + 4u), w2 = readGuest32(rdram, p.addr + 8u);
        p.originals[N](rdram, ctx, runtime);
        const uint32_t v0 = readGuest32(rdram, p.addr), v1 = readGuest32(rdram, p.addr + 4u), v2 = readGuest32(rdram, p.addr + 8u);
        if ((w0 != v0 || w1 != v1 || w2 != v2) && p.logs < 200u)
        {
            ++p.logs;
            std::fprintf(stderr, "[rayman-probe] vsync %llu func 0x%x (a0 0x%x ra 0x%x) changed @%x: %.2f %.2f %.2f -> %.2f %.2f %.2f\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), p.funcs[N], a0, ra, p.addr, bitsToFloat(w0),
                         bitsToFloat(w1), bitsToFloat(w2), bitsToFloat(v0), bitsToFloat(v1), bitsToFloat(v2));
        }
    }

    void installRaymanProbe(PS2Runtime &runtime)
    {
        const char *spec = std::getenv("PS2X_RAYMAN_PROBE");
        if (!spec)
            return;
        static constexpr PS2Runtime::RecompiledFunction kTrampolines[8] = {
            &raymanProbeTrampoline<0>, &raymanProbeTrampoline<1>, &raymanProbeTrampoline<2>, &raymanProbeTrampoline<3>,
            &raymanProbeTrampoline<4>, &raymanProbeTrampoline<5>, &raymanProbeTrampoline<6>, &raymanProbeTrampoline<7>};
        auto &p = g_raymanProbe;
        char *end = nullptr;
        p.addr = static_cast<uint32_t>(std::strtoul(spec, &end, 16));
        while (end && *end && p.count < 8u)
        {
            const uint32_t func = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
            if (func == 0u || !runtime.hasFunction(func))
                continue;
            p.funcs[p.count] = func;
            p.originals[p.count] = runtime.lookupFunction(func);
            if (runtime.registerFunction(func, kTrampolines[p.count]))
                std::cerr << "[hle] Rayman: write probe on 0x" << std::hex << func << " for 0x" << p.addr << std::dec << std::endl;
            ++p.count;
        }
    }

    // 0x1F5910: AI procedures 0xA6-0xA8 (0xA6(n, v): bit n-1 of an actor's flags word, engine+0x2C). With
    // PS2X_RAYMAN_ACTOR_TRACE, every call is logged with the flags of Rayman (engine 0x9711B0 in the first level) before
    // and after, the calling actor context and the node.
    constexpr uint32_t kRaymanProcFlagsAddr = 0x001F5910u;
    PS2Runtime::RecompiledFunction g_raymanProcFlags = nullptr;

    void raymanProcFlagsTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_logs = 0u;
        const uint32_t actor = getRegU32(ctx, 4), node = getRegU32(ctx, 5);
        const uint32_t before = readGuest32(rdram, 0x009711B0u + 0x2Cu);
        g_raymanProcFlags(rdram, ctx, runtime);
        const uint32_t after = readGuest32(rdram, 0x009711B0u + 0x2Cu);
        const uint64_t vsync = ps2xGuestVsync();
        if ((before != after || (vsync >= 8300u && vsync <= 8400u)) && s_logs < 300u)
        {
            ++s_logs;
            std::fprintf(stderr, "[rayman-flags] vsync %llu proc 0x%x node 0x%x (args %x %x | %x %x) from actor 0x%x: rayman flags %08x -> %08x\n",
                         static_cast<unsigned long long>(ps2xGuestVsync()), readGuest32(rdram, node - 8u) & 0xFFFFu, node,
                         readGuest32(rdram, node), readGuest32(rdram, node + 4u), readGuest32(rdram, node + 8u),
                         readGuest32(rdram, node + 12u), actor, before, after);
        }
    }

    void raymanActorFrameTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const auto window = []()
        {
            std::array<unsigned long long, 2> w{0u, 0u};
            if (const char *text = std::getenv("PS2X_RAYMAN_ACTOR_TRACE"))
                std::sscanf(text, "%llu,%llu", &w[0], &w[1]);
            return w;
        }();
        const uint64_t vsync = ps2xGuestVsync();
        if (vsync >= window[0] && vsync <= window[1])
        {
            const uint32_t perso = getRegU32(ctx, 4);
            const uint32_t sub = readGuest32(rdram, perso + 4u);
            const uint32_t stdGame = readGuest32(rdram, sub), engine = readGuest32(rdram, sub + 4u);
            const uint32_t flags = readGuest32(rdram, engine + 0x2Cu), byte34 = readGuest32(rdram, engine + 0x34u) & 0xFFu;
            const uint32_t periods = readGuest32(rdram, stdGame + 0xF0u);
            // Position: the translation of the perso's matrix at sub+0x74 (x, y, z; Z up).
            std::fprintf(stderr,
                         "[rayman-actor] vsync %llu perso 0x%x sub 0x%x std 0x%x (period %u counter %d) engine 0x%x flags %08x byte34 %02x "
                         "pos %.2f %.2f %.2f brain 0x%x -> AI %s\n",
                         static_cast<unsigned long long>(vsync), perso, sub, stdGame, (periods >> 16) & 0xFFu,
                         static_cast<int8_t>(periods >> 24), engine, flags, byte34, readGuestF32(rdram, sub + 0x74u),
                         readGuestF32(rdram, sub + 0x78u), readGuestF32(rdram, sub + 0x7Cu), readGuest32(rdram, sub + 0xCu),
                         ((flags & 0x1800u) == 0x1800u || (flags & 0x10000u) != 0u || ((flags & 0x80000u) != 0u && (byte34 & 0x80u) != 0u))
                             ? "skipped"
                             : "runs");
        }
        g_raymanActorFrame(rdram, ctx, runtime);
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

    void raymanStateSample(const uint8_t *rdram);

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
        raymanStateSample(rdram);
        RaymanNodeTrace &slot = g_raymanNodeRing[g_raymanNodeRingPos++ % 64u];
        slot.ra = getRegU32(ctx, 31);
        slot.actor = getRegU32(ctx, 4);
        slot.node = node;
        slot.type = node >= 0x00100000u ? (readGuest32(rdram, node + 4u) >> 24) : 0xFFu;

        // PS2X_RAYMAN_AI_WATCH="lo,hi,fromVsync,toVsync[,addr]": log every node in [lo, hi) dispatched between those guest
        // VSyncs (node, type, depth, param, a2 result buffer before / after, next node, and the 3 floats at addr after the
        // node, e.g. an actor's position). Bug hunt: which branch of a script runs, what its conditions return, and which
        // node changes a value (CLAUDE.md issue 7).
        static const auto watch = []()
        {
            std::array<uint64_t, 5> w{0u, 0u, 0u, 0u, 0u};
            if (const char *text = std::getenv("PS2X_RAYMAN_AI_WATCH"))
                std::sscanf(text, "%llx,%llx,%llu,%llu,%llx", reinterpret_cast<unsigned long long *>(&w[0]),
                            reinterpret_cast<unsigned long long *>(&w[1]), reinterpret_cast<unsigned long long *>(&w[2]),
                            reinterpret_cast<unsigned long long *>(&w[3]), reinterpret_cast<unsigned long long *>(&w[4]));
            return w;
        }();
        const uint64_t vsync = watch[1] != 0u ? ps2xGuestVsync() : 0u;
        if (watch[1] != 0u && node >= watch[0] && node < watch[1] && vsync >= watch[2] && vsync <= watch[3])
        {
            const uint32_t result = getRegU32(ctx, 6), word = readGuest32(rdram, node + 4u);
            const uint32_t before0 = readGuest32(rdram, result), before1 = readGuest32(rdram, result + 4u);
            g_raymanNodeDispatch(rdram, ctx, runtime);
            // Result record at a2 (often in the scratchpad): +0 value, +0xC value type (6 = integer / boolean).
            const uint32_t at = static_cast<uint32_t>(watch[4]);
            std::fprintf(stderr, "[rayman-ai-watch] vsync %llu actor 0x%x node 0x%x type %u depth %u param 0x%x | result@%x %08x %08x -> %08x %08x (kind %u) | next 0x%x | @%x %.2f %.2f %.2f (%08x %08x %08x)\n",
                         static_cast<unsigned long long>(vsync), slot.actor, node, word >> 24, (word >> 16) & 0xFFu,
                         readGuest32(rdram, node), result, before0, before1, readGuest32(rdram, result),
                         readGuest32(rdram, result + 4u), readGuest32(rdram, result + 0xCu) & 0xFFu, getRegU32(ctx, 2), at,
                         at ? readGuestF32(rdram, at) : 0.0f, at ? readGuestF32(rdram, at + 4u) : 0.0f,
                         at ? readGuestF32(rdram, at + 8u) : 0.0f, at ? readGuest32(rdram, at) : 0u,
                         at ? readGuest32(rdram, at + 4u) : 0u, at ? readGuest32(rdram, at + 8u) : 0u);
            return;
        }
        g_raymanNodeDispatch(rdram, ctx, runtime);
    }

    // Rayman's swim state, read from game memory (CLAUDE.md issue 13). Main character superobject [0x252398];
    // position = its global matrix [so+0x24] + 0x30. Perso data [so+4]: +0x8 dynam -> dynamics [dynam+0x1C]
    // (flags +0xC, 0x800000 = on ground; the water surface counts as ground), +0xC brain -> mind [brain]:
    // +0 AI model (+0 normal / +4 reflex behaviour list: {table, count}, 12-byte entries), +4 / +8 normal / reflex
    // intelligence (+8 current behaviour), +0xC dsgmem -> current dsgvar values [dsgmem+8]: +0x74 water plane
    // height (32767 = no plane found by the ray from the head up), +0x60 depth below it, +0x64 ms in the current
    // swim / move mode, byte +0x217 = 1 at the surface, 0 diving. Behaviour 5 = swimming. Air (0..100, the bar
    // shown while diving) is the float at 0x380B80; it falls 4 per second while diving and is 100 again at the
    // surface.
    struct RaymanState
    {
        bool valid = false;
        uint32_t so = 0u;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        int behaviour = -1, reflex = -1;
        uint32_t dynFlags = 0u;
        bool hasDsg = false;
        float water = 0.0f, depth = 0.0f, air = 0.0f;
        int surfaceFlag = -1;
        const char *state = "none";
    };

    RaymanState readRaymanState(const uint8_t *rdram)
    {
        RaymanState s;
        auto inRam = [](uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; };
        auto rd32 = [&](uint32_t a) -> uint32_t
        {
            uint32_t v = 0u;
            std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4u);
            return v;
        };
        auto rdf = [&](uint32_t a) -> float
        {
            float v = 0.0f;
            std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4u);
            return v;
        };
        s.so = rd32(0x00252398u);
        if (!inRam(s.so) || !inRam(rd32(s.so + 0x24u)) || !inRam(rd32(s.so + 4u)))
            return s;
        const uint32_t matrix = rd32(s.so + 0x24u), data = rd32(s.so + 4u);
        s.x = rdf(matrix + 0x30u);
        s.y = rdf(matrix + 0x34u);
        s.z = rdf(matrix + 0x38u);
        s.air = rdf(0x00380B80u);
        const uint32_t dynam = rd32(data + 8u), dynamics = inRam(dynam) ? rd32(dynam + 0x1Cu) : 0u;
        s.dynFlags = inRam(dynamics) ? rd32(dynamics + 0xCu) : 0u;
        const uint32_t brain = rd32(data + 0xCu), mind = inRam(brain) ? rd32(brain) : 0u;
        if (inRam(mind))
        {
            const uint32_t model = rd32(mind);
            auto behaviourIndex = [&](uint32_t list, uint32_t intel) -> int
            {
                if (!inRam(list) || !inRam(intel))
                    return -1;
                const uint32_t table = rd32(list), count = rd32(list + 4u), current = rd32(intel + 8u);
                if (current < table || (current - table) % 12u != 0u || (current - table) / 12u >= count)
                    return -1;
                return static_cast<int>((current - table) / 12u);
            };
            if (inRam(model))
            {
                s.behaviour = behaviourIndex(rd32(model), rd32(mind + 4u));
                s.reflex = behaviourIndex(rd32(model + 4u), rd32(mind + 8u));
            }
            const uint32_t dsgmem = rd32(mind + 0xCu), cur = inRam(dsgmem) ? rd32(dsgmem + 8u) : 0u;
            if (inRam(cur))
            {
                s.hasDsg = true;
                s.water = rdf(cur + 0x74u);
                s.depth = rdf(cur + 0x60u);
                s.surfaceFlag = rdram[(cur + 0x217u) & 0x01FFFFFFu];
            }
        }
        // Dynamics flags seen (2026-11-09, Minisaurus Plain): 0x0180E007 walking, 0x0300E007 in the air on land,
        // 0x01906007 swimming, 0x01806007 jumping out of the water (behaviours 35 -> 26 -> 4). 0x8000 is only set
        // with the land mechanics; 0x800000 stays set during the jump out of the water, so it means "on ground"
        // only with the land mechanics.
        if (s.behaviour == 5)
            s.state = s.surfaceFlag == 0 ? "diving" : "surface";
        else if ((s.dynFlags & 0x00008000u) == 0u)
            s.state = "air";
        else
            s.state = ((s.dynFlags & 0x00800000u) != 0u && (s.dynFlags & 0x02000000u) == 0u) ? "ground" : "air";
        s.valid = true;
        return s;
    }

    // Appended to the `[threads]` line.
    int raymanGameStatsLine(const uint8_t *rdram, char *buffer, int size)
    {
        const RaymanState s = readRaymanState(rdram);
        if (!s.valid)
            return 0;
        return std::snprintf(buffer, static_cast<size_t>(size),
                             "rayman state=%s behaviour=%d air=%.2f water=%.3f depth=%.3f pos=%.2f,%.2f,%.2f", s.state,
                             s.behaviour, s.air, s.water, s.depth, s.x, s.y, s.z);
    }

    // `[rayman-state]` line whenever the state or the air trend (falling / steady / rising) changes, sampled once per
    // guest VSync from the AI node dispatcher. The [threads] line is too sparse for short states (a jump).
    void raymanStateSample(const uint8_t *rdram)
    {
        static uint64_t lastVsync = ~0ull;
        const uint64_t vsync = ps2xGuestVsync();
        if (vsync == lastVsync)
            return;
        lastVsync = vsync;
        const RaymanState s = readRaymanState(rdram);
        static const char *lastState = "";
        static const char *lastTrend = "";
        static float lastAir = 0.0f;
        static uint32_t lastSo = 0u;
        if (!s.valid)
            return;
        const char *trend = "steady";
        if (s.so == lastSo && s.air < lastAir)
            trend = "falling";
        else if (s.so == lastSo && s.air > lastAir)
            trend = "rising";
        lastAir = s.air;
        lastSo = s.so;
        if (std::strcmp(s.state, lastState) == 0 && std::strcmp(trend, lastTrend) == 0)
            return;
        std::fprintf(stderr, "[rayman-state] vsync=%llu state=%s from=%s air=%.2f trend=%s behaviour=%d dyn=%08x water=%.3f depth=%.3f pos=%.2f,%.2f,%.2f\n",
                     static_cast<unsigned long long>(vsync), s.state, *lastState ? lastState : "-", s.air, trend, s.behaviour,
                     s.dynFlags, s.water, s.depth, s.x, s.y, s.z);
        lastState = s.state;
        lastTrend = trend;
    }

    // PS2X_RAYMAN_TRACK="from,to[,step]": a [rayman-track] line every step script units (default 5) between from and
    // to: Rayman's position, state and air, and the game clock. Script time is 20 ms per unit at every PS2X_FRAMERATE
    // (deterministic runs, see the harness), so two frame rates can be compared at the same real time.
    void raymanTrackHook(uint64_t scriptTime, uint64_t vsync, const uint8_t *rdram)
    {
        struct Config
        {
            bool on = false;
            uint64_t from = 0, to = 0, step = 5;
        };
        static const Config cfg = []
        {
            Config c;
            if (const char *env = std::getenv("PS2X_RAYMAN_TRACK"))
            {
                unsigned long long a = 0, b = 0, step = 5;
                if (std::sscanf(env, "%llu,%llu,%llu", &a, &b, &step) >= 2 && b >= a && step > 0)
                    c = Config{true, a, b, step};
            }
            return c;
        }();
        static uint64_t next = 0;
        if (!cfg.on || scriptTime < cfg.from || scriptTime > cfg.to)
            return;
        if (next < cfg.from)
            next = cfg.from;
        if (scriptTime < next)
            return;
        while (next <= scriptTime)
            next += cfg.step;
        // Wall-clock ms since the first line: against game_ms it shows whether game time keeps real time (normal play).
        static const auto hostStart = std::chrono::steady_clock::now();
        const int64_t hostMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hostStart).count();
        const RaymanState s = readRaymanState(rdram);
        const uint32_t clock = raymanRam32(rdram, 0x0039A3F0u - 0x7108u); // [gp-0x7108]: the frame clock
        uint32_t gameMs = 0u, frames = 0u, n = 0u;
        float gameSeconds = 0.0f;
        if (clock >= 0x00100000u && clock < 0x02000000u)
        {
            gameMs = raymanRam32(rdram, clock + 0x14u);
            n = raymanRam32(rdram, clock + 0x24u);
            frames = raymanRam32(rdram, clock + 0x28u);
            std::memcpy(&gameSeconds, rdram + clock + 0x30u, sizeof(gameSeconds));
        }
        std::fprintf(stderr, "[rayman-track] t=%llu ms=%llu vsync=%llu state=%s behaviour=%d pos=%.3f,%.3f,%.3f air=%.2f "
                             "game_ms=%u game_s=%.3f frames=%u n=%u host_ms=%lld\n",
                     static_cast<unsigned long long>(scriptTime), static_cast<unsigned long long>(scriptTime * 20u),
                     static_cast<unsigned long long>(vsync), s.state, s.behaviour, s.x, s.y, s.z, s.air, gameMs,
                     gameSeconds, frames, n, static_cast<long long>(hostMs));
    }

    void installGameHle(PS2Runtime &runtime, const std::string &gameId)
    {
        if (gameId != "SLES-50044")
            return;
        ps2xSetGameStatsLine(&raymanGameStatsLine);
        ps2xSetGuestVsyncHook(&raymanTrackHook);

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

        installRaymanFramerate(runtime, rdram);
        installRaymanWidescreen(runtime, rdram);

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

        installRaymanProbe(runtime);

        uint32_t entranceWord = 0u;
        std::memcpy(&entranceWord, rdram + kRaymanEntranceByteAddr, sizeof(entranceWord));
        // lw v1,4(a0)
        if (std::getenv("PS2X_RAYMAN_EXPERIMENT_NO_ENTRANCE") && entranceWord == 0x8C830004u &&
            runtime.hasFunction(kRaymanEntranceByteAddr))
        {
            g_raymanEntranceByte = runtime.lookupFunction(kRaymanEntranceByteAddr);
            if (g_raymanEntranceByte && runtime.registerFunction(kRaymanEntranceByteAddr, &raymanEntranceByteExperiment))
                std::cerr << "[hle] Rayman: EXPERIMENT entrance trigger forced to 0 at 0x214358" << std::endl;
        }

        uint32_t actorFrameWord = 0u;
        std::memcpy(&actorFrameWord, rdram + kRaymanActorFrameAddr, sizeof(actorFrameWord));
        // addiu sp,sp,-0x20
        if (std::getenv("PS2X_RAYMAN_ACTOR_TRACE") && actorFrameWord == 0x27BDFFE0u && runtime.hasFunction(kRaymanActorFrameAddr))
        {
            g_raymanActorFrame = runtime.lookupFunction(kRaymanActorFrameAddr);
            if (g_raymanActorFrame && runtime.registerFunction(kRaymanActorFrameAddr, &raymanActorFrameTrace))
                std::cerr << "[hle] Rayman: actor frame trace installed at 0x101BF0" << std::endl;
            uint32_t mechanicsWord = 0u;
            std::memcpy(&mechanicsWord, rdram + kRaymanMechanicsAddr, sizeof(mechanicsWord));
            // addiu sp,sp,-0x60
            if (mechanicsWord == 0x27BDFFA0u && runtime.hasFunction(kRaymanMechanicsAddr))
            {
                g_raymanMechanics = runtime.lookupFunction(kRaymanMechanicsAddr);
                if (g_raymanMechanics && runtime.registerFunction(kRaymanMechanicsAddr, &raymanMechanicsTrace))
                    std::cerr << "[hle] Rayman: mechanics step trace installed at 0x101B20" << std::endl;
            }
            uint32_t sectorWord = 0u;
            std::memcpy(&sectorWord, rdram + kRaymanSectorChangeAddr, sizeof(sectorWord));
            // addiu sp,sp,-0x20
            if (sectorWord == 0x27BDFFE0u && runtime.hasFunction(kRaymanSectorChangeAddr))
            {
                g_raymanSectorChange = runtime.lookupFunction(kRaymanSectorChangeAddr);
                if (g_raymanSectorChange && runtime.registerFunction(kRaymanSectorChangeAddr, &raymanSectorChangeTrace))
                    std::cerr << "[hle] Rayman: sector change trace installed at 0x101828" << std::endl;
            }
            uint32_t procWord = 0u;
            std::memcpy(&procWord, rdram + kRaymanProcFlagsAddr, sizeof(procWord));
            // addiu sp,sp,-0x70
            if (procWord == 0x27BDFF90u && runtime.hasFunction(kRaymanProcFlagsAddr))
            {
                g_raymanProcFlags = runtime.lookupFunction(kRaymanProcFlagsAddr);
                if (g_raymanProcFlags && runtime.registerFunction(kRaymanProcFlagsAddr, &raymanProcFlagsTrace))
                    std::cerr << "[hle] Rayman: flag procedure trace installed at 0x1F5910" << std::endl;
            }
        }

        if (std::getenv("PS2X_RAYMAN_MUSIC_TRACE"))
        {
            uint32_t words[3] = {};
            std::memcpy(&words[0], rdram + kRaymanAdpcmIsPlayingAddr, 4u);
            std::memcpy(&words[1], rdram + kRaymanAdpcmPushAddr, 4u);
            std::memcpy(&words[2], rdram + kRaymanAdpcmEventAddr, 4u);
            // addiu sp,sp,-0x90 / -0x80 / -0x30
            if (words[0] == 0x27BDFF70u && words[1] == 0x27BDFF80u && words[2] == 0x27BDFFD0u &&
                runtime.hasFunction(kRaymanAdpcmIsPlayingAddr) && runtime.hasFunction(kRaymanAdpcmPushAddr) &&
                runtime.hasFunction(kRaymanAdpcmEventAddr))
            {
                g_raymanAdpcmIsPlaying = runtime.lookupFunction(kRaymanAdpcmIsPlayingAddr);
                g_raymanAdpcmPush = runtime.lookupFunction(kRaymanAdpcmPushAddr);
                g_raymanAdpcmEvent = runtime.lookupFunction(kRaymanAdpcmEventAddr);
                if (runtime.registerFunction(kRaymanAdpcmIsPlayingAddr, &raymanAdpcmIsPlayingTrace) &&
                    runtime.registerFunction(kRaymanAdpcmPushAddr, &raymanAdpcmPushTrace) &&
                    runtime.registerFunction(kRaymanAdpcmEventAddr, &raymanAdpcmEventTrace))
                    std::cerr << "[hle] Rayman: ADPCM stream trace installed at 0x169320 / 0x1697A0 / 0x168CB8" << std::endl;
            }
        }

        uint32_t fadeEntryWord = 0u;
        std::memcpy(&fadeEntryWord, rdram + kRaymanFadeQuadAddr, sizeof(fadeEntryWord));
        // lwc1 $f2, 0xC($a2)
        if (std::getenv("PS2X_RAYMAN_FADE_TRACE") && fadeEntryWord == 0xC4C2000Cu && runtime.hasFunction(kRaymanFadeQuadAddr))
        {
            g_raymanFadeQuad = runtime.lookupFunction(kRaymanFadeQuadAddr);
            if (g_raymanFadeQuad && runtime.registerFunction(kRaymanFadeQuadAddr, &raymanFadeQuadTrace))
                std::cerr << "[hle] Rayman: fade quad trace installed at 0x155698" << std::endl;
            uint32_t newWord = 0u, updateWord = 0u;
            std::memcpy(&newWord, rdram + kRaymanEffectNewAddr, sizeof(newWord));
            std::memcpy(&updateWord, rdram + kRaymanEffectUpdateAddr, sizeof(updateWord));
            // addiu sp,sp,-0xC0 / addiu v1,zero,0x60
            if (newWord == 0x27BDFF40u && updateWord == 0x24030060u && runtime.hasFunction(kRaymanEffectNewAddr) &&
                runtime.hasFunction(kRaymanEffectUpdateAddr))
            {
                g_raymanEffectNew = runtime.lookupFunction(kRaymanEffectNewAddr);
                g_raymanEffectUpdate = runtime.lookupFunction(kRaymanEffectUpdateAddr);
                if (runtime.registerFunction(kRaymanEffectNewAddr, &raymanEffectNewTrace) &&
                    runtime.registerFunction(kRaymanEffectUpdateAddr, &raymanEffectUpdateTrace))
                    std::cerr << "[hle] Rayman: screen effect trace installed at 0x1D46A0 / 0x1D6B50" << std::endl;
            }
            uint32_t setWord = 0u, changeWord = 0u;
            std::memcpy(&setWord, rdram + kRaymanSetComportAddr, sizeof(setWord));
            std::memcpy(&changeWord, rdram + kRaymanChangeComportAddr, sizeof(changeWord));
            // addiu sp,sp,-0x30 / addiu sp,sp,-0x20
            if (setWord == 0x27BDFFD0u && changeWord == 0x27BDFFE0u && runtime.hasFunction(kRaymanSetComportAddr) &&
                runtime.hasFunction(kRaymanChangeComportAddr))
            {
                g_raymanSetComport = runtime.lookupFunction(kRaymanSetComportAddr);
                g_raymanChangeComport = runtime.lookupFunction(kRaymanChangeComportAddr);
                if (runtime.registerFunction(kRaymanSetComportAddr, &raymanSetComportTrace) &&
                    runtime.registerFunction(kRaymanChangeComportAddr, &raymanChangeComportTrace))
                    std::cerr << "[hle] Rayman: comport change trace installed at 0x1DFC30 / 0x1F5588" << std::endl;
            }
            uint32_t procWord = 0u, dispatchWord = 0u;
            std::memcpy(&procWord, rdram + kRaymanFxProcAddr, sizeof(procWord));
            std::memcpy(&dispatchWord, rdram + kRaymanFxDispatchAddr, sizeof(dispatchWord));
            // addiu sp,sp,-0x70 / lui at,0x3F80
            if (procWord == 0x27BDFF90u && dispatchWord == 0x3C013F80u && runtime.hasFunction(kRaymanFxProcAddr) &&
                runtime.hasFunction(kRaymanFxDispatchAddr))
            {
                g_raymanFxProc = runtime.lookupFunction(kRaymanFxProcAddr);
                g_raymanFxDispatch = runtime.lookupFunction(kRaymanFxDispatchAddr);
                if (runtime.registerFunction(kRaymanFxProcAddr, &raymanFxProcTrace) &&
                    runtime.registerFunction(kRaymanFxDispatchAddr, &raymanFxDispatchTrace))
                    std::cerr << "[hle] Rayman: AI effect procedure trace installed at 0x2000A0 / 0x1DA3F0" << std::endl;
            }
            uint32_t tickWord = 0u;
            std::memcpy(&tickWord, rdram + kRaymanEffectTickAddr, sizeof(tickWord));
            // lui v0,0x25
            if (tickWord == 0x3C020025u && runtime.hasFunction(kRaymanEffectTickAddr))
            {
                g_raymanEffectTick = runtime.lookupFunction(kRaymanEffectTickAddr);
                if (runtime.registerFunction(kRaymanEffectTickAddr, &raymanEffectTickTrace))
                    std::cerr << "[hle] Rayman: screen effect manager trace installed at 0x1D4FA8" << std::endl;
            }
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
    // PS2X_VU1AOT_REPLAY=<out/vu1aot_mismatch_*.bin>: replay a native-vs-interpreter mismatch offline, no game.
    if (const char *replay = std::getenv("PS2X_VU1AOT_REPLAY"); replay && *replay)
        return ps2xVu1AotReplay(replay);

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
        ps2xVu1ProfileFlush();

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
