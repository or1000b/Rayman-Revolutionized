#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"
#include "gs/pgs/gs_pgs_bridge.h"
#include "gs/pgs/gs_hud.h"
#include "rlgl.h"
#include "ps2_gfx_thread.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <string>

extern "C" uint64_t ps2xGuestVsync(); // ps2_test_harness.cpp

void ps2xDebugDumpSemaphoreHistory(int id); // EeScheduler.cpp (Rayman bring-up)
extern "C" void ps2xRequestGsCapture();         // gs_cpu_backend.cpp
extern "C" void ps2xRequestVu1Capture();        // ps2_vu1_core.cpp: F11, every VU1 run of the next VSyncs
extern "C" void ps2xRequestEeDump();            // ps2_test_harness.cpp: F11, EE RAM at the next VSync
extern "C" int ps2xGfxStatsLine(char *out, int size); // ps2_gfx_thread.inc: graphics thread busy %
extern "C" uint64_t ps2xGameIdleNs(); // EeScheduler.cpp: time the game thread slept waiting for VBlank
extern "C" uint64_t ps2xGuestDisplayFlips();       // EeScheduler.cpp: display flips counted at guest VBlanks
extern "C" uint32_t ps2xVBlankRateOverrideHz();    // EeScheduler.cpp: PS2X_FRAMERATE (0 = unset)
extern "C" int ps2xGsStatsLine(char *buffer, int size);
extern "C" int ps2xSpu2ReadSamples(int16_t *out, int frames); // ps2xIOP SPU2 output ring
extern "C" int ps2xSpu2StatsLine(char *buffer, int size);
extern "C" void ps2xTestHarnessStart();          // ps2_test_harness.cpp
extern "C" bool ps2xTestHarnessTick(uint64_t tick);
extern "C" void ps2xTestHarnessExiting();
extern "C" void ps2xTestThreadStackGuarantee();
extern "C" void ps2xTestHarnessFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t rowPixels);
extern "C" uint64_t ps2xTestLastShotTick();     // ps2_test_harness.cpp: the tick of the last test_shot (0 = none)
void ps2xInstallIopToEeRpcHooks(PS2Runtime *runtime);       // Kernel/Syscalls/RPC.cpp
void ps2xInstallIopCdReader(bool install);                  // Kernel/Stubs/CD.cpp
int ps2xIopToEeRpcStatsLine(char *buffer, int size);

// Game-specific part of the `[threads]` line (main.cpp registers one per game, e.g. Rayman's swim state).
using Ps2xGameStatsLineFn = int (*)(const uint8_t *rdram, char *buffer, int size);
static Ps2xGameStatsLineFn g_gameStatsLine = nullptr;
extern "C" void ps2xSetGameStatsLine(Ps2xGameStatsLineFn fn)
{
    g_gameStatsLine = fn;
}

namespace
{
#if !defined(PLATFORM_VITA)
    // Host output for the emulated SPU2 (48 kHz stereo, pulled by the audio thread).
    AudioStream g_spu2Stream{};
    bool g_spu2StreamActive = false;

    void spu2StreamCallback(void *buffer, unsigned int frames)
    {
        ps2xSpu2ReadSamples(static_cast<int16_t *>(buffer), static_cast<int>(frames));
    }

    void startSpu2Stream()
    {
        if (g_spu2StreamActive || !IsAudioDeviceReady())
            return;
        g_spu2Stream = LoadAudioStream(48000, 16, 2);
        if (!IsAudioStreamValid(g_spu2Stream))
            return;
        SetAudioStreamCallback(g_spu2Stream, spu2StreamCallback);
        PlayAudioStream(g_spu2Stream);
        g_spu2StreamActive = true;
    }

    void stopSpu2Stream()
    {
        if (!g_spu2StreamActive)
            return;
        StopAudioStream(g_spu2Stream);
        UnloadAudioStream(g_spu2Stream);
        g_spu2StreamActive = false;
    }
#endif
}

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
// The VU1 cycle budget of one MSCAL / MSCNT run (PS2X_VU1_BUDGET, default 4194304). The VU runs a program to its E-bit;
// the VIF waits at the next MSCAL / MSCNT until it has. The budget used to be 65536: a longer run stopped in the
// middle, the next MSCAL started over (the rest of the program was lost) and the next MSCNT finished it instead of
// what it asked for. Big polygons near the camera go through the clipper and make long runs: that was CLAUDE.md issue
// 19 (missing walls, the clear colour behind; the bridge in the Minisaurus Plain cut one run every frame), and every
// cut run went to the interpreter (bridge: ~100 -> ~120 FPS). The budget is now only a guard against a runaway
// program.
static uint32_t vu1RunBudget()
{
    static const uint32_t budget = []
    {
        const char *env = std::getenv("PS2X_VU1_BUDGET");
        const unsigned long v = env ? std::strtoul(env, nullptr, 0) : 0ul;
        return v >= 1024ul && v <= (1ul << 30) ? static_cast<uint32_t>(v) : 4194304u;
    }();
    return budget;
}

static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    // Rayman bring-up: tiny sampling profiler (where does the game thread spend its time?).
    std::atomic<uint32_t> g_profLastDispatchPc{0};
    std::atomic<int> g_profActivity{0}; // 0 = EE code, 1 = VU1 microprogram, 2 = GS packet, 3 = IOP

    struct ProfActivityScope
    {
        int previous;
        explicit ProfActivityScope(int activity) : previous(g_profActivity.exchange(activity, std::memory_order_relaxed)) {}
        ~ProfActivityScope() { g_profActivity.store(previous, std::memory_order_relaxed); }
    };

    void pushDispatchPc(uint32_t pc)
    {
        g_profLastDispatchPc.store(pc, std::memory_order_relaxed);
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }
}

// Called by the unhandled-exception filter (main.cpp) on the crashing thread.
extern "C" void ps2xDumpCrashContext()
{
    const char *activity[] = {"EE code", "VU1 microprogram", "GS packet", "IOP"};
    const int act = g_profActivity.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[crash] guest: last dispatched EE function 0x%x, EE thread activity: %s\n",
                 g_profLastDispatchPc.load(std::memory_order_relaxed), (act >= 0 && act < 4) ? activity[act] : "?");
    std::fprintf(stderr, "[crash] this thread's dispatch history: %s\n", formatDispatchHistory().c_str());
    std::fflush(stderr);
}

namespace
{


    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// Window settings (environment, read once): PS2X_FILTER=nearest keeps sharp pixels when the picture is scaled
// (default bilinear); PS2X_ASPECT=4:3 (default: the PS2's TV picture; PS2X_WIDESCREEN's aspect when that is set),
// 16:9, stretch (fill the window) or pixel (the
// frame's own pixel ratio, the old look: 640 x 512 at 1.25); PS2X_WINDOW=<width>x<height> (default 640x448);
// PS2X_FULLSCREEN=1 (borderless at the monitor's resolution; F10 switches); PS2X_SHOW_FPS=0 hides the counter.
// The game's 2D layer drawn by the window (gs_hud.h): the HUD of the frame on screen, its textures (a 4x one from
// hud_textures/<key>.png when present; PS2X_HUD_DUMP=1 writes the originals to hud_dump/ for tools/hud_upscale.py).
namespace
{
    ps2x::hud::Frame g_hudFrame;
    std::unordered_map<uint64_t, Texture2D> g_hudTextures;

    bool hudDumpWanted()
    {
        static const bool on = []
        {
            const char *env = std::getenv("PS2X_HUD_DUMP");
            return env && *env && *env != '0';
        }();
        return on;
    }

    Texture2D hudTexture(uint64_t key)
    {
        const auto found = g_hudTextures.find(key);
        if (found != g_hudTextures.end())
            return found->second;
        Texture2D tex{};
        char path[96];
        std::snprintf(path, sizeof(path), "hud_textures/%016llx.png", static_cast<unsigned long long>(key));
        if (FileExists(path))
            tex = LoadTexture(path);
        if (tex.id == 0 || hudDumpWanted())
        {
            ps2x::hud::Texture src;
            if (ps2x::hud::texture(key, src))
            {
                Image image{src.rgba.data(), static_cast<int>(src.width), static_cast<int>(src.height), 1,
                            PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                if (tex.id == 0)
                    tex = LoadTextureFromImage(image);
                if (hudDumpWanted())
                {
                    std::error_code ec;
                    std::filesystem::create_directories("hud_dump", ec);
                    char dump[96];
                    std::snprintf(dump, sizeof(dump), "hud_dump/%016llx.png", static_cast<unsigned long long>(key));
                    if (!FileExists(dump))
                        ExportImage(image, dump);
                }
            }
        }
        if (tex.id != 0)
        {
            SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
        }
        g_hudTextures.emplace(key, tex);
        return tex;
    }

    // Over the picture drawn at dst: game coordinates 0..512 span dst. Wider than 4:3, the 2D keeps its 4:3 shape
    // around the centre (full-width quads, such as fades, stay full width).
    void drawHudOverlay(const Rectangle &dst)
    {
        if (g_hudFrame.batches.empty() || dst.height <= 0.0f)
            return;
        const float squeeze = std::min(1.0f, (4.0f / 3.0f) / (dst.width / dst.height));
        rlDrawRenderBatchActive();
        rlDisableBackfaceCulling();
        for (const ps2x::hud::Batch &batch : g_hudFrame.batches)
        {
            unsigned int id = rlGetTextureIdDefault();
            if (batch.texture != 0u)
            {
                const Texture2D t = hudTexture(batch.texture);
                if (t.id == 0)
                    continue;
                id = t.id;
            }
            BeginBlendMode(batch.blend == ps2x::hud::BlendAdd ? BLEND_ADDITIVE : BLEND_ALPHA);
            for (size_t first = 0; first < batch.vertices.size(); first += 3000u)
            {
                const size_t count = std::min<size_t>(3000u, batch.vertices.size() - first);
                rlCheckRenderBatchLimit(static_cast<int>(count));
                rlSetTexture(id);
                rlBegin(RL_TRIANGLES);
                rlSetTexture(id); // rlBegin with another mode than the last draw resets the draw's texture to the default
                for (size_t i = first; i < first + count; ++i)
                {
                    const ps2x::hud::Vertex &v = batch.vertices[i];
                    const float gx = v.x / 512.0f, gy = v.y / 512.0f;
                    const float sx = batch.fullWidth ? dst.x + gx * dst.width
                                                     : dst.x + dst.width * 0.5f + (gx - 0.5f) * dst.width * squeeze;
                    rlColor4ub(v.r, v.g, v.b, v.a);
                    rlTexCoord2f(v.u, v.v);
                    rlVertex2f(sx, dst.y + gy * dst.height);
                }
                rlEnd();
                rlSetTexture(0);
            }
            EndBlendMode();
        }
        rlDrawRenderBatchActive();
        rlEnableBackfaceCulling();
    }

    // PS2X_WINDOW_SHOTS=1: with every test_shot, the window as drawn (picture + 2D layer) to out/window_shot_<tick>.png.
    void windowShotIfDue()
    {
        static const bool on = []
        {
            const char *env = std::getenv("PS2X_WINDOW_SHOTS");
            return env && *env && *env != '0';
        }();
        static uint64_t s_done = 0;
        if (!on)
            return;
        const uint64_t tick = ps2xTestLastShotTick();
        if (tick == 0u || tick == s_done)
            return;
        s_done = tick;
        rlDrawRenderBatchActive();
        const int w = GetRenderWidth(), h = GetRenderHeight();
        unsigned char *pixels = rlReadScreenPixels(w, h);
        if (!pixels)
            return;
        Image image{pixels, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        char path[96];
        std::snprintf(path, sizeof(path), "out/window_shot_%llu.png", static_cast<unsigned long long>(tick));
        ExportImage(image, path);
        RL_FREE(pixels);
    }
}

static int HostTextureFilter()
{
    static const int filter = []
    {
        const char *env = std::getenv("PS2X_FILTER");
        return (env && (std::strcmp(env, "nearest") == 0 || std::strcmp(env, "point") == 0)) ? TEXTURE_FILTER_POINT
                                                                                            : TEXTURE_FILTER_BILINEAR;
    }();
    return filter;
}

// Display aspect ratio of the picture, 0 = stretch to the window, -1 = the frame's pixel ratio.
static float HostDisplayAspect()
{
    static const float aspect = []
    {
        const char *env = std::getenv("PS2X_ASPECT");
        if (!env || !*env)
        {
            // PS2X_WIDESCREEN=w:h (main.cpp: the game's 3D view widened to it; 1 = 16:9): shown at that aspect.
            const char *wide = std::getenv("PS2X_WIDESCREEN");
            float w = 0.0f, h = 0.0f;
            if (wide && std::strcmp(wide, "1") == 0)
                return 16.0f / 9.0f;
            if (wide && std::sscanf(wide, "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f && w / h > 4.0f / 3.0f)
                return w / h;
            return 4.0f / 3.0f;
        }
        if (std::strcmp(env, "stretch") == 0)
            return 0.0f;
        if (std::strcmp(env, "pixel") == 0)
            return -1.0f;
        float w = 0.0f, h = 0.0f;
        if (std::sscanf(env, "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
            return w / h;
        return 4.0f / 3.0f;
    }();
    return aspect;
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer;

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(tex.width, tex.height, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    if (ps2x::hud::enabled() && ps2x::pgs::active())
        ps2x::hud::latest(g_hudFrame); // the 2D layer of this scanout
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    // The texture follows the picture's size (640 x 512 for PAL; 1280 x 1024 with PS2X_GS_SSAA >= 4).
    if (width == 0u || height == 0u || s_scratch.size() < static_cast<size_t>(width) * height * 4u)
    {
        width = static_cast<uint32_t>(tex.width);
        height = static_cast<uint32_t>(tex.height);
        s_uploadBuffer.assign(static_cast<size_t>(width) * height * 4u, 0u);
        s_scratch.swap(s_uploadBuffer);
    }
    if (tex.width != static_cast<int>(width) || tex.height != static_cast<int>(height))
    {
        UnloadTexture(tex);
        Image blank = GenImageColor(static_cast<int>(width), static_cast<int>(height), BLANK);
        tex = LoadTextureFromImage(blank);
        UnloadImage(blank);
        SetTextureFilter(tex, HostTextureFilter());
    }

    ps2xTestHarnessFrame(s_scratch.data(), width, height, width);
    UpdateTexture(tex, s_scratch.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);
    ps2xInstallIopToEeRpcHooks(this);
    ps2xInstallIopCdReader(true);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
#if !defined(PLATFORM_VITA)
        stopSpu2Stream();
#endif
        ps2xInstallIopToEeRpcHooks(nullptr);
        ps2xInstallIopCdReader(false);
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    ProfActivityScope profScope(3);
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    {
                                        if (ps2x::gfx::onWorker())
                                        {
                                            // The graphics thread: the profiler's activity is the game thread's.
                                            m_gs.processGIFPacket(data, size);
                                            return;
                                        }
                                        ProfActivityScope profScope(2);
                                        m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     if (ps2x::gfx::onWorker())
                                     {
                                         // The graphics thread: the EE context (D / T bit enables, VPU_STAT) is
                                         // the game thread's; no retail game runs VU1 with those debug bits.
                                         m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                       m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                       m_gs, &m_memory, startPC, top, itop, vu1RunBudget());
                                         return;
                                     }
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     ProfActivityScope profScope(1);
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, vu1RunBudget());
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     if (ps2x::gfx::onWorker())
                                     {
                                         m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                      m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                      m_gs, &m_memory, top, itop, vu1RunBudget());
                                         return;
                                     }
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     ProfActivityScope profScope(1);
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, vu1RunBudget());
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        {
            // ALWAYS_RUN: raylib otherwise parks the window thread in glfwWaitEvents while minimized, and with it
            // GS scanout (the EE then blocks on the GS lock). Unattended test runs also leave the focus alone.
            unsigned int windowFlags = FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN;
            const char *testMode = std::getenv("PS2X_TEST_MODE");
            if (testMode && std::atoi(testMode) != 0)
                windowFlags |= FLAG_WINDOW_UNFOCUSED;
            SetConfigFlags(windowFlags);
        }
        {
            // PS2X_WINDOW=<width>x<height>: the window's size (default 640 x 448); PS2X_FULLSCREEN=1: borderless
            // full screen at the monitor's resolution (F10 switches while playing).
            int windowWidth = HOST_WINDOW_WIDTH, windowHeight = HOST_WINDOW_HEIGHT;
            if (const char *size = std::getenv("PS2X_WINDOW"))
            {
                int w = 0, h = 0;
                if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240 && w <= 7680 && h <= 4320)
                {
                    windowWidth = w;
                    windowHeight = h;
                }
            }
            InitWindow(windowWidth, windowHeight, title);
            const char *full = std::getenv("PS2X_FULLSCREEN");
            const char *testMode = std::getenv("PS2X_TEST_MODE");
            if (full && std::strcmp(full, "1") == 0 && !(testMode && std::atoi(testMode) != 0))
                ToggleBorderlessWindowed();
            // raylib closes the window on ESC by default, but ESC is the keyboard's Circle (ps2_pad.cpp: back in
            // menus): only the window's close button ends the game.
            SetExitKey(KEY_NULL);
            // Controllers (ps2_pad.cpp ps2xHostGamepadRead): GLFW knows Xbox (XInput) and DualShock 3 / 4 /
            // DualSense pads; the DualSense Edge is added here (the DualSense layout). PS2X_PAD_MAPPINGS=<file>, or
            // gamecontrollerdb.txt in the working folder: more SDL GameControllerDB lines for other pads.
            SetGamepadMappings("030000004c050000f20d000000000000,DualSense Edge Wireless Controller,a:b1,b:b2,back:b8,"
                               "dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,"
                               "lefttrigger:a3,leftx:a0,lefty:a1,misc1:b13,rightshoulder:b5,rightstick:b11,"
                               "righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,platform:Windows,");
            const char *mappingFile = std::getenv("PS2X_PAD_MAPPINGS");
            if (!mappingFile || !*mappingFile)
                mappingFile = "gamecontrollerdb.txt";
            if (FileExists(mappingFile))
            {
                if (char *mappings = LoadFileText(mappingFile))
                {
                    std::cerr << "[pad] controller mappings from " << mappingFile << ": "
                              << (SetGamepadMappings(mappings) ? "loaded" : "not understood") << std::endl;
                    UnloadFileText(mappings);
                }
            }
        }
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        startSpu2Stream();
#endif
        // The window draws at least as often as the game shows frames (PS2X_FRAMERATE=120: 120 per second; was 60
        // always, so frames above 60 were never seen). A new frame is only fetched after a guest VSync. Test runs keep
        // 60: a non-deterministic scenario's ticks are window frames (pacing_realtime at 120).
        {
            const char *testMode = std::getenv("PS2X_TEST_MODE");
            const bool testRun = testMode && std::atoi(testMode) != 0;
            SetTargetFPS(testRun ? 60 : static_cast<int>(std::max<uint32_t>(60u, ps2xVBlankRateOverrideHz())));
        }
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    // PS2X_MC_ROOT=<folder>: memory card slot 1 lives there instead of <elf dir>/mc0 (slot 2 next to it,
    // "mc1" if the folder is called mc0). run_tests gives every run an empty card of its own this way.
    if (const char *mcRoot = std::getenv("PS2X_MC_ROOT"); mcRoot && *mcRoot)
    {
        paths.mcRoot = std::filesystem::path(mcRoot);
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }

        // Rayman bring-up: keep the whole EE state of the first bad branch for offline analysis
        // (out/crash_ee_ram.bin = 32 MiB RDRAM, registers in the log line below).
        std::ostringstream regs;
        regs << "[guest-branch:regs]" << std::hex;
        for (int i = 0; i < 32; ++i)
        {
            const uint64_t lo = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[i]));
            regs << " r" << std::dec << i << std::hex << "=" << lo;
        }
        std::cerr << regs.str() << std::endl;
        if (rdram)
        {
            std::error_code dirError;
            std::filesystem::create_directories("out", dirError);
            if (FILE *dump = std::fopen("out/crash_ee_ram.bin", "wb"))
            {
                const size_t written = std::fwrite(rdram, 1u, PS2_RAM_SIZE, dump);
                std::fclose(dump);
                std::cerr << "[guest-branch] EE RAM written to out/crash_ee_ram.bin (" << written << " bytes)" << std::endl;
            }
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

// Bug hunt: the last guest branches of this thread and how each ended. outcome: K = checkpoint (host stack
// unwinds to the scheduler), J = jump handed to the scheduler, M = missing function, T = call returned to the
// fall-through, F = call ended elsewhere (unwinds), ? = still running. Printed by game reports (main.cpp).
namespace
{
    struct BranchTraceEntry
    {
        uint32_t target, source, a1Before, pcAfter, v0After;
        int kind;
        char outcome;
    };
    thread_local BranchTraceEntry t_branchTrace[64];
    thread_local uint32_t t_branchTracePos = 0u;

    // Set when a branch hands control to the scheduler (checkpoint or a jump out of the function): every host
    // frame must now return false until the scheduler resumes the guest. Without it, a call whose target equals the
    // pending pc (recursion: f -> ... -> f hits a checkpoint while about to call f again) looked like
    // "callee returned without touching pc" and resumed after the call with garbage registers.
    thread_local bool t_guestYieldPending = false;

    // Guest call stack of this thread (dispatchGuestBranch): the recursion guard's report and the watchpoint below.
    struct GuestCallFrame
    {
        uint32_t target, source, a0, a1, a2, a3, sp, ra;
    };
    constexpr uint32_t kMaxGuestCallDepth = 4000u;
    thread_local uint32_t t_guestCallDepth = 0u;
    thread_local GuestCallFrame t_guestCallFrames[kMaxGuestCallDepth];

    // Bug hunt: a memory watchpoint at call granularity. PS2X_EE_WATCH="addr[,addr...][;fromVsync,toVsync]" (hex; 0x70000000+ = scratchpad
    // addresses, decimal VSyncs, up to 4 words of EE RAM) logs `[ee-watch]` whenever a word changed since the last guest
    // call / jump / return: "at entry" of a call from S means the code before S in S's function wrote it, "after
    // return" of a call to T means T's own code (after its last nested call) did. Every guest call goes through
    // dispatchGuestBranch, so unlike a wrapper around one function this sees writes wherever they happen.
    struct EeWatch
    {
        uint32_t addr[4] = {};
        uint32_t last[4] = {};
        uint32_t count = 0u;
        uint64_t from = 0u, to = ~0ull;
        uint32_t logged = 0u;
    };
    EeWatch &eeWatch()
    {
        static EeWatch w = []
        {
            EeWatch r;
            const char *env = std::getenv("PS2X_EE_WATCH");
            if (!env || !*env)
                return r;
            std::string spec(env);
            const size_t semi = spec.find(';');
            std::string addrs = spec.substr(0, semi);
            size_t pos = 0;
            while (pos < addrs.size() && r.count < 4u)
            {
                const size_t comma = addrs.find(',', pos);
                const std::string tok = addrs.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!tok.empty())
                {
                    const uint32_t a = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 16));
                    r.addr[r.count++] = (a & 0xF0000000u) == 0x70000000u ? (a & 0x70003FFCu) : (a & 0x1FFFFFCu);
                }
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
            if (semi != std::string::npos)
            {
                const std::string range = spec.substr(semi + 1);
                r.from = std::strtoull(range.c_str(), nullptr, 10);
                const size_t comma = range.find(',');
                if (comma != std::string::npos)
                    r.to = std::strtoull(range.c_str() + comma + 1, nullptr, 10);
            }
            std::fprintf(stderr, "[ee-watch] watching %u word(s) from VSync %llu to %llu\n", r.count,
                         static_cast<unsigned long long>(r.from), static_cast<unsigned long long>(r.to));
            return r;
        }();
        return w;
    }

    const uint8_t *g_eeWatchScratchpad = nullptr; // set by dispatchGuestBranch: watch addresses 0x70000000+ read it

    void eeWatchCheck(const uint8_t *rdram, R5900Context *ctx, const char *where, uint32_t sourcePc, uint32_t targetPc)
    {
        EeWatch &w = eeWatch();
        if (w.count == 0u)
            return;
        for (uint32_t i = 0; i < w.count; ++i)
        {
            uint32_t value;
            if ((w.addr[i] & 0xF0000000u) == 0x70000000u)
            {
                if (!g_eeWatchScratchpad)
                    continue;
                std::memcpy(&value, g_eeWatchScratchpad + (w.addr[i] & 0x3FFCu), 4u);
            }
            else
                std::memcpy(&value, rdram + w.addr[i], 4u);
            if (value == w.last[i])
                continue;
            const uint32_t old = w.last[i];
            w.last[i] = value;
            const uint64_t vsync = ps2xGuestVsync();
            if (vsync < w.from || vsync >= w.to || w.logged >= 4000u)
                continue;
            ++w.logged;
            float oldF, newF;
            std::memcpy(&oldF, &old, 4u);
            std::memcpy(&newF, &value, 4u);
            std::fprintf(stderr, "[ee-watch] vsync %llu [0x%x] 0x%08x (%g) -> 0x%08x (%g) %s 0x%x -> 0x%x ra=0x%x stack:",
                         static_cast<unsigned long long>(vsync), w.addr[i], old, oldF, value, newF, where, sourcePc,
                         targetPc, getRegU32(ctx, 31));
            const uint32_t first = t_guestCallDepth > 12u ? t_guestCallDepth - 12u : 0u;
            for (uint32_t f = first; f < t_guestCallDepth; ++f)
                std::fprintf(stderr, " %x<-%x", t_guestCallFrames[f].target, t_guestCallFrames[f].source);
            std::fprintf(stderr, "\n");
        }
    }

    // Bug hunt: log the arguments of chosen guest functions at entry, with memory they point to.
    // PS2X_EE_CALL_LOG="func[:reg:bytes...][,func...][;fromVsync,toVsync]" (hex; reg = a0-a3, t0-t3, s0-s7 or sp), e.g.
    // "12e260:a3:1a0:a0:340" logs each call of 0x12e260 with 0x1a0 bytes at a3 and 0x340 bytes at a0 (words and floats).
    struct EeCallLog
    {
        struct Dump
        {
            int reg = 0;
            uint32_t bytes = 0u;
        };
        struct Func
        {
            uint32_t pc = 0u;
            Dump dumps[4];
            uint32_t dumpCount = 0u;
        };
        Func funcs[8];
        uint32_t count = 0u;
        uint64_t from = 0u, to = ~0ull;
        uint32_t logged = 0u;
    };
    int eeRegisterByName(const std::string &name)
    {
        static const char *const kNames[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                               "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                               "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
        for (int i = 0; i < 32; ++i)
            if (name == kNames[i])
                return i;
        return -1;
    }
    EeCallLog &eeCallLog()
    {
        static EeCallLog c = []
        {
            EeCallLog r;
            const char *env = std::getenv("PS2X_EE_CALL_LOG");
            if (!env || !*env)
                return r;
            std::string spec(env);
            const size_t semi = spec.find(';');
            std::stringstream funcs(spec.substr(0, semi));
            std::string item;
            while (std::getline(funcs, item, ',') && r.count < 8u)
            {
                std::stringstream parts(item);
                std::string part;
                if (!std::getline(parts, part, ':') || part.empty())
                    continue;
                EeCallLog::Func &f = r.funcs[r.count++];
                f.pc = static_cast<uint32_t>(std::strtoul(part.c_str(), nullptr, 16));
                std::string reg, bytes;
                while (f.dumpCount < 4u && std::getline(parts, reg, ':') && std::getline(parts, bytes, ':'))
                {
                    const int index = eeRegisterByName(reg);
                    if (index >= 0)
                        f.dumps[f.dumpCount++] = {index, static_cast<uint32_t>(std::strtoul(bytes.c_str(), nullptr, 16))};
                }
            }
            if (semi != std::string::npos)
            {
                const std::string range = spec.substr(semi + 1);
                r.from = std::strtoull(range.c_str(), nullptr, 10);
                const size_t comma = range.find(',');
                if (comma != std::string::npos)
                    r.to = std::strtoull(range.c_str() + comma + 1, nullptr, 10);
            }
            std::fprintf(stderr, "[ee-call] logging %u function(s) from VSync %llu to %llu\n", r.count,
                         static_cast<unsigned long long>(r.from), static_cast<unsigned long long>(r.to));
            return r;
        }();
        return c;
    }

    // Bug hunt: PS2X_EE_CALL_TREE="rootFunc;vsync" (hex; decimal) prints every guest call made while rootFunc is on the
    // guest call stack during that guest VSync (`[ee-tree]`, indented by depth): which functions a routine really runs,
    // including calls through function pointers. Capped at 4000 lines.
    void eeCallTreeCheck(R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
    {
        static uint32_t root = 0u, logged = 0u;
        static uint64_t at = 0u;
        static const bool enabled = []
        {
            const char *env = std::getenv("PS2X_EE_CALL_TREE");
            if (!env || !*env)
                return false;
            char *end = nullptr;
            root = static_cast<uint32_t>(std::strtoul(env, &end, 16));
            at = (end && *end == ';') ? std::strtoull(end + 1, nullptr, 10) : 0u;
            return root != 0u;
        }();
        if (!enabled || logged >= 4000u || ps2xGuestVsync() != at)
            return;
        uint32_t rootDepth = 0u;
        bool found = false;
        for (uint32_t f = 0; f < t_guestCallDepth; ++f)
        {
            if (t_guestCallFrames[f].target == root)
            {
                rootDepth = f;
                found = true;
                break;
            }
        }
        // A scheduler yield inside the root unwinds the host stack; the guest then resumes without the root's frame.
        // Keep following (lines marked '~', absolute host depth) so the rest of the root's work stays visible.
        static bool following = false;
        static uint32_t followed = 0u;
        if (targetPc == root)
        {
            following = true;
            followed = 0u;
        }
        if (!found && targetPc != root)
        {
            if (!following || ++followed > 1500u)
                return;
            ++logged;
            std::fprintf(stderr, "[ee-tree] ~%u %*s0x%x from 0x%x a0=%x a1=%x a2=%x a3=%x t0=%x t1=%x f12=%g\n", t_guestCallDepth,
                         static_cast<int>(t_guestCallDepth * 2u), "", targetPc, sourcePc, getRegU32(ctx, 4), getRegU32(ctx, 5),
                         getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8), getRegU32(ctx, 9), ctx->f[12]);
            return;
        }
        ++logged;
        const uint32_t depth = found ? t_guestCallDepth - rootDepth : 0u;
        std::fprintf(stderr, "[ee-tree] %*s0x%x from 0x%x a0=%x a1=%x a2=%x a3=%x t0=%x t1=%x f12=%g\n", static_cast<int>(depth * 2u), "",
                     targetPc, sourcePc, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7),
                     getRegU32(ctx, 8), getRegU32(ctx, 9), ctx->f[12]);
    }

    struct EeCallReturn
    {
        bool pending = false;
        uint32_t target = 0u, source = 0u, depth = 0u;
        uint32_t bases[4] = {};
        uint32_t bytes[4] = {};
        uint32_t count = 0u;
    };
    thread_local EeCallReturn t_eeCallReturns[8];

    void eeCallDump(const uint8_t *rdram, const char *tag, uint32_t base, uint32_t bytes)
    {
        for (uint32_t off = 0; off < bytes && base + off + 16u <= PS2_RAM_SIZE; off += 16u)
        {
            uint32_t w[4];
            float v[4];
            std::memcpy(w, rdram + base + off, 16u);
            std::memcpy(v, rdram + base + off, 16u);
            std::fprintf(stderr, "%s   @%x+%03x: %08x %08x %08x %08x | %g %g %g %g\n", tag, base, off, w[0], w[1], w[2], w[3], v[0],
                         v[1], v[2], v[3]);
        }
    }

    void eeCallLogCheck(const uint8_t *rdram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
    {
        EeCallLog &c = eeCallLog();
        if (c.count == 0u)
            return;
        for (uint32_t i = 0; i < c.count; ++i)
        {
            const EeCallLog::Func &f = c.funcs[i];
            if (f.pc != targetPc)
                continue;
            const uint64_t vsync = ps2xGuestVsync();
            if (vsync < c.from || vsync >= c.to || c.logged >= 2000u)
                return;
            ++c.logged;
            std::fprintf(stderr,
                         "[ee-call] vsync %llu 0x%x from 0x%x a0=%x a1=%x a2=%x a3=%x t0=%x t1=%x t2=%x f12=%g sp=%x ra=%x "
                         "s0=%x s1=%x s2=%x s3=%x s4=%x s5=%x s6=%x s7=%x fp=%x\n",
                         static_cast<unsigned long long>(vsync), targetPc, sourcePc, getRegU32(ctx, 4), getRegU32(ctx, 5),
                         getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8), getRegU32(ctx, 9), getRegU32(ctx, 10),
                         ctx->f[12], getRegU32(ctx, 29), getRegU32(ctx, 31), getRegU32(ctx, 16), getRegU32(ctx, 17),
                         getRegU32(ctx, 18), getRegU32(ctx, 19), getRegU32(ctx, 20), getRegU32(ctx, 21), getRegU32(ctx, 22),
                         getRegU32(ctx, 23), getRegU32(ctx, 30));
            EeCallReturn &ret = t_eeCallReturns[i];
            ret = EeCallReturn{true, targetPc, sourcePc, t_guestCallDepth, {}, {}, f.dumpCount};
            for (uint32_t d = 0; d < f.dumpCount; ++d)
            {
                const uint32_t base = getRegU32(ctx, f.dumps[d].reg) & 0x1FFFFFCu;
                const uint32_t bytes = std::min<uint32_t>(f.dumps[d].bytes, 0x1000u);
                ret.bases[d] = base;
                ret.bytes[d] = bytes;
                eeCallDump(rdram, "[ee-call]", base, bytes);
            }
            return;
        }
    }

    // After a logged call returns normally (not unwound by a yield): the same memory again, as `[ee-ret]` lines.
    void eeCallLogReturn(const uint8_t *rdram, const R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
    {
        EeCallLog &c = eeCallLog();
        for (uint32_t i = 0; i < c.count; ++i)
        {
            EeCallReturn &ret = t_eeCallReturns[i];
            if (!ret.pending || ret.target != targetPc || ret.source != sourcePc || ret.depth + 1u != t_guestCallDepth)
                continue;
            ret.pending = false;
            std::fprintf(stderr, "[ee-ret] 0x%x from 0x%x returned v0=%x v1=%x f0=%g\n", targetPc, sourcePc,
                         getRegU32(ctx, 2), getRegU32(ctx, 3), static_cast<double>(ctx->f[0]));
            for (uint32_t d = 0; d < ret.count; ++d)
                eeCallDump(rdram, "[ee-ret]", ret.bases[d], ret.bytes[d]);
        }
    }
}

// Scheduler (EeScheduler::run): about to (re)enter guest code at the root of the host stack.
extern "C" void ps2xGuestYieldReset()
{
    t_guestYieldPending = false;
}

// For HLE wrappers around a guest function: true when the wrapped function handed control to the scheduler instead
// of returning (its rest runs later from the scheduler, not through the wrapper).
extern "C" bool ps2xGuestYieldPending()
{
    return t_guestYieldPending;
}

extern "C" void ps2xDumpBranchTrace()
{
    std::fprintf(stderr, "[branch-trace] last 64 guest branches of this thread, oldest first:\n");
    for (uint32_t i = 0; i < 64u; ++i)
    {
        const BranchTraceEntry &e = t_branchTrace[(t_branchTracePos + i) % 64u];
        if (e.target == 0u && e.source == 0u)
            continue;
        std::fprintf(stderr, "[branch-trace]   0x%06x -> 0x%06x kind=%d a1=0x%x => %c pc=0x%x v0=0x%x\n", e.source, e.target,
                     e.kind, e.a1Before, e.outcome, e.pcAfter, e.v0After);
    }
    std::fflush(stderr);
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
    g_eeWatchScratchpad = m_memory.getScratchpad();
    eeWatchCheck(rdram, ctx, "at entry", sourcePc, targetPc);
    if (isCall)
    {
        eeCallLogCheck(rdram, ctx, sourcePc, targetPc);
        eeCallTreeCheck(ctx, sourcePc, targetPc);
    }

    BranchTraceEntry &trace = t_branchTrace[t_branchTracePos++ % 64u];
    // Field by field: assigning a BranchTraceEntry{...} temporary built it on the stack with narrow stores and copied
    // it with a 16-byte load, a store-forwarding stall on every guest call (~4% of the game thread, 2026-11-17).
    trace.target = targetPc;
    trace.source = sourcePc;
    trace.a1Before = getRegU32(ctx, 5);
    trace.pcAfter = 0u;
    trace.v0After = 0u;
    trace.kind = static_cast<int>(kind);
    trace.outcome = '?';
    struct BranchTraceClose
    {
        BranchTraceEntry &entry;
        R5900Context *context;
        ~BranchTraceClose()
        {
            entry.pcAfter = context->pc;
            entry.v0After = getRegU32(context, 2);
        }
    } traceClose{trace, ctx};

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        trace.outcome = 'K';
        t_guestYieldPending = true;
        return false;
    }

    if (!isCall)
    {
        trace.outcome = 'J';
        // Returns only pass through here with PS2X_STRICT_RETURN_DIAGNOSTICS; they are not hand-offs.
        if (kind != GuestBranchKind::Return)
            t_guestYieldPending = true;
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    // Runaway guest recursion guard. Each guest call is a nested host call, so a guest walk that never ends (Rayman's
    // AI evaluator on an uninitialised, all-zero tree) overflows the host stack and kills the process with no report.
    // Real guest call chains stay well below this depth.
    if (t_guestCallDepth >= kMaxGuestCallDepth)
    {
        static std::atomic<bool> s_reported{false};
        if (!s_reported.exchange(true))
        {
            std::fprintf(stderr, "[guest-recursion] guest call depth %u reached, calling 0x%x from 0x%x; outermost and innermost frames:\n",
                         t_guestCallDepth, targetPc, sourcePc);
            for (uint32_t i = 0; i < kMaxGuestCallDepth; ++i)
            {
                if (i == 24u && kMaxGuestCallDepth > 48u)
                {
                    std::fprintf(stderr, "[guest-recursion]   ...\n");
                    i = kMaxGuestCallDepth - 24u;
                }
                const GuestCallFrame &f = t_guestCallFrames[i];
                std::fprintf(stderr, "[guest-recursion]   #%u call 0x%x from 0x%x a0=%x a1=%x a2=%x a3=%x sp=%x ra=%x\n", i,
                             f.target, f.source, f.a0, f.a1, f.a2, f.a3, f.sp, f.ra);
            }
            if (FILE *dump = std::fopen("out/crash_ee_ram.bin", "wb"))
            {
                std::fwrite(rdram, 1u, PS2_RAM_SIZE, dump);
                std::fclose(dump);
                std::fprintf(stderr, "[guest-recursion] EE RAM written to out/crash_ee_ram.bin\n");
            }
            std::fflush(stderr);
        }
        requestStop();
        return false;
    }
    {
        // Field by field, like the trace entry above (no temporary copied with wide loads).
        GuestCallFrame &frame = t_guestCallFrames[t_guestCallDepth];
        frame.target = targetPc;
        frame.source = sourcePc;
        frame.a0 = getRegU32(ctx, 4);
        frame.a1 = getRegU32(ctx, 5);
        frame.a2 = getRegU32(ctx, 6);
        frame.a3 = getRegU32(ctx, 7);
        frame.sp = getRegU32(ctx, 29);
        frame.ra = getRegU32(ctx, 31);
    }
    // A destructor, because guest thread switches unwind the host stack with an exception (ee_scheduler.h).
    struct DepthScope
    {
        DepthScope() { ++t_guestCallDepth; }
        ~DepthScope() { --t_guestCallDepth; }
    } depthScope;

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    targetFn(rdram, ctx, this);
    eeWatchCheck(rdram, ctx, "after return", sourcePc, targetPc);
    eeCallLogReturn(rdram, ctx, sourcePc, targetPc);

    if (isStopRequested() || ctx->pc == 0u || t_guestYieldPending)
    {
        trace.outcome = 'F';
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    trace.outcome = (ctx->pc == fallthroughPc) ? 'T' : 'F';
    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler->checkpointDue(cycles);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(frameTex, HostTextureFilter());

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        ps2xTestThreadStackGuarantee();
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    std::atomic<bool> profilerStop{false};
    std::thread profilerThread([&]()
                               {
        std::unordered_map<uint32_t, uint32_t> hist;
        uint32_t total = 0, vu1 = 0, gs = 0, iop = 0, idle = 0;
        auto windowStart = std::chrono::steady_clock::now();
        while (!profilerStop.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++total;
            const int activity = g_profActivity.load(std::memory_order_relaxed);
            if (activity == 1)
                ++vu1;
            else if (activity == 2)
                ++gs;
            else if (activity == 3)
                ++iop;
            else if (!m_eeScheduler || !m_eeScheduler->isExecutingGuest())
                ++idle;
            else
                ++hist[g_profLastDispatchPc.load(std::memory_order_relaxed)];

            const auto now = std::chrono::steady_clock::now();
            if (now - windowStart >= std::chrono::seconds(10) && total > 0)
            {
                std::vector<std::pair<uint32_t, uint32_t>> top(hist.begin(), hist.end());
                std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                std::ostringstream line;
                line << "[prof] last 10s: VU1=" << (vu1 * 100 / total) << "% GS=" << (gs * 100 / total)
                     << "% IOP=" << (iop * 100 / total) << "% idle=" << (idle * 100 / total) << "% EE top:";
                for (size_t i = 0; i < top.size() && i < 12; ++i)
                    line << " 0x" << std::hex << top[i].first << std::dec << "=" << (top[i].second * 100 / total) << "%";
                std::cerr << line.str() << std::endl;
                hist.clear();
                total = vu1 = gs = iop = idle = 0;
                windowStart = now;
            }
        } });

    uint64_t tick = 0;
    // Game frame counter: the game flips its display buffer (DISPFB1) once per rendered frame; the flips are counted
    // at guest VBlanks (EeScheduler.cpp), not here once per host frame (that missed flips at high frame rates).
    uint64_t fpsFlipsAtWindowStart = ps2xGuestDisplayFlips();
    double gameFps = 0.0;
    auto fpsWindowStart = std::chrono::steady_clock::now();
    ps2xTestHarnessStart();
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        {
            const auto fpsNow = std::chrono::steady_clock::now();
            const double fpsSeconds = std::chrono::duration<double>(fpsNow - fpsWindowStart).count();
            if (fpsSeconds >= 1.0)
            {
                const uint64_t flips = ps2xGuestDisplayFlips();
                gameFps = static_cast<double>(flips - fpsFlipsAtWindowStart) / fpsSeconds;
                fpsFlipsAtWindowStart = flips;
                fpsWindowStart = fpsNow;
            }
        }
        {
            // PS2X_FPS_LOG=<ms> (speed research): every <ms> of real time, an `[fps]` line with the guest VSync, the
            // game FPS in that window and the longest wait between two game frames seen by the window loop.
            static const int fpsLogMs = []
            {
                const char *env = std::getenv("PS2X_FPS_LOG");
                return env ? std::max(0, std::atoi(env)) : 0;
            }();
            if (fpsLogMs > 0)
            {
                static auto logStart = std::chrono::steady_clock::now();
                static auto lastFlipAt = logStart;
                static uint64_t logFlips = ps2xGuestDisplayFlips();
                static uint64_t lastFlips = logFlips;
                static double worstMs = 0.0;
                const auto now = std::chrono::steady_clock::now();
                const uint64_t flips = ps2xGuestDisplayFlips();
                if (flips != lastFlips)
                {
                    worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(now - lastFlipAt).count());
                    lastFlipAt = now;
                    lastFlips = flips;
                }
                const double windowMs = std::chrono::duration<double, std::milli>(now - logStart).count();
                if (windowMs >= fpsLogMs)
                {
                    std::fprintf(stderr, "[fps] vsync=%llu fps=%.1f worst_ms=%.1f\n",
                                 static_cast<unsigned long long>(ps2xGuestVsync()),
                                 static_cast<double>(flips - logFlips) * 1000.0 / windowMs, worstMs);
                    logStart = now;
                    logFlips = flips;
                    worstMs = 0.0;
                }
            }
        }
        {
            tick++;
            if (ps2xTestHarnessTick(tick))
            {
                requestStop();
                break;
            }
            // Rayman bring-up: always report where the guest threads are (~every 2 s),
            // even with aggressive logs off. This is what tells us why a screen stays black.
            if ((tick % 120) == 0)
            {
                const auto threadSnapshot = m_eeScheduler->snapshot();
                std::ostringstream threadLine;
                threadLine << "[threads] tick=" << tick << " gameFps=" << static_cast<int>(gameFps * 10.0 + 0.5) / 10.0;
                std::vector<int> blockedSemas;
                for (const auto &t : threadSnapshot.threads)
                {
                    if (t.waitReason == EeWaitReason::Semaphore)
                        blockedSemas.push_back(t.waitId);
                    threadLine << " | t" << t.id << " st=" << static_cast<int>(t.status)
                               << " wait=" << static_cast<int>(t.waitReason) << ":" << t.waitId
                               << " pri=" << t.currentPriority
                               << std::hex << " pc=0x" << t.pc << " ra=0x" << t.ra << std::dec;
                }
                threadLine << " | dma=" << m_memory.dmaStartCount()
                           << " gif=" << m_memory.gifCopyCount()
                           << " vif=" << m_memory.vifWriteCount();
                char gsStats[256];
                if (ps2x::pgs::active())
                    threadLine << " | " << ps2x::pgs::statsLine();
                else if (ps2xGsStatsLine(gsStats, static_cast<int>(sizeof(gsStats))) > 0)
                    threadLine << " | " << gsStats;
                char gfxStats[96];
                if (ps2xGfxStatsLine(gfxStats, static_cast<int>(sizeof(gfxStats))) > 0)
                    threadLine << " | " << gfxStats;
                {
                    // The game thread's busy share since the last line: real time minus its sleep waiting for the
                    // next VBlank (2026-11-17: dips where neither the GS nor the graphics thread was at 100%).
                    static uint64_t s_lastIdleNs = ps2xGameIdleNs();
                    static auto s_lastLine = std::chrono::steady_clock::now();
                    const uint64_t idleNs = ps2xGameIdleNs();
                    const auto lineNow = std::chrono::steady_clock::now();
                    const double spanNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(lineNow - s_lastLine).count());
                    if (spanNs > 0.0)
                    {
                        const double busy = 100.0 * (1.0 - static_cast<double>(idleNs - s_lastIdleNs) / spanNs);
                        threadLine << " | game busy=" << static_cast<int>(std::clamp(busy, 0.0, 100.0) + 0.5) << "%";
                    }
                    s_lastIdleNs = idleNs;
                    s_lastLine = lineNow;
                }
                char soundStats[320];
                if (ps2xSpu2StatsLine(soundStats, static_cast<int>(sizeof(soundStats))) > 0)
                    threadLine << " | " << soundStats;
                if (ps2xIopToEeRpcStatsLine(soundStats, static_cast<int>(sizeof(soundStats))) > 0)
                    threadLine << " | " << soundStats;
                if (g_gameStatsLine)
                {
                    char gameStats[256];
                    if (g_gameStatsLine(m_memory.getRDRAM(), gameStats, static_cast<int>(sizeof(gameStats))) > 0)
                        threadLine << " | " << gameStats;
                }
                std::cerr << threadLine.str() << std::endl;
                for (int semaId : blockedSemas)
                    ps2xDebugDumpSemaphoreHistory(semaId);
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);

        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        // The picture at its display aspect (default 4:3, as on a TV), as large as the window allows.
        const float aspect = HostDisplayAspect();
        float dstWidth = screenWidth, dstHeight = screenHeight;
        if (aspect < 0.0f)
        {
            const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
            dstWidth = srcWidth * scale;
            dstHeight = srcHeight * scale;
        }
        else if (aspect > 0.0f)
        {
            dstWidth = std::min(screenWidth, screenHeight * aspect);
            dstHeight = dstWidth / aspect;
        }
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        drawHudOverlay(dstRect);
        windowShotIfDue();
        static const bool showFps = []
        {
            const char *env = std::getenv("PS2X_SHOW_FPS");
            return !(env && std::strcmp(env, "0") == 0);
        }();
        if (showFps)
            DrawText(TextFormat("Game FPS: %.1f", gameFps), 12, 12, 24, YELLOW);
        if (IsKeyPressed(KEY_F10))
            ToggleBorderlessWindowed(); // full screen at the monitor's resolution, and back
        if (IsKeyPressed(KEY_F11))
        {
            // Bug reports: record GS commands for offline analysis. The GPU renderer (normal play) writes
            // out/gif_capture.bin + out/gif_capture_frame*.bmp; the CPU renderer out/gscapture.bin.
            if (ps2x::pgs::active())
                ps2x::pgs::requestCapture();
            else
                ps2xRequestGsCapture();
            ps2xRequestVu1Capture(); // out/vu1cap: the VU1 runs behind those primitives
            ps2xRequestEeDump();     // out/gif_capture_ee_ram.bin: the game state (compare with PCSX2)
            std::cerr << "[gs] capture requested (F11)" << std::endl;
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    ps2xTestHarnessExiting();
    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }
    ps2x::gfx::stop(); // the graphics thread finishes what the game posted
    profilerStop.store(true, std::memory_order_relaxed);
    if (profilerThread.joinable())
    {
        profilerThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
