// GPU GS renderer bridge: feeds the game's GS command stream to paraLLEl-GS and reads the scanout back.
// See gs_pgs_bridge.h. paraLLEl-GS and Granite are LGPL-3.0-or-later / MIT; this file is GPL-3.0 like
// the rest of PS2Recomp.

#include "gs_pgs_bridge.h"
#include "gs_hud.h"

namespace ps2x::pgs
{
    namespace
    {
        thread_local uint32_t t_currentPath = 3u;
    }
    void setCurrentPath(uint32_t path)
    {
        t_currentPath = (path >= 1u && path <= 3u) ? path : 3u;
    }
    uint32_t currentPath()
    {
        return t_currentPath;
    }
}

#if defined(PS2X_HAS_PARALLEL_GS)

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "thread_id.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

extern "C" uint64_t ps2xGuestVsync(); // ps2_test_harness.cpp

namespace ps2x::pgs
{
    namespace
    {
        // Deterministic mode (PS2X_DETERMINISTIC=1, see ps2_test_harness.cpp): SIGNAL / FINISH / LABEL reach the
        // guest when the register write passes through the bridge (deterministicSignal), not when the GPU thread
        // reports it, whose timing depends on the host.
        // The GS thread is on (see "the GS thread" below): SIGNAL / FINISH / LABEL then reach the guest from the game
        // thread when it queues the write, never from paraLLEl-GS's callbacks or noteRegister on the worker.
        bool g_threaded = false;
        // PS2X_GS_SSAA >= 4 (start()): VSyncInfo::high_resolution_scanout.
        bool g_hiresScanout = false;

        bool deterministicMode()
        {
            static const bool on = []()
            {
                const char *value = std::getenv("PS2X_DETERMINISTIC");
                return value && std::atoi(value) != 0;
            }();
            return on;
        }

        class SignalForwarder final : public ParallelGS::SignalInterface
        {
        public:
            SignalSink *sink = nullptr;
            bool on_signal(uint64_t payload) override
            {
                if (sink && !deterministicMode() && !g_threaded)
                    sink->signal(payload);
                return false;
            }
            bool on_finish(uint64_t payload) override
            {
                if (sink && !deterministicMode() && !g_threaded)
                    sink->finish(payload);
                return false;
            }
            bool on_label(uint64_t payload) override
            {
                if (sink && !deterministicMode() && !g_threaded)
                    sink->label(payload);
                return false;
            }
        };

        struct State
        {
            std::unique_ptr<Vulkan::Context> context;
            std::unique_ptr<Vulkan::Device> device;
            std::unique_ptr<ParallelGS::GSInterface> gs;
            SignalForwarder signals;
            Vulkan::BufferHandle readback;
            VkDeviceSize readbackSize = 0;
            std::string gpuName;
            uint64_t frames = 0;
            uint64_t gifBytes = 0;
            uint32_t lastWidth = 0;
            uint32_t lastHeight = 0;
            double scanoutMs = 0.0;
            uint32_t scanoutSamples = 0;
        };

        State *g_state = nullptr;

        // A scanout picture still on its way back from the GPU (PS2X_GS_ASYNC_SCANOUT, on by default with the GS
        // thread): the GS thread records the copy into the readback buffer and goes on with the next frame; the
        // window thread, which waits for that picture anyway, waits for the fence and copies the pixels. Before, the
        // GS thread itself waited (3-4 ms a frame in busy places: the GPU drawing the whole frame), so the CPU work
        // of the next frame and the GPU work of this one ran one after the other (2026-11-16: GS thread 91% busy at
        // 86 FPS where the shop save loads, smooth mode).
        struct PendingReadback
        {
            Vulkan::Device *device = nullptr;
            Vulkan::Fence fence;
            Vulkan::BufferHandle buffer;
            uint32_t width = 0;
            uint32_t height = 0;
        };

        bool asyncScanoutWanted()
        {
            static const bool wanted = []
            {
                const char *env = std::getenv("PS2X_GS_ASYNC_SCANOUT");
                return !(env && std::strcmp(env, "0") == 0);
            }();
            return wanted;
        }

        // Copies a finished readback into rgba (opaque alpha, as the window expects). Any thread.
        bool finishReadback(PendingReadback &pending, std::vector<uint8_t> &rgba)
        {
            if (!pending.device || !pending.fence || !pending.buffer)
                return false;
            pending.fence->wait();
            const size_t size = static_cast<size_t>(pending.width) * pending.height * 4u;
            const auto *pixels = static_cast<const uint8_t *>(
                pending.device->map_host_buffer(*pending.buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (!pixels)
                return false;
            rgba.resize(size);
            std::memcpy(rgba.data(), pixels, size);
            pending.device->unmap_host_buffer(*pending.buffer, Vulkan::MEMORY_ACCESS_READ_BIT);
            for (size_t i = 3; i < rgba.size(); i += 4)
                rgba[i] = 0xFFu;
            return true;
        }

        // paraLLEl-GS / Granite are single-threaded: whoever calls into them (the GS thread; without it the EE
        // thread streaming GIF data and the window thread scanning out) holds this lock (recursive: scanout calls
        // flush etc.).
        std::recursive_mutex g_mutex;

        PrivRegs g_lastRegs{};

        // GIF data arrives in DMA-sized pieces that can split a GIF packet anywhere (RenderWare sends the tag
        // and its vertex data from different DMA tags). paraLLEl-GS's fast draw path assumes whole loops are
        // present and spins forever on a piece shorter than one loop, so each path is reassembled here and
        // only complete packets (tag + all of its data) are handed over.
        std::vector<uint8_t> g_pendingGif[4];

        size_t gifPacketBytes(const uint8_t *tag)
        {
            uint64_t lo = 0u;
            std::memcpy(&lo, tag, sizeof(lo));
            const size_t nloop = static_cast<size_t>(lo & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            size_t nreg = static_cast<size_t>((lo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            size_t qwords = nloop;            // IMAGE / disabled
            if (flg == 0u)
                qwords = nloop * nreg;        // PACKED
            else if (flg == 1u)
                qwords = (nloop * nreg + 1u) / 2u; // REGLIST
            return 16u + qwords * 16u;
        }

        // Length of the complete packets at the start of [data, data + bytes).
        size_t completeGifBytes(const uint8_t *data, size_t bytes)
        {
            size_t offset = 0u;
            while (bytes - offset >= 16u)
            {
                const size_t need = gifPacketBytes(data + offset);
                if (bytes - offset < need)
                    break;
                offset += need;
            }
            return offset;
        }

        // Rayman bring-up: a light walk over every complete GIF packet before paraLLEl-GS sees it.
        // Counts per scanout what the frame asks of the GPU and logs values that are out of range for
        // real hardware (they can make a GPU shader run long enough for a driver reset).
        struct GifStats
        {
            uint64_t packets = 0, qwords = 0, imageQwords = 0, kicks = 0, prims = 0;
            uint64_t tex0 = 0, clutLoads = 0, uploads = 0, localCopies = 0;
            uint64_t bigPrims = 0, badTex = 0, badTransfers = 0;
            uint32_t maxPrimW = 0, maxPrimH = 0;
        };
        GifStats g_frameStats;  // the frame being built
        GifStats g_windowStats; // since the last statsLine
        uint64_t g_anomalyLogs = 0;
        bool g_traceFrames = false;

        struct GifRegShadow
        {
            uint32_t primType = 0;
            uint32_t ctxt = 0;
            uint64_t xyoffset[2] = {};
            uint64_t trxreg = 0;
            uint64_t trxpos = 0;
            uint64_t bitbltbuf = 0;
            int32_t vx[3] = {}, vy[3] = {};
            uint32_t vz = 0;
            float s = 0.0f, t = 0.0f, q = 1.0f;
            float vu[3] = {}, vv[3] = {};
            uint32_t vcount = 0;
            // For the screen-covering primitive report.
            uint64_t prim = 0;
            uint32_t rgba = 0;
            uint64_t alpha[2] = {};
            uint64_t frame[2] = {};
            uint64_t tex0[2] = {};
            uint64_t test[2] = {};
            uint64_t zbuf[2] = {};
            uint64_t tex1[2] = {};
            uint64_t miptbp1[2] = {};
        } g_shadow;
        // TEX1 / MIPTBP1 / TEX0 combinations used by world draws (textured, no Cd*As) in report frames, with counts.
        struct TexState
        {
            uint64_t tex1, miptbp1, tex0;
            uint64_t count;
        };
        TexState g_texStates[16] = {};
        uint32_t g_texStateCount = 0;
        // Every primitive of a report frame, grouped by (type, ctxt, TME, ABE, ALPHA, FBMSK): count, screen area
        // (bounding box clipped to 640x512, halved for triangles) and the last vertex colour seen.
        struct BlendGroup
        {
            uint64_t key;
            uint64_t count;
            double area;
            uint32_t rgba;
            uint64_t frame;
        };
        BlendGroup g_blendGroups[48] = {};
        uint32_t g_blendGroupCount = 0;
        // Screen area (pixels, bounding boxes clipped to 640x512) drawn by "Cd * As" multiply passes this frame,
        // and the TEST / ZBUF state of the last one.
        uint64_t g_multiplyArea = 0;
        uint64_t g_multiplyPrims = 0;
        uint64_t g_multiplyTest = 0, g_multiplyZbuf = 0, g_multiplyFrame = 0;
        // [0] = world (textured, no Cd*As blending), [1] = multiply passes: Z range and distinct Z values seen.
        uint32_t g_zMin[2] = {0xFFFFFFFFu, 0xFFFFFFFFu}, g_zMax[2] = {};
        uint64_t g_zCount[2] = {}, g_zZero[2] = {};
        // Z buckets per pass kind: <= 0xFFFF, <= 0xFFFFF, larger.
        uint64_t g_zHist[2][3] = {};
        // Texture coordinates of textured, non-FST world primitives (no Cd*As blend) in report frames.
        struct StqStats
        {
            uint64_t verts = 0, qZero = 0, qNeg = 0, tris = 0, flatTris = 0;
            float qMin = 1e30f, qMax = -1e30f, uMin = 1e30f, uMax = -1e30f, vMin = 1e30f, vMax = -1e30f;
        } g_stq;
        uint64_t g_worldTest = 0, g_worldZbuf = 0;
        uint64_t g_frameNumber = 0;
        uint32_t g_coverLogsThisFrame = 0;
        uint64_t g_coverLogs = 0;
        uint32_t g_bigPrimLogs = 0;
        // Vertex colour histogram of the frame: [textured][max(R,G,B) bucket 0-7 / 8-31 / 32-95 / 96+].
        uint64_t g_colourHist[2][4] = {};

        // PS2X_PGS_COVER_FROM=<frame>: first frame of the report (frames ~ guest VSyncs); every 120th frame after it.
        uint64_t coverFrom()
        {
            static const uint64_t from = []()
            {
                const char *value = std::getenv("PS2X_PGS_COVER_FROM");
                return value ? std::strtoull(value, nullptr, 10) : 0ull;
            }();
            return from;
        }

        bool coverFrame()
        {
            return g_frameNumber >= coverFrom() && ((g_frameNumber - coverFrom()) % 120u) == 0u;
        }

        // Bug hunt (dark lightmap passes): host->local uploads into GS blocks PS2X_PGS_WATCH_LO..HI (default
        // 0x3800..0x3a40, the level's T8 lightmaps and their CLUT), from frame PS2X_PGS_COVER_FROM on, with value
        // statistics of the uploaded data.
        struct UploadWatch
        {
            bool active = false;
            uint32_t dbp = 0, dbw = 0, dpsm = 0, rrw = 0, rrh = 0, dx = 0, dy = 0;
            uint64_t expect = 0, got = 0;
            uint32_t minB = 255, maxB = 0, minA = 255, maxA = 0;
            uint64_t sumB = 0, sumA = 0, countA = 0;
            uint32_t first[4] = {};
        } g_upload;
        uint32_t g_uploadLogs = 0;

        uint32_t watchBound(const char *name, uint32_t fallback)
        {
            const char *value = std::getenv(name);
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : fallback;
        }

        // PS2X_PGS_CAPTURE_VSYNC=<guest vsync>: everything the GS receives during one whole scanout frame (the first
        // one that starts after that guest VSync) goes to out/gif_capture.bin, for offline analysis with
        // tools/gifcap.py. Records: u32 kind (0 = GIF data, 1 = register write), u32 path index or register,
        // u64 byte count or register value, then the bytes (GIF data only). Game content: never commit a capture.
        std::FILE *g_captureFile = nullptr;
        bool g_captureDone = false;
        // F11 in the window (requestCapture): the next two scanout frames, with the scanout pictures at the start, in
        // between and at the end (out/gif_capture_frame0..2.bmp), so the commands can be matched to what was on screen.
        std::atomic<bool> g_captureRequested{false};
        uint32_t g_captureFramesLeft = 0;
        uint32_t g_capturePictures = 0, g_capturePictureIndex = 0;

        void captureRecord(uint32_t kind, uint32_t id, uint64_t sizeOrValue, const uint8_t *data)
        {
            if (!g_captureFile)
                return;
            std::fwrite(&kind, 4u, 1u, g_captureFile);
            std::fwrite(&id, 4u, 1u, g_captureFile);
            std::fwrite(&sizeOrValue, 8u, 1u, g_captureFile);
            if (kind == 0u && data)
                std::fwrite(data, 1u, static_cast<size_t>(sizeOrValue), g_captureFile);
        }

        // At every scanout: ends a running capture, or starts one once the guest has reached the asked VSync.
        void captureFrameBoundary(uint64_t frame)
        {
            static const uint32_t at = watchBound("PS2X_PGS_CAPTURE_VSYNC", 0u);
            if (g_captureFile)
            {
                if (g_captureFramesLeft > 1u)
                {
                    --g_captureFramesLeft;
                    return;
                }
                std::fclose(g_captureFile);
                g_captureFile = nullptr;
                g_captureDone = true;
                std::fprintf(stderr, "[pgs-capture] done: out/gif_capture.bin (frame %llu)\n", (unsigned long long)frame);
                return;
            }
            const bool requested = g_captureRequested.exchange(false);
            if (!requested && (at == 0u || g_captureDone || ps2xGuestVsync() < at))
                return;
            g_captureFile = std::fopen("out/gif_capture.bin", "wb");
            if (!requested)
                g_captureDone = g_captureFile == nullptr;
            // PS2X_PGS_CAPTURE_FRAMES=<n> (default 1): scanout frames to capture from the asked VSync (a menu at
            // 25 FPS leaves some 60 Hz scanout frames without any drawing).
            g_captureFramesLeft = requested ? 2u : std::max(1u, watchBound("PS2X_PGS_CAPTURE_FRAMES", 1u));
            g_capturePictures = g_captureFile ? g_captureFramesLeft + 1u : 0u;
            g_capturePictureIndex = 0u;
            std::fprintf(stderr, "[pgs-capture] %s at frame %llu, guest vsync %llu\n", g_captureFile ? "start" : "cannot open out/gif_capture.bin",
                         (unsigned long long)frame, (unsigned long long)ps2xGuestVsync());
        }

        uint32_t psmBits(uint32_t psm)
        {
            switch (psm)
            {
            case 0x00: case 0x30: return 32u;
            case 0x01: case 0x31: return 24u;
            case 0x02: case 0x0A: case 0x32: case 0x3A: return 16u;
            case 0x13: return 8u;
            case 0x14: return 4u;
            default: return 32u;
            }
        }

        void finishUploadWatch()
        {
            auto &u = g_upload;
            std::fprintf(stderr,
                         "[pgs-upload] frame %llu dbp=%#x dbw=%u dpsm=%#x at %u,%u size %ux%u bytes %llu/%llu byte min/avg/max %u/%.1f/%u "
                         "alpha(byte 3 of 4) min/avg/max %u/%.1f/%u first %08x %08x %08x %08x\n",
                         (unsigned long long)g_frameNumber, u.dbp, u.dbw, u.dpsm, u.dx, u.dy, u.rrw, u.rrh, (unsigned long long)u.got,
                         (unsigned long long)u.expect, u.minB, u.got ? double(u.sumB) / double(u.got) : 0.0, u.maxB, u.minA,
                         u.countA ? double(u.sumA) / double(u.countA) : 0.0, u.maxA, u.first[0], u.first[1], u.first[2], u.first[3]);
            u.active = false;
        }

        void noteImageData(const uint8_t *data, size_t bytes)
        {
            auto &u = g_upload;
            if (!u.active)
                return;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(bytes, u.expect - u.got));
            for (size_t i = 0; i < take; ++i)
            {
                const uint64_t at = u.got + i;
                const uint32_t b = data[i];
                u.minB = std::min(u.minB, b);
                u.maxB = std::max(u.maxB, b);
                u.sumB += b;
                if ((at & 3u) == 3u)
                {
                    u.minA = std::min(u.minA, b);
                    u.maxA = std::max(u.maxA, b);
                    u.sumA += b;
                    ++u.countA;
                }
                if (at < 16u)
                    u.first[at / 4u] |= b << (8u * (at % 4u));
            }
            u.got += take;
            if (u.got >= u.expect)
                finishUploadWatch();
        }

        void logAnomaly(const char *what, uint64_t value)
        {
            if (g_anomalyLogs++ < 40u)
                std::fprintf(stderr, "[pgs-gif] anomaly %s value=%016llx\n", what, static_cast<unsigned long long>(value));
        }

        void noteVertex(uint32_t x16, uint32_t y16, bool kick, uint32_t z = 0)
        {
            auto &sh = g_shadow;
            sh.vz = z;
            {
                const float q = sh.q;
                const float u = q != 0.0f ? sh.s / q : 0.0f, v = q != 0.0f ? sh.t / q : 0.0f;
                sh.vu[0] = sh.vu[1];
                sh.vv[0] = sh.vv[1];
                sh.vu[1] = sh.vu[2];
                sh.vv[1] = sh.vv[2];
                sh.vu[2] = u;
                sh.vv[2] = v;
            }
            const uint64_t ofs = sh.xyoffset[sh.ctxt & 1u];
            const int32_t px = (static_cast<int32_t>(x16) - static_cast<int32_t>(ofs & 0xFFFFu)) >> 4;
            const int32_t py = (static_cast<int32_t>(y16) - static_cast<int32_t>((ofs >> 32) & 0xFFFFu)) >> 4;
            sh.vx[0] = sh.vx[1];
            sh.vy[0] = sh.vy[1];
            sh.vx[1] = sh.vx[2];
            sh.vy[1] = sh.vy[2];
            sh.vx[2] = px;
            sh.vy[2] = py;
            ++sh.vcount;
            if (!kick)
                return;
            ++g_frameStats.kicks;
            uint32_t need = 1u;
            switch (sh.primType)
            {
            case 1: case 2: need = 2u; break;         // line, line strip
            case 3: case 4: case 5: need = 3u; break; // triangle, strip, fan
            case 6: need = 2u; break;                 // sprite
            default: need = 1u; break;                // point
            }
            if (sh.vcount < need)
                return;
            if (sh.primType == 3u || sh.primType == 1u || sh.primType == 6u)
                sh.vcount = 0;
            ++g_frameStats.prims;
            int32_t minX = sh.vx[2], maxX = sh.vx[2], minY = sh.vy[2], maxY = sh.vy[2];
            for (uint32_t i = 3u - need; i < 3u; ++i)
            {
                minX = std::min(minX, sh.vx[i]);
                maxX = std::max(maxX, sh.vx[i]);
                minY = std::min(minY, sh.vy[i]);
                maxY = std::max(maxY, sh.vy[i]);
            }
            const uint32_t w = static_cast<uint32_t>(maxX - minX), h = static_cast<uint32_t>(maxY - minY);
            g_frameStats.maxPrimW = std::max(g_frameStats.maxPrimW, w);
            g_frameStats.maxPrimH = std::max(g_frameStats.maxPrimH, h);
            if (w > 2048u || h > 2048u)
                ++g_frameStats.bigPrims;

            if (coverFrame())
            {
                const uint32_t c = sh.ctxt & 1u;
                const uint64_t key = (static_cast<uint64_t>(sh.primType) << 60) | (static_cast<uint64_t>(c) << 59) |
                                     (static_cast<uint64_t>((sh.prim >> 4) & 1u) << 58) | (static_cast<uint64_t>((sh.prim >> 6) & 1u) << 57) |
                                     ((sh.alpha[c] & 0xFFull) << 40) | ((sh.alpha[c] >> 32) & 0xFFull) << 32 | (sh.frame[c] >> 32);
                const int32_t x0 = std::max(minX, 0), x1 = std::min(maxX, 640), y0 = std::max(minY, 0), y1 = std::min(maxY, 512);
                const double area = (x1 > x0 && y1 > y0) ? double(x1 - x0) * double(y1 - y0) / (sh.primType == 6u ? 1.0 : 2.0) : 0.0;
                bool found = false;
                for (uint32_t i = 0; i < g_blendGroupCount && !found; ++i)
                {
                    if (g_blendGroups[i].key == key)
                    {
                        ++g_blendGroups[i].count;
                        g_blendGroups[i].area += area;
                        g_blendGroups[i].rgba = sh.rgba;
                        found = true;
                    }
                }
                if (!found && g_blendGroupCount < 48u)
                    g_blendGroups[g_blendGroupCount++] = BlendGroup{key, 1u, area, sh.rgba, sh.frame[c]};
            }

            // Bug hunt (black level after the intro): primitives that cover most of a 640x448 screen, with the state
            // that decides what they do to the picture (colour, blending, texture), every 120th frame.
            if (coverFrame() && ((sh.prim >> 4) & 1u) != 0u)
            {
                const bool multiply = ((sh.prim >> 6) & 1u) != 0u && (sh.alpha[sh.ctxt & 1u] & 0xFFu) == 0x89u;
                const int k = multiply ? 1 : 0;
                g_zMin[k] = std::min(g_zMin[k], sh.vz);
                g_zMax[k] = std::max(g_zMax[k], sh.vz);
                ++g_zCount[k];
                g_zZero[k] += sh.vz == 0u ? 1u : 0u;
                ++g_zHist[k][sh.vz <= 0xFFFFu ? 0 : sh.vz <= 0xFFFFFu ? 1 : 2];
                if (!multiply)
                {
                    const uint32_t c = sh.ctxt & 1u;
                    bool found = false;
                    for (uint32_t i = 0; i < g_texStateCount && !found; ++i)
                    {
                        auto &ts = g_texStates[i];
                        if (ts.tex1 == sh.tex1[c] && ts.miptbp1 == sh.miptbp1[c] && (ts.tex0 & 0x3FFFu) == (sh.tex0[c] & 0x3FFFu))
                        {
                            ++ts.count;
                            found = true;
                        }
                    }
                    if (!found && g_texStateCount < 16u)
                        g_texStates[g_texStateCount++] = TexState{sh.tex1[c], sh.miptbp1[c], sh.tex0[c], 1u};
                    g_worldTest = sh.test[sh.ctxt & 1u];
                    g_worldZbuf = sh.zbuf[sh.ctxt & 1u];
                    if (((sh.prim >> 8) & 1u) == 0u) // STQ, not UV
                    {
                        auto &st = g_stq;
                        ++st.verts;
                        st.qZero += sh.q == 0.0f ? 1u : 0u;
                        st.qNeg += sh.q < 0.0f ? 1u : 0u;
                        st.qMin = std::min(st.qMin, sh.q);
                        st.qMax = std::max(st.qMax, sh.q);
                        st.uMin = std::min(st.uMin, sh.vu[2]);
                        st.uMax = std::max(st.uMax, sh.vu[2]);
                        st.vMin = std::min(st.vMin, sh.vv[2]);
                        st.vMax = std::max(st.vMax, sh.vv[2]);
                        if (sh.primType >= 3u && sh.primType <= 5u)
                        {
                            ++st.tris;
                            const float du = std::max(std::fabs(sh.vu[0] - sh.vu[2]), std::fabs(sh.vu[1] - sh.vu[2]));
                            const float dv = std::max(std::fabs(sh.vv[0] - sh.vv[2]), std::fabs(sh.vv[1] - sh.vv[2]));
                            st.flatTris += (du < 1e-4f && dv < 1e-4f) ? 1u : 0u;
                        }
                    }
                }
            }
            if (coverFrame() && ((sh.prim >> 6) & 1u) != 0u && (sh.alpha[sh.ctxt & 1u] & 0xFFu) == 0x89u)
            {
                const int32_t x0 = std::max(minX, 0), x1 = std::min(maxX, 640), y0 = std::max(minY, 0), y1 = std::min(maxY, 512);
                if (x1 > x0 && y1 > y0)
                    g_multiplyArea += static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0) / (sh.primType == 6u ? 1u : 2u);
                ++g_multiplyPrims;
                g_multiplyTest = sh.test[sh.ctxt & 1u];
                g_multiplyZbuf = sh.zbuf[sh.ctxt & 1u];
                g_multiplyFrame = sh.frame[sh.ctxt & 1u];
            }
            if (coverFrame())
            {
                const uint32_t r = sh.rgba & 0xFFu, g = (sh.rgba >> 8) & 0xFFu, b = (sh.rgba >> 16) & 0xFFu;
                const uint32_t m = std::max(r, std::max(g, b));
                ++g_colourHist[(sh.prim >> 4) & 1u][m < 8u ? 0 : m < 32u ? 1 : m < 96u ? 2 : 3];
            }
            // PS2X_PGS_BIGPRIM=1: every frame from PS2X_PGS_COVER_FROM on, untextured primitives that cover most of the
            // screen, with the depth state that decides whether they hide the picture (TEST, ZBUF, vertex Z).
            // PS2X_PGS_BIGPRIM=2: fans and sprites only (overlays, clears), not the world's strips.
            // PS2X_PGS_BIGPRIM=3: fans and sprites, textured ones too (post-processing passes that redraw the frame).
            // At most PS2X_PGS_BIGPRIM_MAX lines (default 3000).
            static const uint32_t bigPrimLog = watchBound("PS2X_PGS_BIGPRIM", 0u);
            static const uint32_t bigPrimMax = watchBound("PS2X_PGS_BIGPRIM_MAX", 3000u);
            if (bigPrimLog != 0u && w >= 500u && h >= 350u && w <= 2048u && h <= 2048u &&
                (bigPrimLog == 3u || ((sh.prim >> 4) & 1u) == 0u) && (bigPrimLog == 1u || sh.primType >= 5u) &&
                g_frameNumber >= coverFrom() && g_bigPrimLogs < bigPrimMax)
            {
                ++g_bigPrimLogs;
                const uint32_t c = sh.ctxt & 1u;
                const uint64_t test = sh.test[c];
                std::fprintf(stderr,
                             "[pgs-bigprim] frame %llu type %u tme %u abe %u ctxt %u box=%d,%d..%d,%d rgba=%08x z=%#x alpha=%010llx "
                             "TEST=%llx (ATE %u ATST %u AREF %02x AFAIL %u ZTE %u ZTST %u) ZBUF=%llx (ZBP %#x PSM %#x ZMSK %u) "
                             "FRAME=%llx TEX0=%llx\n",
                             (unsigned long long)g_frameNumber, sh.primType, (unsigned)((sh.prim >> 4) & 1u),
                             (unsigned)((sh.prim >> 6) & 1u), c, minX, minY,
                             maxX, maxY, sh.rgba, sh.vz, (unsigned long long)sh.alpha[c], (unsigned long long)test,
                             (unsigned)(test & 1u), (unsigned)((test >> 1) & 7u), (unsigned)((test >> 4) & 0xFFu),
                             (unsigned)((test >> 12) & 3u), (unsigned)((test >> 16) & 1u), (unsigned)((test >> 17) & 3u),
                             (unsigned long long)sh.zbuf[c], (unsigned)(sh.zbuf[c] & 0x1FFu), (unsigned)((sh.zbuf[c] >> 24) & 0xFu),
                             (unsigned)((sh.zbuf[c] >> 32) & 1u), (unsigned long long)sh.frame[c],
                             (unsigned long long)sh.tex0[c]);
            }
            if (w >= 500u && h >= 350u && w <= 2048u && h <= 2048u && coverFrame() &&
                g_coverLogsThisFrame < 8u && g_coverLogs < 600u)
            {
                ++g_coverLogsThisFrame;
                ++g_coverLogs;
                const uint32_t c = sh.ctxt & 1u;
                std::fprintf(stderr,
                             "[pgs-cover] frame %llu prim=%03llx (type %u tme %u abe %u fst %u ctxt %u) box=%d,%d..%d,%d rgba=%08x "
                             "alpha=%010llx frame=%llx tex0=%016llx\n",
                             (unsigned long long)g_frameNumber, (unsigned long long)(sh.prim & 0x7FFu), sh.primType,
                             (unsigned)((sh.prim >> 4) & 1u), (unsigned)((sh.prim >> 6) & 1u), (unsigned)((sh.prim >> 8) & 1u), c,
                             minX, minY, maxX, maxY, sh.rgba, (unsigned long long)sh.alpha[c], (unsigned long long)sh.frame[c],
                             (unsigned long long)sh.tex0[c]);
            }
        }

        void deterministicSignal(uint32_t reg, uint64_t value)
        {
            if (!deterministicMode() || g_threaded || !g_state || !g_state->signals.sink)
                return;
            if (reg == 0x60u)
                g_state->signals.sink->signal(value);
            else if (reg == 0x61u)
                g_state->signals.sink->finish(value);
            else if (reg == 0x62u)
                g_state->signals.sink->label(value);
        }

        void noteRegister(uint32_t reg, uint64_t value)
        {
            auto &sh = g_shadow;
            deterministicSignal(reg, value);
            switch (reg)
            {
            case 0x00: // PRIM
                sh.primType = static_cast<uint32_t>(value & 7u);
                sh.ctxt = static_cast<uint32_t>((value >> 9) & 1u);
                sh.prim = value;
                sh.vcount = 0;
                break;
            case 0x01: // RGBAQ (Q = bits 32-63)
            {
                sh.rgba = static_cast<uint32_t>(value);
                const uint32_t qBits = static_cast<uint32_t>(value >> 32);
                std::memcpy(&sh.q, &qBits, 4u);
                break;
            }
            case 0x02: // ST
            {
                const uint32_t sBits = static_cast<uint32_t>(value), tBits = static_cast<uint32_t>(value >> 32);
                std::memcpy(&sh.s, &sBits, 4u);
                std::memcpy(&sh.t, &tBits, 4u);
                break;
            }
            case 0x42: case 0x43: // ALPHA_1 / _2
                sh.alpha[reg - 0x42] = value;
                break;
            case 0x4C: case 0x4D: // FRAME_1 / _2
                sh.frame[reg - 0x4C] = value;
                break;
            case 0x14: case 0x15: // TEX1_1 / _2
                sh.tex1[reg - 0x14] = value;
                break;
            case 0x34: case 0x35: // MIPTBP1_1 / _2
                sh.miptbp1[reg - 0x34] = value;
                break;
            case 0x47: case 0x48: // TEST_1 / _2
                sh.test[reg - 0x47] = value;
                break;
            case 0x4E: case 0x4F: // ZBUF_1 / _2
                sh.zbuf[reg - 0x4E] = value;
                break;
            case 0x04: case 0x05: // XYZF2 / XYZ2: vertex + kick
                noteVertex(static_cast<uint32_t>(value & 0xFFFFu), static_cast<uint32_t>((value >> 16) & 0xFFFFu), true,
                           reg == 0x04 ? static_cast<uint32_t>((value >> 32) & 0xFFFFFFu) : static_cast<uint32_t>(value >> 32));
                break;
            case 0x0C: case 0x0D: // XYZF3 / XYZ3: vertex, no kick
                noteVertex(static_cast<uint32_t>(value & 0xFFFFu), static_cast<uint32_t>((value >> 16) & 0xFFFFu), false);
                break;
            case 0x06: case 0x07: // TEX0_1 / TEX0_2
            {
                sh.tex0[reg - 0x06] = value;
                ++g_frameStats.tex0;
                const uint32_t tw = static_cast<uint32_t>((value >> 26) & 0xFu);
                const uint32_t th = static_cast<uint32_t>((value >> 30) & 0xFu);
                if (((value >> 61) & 7u) != 0u)
                    ++g_frameStats.clutLoads;
                if (tw > 10u || th > 10u)
                {
                    ++g_frameStats.badTex;
                    logAnomaly("TEX0 TW/TH > 10", value);
                }
                break;
            }
            case 0x18: case 0x19: // XYOFFSET_1 / _2
                sh.xyoffset[reg - 0x18] = value;
                break;
            case 0x50:
                sh.bitbltbuf = value;
                break;
            case 0x51:
                sh.trxpos = value;
                break;
            case 0x52:
                sh.trxreg = value;
                break;
            case 0x53: // TRXDIR starts a transfer
            {
                const uint32_t dir = static_cast<uint32_t>(value & 3u);
                const uint32_t rrw = static_cast<uint32_t>(sh.trxreg & 0xFFFu);
                const uint32_t rrh = static_cast<uint32_t>((sh.trxreg >> 32) & 0xFFFu);
                if (dir == 2u)
                    ++g_frameStats.localCopies;
                else
                    ++g_frameStats.uploads;
                const uint32_t dbw = static_cast<uint32_t>((sh.bitbltbuf >> 48) & 0x3Fu);
                if (g_upload.active)
                    finishUploadWatch(); // the previous one ended short
                const uint32_t dbp = static_cast<uint32_t>((sh.bitbltbuf >> 32) & 0x3FFFu);
                static const uint32_t watchLo = watchBound("PS2X_PGS_WATCH_LO", 0x3800u);
                static const uint32_t watchHi = watchBound("PS2X_PGS_WATCH_HI", 0x3a40u);
                if (dir == 0u && dbp >= watchLo && dbp < watchHi && g_frameNumber >= coverFrom() && g_uploadLogs < 120u)
                {
                    ++g_uploadLogs;
                    g_upload = UploadWatch{};
                    g_upload.active = true;
                    g_upload.dbp = dbp;
                    g_upload.dbw = dbw;
                    g_upload.dpsm = static_cast<uint32_t>((sh.bitbltbuf >> 56) & 0x3Fu);
                    g_upload.rrw = rrw;
                    g_upload.rrh = rrh;
                    g_upload.dx = static_cast<uint32_t>((sh.trxpos >> 32) & 0x7FFu);
                    g_upload.dy = static_cast<uint32_t>((sh.trxpos >> 48) & 0x7FFu);
                    g_upload.expect = static_cast<uint64_t>(rrw) * rrh * psmBits(g_upload.dpsm) / 8u;
                    if (g_upload.expect == 0u)
                        g_upload.active = false;
                }
                if (dir != 3u && (rrw == 0u || rrh == 0u || (dbw == 0u && dir != 1u)))
                {
                    ++g_frameStats.badTransfers;
                    logAnomaly("TRXDIR with empty TRXREG or DBW=0, trxreg", sh.trxreg);
                    logAnomaly("  ... bitbltbuf", sh.bitbltbuf);
                }
                break;
            }
            default:
                break;
            }
        }

        // Bug-hunt experiments on the GIF stream (PS2X_PGS_EXPERIMENT, bit mask; off by default):
        //   1 = neutralise the multiply passes (ALPHA 0x89 = Cd * As becomes Cd: the lightmaps are not applied);
        //   2 = do not draw triangles that use a vertex at GS (0,0) (VU1 clipper garbage, see CLAUDE.md issue 6);
        //   4 = do not draw context-1 primitives (Rayman: lightmap, fog and shadow passes);
        //   8 = do not draw textured context-0 primitives (Rayman: the base pass of the world);
        //  16 = draw textured context-0 primitives without alpha blending (opaque);
        //  32 = do not draw untextured, alpha-blended black primitives (fades and dimming overlays);
        //  64 = TEST AFAIL FB_ONLY becomes KEEP (pixels failing the alpha test are not written at all).
        uint32_t experimentMask()
        {
            static const uint32_t mask = watchBound("PS2X_PGS_EXPERIMENT", 0u);
            return mask;
        }

        uint64_t g_experimentPrim = 0u; // PRIM as the experiments see it (the GS register persists across packets)
        uint32_t g_experimentRgb = 0u;  // R, G, B of RGBAQ as the experiments see it

        bool experimentSkips(uint32_t mask)
        {
            const bool ctxt1 = ((g_experimentPrim >> 9) & 1u) != 0u, textured = ((g_experimentPrim >> 4) & 1u) != 0u;
            const bool blended = ((g_experimentPrim >> 6) & 1u) != 0u;
            return ((mask & 4u) != 0u && ctxt1) || ((mask & 8u) != 0u && !ctxt1 && textured) ||
                   ((mask & 32u) != 0u && !textured && blended && g_experimentRgb == 0u);
        }

        void experimentGif(uint8_t *data, size_t bytes, uint32_t mask)
        {
            size_t offset = 0u;
            while (bytes - offset >= 16u)
            {
                uint64_t lo = 0u, regs = 0u;
                std::memcpy(&lo, data + offset, 8u);
                std::memcpy(&regs, data + offset + 8u, 8u);
                const size_t packetBytes = gifPacketBytes(data + offset);
                if (bytes - offset < packetBytes)
                    break;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                uint8_t *body = data + offset + 16u;
                uint32_t suppressKicks = 0u;
                if ((lo >> 46) & 1u) // PRE
                {
                    g_experimentPrim = (lo >> 47) & 0x7FFu;
                    if ((mask & 16u) != 0u && (g_experimentPrim & 0x210u) == 0x10u) // textured, context 0
                    {
                        lo &= ~(1ull << (47 + 6)); // ABE off
                        std::memcpy(data + offset, &lo, 8u);
                    }
                }
                if (flg == 0u) // PACKED
                {
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            uint8_t *q = body + (static_cast<size_t>(loop) * nreg + r) * 16u;
                            uint64_t qlo = 0u, qhi = 0u;
                            std::memcpy(&qlo, q, 8u);
                            std::memcpy(&qhi, q + 8u, 8u);
                            const uint32_t desc = static_cast<uint32_t>((regs >> (4u * r)) & 0xFu);
                            const bool primWrite = desc == 0x0u || (desc == 0xEu && (qhi & 0xFFu) == 0x00u);
                            if (primWrite)
                            {
                                g_experimentPrim = qlo & 0x7FFu;
                                if ((mask & 16u) != 0u && (g_experimentPrim & 0x210u) == 0x10u)
                                {
                                    qlo &= ~0x40ull; // ABE off
                                    std::memcpy(q, &qlo, 8u);
                                }
                            }
                            if (desc == 0x1u) // RGBAQ packed: R 0-7, G 32-39, B 64-71
                                g_experimentRgb = static_cast<uint32_t>((qlo & 0xFFu) | ((qlo >> 24) & 0xFF00u) | ((qhi & 0xFFu) << 16));
                            if (desc == 0xEu)
                            {
                                const uint32_t adReg = static_cast<uint32_t>(qhi & 0xFFu);
                                if (adReg == 0x01u)
                                    g_experimentRgb = static_cast<uint32_t>(qlo & 0xFFFFFFu);
                                if ((mask & 1u) != 0u && (adReg == 0x42u || adReg == 0x43u) && (qlo & 0xFFu) == 0x89u)
                                {
                                    qlo = (qlo & ~0xFFull) | 0x65u; // (Cd - Cd) * FIX + Cd = Cd
                                    std::memcpy(q, &qlo, 8u);
                                }
                                if ((mask & 64u) != 0u && (adReg == 0x47u || adReg == 0x48u) && ((qlo >> 12) & 3u) == 1u)
                                {
                                    qlo &= ~(3ull << 12); // AFAIL FB_ONLY -> KEEP: pixels failing the alpha test are not written
                                    std::memcpy(q, &qlo, 8u);
                                }
                                if ((adReg == 0x04u || adReg == 0x05u) && experimentSkips(mask))
                                {
                                    qhi = (qhi & ~0xFFull) | (adReg + 8u); // XYZ(F)2 -> XYZ(F)3: no drawing kick
                                    std::memcpy(q + 8u, &qhi, 8u);
                                }
                            }
                            if ((desc == 0x4u || desc == 0x5u) && experimentSkips(mask))
                            {
                                qhi |= 1ull << 47; // ADC: no drawing kick
                                std::memcpy(q + 8u, &qhi, 8u);
                            }
                            if ((mask & 2u) != 0u && (desc == 0x4u || desc == 0x5u))
                            {
                                // A strip or fan triangle uses the last three vertices: suppress three kicks.
                                if ((qlo & 0xFFFFu) == 0u && ((qlo >> 32) & 0xFFFFu) == 0u)
                                    suppressKicks = 3u;
                                if (suppressKicks != 0u)
                                {
                                    --suppressKicks;
                                    qhi |= 1ull << 47; // ADC: no drawing kick
                                    std::memcpy(q + 8u, &qhi, 8u);
                                }
                            }
                        }
                    }
                }
                offset += packetBytes;
            }
        }

        void analyzeGif(const uint8_t *data, size_t bytes)
        {
            size_t offset = 0u;
            while (bytes - offset >= 16u)
            {
                uint64_t lo = 0u, regs = 0u;
                std::memcpy(&lo, data + offset, 8u);
                std::memcpy(&regs, data + offset + 8u, 8u);
                const size_t packetBytes = gifPacketBytes(data + offset);
                if (bytes - offset < packetBytes)
                    break;
                ++g_frameStats.packets;
                g_frameStats.qwords += packetBytes / 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                if ((lo >> 46) & 1u) // PRE: the tag's PRIM field goes to PRIM
                    noteRegister(0x00, (lo >> 47) & 0x7FFu);
                const uint8_t *body = data + offset + 16u;
                if (flg == 0u) // PACKED
                {
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                    {
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            const uint8_t *q = body + (static_cast<size_t>(loop) * nreg + r) * 16u;
                            uint64_t qlo = 0u, qhi = 0u;
                            std::memcpy(&qlo, q, 8u);
                            std::memcpy(&qhi, q + 8u, 8u);
                            const uint32_t desc = static_cast<uint32_t>((regs >> (4u * r)) & 0xFu);
                            switch (desc)
                            {
                            case 0x0:
                                noteRegister(0x00, qlo);
                                break;
                            case 0x2:
                            {
                                const uint32_t sBits = static_cast<uint32_t>(qlo), tBits = static_cast<uint32_t>(qlo >> 32),
                                               qBits = static_cast<uint32_t>(qhi);
                                std::memcpy(&g_shadow.s, &sBits, 4u);
                                std::memcpy(&g_shadow.t, &tBits, 4u);
                                std::memcpy(&g_shadow.q, &qBits, 4u);
                                break;
                            }
                            case 0x1:
                                g_shadow.rgba = static_cast<uint32_t>((qlo & 0xFFu) | (((qlo >> 32) & 0xFFu) << 8) |
                                                                      ((qhi & 0xFFu) << 16) | (((qhi >> 32) & 0xFFu) << 24));
                                break;
                            case 0x4: // XYZF2 / XYZ2 packed: X bits 0-15, Y bits 32-47, ADC bit 111
                            case 0x5:
                                noteVertex(static_cast<uint32_t>(qlo & 0xFFFFu), static_cast<uint32_t>((qlo >> 32) & 0xFFFFu),
                                           ((qhi >> 47) & 1u) == 0u,
                                           desc == 0x4 ? static_cast<uint32_t>((qhi >> 4) & 0xFFFFFFu) : static_cast<uint32_t>(qhi));
                                break;
                            case 0xC: case 0xD:
                                noteVertex(static_cast<uint32_t>(qlo & 0xFFFFu), static_cast<uint32_t>((qlo >> 32) & 0xFFFFu), false);
                                break;
                            case 0x6: case 0x7:
                                noteRegister(desc, qlo);
                                break;
                            case 0xE: // A+D
                            {
                                const uint32_t adReg = static_cast<uint32_t>(qhi & 0xFFu);
                                noteRegister(adReg, qlo);
                                break;
                            }
                            default:
                                break;
                            }
                        }
                    }
                }
                else if (flg == 1u) // REGLIST
                {
                    const size_t words = static_cast<size_t>(nloop) * nreg;
                    for (size_t i = 0; i < words; ++i)
                    {
                        uint64_t value = 0u;
                        std::memcpy(&value, body + i * 8u, 8u);
                        const uint32_t desc = static_cast<uint32_t>((regs >> (4u * (i % nreg))) & 0xFu);
                        if (desc <= 0xDu)
                            noteRegister(desc, value);
                    }
                }
                else
                {
                    g_frameStats.imageQwords += nloop;
                    noteImageData(body, std::min<size_t>(static_cast<size_t>(nloop) * 16u, bytes - offset - 16u));
                }
                offset += packetBytes;
            }
        }

        void endFrameStats()
        {
            if (coverFrame() && g_coverLogs < 600u)
            {
                ++g_coverLogs;
                std::fprintf(stderr,
                             "[pgs-cover] frame %llu vertex colour max(R,G,B): untextured 0-7:%llu 8-31:%llu 32-95:%llu 96+:%llu | "
                             "textured 0-7:%llu 8-31:%llu 32-95:%llu 96+:%llu\n",
                             (unsigned long long)g_frameNumber, (unsigned long long)g_colourHist[0][0],
                             (unsigned long long)g_colourHist[0][1], (unsigned long long)g_colourHist[0][2],
                             (unsigned long long)g_colourHist[0][3], (unsigned long long)g_colourHist[1][0],
                             (unsigned long long)g_colourHist[1][1], (unsigned long long)g_colourHist[1][2],
                             (unsigned long long)g_colourHist[1][3]);
            }
            if (coverFrame() && g_coverLogs < 600u)
            {
                ++g_coverLogs;
                // TEST: ATE 0, ATST 1-3, AREF 4-11, AFAIL 12-13, DATE 14, DATM 15, ZTE 16, ZTST 17-18 (0 never, 1 always,
                // 2 gequal, 3 greater). ZBUF: ZBP 0-8, PSM 24-27, ZMSK 32.
                std::fprintf(stderr,
                             "[pgs-cover] frame %llu multiply passes (Cd*As): %llu prims, area %.2f screens; last TEST=%llx (ZTE %u ZTST %u) "
                             "ZBUF=%llx (ZBP %#x ZMSK %u) FRAME=%llx\n",
                             (unsigned long long)g_frameNumber, (unsigned long long)g_multiplyPrims, double(g_multiplyArea) / (640.0 * 448.0),
                             (unsigned long long)g_multiplyTest, (unsigned)((g_multiplyTest >> 16) & 1u),
                             (unsigned)((g_multiplyTest >> 17) & 3u), (unsigned long long)g_multiplyZbuf,
                             (unsigned)(g_multiplyZbuf & 0x1FFu), (unsigned)((g_multiplyZbuf >> 32) & 1u),
                             (unsigned long long)g_multiplyFrame);
            }
            if (coverFrame() && g_coverLogs < 600u)
            {
                ++g_coverLogs;
                std::fprintf(stderr,
                             "[pgs-cover] frame %llu Z of textured vertices: world %llu verts Z %#x..%#x (zero %llu) TEST=%llx ZBUF=%llx | "
                             "multiply %llu verts Z %#x..%#x (zero %llu) | Z buckets <=16/<=20/>20 bits: world %llu/%llu/%llu "
                             "multiply %llu/%llu/%llu\n",
                             (unsigned long long)g_frameNumber, (unsigned long long)g_zCount[0], g_zCount[0] ? g_zMin[0] : 0u, g_zMax[0],
                             (unsigned long long)g_zZero[0], (unsigned long long)g_worldTest, (unsigned long long)g_worldZbuf,
                             (unsigned long long)g_zCount[1], g_zCount[1] ? g_zMin[1] : 0u, g_zMax[1], (unsigned long long)g_zZero[1],
                             (unsigned long long)g_zHist[0][0], (unsigned long long)g_zHist[0][1], (unsigned long long)g_zHist[0][2],
                             (unsigned long long)g_zHist[1][0], (unsigned long long)g_zHist[1][1], (unsigned long long)g_zHist[1][2]);
            }
            if (coverFrame() && g_coverLogs < 600u)
            {
                ++g_coverLogs;
                const auto &st = g_stq;
                std::fprintf(stderr,
                             "[pgs-cover] frame %llu STQ world: %llu verts, Q %g..%g (zero %llu, negative %llu), S/Q %g..%g, T/Q %g..%g, "
                             "triangles %llu of which all 3 vertices share one texel coordinate: %llu\n",
                             (unsigned long long)g_frameNumber, (unsigned long long)st.verts, st.verts ? st.qMin : 0.0f,
                             st.verts ? st.qMax : 0.0f, (unsigned long long)st.qZero, (unsigned long long)st.qNeg,
                             st.verts ? st.uMin : 0.0f, st.verts ? st.uMax : 0.0f, st.verts ? st.vMin : 0.0f, st.verts ? st.vMax : 0.0f,
                             (unsigned long long)st.tris, (unsigned long long)st.flatTris);
            }
            if (coverFrame() && g_coverLogs < 600u)
            {
                for (uint32_t i = 0; i < g_texStateCount && g_coverLogs < 600u; ++i)
                {
                    ++g_coverLogs;
                    const auto &ts = g_texStates[i];
                    // TEX1: LCM 0, MXL 2-4, MMAG 5, MMIN 6-8, MTBA 9, L 19-20, K 32-43 (signed, 1/16 units).
                    const int32_t k = static_cast<int32_t>(static_cast<uint32_t>((ts.tex1 >> 32) & 0xFFFu) << 20) >> 20;
                    std::fprintf(stderr,
                                 "[pgs-cover] frame %llu world tex state x%llu: TEX1=%llx (LCM %u MXL %u MMAG %u MMIN %u MTBA %u L %u K %.2f) "
                                 "MIPTBP1=%llx TEX0=%llx\n",
                                 (unsigned long long)g_frameNumber, (unsigned long long)ts.count, (unsigned long long)ts.tex1,
                                 (unsigned)(ts.tex1 & 1u), (unsigned)((ts.tex1 >> 2) & 7u), (unsigned)((ts.tex1 >> 5) & 1u),
                                 (unsigned)((ts.tex1 >> 6) & 7u), (unsigned)((ts.tex1 >> 9) & 1u), (unsigned)((ts.tex1 >> 19) & 3u),
                                 k / 16.0, (unsigned long long)ts.miptbp1, (unsigned long long)ts.tex0);
                }
            }
            g_texStateCount = 0;
            if (coverFrame() && g_coverLogs < 600u)
            {
                for (uint32_t i = 0; i < g_blendGroupCount && g_coverLogs < 600u; ++i)
                {
                    const auto &b = g_blendGroups[i];
                    if (b.area < 640.0 * 448.0 * 0.05)
                        continue; // only groups that cover at least 5% of a screen
                    ++g_coverLogs;
                    std::fprintf(stderr,
                                 "[pgs-blend] frame %llu type %u ctxt %u tme %u abe %u ALPHA=%02x FIX=%02x FBMSK=%08x FBP=%#x: %llu prims, "
                                 "%.2f screens, last rgba %08x\n",
                                 (unsigned long long)g_frameNumber, (unsigned)(b.key >> 60), (unsigned)((b.key >> 59) & 1u),
                                 (unsigned)((b.key >> 58) & 1u), (unsigned)((b.key >> 57) & 1u), (unsigned)((b.key >> 40) & 0xFFu),
                                 (unsigned)((b.key >> 32) & 0xFFu), (unsigned)(b.key & 0xFFFFFFFFu), (unsigned)(b.frame & 0x1FFu),
                                 (unsigned long long)b.count, b.area / (640.0 * 448.0), b.rgba);
                }
            }
            g_blendGroupCount = 0;
            g_stq = StqStats{};
            for (int k = 0; k < 2; ++k)
            {
                g_zMin[k] = 0xFFFFFFFFu;
                g_zMax[k] = 0u;
                g_zCount[k] = g_zZero[k] = 0u;
                g_zHist[k][0] = g_zHist[k][1] = g_zHist[k][2] = 0u;
            }
            g_multiplyArea = g_multiplyPrims = 0;
            std::memset(g_colourHist, 0, sizeof(g_colourHist));
            ++g_frameNumber;
            g_coverLogsThisFrame = 0;
            auto &f = g_frameStats;
            auto &w = g_windowStats;
            if ((g_traceFrames && f.packets) || f.bigPrims || f.badTex || f.badTransfers)
            {
                if (g_traceFrames || g_anomalyLogs < 200u)
                {
                    ++g_anomalyLogs;
                    std::fprintf(stderr,
                                 "[pgs-gif] frame pkts=%llu qw=%llu imageQw=%llu kicks=%llu prims=%llu tex0=%llu clut=%llu "
                                 "uploads=%llu copies=%llu maxPrim=%ux%u big=%llu badTex=%llu badXfer=%llu\n",
                                 (unsigned long long)f.packets, (unsigned long long)f.qwords, (unsigned long long)f.imageQwords,
                                 (unsigned long long)f.kicks, (unsigned long long)f.prims, (unsigned long long)f.tex0,
                                 (unsigned long long)f.clutLoads, (unsigned long long)f.uploads, (unsigned long long)f.localCopies,
                                 f.maxPrimW, f.maxPrimH, (unsigned long long)f.bigPrims, (unsigned long long)f.badTex,
                                 (unsigned long long)f.badTransfers);
                    std::fflush(stderr);
                }
            }
            w.packets += f.packets;
            w.qwords += f.qwords;
            w.imageQwords += f.imageQwords;
            w.kicks += f.kicks;
            w.prims += f.prims;
            w.tex0 += f.tex0;
            w.clutLoads += f.clutLoads;
            w.uploads += f.uploads;
            w.localCopies += f.localCopies;
            w.bigPrims += f.bigPrims;
            w.badTex += f.badTex;
            w.badTransfers += f.badTransfers;
            w.maxPrimW = std::max(w.maxPrimW, f.maxPrimW);
            w.maxPrimH = std::max(w.maxPrimH, f.maxPrimH);
            f = GifStats{};
        }

        // The runtime's high-level libgraph stubs (sceGsResetGraph / sceGsSetDefDispEnv) program DISPLAY in
        // framebuffer pixels (MAGH = 0, DW = width - 1, DX/DY = 0 or small). Real hardware counts DW/DX in
        // video clocks (2560 per line, MAGH = clocks per pixel - 1). Convert such values so the scanout
        // covers the whole screen instead of a quarter of it.
        uint64_t hardwareDisplay(uint64_t display, bool pal)
        {
            const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
            const uint32_t dw = static_cast<uint32_t>((display >> 32) & 0xFFFu);
            if (display == 0u || magh != 0u || dw + 1u > 1024u)
                return display;
            const uint32_t pixels = dw + 1u;
            const uint32_t newMagh = std::min<uint32_t>(15u, std::max<uint32_t>(1u, 2560u / pixels) - 1u);
            const uint32_t clocks = newMagh + 1u;
            // Place it at the start of the visible area paraLLEl-GS assumes for analog modes (169 / 159 pixels
            // of 4 clocks, 36 / 25 lines per field); the stubs' own offsets use a different convention.
            const uint32_t newDx = pal ? 676u : 636u;
            const uint32_t newDy = pal ? 72u : 50u;
            const uint32_t newDw = std::min<uint32_t>(0xFFFu, pixels * clocks - 1u);
            uint64_t out = display & ~((0xFFFull) | (0x7FFull << 12) | (0xFull << 23) | (0xFFFull << 32));
            out |= static_cast<uint64_t>(newDx) | (static_cast<uint64_t>(newDy) << 12) |
                   (static_cast<uint64_t>(newMagh) << 23) | (static_cast<uint64_t>(newDw) << 32);
            return out;
        }

        // Rayman bring-up: a picture of the GPU output every ~5 s (out/pgs_frame.bmp, 32-bit top-down).
        void dumpFrameBmp(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h, const char *path = "out/pgs_frame.bmp")
        {
            if (w == 0u || h == 0u || rgba.size() < static_cast<size_t>(w) * h * 4u)
                return;
            FILE *f = std::fopen(path, "wb");
            if (!f)
                return;
            const uint32_t imageBytes = w * h * 4u;
            uint8_t header[54] = {};
            const auto put16 = [&](int at, uint32_t v) { header[at] = uint8_t(v); header[at + 1] = uint8_t(v >> 8); };
            const auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i)); };
            header[0] = 'B';
            header[1] = 'M';
            put32(2, 54u + imageBytes);
            put32(10, 54u);
            put32(14, 40u);
            put32(18, w);
            put32(22, static_cast<uint32_t>(-static_cast<int32_t>(h))); // top-down
            put16(26, 1u);
            put16(28, 32u);
            put32(34, imageBytes);
            std::fwrite(header, 1u, sizeof(header), f);
            std::vector<uint8_t> bgra(imageBytes);
            for (size_t i = 0; i < imageBytes; i += 4)
            {
                bgra[i] = rgba[i + 2];
                bgra[i + 1] = rgba[i + 1];
                bgra[i + 2] = rgba[i];
                bgra[i + 3] = 0xFFu;
            }
            std::fwrite(bgra.data(), 1u, bgra.size(), f);
            std::fclose(f);
        }

        // Granite keys command pools by a per-thread index. The GS thread, the EE thread and the window thread
        // call in (never at the same time: g_mutex), so all use index 0.
        void bindThread()
        {
            static thread_local bool bound = false;
            if (!bound)
            {
                Util::register_thread_index(0);
                bound = true;
            }
        }

        uint32_t pathIndex(uint32_t path)
        {
            // paraLLEl-GS keeps GIF tag state for PATH1..3 in slots 1..3 (slot 0 unused by the runtime).
            return (path >= 1u && path <= 3u) ? path : 3u;
        }
    }

    namespace
    {
        // A register address no GS register uses (they end at 0x63): carries the HUD layer's frame mark through the
        // same ordered path as the game's GS writes (ps2xHudFrameMark, gs_hud.h markFrame).
        constexpr uint32_t kHudFrameMarkReg = 0x7Fu;
    }

    namespace
    {
        // ------------------------------------------------------------------ the work itself (under g_mutex)

        // Complete GIF packets of one path to paraLLEl-GS (with the bring-up walk, a running capture, experiments).
        void forwardNow(uint32_t index, const uint8_t *packets, size_t size)
        {
            if (experimentMask() != 0u)
            {
                std::vector<uint8_t> changed(packets, packets + size);
                experimentGif(changed.data(), changed.size(), experimentMask());
                analyzeGif(changed.data(), changed.size());
                captureRecord(0u, index, changed.size(), changed.data());
                g_state->gs->gif_transfer(index, changed.data(), changed.size());
                return;
            }
            analyzeGif(packets, size);
            captureRecord(0u, index, size, packets);
            if (hud::enabled())
            {
                // The game's 2D layer is drawn by the window at screen resolution (gs_hud.h): paraLLEl-GS gets those
                // vertices non-drawing.
                static std::vector<uint8_t> s_hudScratch; // under g_mutex
                packets = hud::filterPacket(packets, size, s_hudScratch);
            }
            g_state->gs->gif_transfer(index, packets, size);
        }

        void writeRegisterNow(uint32_t reg, uint64_t value)
        {
            if (reg == kHudFrameMarkReg) // not a GS register: the HUD layer's frame mark (ps2xHudFrameMark)
            {
                hud::markFrame(value != 0u);
                return;
            }
            noteRegister(reg, value); // also delivers SIGNAL / FINISH / LABEL in deterministic synchronous mode
            captureRecord(1u, reg, value, nullptr);
            if (hud::enabled())
                hud::registerWrite(reg, value);
            g_state->gs->write_register(static_cast<ParallelGS::RegisterAddr>(reg), value);
        }

        size_t readTransferNow(void *dst, size_t bytes)
        {
            const size_t qwords = (bytes + 15u) / 16u;
            if (qwords * 16u == bytes)
            {
                g_state->gs->read_transfer_fifo(dst, static_cast<uint32_t>(qwords));
            }
            else
            {
                std::vector<uint8_t> tmp(qwords * 16u);
                g_state->gs->read_transfer_fifo(tmp.data(), static_cast<uint32_t>(qwords));
                std::memcpy(dst, tmp.data(), bytes);
            }
            return bytes;
        }

        bool scanoutNow(const PrivRegs &regs, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height,
                        PendingReadback *async = nullptr)
        {
            width = height = 0u;
            const auto begin = std::chrono::steady_clock::now();

            // Privileged registers (PMODE, SMODE*, DISPFB*, DISPLAY*, BGCOLOR ...): 64-bit bitfield structs.
            auto &priv = g_state->gs->get_priv_register_state();
            const auto put = [](auto &field, uint64_t value)
            {
                // Bitfield structs mirroring the 64-bit registers (one is a few bits wider by declaration).
                std::memcpy(&field, &value, std::min(sizeof(field), sizeof(value)));
            };
            // SMODE1 is written by the BIOS (SetGsCrt), which the runtime high-level emulates without it, so it
            // can still be 0 here. paraLLEl-GS needs CMOD/LC to know the video mode: use the analog PAL/NTSC
            // value then (PAL when the display area is taller than NTSC's 480 lines).
            uint64_t smode1 = regs.smode1;
            if (((smode1 >> 3) & 0x7Full) == 0ull)
            {
                const uint64_t display = (regs.pmode & 2ull) != 0ull ? regs.display2 : regs.display1;
                const uint32_t lines = static_cast<uint32_t>((display >> 44) & 0x7FFull) + 1u;
                const uint64_t cmod = lines > 480u ? 3ull : 2ull;
                smode1 = 4ull | (32ull << 3) | (1ull << 10) | (cmod << 13) | (1ull << 16) | (1ull << 17) |
                         (4ull << 21) | (1ull << 30);
            }
            put(priv.pmode, regs.pmode);
            put(priv.smode1, smode1);
            put(priv.smode2, regs.smode2);
            put(priv.srfsh, regs.srfsh);
            put(priv.synch1, regs.synch1);
            put(priv.synch2, regs.synch2);
            put(priv.syncv, regs.syncv);
            const bool pal = ((smode1 >> 13) & 3ull) == 3ull;
            g_lastRegs = regs;
            put(priv.dispfb1, regs.dispfb1);
            put(priv.display1, hardwareDisplay(regs.display1, pal));
            put(priv.dispfb2, regs.dispfb2);
            put(priv.display2, hardwareDisplay(regs.display2, pal));
            put(priv.extbuf, regs.extbuf);
            put(priv.extdata, regs.extdata);
            put(priv.extwrite, regs.extwrite);
            put(priv.bgcolor, regs.bgcolor);

            ParallelGS::VSyncInfo info = {};
            info.phase = 0;
            info.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
            info.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            info.force_progressive = true;
            info.anti_blur = true;
            info.overscan = false;
            info.crtc_offsets = false;
            info.adapt_to_internal_horizontal_resolution = false;
            // PS2X_GS_SSAA >= 4: scan out at twice the resolution from the super samples (see start()).
            info.high_resolution_scanout = g_hiresScanout;

            if (watchBound("PS2X_PGS_BIGPRIM", 0u) != 0u && g_frameNumber >= coverFrom() &&
                g_bigPrimLogs < watchBound("PS2X_PGS_BIGPRIM_MAX", 3000u))
            {
                ++g_bigPrimLogs;
                std::fprintf(stderr, "[pgs-scan] frame %llu guest vsync %llu pmode=%llx dispfb1=%llx dispfb2=%llx\n",
                             (unsigned long long)g_frameNumber, (unsigned long long)ps2xGuestVsync(), (unsigned long long)regs.pmode,
                             (unsigned long long)regs.dispfb1, (unsigned long long)regs.dispfb2);
            }
            endFrameStats();
            captureFrameBoundary(g_frameNumber);
            if (hud::enabled())
                hud::publish(regs.pmode, regs.dispfb1, regs.dispfb2); // the displayed buffer's 2D layer
            g_state->gs->flush();
            ParallelGS::ScanoutResult result = g_state->gs->vsync(info);
            if (!result.image)
                return false;

            auto &device = *g_state->device;
            const uint32_t w = result.image->get_width();
            const uint32_t h = result.image->get_height();
            const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4u;
            if (!g_state->readback || g_state->readbackSize < size)
            {
                Vulkan::BufferCreateInfo info = {};
                info.size = size;
                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                info.domain = Vulkan::BufferDomain::CachedHost;
                g_state->readback = device.create_buffer(info);
                g_state->readbackSize = size;
            }

            auto cmd = device.request_command_buffer();
            cmd->image_barrier(*result.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            cmd->copy_image_to_buffer(*g_state->readback, *result.image, 0, {}, {w, h, 1}, 0, 0,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            Vulkan::Fence fence;
            device.submit(cmd, &fence);

            PendingReadback readback;
            readback.device = &device;
            readback.fence = fence;
            readback.buffer = g_state->readback;
            readback.width = w;
            readback.height = h;
            width = w;
            height = h;
            g_state->lastWidth = w;
            g_state->lastHeight = h;
            ++g_state->frames;
            if (async && g_capturePictures == 0u)
            {
                *async = std::move(readback); // the window thread waits and copies (finishReadback)
                g_state->scanoutMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                ++g_state->scanoutSamples;
                return true;
            }
            if (!finishReadback(readback, rgba))
                return false;
            static uint64_t s_dumpCounter = 0u;
            if ((s_dumpCounter++ % 300u) == 30u)
                dumpFrameBmp(rgba, w, h);
            if (g_capturePictures != 0u)
            {
                --g_capturePictures;
                char path[64];
                std::snprintf(path, sizeof(path), "out/gif_capture_frame%u.bmp", g_capturePictureIndex++);
                dumpFrameBmp(rgba, w, h, path);
            }
            g_state->scanoutMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            ++g_state->scanoutSamples;
            return true;
        }

        // ------------------------------------------------------------------ the GS thread (PS2X_GS_THREAD)
        //
        // paraLLEl-GS's CPU side (building primitives, hazards, batches) and the bring-up walk took ~20% of the
        // game thread at 60 FPS, and the window thread's scanout (flush, vsync, readback, fence wait: 3-4 ms) held
        // the lock the game thread needs for every packet (~9% waiting). With the GS thread (default; 0 = the old
        // synchronous bridge) the game thread only copies each complete packet / register write into a queue; one
        // worker thread runs everything in order, scanouts included: the window thread queues a scanout request
        // (with the privileged registers of that moment, so the picture matches that point of the stream) and
        // waits for that picture. Everything the GS sends back to the guest is kept synchronous:
        //   - SIGNAL / FINISH / LABEL reach the guest when the game thread queues the write (deterministicSignal's
        //     rule, which the deterministic test mode always used), not when the worker gets there;
        //   - a GS -> host image transfer (readTransfer) waits until the worker has done everything queued before.
        // So the guest sees what it saw before, deterministic runs stay deterministic, and the stream order is kept.
        struct Command
        {
            enum Kind : uint8_t
            {
                Gif,
                Register,
                Scanout
            } kind;
            uint32_t a;    // Gif: path index; Register: register; Scanout: index in Batch::regs
            uint64_t b;    // Gif: offset in Batch::bytes; Register: value; Scanout: request id
            uint32_t size; // Gif: bytes
        };
        struct Batch
        {
            std::vector<Command> commands;
            std::vector<uint8_t> bytes;
            std::vector<PrivRegs> regs;
        };

        constexpr size_t kMaxQueuedBytes = 8u << 20; // the game thread waits when the worker is this far behind

        std::mutex g_queueMutex;
        std::condition_variable g_workCv;  // the worker waits for work
        std::condition_variable g_roomCv;  // the game thread waits for room or for a drain
        Batch g_filling;                   // the commands queued since the worker last took the queue
        uint64_t g_enqueued = 0, g_done = 0;
        size_t g_pendingBytes = 0;         // GIF bytes queued or being processed
        bool g_workerIdle = false, g_workerStop = false;
        uint64_t g_roomWaits = 0, g_drains = 0;
        std::atomic<uint64_t> g_workerBusyNs{0}; // time the GS thread spent working (statsLine: busy %)
        std::atomic<uint64_t> g_scanoutTimeouts{0};
        std::thread g_worker;

        // Scanout results: the worker's newest picture, handed to the window thread.
        std::mutex g_scanMutex;
        std::condition_variable g_scanCv;
        std::vector<uint8_t> g_scanPixels;
        uint32_t g_scanWidth = 0, g_scanHeight = 0;
        bool g_scanOk = false;
        uint64_t g_scanRequested = 0, g_scanDone = 0;
        PendingReadback g_scanPending; // async scanout: the picture still on the GPU (finished by the window thread)

        // Producer side (game thread): the GIF reassembly buffers and the byte count.
        std::mutex g_producerMutex;
        std::atomic<uint64_t> g_gifBytes{0};

        bool gsThreadWanted()
        {
            const char *value = std::getenv("PS2X_GS_THREAD");
            return !(value && std::strcmp(value, "0") == 0);
        }

        // Called with g_queueMutex held: wakes the worker if it sleeps.
        bool wakeWorkerLocked()
        {
            if (!g_workerIdle)
                return false;
            g_workerIdle = false;
            return true;
        }

        // Staging (2026-11-12): the game sends thousands of small VU1 packets per frame, and queueing each one took
        // the queue lock (contended with the worker's swap) and woke the worker, which had gone back to sleep after
        // the previous one: ~5% of the game thread at 120 FPS. Packets and register writes now collect here and move
        // to the queue kStageBytes at a time; a scanout request, a drain and the stop flush what is staged, so the
        // order of the stream is unchanged and a picture never misses what was sent before it.
        constexpr size_t kStageBytes = 32u << 10;
        std::mutex g_stageMutex;
        Batch g_staged;

        // Called with g_stageMutex held.
        void flushStagedLocked()
        {
            if (g_staged.commands.empty())
                return;
            std::unique_lock<std::mutex> lock(g_queueMutex);
            if (g_pendingBytes >= kMaxQueuedBytes)
            {
                ++g_roomWaits;
                g_roomCv.wait(lock, [] { return g_pendingBytes < kMaxQueuedBytes || g_workerStop; });
            }
            const size_t base = g_filling.bytes.size();
            g_filling.bytes.insert(g_filling.bytes.end(), g_staged.bytes.begin(), g_staged.bytes.end());
            for (Command c : g_staged.commands)
            {
                if (c.kind == Command::Gif)
                    c.b += base;
                g_filling.commands.push_back(c);
            }
            g_pendingBytes += g_staged.bytes.size();
            g_enqueued += g_staged.commands.size();
            const bool wake = wakeWorkerLocked();
            lock.unlock();
            if (wake)
                g_workCv.notify_one();
            g_staged.commands.clear();
            g_staged.bytes.clear();
        }

        void flushStaged()
        {
            std::lock_guard<std::mutex> stage(g_stageMutex);
            flushStagedLocked();
        }

        void enqueueGif(uint32_t index, const uint8_t *packets, size_t size)
        {
            std::lock_guard<std::mutex> stage(g_stageMutex);
            g_staged.commands.push_back(Command{Command::Gif, index, g_staged.bytes.size(), static_cast<uint32_t>(size)});
            g_staged.bytes.insert(g_staged.bytes.end(), packets, packets + size);
            if (g_staged.bytes.size() >= kStageBytes)
                flushStagedLocked();
        }

        void enqueueRegister(uint32_t reg, uint64_t value)
        {
            std::lock_guard<std::mutex> stage(g_stageMutex);
            g_staged.commands.push_back(Command{Command::Register, reg, value, 0u});
        }

        // Waits until the worker has done every command queued so far.
        void drain()
        {
            flushStaged();
            std::unique_lock<std::mutex> lock(g_queueMutex);
            const uint64_t target = g_enqueued;
            if (g_done >= target)
                return;
            ++g_drains;
            const bool wake = wakeWorkerLocked();
            if (wake)
                g_workCv.notify_one();
            g_roomCv.wait(lock, [target] { return g_done >= target || g_workerStop; });
        }

        void runScanoutRequest(const PrivRegs &regs, uint64_t id)
        {
            static std::vector<uint8_t> s_pixels;
            uint32_t w = 0u, h = 0u;
            PendingReadback pending;
            const bool ok = scanoutNow(regs, s_pixels, w, h, asyncScanoutWanted() ? &pending : nullptr);
            {
                std::lock_guard<std::mutex> lock(g_scanMutex);
                g_scanPending = std::move(pending); // empty when the pixels came back already
                if (ok && !g_scanPending.fence)
                {
                    std::swap(g_scanPixels, s_pixels);
                    g_scanWidth = w;
                    g_scanHeight = h;
                }
                g_scanOk = ok;
                g_scanDone = id;
            }
            g_scanCv.notify_all();
        }

        void workerLoop()
        {
            Batch work;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(g_queueMutex);
                    if (g_filling.commands.empty())
                    {
                        g_workerIdle = true;
                        g_workCv.wait(lock, [] { return g_workerStop || !g_filling.commands.empty(); });
                        g_workerIdle = false;
                    }
                    if (g_filling.commands.empty())
                        return; // stopped, nothing left
                    std::swap(work, g_filling);
                }
                const auto busyFrom = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::recursive_mutex> lock(g_mutex);
                    bindThread();
                    if (g_state)
                    {
                        for (const Command &c : work.commands)
                        {
                            if (c.kind == Command::Gif)
                                forwardNow(c.a, work.bytes.data() + c.b, c.size);
                            else if (c.kind == Command::Register)
                                writeRegisterNow(c.a, c.b);
                            else
                                runScanoutRequest(work.regs[c.a], c.b);
                        }
                    }
                }
                g_workerBusyNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - busyFrom).count()),
                                         std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(g_queueMutex);
                    g_done += work.commands.size();
                    g_pendingBytes -= work.bytes.size();
                }
                g_roomCv.notify_all();
                work.commands.clear();
                work.bytes.clear();
                work.regs.clear();
            }
        }

        // SIGNAL / FINISH / LABEL of complete packets, on the game thread when the GS thread is on (see above).
        void deliverSignal(uint32_t reg, uint64_t value)
        {
            if (!g_state || !g_state->signals.sink)
                return;
            if (reg == 0x60u)
                g_state->signals.sink->signal(value);
            else if (reg == 0x61u)
                g_state->signals.sink->finish(value);
            else if (reg == 0x62u)
                g_state->signals.sink->label(value);
        }

        // Only A+D in PACKED mode reaches these registers (REGLIST descriptors stop at XYZ3, IMAGE carries pixels).
        void scanSignals(const uint8_t *data, size_t bytes)
        {
            size_t offset = 0u;
            while (bytes - offset >= 16u)
            {
                const size_t packetBytes = gifPacketBytes(data + offset);
                if (bytes - offset < packetBytes)
                    break;
                uint64_t lo = 0u, regs = 0u;
                std::memcpy(&lo, data + offset, 8u);
                std::memcpy(&regs, data + offset + 8u, 8u);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                bool anyAd = false;
                for (uint32_t r = 0; r < nreg && !anyAd; ++r)
                    anyAd = ((regs >> (4u * r)) & 0xFu) == 0xEu;
                if (flg == 0u && anyAd)
                {
                    const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                    const uint8_t *body = data + offset + 16u;
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            if (((regs >> (4u * r)) & 0xFu) != 0xEu)
                                continue;
                            const uint8_t *q = body + (static_cast<size_t>(loop) * nreg + r) * 16u;
                            uint64_t qlo = 0u, qhi = 0u;
                            std::memcpy(&qlo, q, 8u);
                            std::memcpy(&qhi, q + 8u, 8u);
                            const uint32_t adReg = static_cast<uint32_t>(qhi & 0xFFu);
                            if (adReg >= 0x60u && adReg <= 0x62u)
                                deliverSignal(adReg, qlo);
                        }
                }
                offset += packetBytes;
            }
        }

        // Complete packets of one path: to the queue (GS thread) or to paraLLEl-GS now.
        void deliverPackets(uint32_t index, const uint8_t *packets, size_t size)
        {
            if (g_threaded)
            {
                scanSignals(packets, size);
                enqueueGif(index, packets, size);
                return;
            }
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (!g_state)
                return;
            bindThread();
            forwardNow(index, packets, size);
        }
    }

    bool available()
    {
        return true;
    }

    bool start(SignalSink *sink, std::string &error)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_state)
            return true;
        bindThread();

        if (!Vulkan::Context::init_loader(nullptr))
        {
            error = "Vulkan loader (vulkan-1.dll) not found";
            return false;
        }

        auto state = std::make_unique<State>();
        state->context = std::make_unique<Vulkan::Context>();
        state->context->set_num_thread_indices(1);
        if (!state->context->init_instance_and_device(nullptr, 0, nullptr, 0,
                                                      Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                                          Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                                          Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
        {
            error = "could not create a Vulkan device";
            return false;
        }

        state->device = std::make_unique<Vulkan::Device>();
        state->device->set_context(*state->context);
        state->device->init_frame_contexts(4);
        state->gpuName = state->device->get_gpu_properties().deviceName;

        state->gs = std::make_unique<ParallelGS::GSInterface>();
        ParallelGS::GSOptions options = {};
        // PS2X_GS_SSAA=1/2/4/8/16 (default 1): samples per pixel (super-sampling anti-aliasing). From 4 on, the picture
        // is also scanned out at twice the width and height (high-resolution scanout, 1280 x 1024 for PAL), unless
        // PS2X_GS_HIRES=0. PS2X_GS_SSAA_TEXTURES=1: also super-sample render targets used as textures (paraLLEl-GS
        // super_sampled_textures; helps effects that draw into a texture first). Only the picture changes: what the
        // game sees (signals, transfers) does not.
        const uint32_t ssaa = watchBound("PS2X_GS_SSAA", 1u);
        options.super_sampling = ssaa >= 16u ? ParallelGS::SuperSampling::X16
                                 : ssaa >= 8u ? ParallelGS::SuperSampling::X8
                                 : ssaa >= 4u ? ParallelGS::SuperSampling::X4
                                 : ssaa >= 2u ? ParallelGS::SuperSampling::X2
                                              : ParallelGS::SuperSampling::X1;
        options.super_sampled_textures = watchBound("PS2X_GS_SSAA_TEXTURES", 0u) != 0u;
        g_hiresScanout = ssaa >= 4u && watchBound("PS2X_GS_HIRES", 1u) != 0u;
        std::fprintf(stderr, "[pgs] super-sampling x%u%s%s\n", static_cast<unsigned>(options.super_sampling),
                     g_hiresScanout ? ", high-resolution scanout" : "", options.super_sampled_textures ? ", super-sampled textures" : "");
        if (!state->gs->init(state->device.get(), options))
        {
            error = "paraLLEl-GS init failed on " + state->gpuName;
            return false;
        }

        if (const char *trace = std::getenv("PS2X_PGS_TRACE"))
            g_traceFrames = std::atoi(trace) != 0;
        state->signals.sink = sink;
        state->gs->set_signal_interface(&state->signals);
        g_state = state.release();
        g_threaded = gsThreadWanted();
        if (g_threaded)
            g_worker = std::thread(workerLoop);
        std::fprintf(stderr, "[pgs] GS thread %s (PS2X_GS_THREAD)\n", g_threaded ? "on" : "off");
        return true;
    }

    bool active()
    {
        return g_state != nullptr;
    }

    void shutdown()
    {
        if (g_threaded)
        {
            drain();
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                g_workerStop = true;
            }
            g_workCv.notify_all();
            g_roomCv.notify_all();
            if (g_worker.joinable())
                g_worker.join();
            g_threaded = false;
        }
        {
            // Lock order: g_producerMutex before g_mutex (gifTransfer).
            std::lock_guard<std::mutex> producer(g_producerMutex);
            for (auto &pending : g_pendingGif)
                pending.clear();
            std::lock_guard<std::mutex> stage(g_stageMutex);
            g_staged = Batch{};
        }
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return;
        {
            std::lock_guard<std::mutex> scan(g_scanMutex);
            g_scanPending = PendingReadback{};
        }
        if (g_state->device)
            g_state->device->wait_idle();
        g_state->readback.reset();
        g_state->gs.reset();
        g_state->device.reset();
        g_state->context.reset();
        delete g_state;
        g_state = nullptr;
    }

    void gifTransfer(uint32_t path, const void *data, size_t bytes)
    {
        if (!g_state || !data)
            return;
        bytes &= ~static_cast<size_t>(15u);
        if (bytes == 0u)
            return;
        std::lock_guard<std::mutex> producer(g_producerMutex);
        g_gifBytes.fetch_add(bytes, std::memory_order_relaxed);
        const uint32_t index = pathIndex(path);
        std::vector<uint8_t> &pending = g_pendingGif[index];
        const auto *input = static_cast<const uint8_t *>(data);
        std::vector<uint8_t> joined;
        if (!pending.empty())
        {
            joined.swap(pending);
            joined.insert(joined.end(), input, input + bytes);
            input = joined.data();
            bytes = joined.size();
        }
        const size_t complete = completeGifBytes(input, bytes);
        if (complete != 0u)
            deliverPackets(index, input, complete);
        if (complete == bytes)
            return;

        const uint8_t *rest = input + complete;
        const size_t restBytes = bytes - complete;
        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, rest, sizeof(tagLo));
        const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3u);
        if (flg >= 2u)
        {
            // A partial IMAGE packet is sent on at once, re-tagged to the qwords present, the way the CPU
            // renderer treats it. The rest of the image does not arrive raw: the VIF (DIRECT continuation)
            // re-tags it as a new IMAGE packet. Keeping the old tag pending would read that new tag as pixels
            // and shift the whole path by a qword, turning texture data into GS register writes.
            const size_t imageQw = (restBytes - 16u) / 16u;
            if (imageQw != 0u)
            {
                std::vector<uint8_t> partial(rest, rest + 16u + imageQw * 16u);
                const uint64_t retagged = (tagLo & ~0x7FFFull) | static_cast<uint64_t>(imageQw);
                std::memcpy(partial.data(), &retagged, sizeof(retagged));
                deliverPackets(index, partial.data(), partial.size());
            }
            return;
        }
        // PACKED / REGLIST: wait for the rest (paraLLEl-GS needs whole loops). A packet that never completes
        // within 4 MiB means the stream is out of step: drop it rather than parse everything after it as data.
        if (restBytes > (4u << 20))
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            logAnomaly("dropped an incomplete GIF packet larger than 4 MiB, tag", tagLo);
            return;
        }
        pending.assign(rest, rest + restBytes);
    }

    void writeRegister(uint32_t reg, uint64_t value)
    {
        if (!g_state)
            return;
        reg &= 0xFFu;
        if (g_threaded)
        {
            if (reg >= 0x60u && reg <= 0x62u)
                deliverSignal(reg, value);
            enqueueRegister(reg, value);
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return;
        bindThread();
        writeRegisterNow(reg, value);
    }

    size_t readTransfer(void *dst, size_t bytes)
    {
        if (!g_state || !dst || bytes == 0u)
            return 0u;
        if (g_threaded)
            drain();
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return 0u;
        bindThread();
        return readTransferNow(dst, bytes);
    }

    bool scanout(const PrivRegs &regs, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        width = height = 0u;
        if (!g_state)
            return false;
        if (!g_threaded)
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (!g_state)
                return false;
            bindThread();
            return scanoutNow(regs, rgba, width, height);
        }
        uint64_t id = 0;
        {
            // What the game staged so far comes before this picture; the stage lock keeps a packet staged
            // meanwhile behind the request.
            std::lock_guard<std::mutex> stage(g_stageMutex);
            flushStagedLocked();
            std::unique_lock<std::mutex> lock(g_queueMutex);
            id = ++g_scanRequested;
            g_filling.commands.push_back(Command{Command::Scanout, static_cast<uint32_t>(g_filling.regs.size()), id, 0u});
            g_filling.regs.push_back(regs);
            ++g_enqueued;
            const bool wake = wakeWorkerLocked();
            lock.unlock();
            if (wake)
                g_workCv.notify_one();
        }
        // The worker gets there after what the game queued before; a frame that takes too long keeps the previous
        // picture on screen (the window thread must not hang).
        std::unique_lock<std::mutex> lock(g_scanMutex);
        if (!g_scanCv.wait_for(lock, std::chrono::milliseconds(250), [id] { return g_scanDone >= id; }))
        {
            ++g_scanoutTimeouts;
            return false;
        }
        if (!g_scanOk)
            return false;
        g_scanOk = false; // handed over
        if (g_scanPending.fence)
        {
            // The next scanout request comes from this thread after this copy, so the GS thread does not touch the
            // readback buffer meanwhile.
            PendingReadback pending = std::move(g_scanPending);
            g_scanPending = PendingReadback{};
            lock.unlock();
            if (!finishReadback(pending, rgba))
                return false;
            width = pending.width;
            height = pending.height;
            static auto s_lastDump = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - s_lastDump >= std::chrono::seconds(5))
            {
                s_lastDump = now;
                dumpFrameBmp(rgba, width, height); // out/pgs_frame.bmp about every 5 s (CLAUDE.md)
            }
            return true;
        }
        std::swap(rgba, g_scanPixels);
        width = g_scanWidth;
        height = g_scanHeight;
        return true;
    }

    std::string statsLine()
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return {};
        char line[896];
        const double avg = g_state->scanoutSamples ? g_state->scanoutMs / g_state->scanoutSamples : 0.0;
        uint64_t roomWaits = 0, drains = 0, timeouts = 0;
        size_t pending = 0;
        {
            std::lock_guard<std::mutex> queue(g_queueMutex);
            roomWaits = g_roomWaits;
            drains = g_drains;
            timeouts = g_scanoutTimeouts.exchange(0u);
            pending = g_pendingBytes;
            g_roomWaits = g_drains = 0;
        }
        // Share of real time since the last stats line that the GS thread spent working (100% = it is the limit).
        static auto busySince = std::chrono::steady_clock::now();
        const auto busyNow = std::chrono::steady_clock::now();
        const double busyWindowNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(busyNow - busySince).count());
        const int busyPercent = busyWindowNs > 0.0 ? static_cast<int>(g_workerBusyNs.exchange(0u) * 100.0 / busyWindowNs + 0.5) : 0;
        busySince = busyNow;
        std::snprintf(line, sizeof(line),
                      "pgs: %s frames=%llu gif=%lluK scanout=%ux%u %.2fms pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx"
                      " | gifstats kicks=%llu prims=%llu tex0=%llu clut=%llu uploads=%llu copies=%llu maxPrim=%ux%u big=%llu badTex=%llu badXfer=%llu"
                      " | gsthread %s queued=%zuK full=%llu drains=%llu late_scanouts=%llu busy=%d%%",
                      g_state->gpuName.c_str(), static_cast<unsigned long long>(g_state->frames),
                      static_cast<unsigned long long>(g_gifBytes.exchange(0u, std::memory_order_relaxed) / 1024u), g_state->lastWidth,
                      g_state->lastHeight, avg,
                      static_cast<unsigned long long>(g_lastRegs.pmode), static_cast<unsigned long long>(g_lastRegs.smode2),
                      static_cast<unsigned long long>(g_lastRegs.dispfb1), static_cast<unsigned long long>(g_lastRegs.display1),
                      static_cast<unsigned long long>(g_lastRegs.dispfb2), static_cast<unsigned long long>(g_lastRegs.display2),
                      (unsigned long long)g_windowStats.kicks, (unsigned long long)g_windowStats.prims,
                      (unsigned long long)g_windowStats.tex0, (unsigned long long)g_windowStats.clutLoads,
                      (unsigned long long)g_windowStats.uploads, (unsigned long long)g_windowStats.localCopies,
                      g_windowStats.maxPrimW, g_windowStats.maxPrimH, (unsigned long long)g_windowStats.bigPrims,
                      (unsigned long long)g_windowStats.badTex, (unsigned long long)g_windowStats.badTransfers,
                      g_threaded ? "on" : "off", pending / 1024u, (unsigned long long)roomWaits, (unsigned long long)drains,
                      (unsigned long long)timeouts, busyPercent);
        g_windowStats = GifStats{};
        g_state->frames = 0;
        g_state->scanoutMs = 0.0;
        g_state->scanoutSamples = 0;
        return std::string(line) + hud::statsLine();
    }

    void requestCapture()
    {
        g_captureRequested.store(true);
        std::fprintf(stderr, "[pgs-capture] requested (F11): the next two frames go to out/gif_capture.bin, pictures to "
                             "out/gif_capture_frame0..2.bmp\n");
    }
}

#else // !PS2X_HAS_PARALLEL_GS

namespace ps2x::pgs
{
    bool available() { return false; }
    bool start(SignalSink *, std::string &error)
    {
        error = "built without paraLLEl-GS (PS2X_ENABLE_PARALLEL_GS=OFF)";
        return false;
    }
    bool active() { return false; }
    void shutdown() {}
    void gifTransfer(uint32_t, const void *, size_t) {}
    void writeRegister(uint32_t, uint64_t) {}
    size_t readTransfer(void *, size_t) { return 0u; }
    bool scanout(const PrivRegs &, std::vector<uint8_t> &, uint32_t &width, uint32_t &height)
    {
        width = height = 0u;
        return false;
    }
    std::string statsLine() { return {}; }
    void requestCapture() {}
}

#endif
