// GPU GS renderer bridge: feeds the game's GS command stream to paraLLEl-GS and reads the scanout back.
// See gs_pgs_bridge.h. paraLLEl-GS and Granite are LGPL-3.0-or-later / MIT; this file is GPL-3.0 like
// the rest of PS2Recomp.

#include "gs_pgs_bridge.h"

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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

namespace ps2x::pgs
{
    namespace
    {
        // Deterministic mode (PS2X_DETERMINISTIC=1, see ps2_test_harness.cpp): SIGNAL / FINISH / LABEL reach the
        // guest when the register write passes through the bridge (deterministicSignal), not when the GPU thread
        // reports it, whose timing depends on the host.
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
                if (sink && !deterministicMode())
                    sink->signal(payload);
                return false;
            }
            bool on_finish(uint64_t payload) override
            {
                if (sink && !deterministicMode())
                    sink->finish(payload);
                return false;
            }
            bool on_label(uint64_t payload) override
            {
                if (sink && !deterministicMode())
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

        // paraLLEl-GS / Granite are single-threaded: the EE thread streams GIF data while the window thread
        // scans out frames, so every entry point takes this lock (recursive: scanout calls flush etc.).
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
            uint64_t bitbltbuf = 0;
            int32_t vx[3] = {}, vy[3] = {};
            uint32_t vcount = 0;
        } g_shadow;

        void logAnomaly(const char *what, uint64_t value)
        {
            if (g_anomalyLogs++ < 40u)
                std::fprintf(stderr, "[pgs-gif] anomaly %s value=%016llx\n", what, static_cast<unsigned long long>(value));
        }

        void noteVertex(uint32_t x16, uint32_t y16, bool kick)
        {
            auto &sh = g_shadow;
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
        }

        void deterministicSignal(uint32_t reg, uint64_t value)
        {
            if (!deterministicMode() || !g_state || !g_state->signals.sink)
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
                sh.vcount = 0;
                break;
            case 0x04: case 0x05: // XYZF2 / XYZ2: vertex + kick
                noteVertex(static_cast<uint32_t>(value & 0xFFFFu), static_cast<uint32_t>((value >> 16) & 0xFFFFu), true);
                break;
            case 0x0C: case 0x0D: // XYZF3 / XYZ3: vertex, no kick
                noteVertex(static_cast<uint32_t>(value & 0xFFFFu), static_cast<uint32_t>((value >> 16) & 0xFFFFu), false);
                break;
            case 0x06: case 0x07: // TEX0_1 / TEX0_2
            {
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
                            case 0x4: // XYZF2 / XYZ2 packed: X bits 0-15, Y bits 32-47, ADC bit 111
                            case 0x5:
                                noteVertex(static_cast<uint32_t>(qlo & 0xFFFFu), static_cast<uint32_t>((qlo >> 32) & 0xFFFFu),
                                           ((qhi >> 47) & 1u) == 0u);
                                break;
                            case 0xC: case 0xD:
                                noteVertex(static_cast<uint32_t>(qlo & 0xFFFFu), static_cast<uint32_t>((qlo >> 32) & 0xFFFFu), false);
                                break;
                            case 0x6: case 0x7:
                                noteRegister(desc, qlo);
                                break;
                            case 0xE: // A+D
                                noteRegister(static_cast<uint32_t>(qhi & 0xFFu), qlo);
                                break;
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
                }
                offset += packetBytes;
            }
        }

        void endFrameStats()
        {
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
        void dumpFrameBmp(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
        {
            if (w == 0u || h == 0u || rgba.size() < static_cast<size_t>(w) * h * 4u)
                return;
            FILE *f = std::fopen("out/pgs_frame.bmp", "wb");
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

        // Granite keys command pools by a per-thread index. The EE thread and the window thread both
        // call in (never at the same time: the GS frontend's mutex), so both use index 0.
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
        options.super_sampling = ParallelGS::SuperSampling::X1;
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
        return true;
    }

    bool active()
    {
        return g_state != nullptr;
    }

    void shutdown()
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return;
        if (g_state->device)
            g_state->device->wait_idle();
        g_state->readback.reset();
        for (auto &pending : g_pendingGif)
            pending.clear();
        g_state->gs.reset();
        g_state->device.reset();
        g_state->context.reset();
        delete g_state;
        g_state = nullptr;
    }

    void gifTransfer(uint32_t path, const void *data, size_t bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state || !data)
            return;
        bindThread();
        bytes &= ~static_cast<size_t>(15u);
        if (bytes == 0u)
            return;
        g_state->gifBytes += bytes;
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
        {
            analyzeGif(input, complete);
            g_state->gs->gif_transfer(index, input, complete);
        }
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
                analyzeGif(partial.data(), partial.size());
                g_state->gs->gif_transfer(index, partial.data(), partial.size());
            }
            return;
        }
        // PACKED / REGLIST: wait for the rest (paraLLEl-GS needs whole loops). A packet that never completes
        // within 4 MiB means the stream is out of step: drop it rather than parse everything after it as data.
        if (restBytes > (4u << 20))
        {
            logAnomaly("dropped an incomplete GIF packet larger than 4 MiB, tag", tagLo);
            return;
        }
        pending.assign(rest, rest + restBytes);
    }

    void writeRegister(uint32_t reg, uint64_t value)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return;
        bindThread();
        deterministicSignal(reg & 0xFFu, value);
        g_state->gs->write_register(static_cast<ParallelGS::RegisterAddr>(reg & 0xFFu), value);
    }

    size_t readTransfer(void *dst, size_t bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state || !dst || bytes == 0u)
            return 0u;
        bindThread();
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

    bool scanout(const PrivRegs &regs, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        width = height = 0u;
        if (!g_state)
            return false;
        bindThread();
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

        endFrameStats();
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
        fence->wait();

        const auto *pixels = static_cast<const uint8_t *>(device.map_host_buffer(*g_state->readback, Vulkan::MEMORY_ACCESS_READ_BIT));
        if (!pixels)
            return false;
        rgba.resize(static_cast<size_t>(size));
        std::memcpy(rgba.data(), pixels, static_cast<size_t>(size));
        device.unmap_host_buffer(*g_state->readback, Vulkan::MEMORY_ACCESS_READ_BIT);
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 0xFFu;

        width = w;
        height = h;
        static uint64_t s_dumpCounter = 0u;
        if ((s_dumpCounter++ % 300u) == 30u)
            dumpFrameBmp(rgba, w, h);
        g_state->lastWidth = w;
        g_state->lastHeight = h;
        ++g_state->frames;
        g_state->scanoutMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        ++g_state->scanoutSamples;
        return true;
    }

    std::string statsLine()
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return {};
        char line[768];
        const double avg = g_state->scanoutSamples ? g_state->scanoutMs / g_state->scanoutSamples : 0.0;
        std::snprintf(line, sizeof(line),
                      "pgs: %s frames=%llu gif=%lluK scanout=%ux%u %.2fms pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx"
                      " | gifstats kicks=%llu prims=%llu tex0=%llu clut=%llu uploads=%llu copies=%llu maxPrim=%ux%u big=%llu badTex=%llu badXfer=%llu",
                      g_state->gpuName.c_str(), static_cast<unsigned long long>(g_state->frames),
                      static_cast<unsigned long long>(g_state->gifBytes / 1024u), g_state->lastWidth, g_state->lastHeight, avg,
                      static_cast<unsigned long long>(g_lastRegs.pmode), static_cast<unsigned long long>(g_lastRegs.smode2),
                      static_cast<unsigned long long>(g_lastRegs.dispfb1), static_cast<unsigned long long>(g_lastRegs.display1),
                      static_cast<unsigned long long>(g_lastRegs.dispfb2), static_cast<unsigned long long>(g_lastRegs.display2),
                      (unsigned long long)g_windowStats.kicks, (unsigned long long)g_windowStats.prims,
                      (unsigned long long)g_windowStats.tex0, (unsigned long long)g_windowStats.clutLoads,
                      (unsigned long long)g_windowStats.uploads, (unsigned long long)g_windowStats.localCopies,
                      g_windowStats.maxPrimW, g_windowStats.maxPrimH, (unsigned long long)g_windowStats.bigPrims,
                      (unsigned long long)g_windowStats.badTex, (unsigned long long)g_windowStats.badTransfers);
        g_windowStats = GifStats{};
        g_state->frames = 0;
        g_state->gifBytes = 0;
        g_state->scanoutMs = 0.0;
        g_state->scanoutSamples = 0;
        return line;
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
}

#endif
