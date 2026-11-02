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
#include <cstring>
#include <memory>
#include <mutex>

namespace ps2x::pgs
{
    namespace
    {
        class SignalForwarder final : public ParallelGS::SignalInterface
        {
        public:
            SignalSink *sink = nullptr;
            bool on_signal(uint64_t payload) override
            {
                if (sink)
                    sink->signal(payload);
                return false;
            }
            bool on_finish(uint64_t payload) override
            {
                if (sink)
                    sink->finish(payload);
                return false;
            }
            bool on_label(uint64_t payload) override
            {
                if (sink)
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
        if (pending.empty())
        {
            const size_t complete = completeGifBytes(input, bytes);
            if (complete != 0u)
                g_state->gs->gif_transfer(index, input, complete);
            if (complete < bytes)
                pending.assign(input + complete, input + bytes);
            return;
        }
        pending.insert(pending.end(), input, input + bytes);
        const size_t complete = completeGifBytes(pending.data(), pending.size());
        if (complete != 0u)
        {
            g_state->gs->gif_transfer(index, pending.data(), complete);
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(complete));
        }
    }

    void writeRegister(uint32_t reg, uint64_t value)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_state)
            return;
        bindThread();
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
        char line[384];
        const double avg = g_state->scanoutSamples ? g_state->scanoutMs / g_state->scanoutSamples : 0.0;
        std::snprintf(line, sizeof(line),
                      "pgs: %s frames=%llu gif=%lluK scanout=%ux%u %.2fms pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx",
                      g_state->gpuName.c_str(), static_cast<unsigned long long>(g_state->frames),
                      static_cast<unsigned long long>(g_state->gifBytes / 1024u), g_state->lastWidth, g_state->lastHeight, avg,
                      static_cast<unsigned long long>(g_lastRegs.pmode), static_cast<unsigned long long>(g_lastRegs.smode2),
                      static_cast<unsigned long long>(g_lastRegs.dispfb1), static_cast<unsigned long long>(g_lastRegs.display1),
                      static_cast<unsigned long long>(g_lastRegs.dispfb2), static_cast<unsigned long long>(g_lastRegs.display2));
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
