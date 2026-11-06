#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdio>

extern "C" uint64_t ps2xGuestVsync(); // ps2_test_harness.cpp
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *hThread, int nPriority);
#endif

using namespace GSInternal;

// ---------------------------------------------------------------------------------------------
// Deferred engine helpers
// ---------------------------------------------------------------------------------------------
namespace
{
    constexpr int kBandRows = 8;
    constexpr size_t kMaxSegmentPrims = 4096;
    constexpr size_t kKickPrims = 48;
    constexpr size_t kMaxFifoSegments = 64;
    constexpr size_t kMaxQueuedPrims = 65536;
    constexpr size_t kMaxQueuedBytes = 64u * 1024u * 1024u;
    constexpr size_t kMaxSegmentUploadBytes = 8u * 1024u * 1024u;
    constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;

    std::atomic<bool> g_captureRequested{false};
    std::atomic<uint64_t> g_nextBackendInstance{1};

    std::atomic<uint64_t> g_statPrims{0};
    std::atomic<uint64_t> g_statSegments{0};
    std::atomic<uint64_t> g_statHazardCloses{0};
    std::atomic<uint64_t> g_statDrains{0};
    std::atomic<uint64_t> g_statDrainWaitUs{0};
    std::atomic<uint64_t> g_statSpaceWaitUs{0};
    std::atomic<uint64_t> g_statUploads{0};
    std::atomic<uint64_t> g_statClutLoads{0};
    std::atomic<uint32_t> g_statWorkers{0};

    void lowerCurrentThreadPriority()
    {
#if defined(_WIN32)
        // THREAD_PRIORITY_BELOW_NORMAL: the EE thread is the critical path; GS workers must not
        // preempt it when the machine is saturated.
        SetThreadPriority(GetCurrentThread(), -1);
#endif
    }

    uint64_t elapsedUs(std::chrono::steady_clock::time_point since)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - since)
                                         .count());
    }

    // Byte ranges [lo, hi) inside the 4 MiB GS local memory. Conservative: merging only grows.
    class RangeSet
    {
    public:
        void clear() { m_count = 0; }
        bool empty() const { return m_count == 0; }

        bool overlaps(uint32_t lo, uint32_t hi) const
        {
            for (size_t i = 0; i < m_count; ++i)
            {
                if (lo < m_ranges[i].hi && m_ranges[i].lo < hi)
                    return true;
            }
            return false;
        }

        bool overlaps(const RangeSet &other) const
        {
            for (size_t i = 0; i < other.m_count; ++i)
            {
                if (overlaps(other.m_ranges[i].lo, other.m_ranges[i].hi))
                    return true;
            }
            return false;
        }

        void add(uint32_t lo, uint32_t hi)
        {
            if (hi <= lo)
                return;
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (size_t i = 0; i < m_count; ++i)
                {
                    if (lo <= m_ranges[i].hi && m_ranges[i].lo <= hi)
                    {
                        lo = std::min(lo, m_ranges[i].lo);
                        hi = std::max(hi, m_ranges[i].hi);
                        m_ranges[i] = m_ranges[--m_count];
                        merged = true;
                        break;
                    }
                }
            }
            if (m_count < kMax)
            {
                m_ranges[m_count++] = {lo, hi};
                return;
            }
            size_t best = 0;
            uint32_t bestGap = UINT32_MAX;
            for (size_t i = 0; i < m_count; ++i)
            {
                const uint32_t gap = (hi < m_ranges[i].lo) ? (m_ranges[i].lo - hi) : (lo - m_ranges[i].hi);
                if (gap < bestGap)
                {
                    bestGap = gap;
                    best = i;
                }
            }
            m_ranges[best].lo = std::min(m_ranges[best].lo, lo);
            m_ranges[best].hi = std::max(m_ranges[best].hi, hi);
        }

        void addSet(const RangeSet &other)
        {
            for (size_t i = 0; i < other.m_count; ++i)
                add(other.m_ranges[i].lo, other.m_ranges[i].hi);
        }

    private:
        struct Range
        {
            uint32_t lo;
            uint32_t hi;
        };
        static constexpr size_t kMax = 12;
        Range m_ranges[kMax]{};
        size_t m_count = 0;
    };

    void psmPageExtent(uint32_t psm, uint32_t &pw, uint32_t &ph)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            pw = 64u;
            ph = 64u;
            break;
        case GS_PSM_T8:
            pw = 128u;
            ph = 64u;
            break;
        case GS_PSM_T4:
            pw = 128u;
            ph = 128u;
            break;
        default:
            pw = 64u;
            ph = 32u;
            break;
        }
    }

    // Adds the local-memory span touched by the pixel rectangle [x0..x1] x [y0..y1] of a buffer
    // (bp in 256-byte blocks, bw in 64-pixel units). Whole pages, so it over-approximates.
    void addRectRange(RangeSet &set, uint32_t bp, uint32_t bw, uint32_t psm,
                      uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        uint32_t pw = 64u;
        uint32_t ph = 32u;
        psmPageExtent(psm, pw, ph);
        const uint64_t pagesPerRow = std::max<uint64_t>(
            1u, (static_cast<uint64_t>(std::max<uint32_t>(bw, 1u)) * 64u) / pw);
        const uint64_t firstPage = static_cast<uint64_t>(y0 / ph) * pagesPerRow + (x0 / pw);
        const uint64_t lastPage = static_cast<uint64_t>(y1 / ph) * pagesPerRow + (x1 / pw);
        const uint64_t lo = static_cast<uint64_t>(bp) * 256u + firstPage * 8192u;
        const uint64_t hi = static_cast<uint64_t>(bp) * 256u + (lastPage + 1u) * 8192u;
        if (hi <= lo)
            return;
        if (hi - lo >= kVramBytes)
        {
            set.add(0u, kVramBytes);
            return;
        }
        const uint64_t wlo = lo % kVramBytes;
        const uint64_t whi = wlo + (hi - lo);
        if (whi <= kVramBytes)
        {
            set.add(static_cast<uint32_t>(wlo), static_cast<uint32_t>(whi));
        }
        else
        {
            set.add(static_cast<uint32_t>(wlo), kVramBytes);
            set.add(0u, static_cast<uint32_t>(whi - kVramBytes));
        }
    }

    uint8_t layoutClass(uint32_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return 1u;
        case GS_PSM_CT16:
            return 2u;
        case GS_PSM_CT16S:
            return 3u;
        case GS_PSM_Z32:
        case GS_PSM_Z24:
            return 4u;
        case GS_PSM_Z16:
            return 5u;
        case GS_PSM_Z16S:
            return 6u;
        default:
            return static_cast<uint8_t>(16u + (psm & 0x3Fu));
        }
    }

    struct WriteTarget
    {
        uint32_t bp = 0;
        uint32_t bw = 0;
        uint8_t layout = 0;
        RangeSet range;
    };

    // Base pointer / buffer width of a mip level (0 = TEX0).
    void mipLevelBase(const GSDrawState &state, uint32_t level, uint32_t &tbp, uint32_t &tbw)
    {
        const GSContext &ctx = state.context;
        if (level == 0u)
        {
            tbp = ctx.tex0.tbp0;
            tbw = ctx.tex0.tbw;
            return;
        }
        const bool mtba = ((ctx.tex1 >> 9u) & 1ull) != 0ull;
        if (mtba && level <= 3u)
        {
            uint32_t bp = ctx.tex0.tbp0;
            uint32_t bw = std::max<uint32_t>(ctx.tex0.tbw, 1u);
            uint32_t w = state.textureWidth;
            uint32_t h = state.textureHeight;
            const uint32_t bpp = bitsPerPixel(ctx.tex0.psm);
            for (uint32_t l = 1u; l <= level; ++l)
            {
                bp += std::max<uint32_t>(1u, (w * h * bpp / 8u) / 256u);
                w = std::max<uint32_t>(1u, w >> 1u);
                h = std::max<uint32_t>(1u, h >> 1u);
                bw = std::max<uint32_t>(1u, bw >> 1u);
            }
            tbp = bp & 0x3FFFu;
            tbw = bw;
            return;
        }
        const uint64_t reg = (level <= 3u) ? ctx.miptbp1 : ctx.miptbp2;
        const uint32_t idx = (level <= 3u) ? (level - 1u) : (level - 4u);
        tbp = static_cast<uint32_t>((reg >> (idx * 20u)) & 0x3FFFu);
        tbw = static_cast<uint32_t>((reg >> (idx * 20u + 14u)) & 0x3Fu);
    }

    bool mipmapActive(const GSDrawState &state)
    {
        const uint64_t tex1 = state.context.tex1;
        const uint32_t mxl = static_cast<uint32_t>((tex1 >> 2u) & 0x7u);
        const uint32_t mmin = static_cast<uint32_t>((tex1 >> 6u) & 0x7u);
        return mxl > 0u && mmin >= 2u && mmin <= 5u;
    }

    struct PrimFootprint
    {
        int x0 = 0;
        int y0 = 0;
        int x1 = -1;
        int y1 = -1;
        RangeSet reads;
        WriteTarget targets[2];
        int targetCount = 0;
    };

    bool computeFootprint(const GSPrimitiveBatch &batch, PrimFootprint &fp)
    {
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        const float ofx = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
        const int count = std::max<int>(1, std::min<int>(batch.vertexCount, 3));

        float minX = batch.vertices[0].x;
        float maxX = minX;
        float minY = batch.vertices[0].y;
        float maxY = minY;
        for (int i = 1; i < count; ++i)
        {
            minX = std::min(minX, batch.vertices[static_cast<size_t>(i)].x);
            maxX = std::max(maxX, batch.vertices[static_cast<size_t>(i)].x);
            minY = std::min(minY, batch.vertices[static_cast<size_t>(i)].y);
            maxY = std::max(maxY, batch.vertices[static_cast<size_t>(i)].y);
        }

        int x0 = static_cast<int>(std::floor(minX - ofx)) - 1;
        int x1 = static_cast<int>(std::ceil(maxX - ofx)) + 1;
        int y0 = static_cast<int>(std::floor(minY - ofy)) - 1;
        int y1 = static_cast<int>(std::ceil(maxY - ofy)) + 1;
        x0 = std::max<int>(x0, ctx.scissor.x0);
        x1 = std::min<int>(x1, ctx.scissor.x1);
        y0 = std::max<int>(y0, ctx.scissor.y0);
        y1 = std::min<int>(y1, ctx.scissor.y1);
        if (x1 < x0 || y1 < y0)
            return false;
        fp.x0 = x0;
        fp.x1 = x1;
        fp.y0 = y0;
        fp.y1 = y1;

        if (state.prim.tme && state.prim.type != GS_PRIM_POINT &&
            state.prim.type != GS_PRIM_LINE && state.prim.type != GS_PRIM_LINESTRIP)
        {
            const GSTex0Reg &tex = ctx.tex0;
            const uint64_t clamp = ctx.clamp;
            uint32_t maxU = static_cast<uint32_t>(state.textureWidth) - 1u;
            uint32_t maxV = static_cast<uint32_t>(state.textureHeight) - 1u;
            const uint32_t wms = static_cast<uint32_t>(clamp & 0x3u);
            const uint32_t wmt = static_cast<uint32_t>((clamp >> 2u) & 0x3u);
            const uint32_t minUReg = static_cast<uint32_t>((clamp >> 4u) & 0x3FFu);
            const uint32_t maxUReg = static_cast<uint32_t>((clamp >> 14u) & 0x3FFu);
            const uint32_t minVReg = static_cast<uint32_t>((clamp >> 24u) & 0x3FFu);
            const uint32_t maxVReg = static_cast<uint32_t>((clamp >> 34u) & 0x3FFu);
            if (wms == 2u)
                maxU = std::max(maxU, maxUReg);
            else if (wms == 3u)
                maxU = std::max(maxU, minUReg | maxUReg);
            if (wmt == 2u)
                maxV = std::max(maxV, maxVReg);
            else if (wmt == 3u)
                maxV = std::max(maxV, minVReg | maxVReg);
            addRectRange(fp.reads, tex.tbp0, tex.tbw, tex.psm, 0u, 0u, maxU, maxV);

            if (mipmapActive(state))
            {
                const uint32_t mxl = static_cast<uint32_t>((ctx.tex1 >> 2u) & 0x7u);
                for (uint32_t level = 1u; level <= std::min<uint32_t>(mxl, 6u); ++level)
                {
                    uint32_t tbp = 0u;
                    uint32_t tbw = 0u;
                    mipLevelBase(state, level, tbp, tbw);
                    const uint32_t lw = std::max<uint32_t>(1u, (maxU + 1u) >> level);
                    const uint32_t lh = std::max<uint32_t>(1u, (maxV + 1u) >> level);
                    addRectRange(fp.reads, tbp, tbw, tex.psm, 0u, 0u, lw - 1u, lh - 1u);
                }
            }
        }

        // Frame buffer target.
        {
            WriteTarget &t = fp.targets[fp.targetCount++];
            t.bp = framePageBaseToBlock(ctx.frame.fbp);
            t.bw = std::max<uint32_t>(ctx.frame.fbw, 1u);
            t.layout = layoutClass(ctx.frame.psm);
            addRectRange(t.range, t.bp, t.bw, ctx.frame.psm,
                         static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                         static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));
        }
        // Depth buffer target (read for the test, written unless masked).
        const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17u) & 0x3u);
        if (!ctx.zbuf.zmask || ztst >= 2u)
        {
            WriteTarget &t = fp.targets[fp.targetCount++];
            t.bp = framePageBaseToBlock(ctx.zbuf.zbp);
            t.bw = std::max<uint32_t>(ctx.frame.fbw, 1u);
            t.layout = layoutClass(ctx.zbuf.psm);
            addRectRange(t.range, t.bp, t.bw, ctx.zbuf.psm,
                         static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                         static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));
        }
        return true;
    }

    uint32_t uploadPixelsForBytes(uint32_t psm, uint32_t bytes)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
            return bytes / 4u;
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return bytes / 3u;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return bytes / 2u;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            return bytes;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return bytes * 2u;
        default:
            return 0u;
        }
    }

    uint32_t uploadBytesForPixels(uint32_t psm, uint32_t pixels)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
            return pixels * 4u;
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return pixels * 3u;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return pixels * 2u;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            return pixels;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return (pixels + 1u) / 2u;
        default:
            return 0u;
        }
    }

    // Capture record kinds.
    enum : uint32_t
    {
        kCapEnd = 0u,
        kCapPrim = 1u,
        kCapClutLoad = 2u,
        kCapUpload = 3u,
        kCapLocalCopy = 4u,
        kCapLocalToHost = 5u,
        kCapClear = 6u,
        kCapWriteVram = 7u,
        kCapReset = 8u,
        kCapFinish = 9u,
    };
}

struct GSCpuBackend::PreOp
{
    enum Kind : uint8_t
    {
        LoadClut = 0,
        Upload = 1,
        LocalCopy = 2,
    };
    Kind kind = LoadClut;
    GSTex0Reg tex0{};
    GSTexClutReg texclut{};
    GSTransferCommand transfer{};
    uint32_t startPixel = 0;
    uint32_t pixelCount = 0;
    size_t dataOffset = 0;
    uint32_t dataSize = 0;
};

struct GSCpuBackend::QueuedPrim
{
    GSPrimitiveBatch batch;
    uint32_t clutSlot = 0;
};

struct GSCpuBackend::Segment
{
    uint64_t id = 0;
    std::vector<PreOp> preops;
    std::vector<uint8_t> uploadData;
    std::vector<QueuedPrim> prims;
    std::vector<int32_t> primRows; // y0, y1 pairs (compact, scanned by every band)
    std::vector<std::array<uint16_t, 512>> clutSlots;
    uint32_t clutLoads = 0;
    RangeSet reads;
    RangeSet writes;
    std::vector<WriteTarget> targets;
    int minY = INT_MAX;
    int maxY = INT_MIN;
    bool serial = false; // execute as a single band (primitive wraps past the buffer width)

    bool empty() const { return prims.empty() && preops.empty(); }

    void reset()
    {
        id = 0;
        preops.clear();
        uploadData.clear();
        prims.clear();
        primRows.clear();
        clutLoads = 0;
        reads.clear();
        writes.clear();
        targets.clear();
        minY = INT_MAX;
        maxY = INT_MIN;
        serial = false;
    }

    bool primHazard(const PrimFootprint &fp) const
    {
        if (prims.empty())
            return false;
        if (!fp.reads.empty() && writes.overlaps(fp.reads))
            return true; // samples what an earlier primitive draws
        for (int i = 0; i < fp.targetCount; ++i)
        {
            const WriteTarget &t = fp.targets[i];
            if (reads.overlaps(t.range))
                return true; // draws over what an earlier primitive samples
            for (const WriteTarget &existing : targets)
            {
                const bool same = existing.bp == t.bp && existing.bw == t.bw && existing.layout == t.layout;
                if (!same && existing.range.overlaps(t.range))
                    return true; // aliasing buffers with different row mapping
            }
        }
        return false;
    }

    void addPrim(const GSPrimitiveBatch &batch, const PrimFootprint &fp)
    {
        prims.emplace_back();
        QueuedPrim &qp = prims.back();
        qp.batch = batch;
        qp.clutSlot = clutLoads;
        primRows.push_back(fp.y0);
        primRows.push_back(fp.y1);
        minY = std::min(minY, fp.y0);
        maxY = std::max(maxY, fp.y1);
        reads.addSet(fp.reads);
        for (int i = 0; i < fp.targetCount; ++i)
        {
            const WriteTarget &t = fp.targets[i];
            writes.addSet(t.range);
            bool merged = false;
            for (WriteTarget &existing : targets)
            {
                if (existing.bp == t.bp && existing.bw == t.bw && existing.layout == t.layout)
                {
                    existing.range.addSet(t.range);
                    merged = true;
                    break;
                }
            }
            if (!merged)
                targets.push_back(t);
        }
    }
};

// Fork-join pool for the bands of one segment. The dispatcher thread participates.
struct GSCpuBackend::BandPool
{
    explicit BandPool(int workers)
    {
        for (int i = 0; i < workers; ++i)
            m_threads.emplace_back([this]() { workerLoop(); });
    }

    ~BandPool()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        for (auto &t : m_threads)
        {
            if (t.joinable())
                t.join();
        }
    }

    void run(int count, const std::function<void(int)> &fn)
    {
        if (count <= 0)
            return;
        if (count == 1 || m_threads.empty())
        {
            for (int i = 0; i < count; ++i)
                fn(i);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_job = &fn;
            m_next.store(0, std::memory_order_relaxed);
            m_count = count;
            m_busy = static_cast<int>(m_threads.size());
            ++m_generation;
        }
        m_wake.notify_all();
        runItems(fn, count);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this]() { return m_busy == 0; });
        m_job = nullptr;
    }

private:
    void runItems(const std::function<void(int)> &fn, int count)
    {
        for (;;)
        {
            const int item = m_next.fetch_add(1, std::memory_order_relaxed);
            if (item >= count)
                break;
            fn(item);
        }
    }

    void workerLoop()
    {
        lowerCurrentThreadPriority();
        uint64_t seen = 0;
        for (;;)
        {
            const std::function<void(int)> *job = nullptr;
            int count = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [&]() { return m_stop || m_generation != seen; });
                if (m_stop)
                    return;
                seen = m_generation;
                job = m_job;
                count = m_count;
            }
            if (job)
                runItems(*job, count);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_busy == 0)
                    m_done.notify_one();
            }
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    const std::function<void(int)> *m_job = nullptr;
    std::atomic<int> m_next{0};
    int m_count = 0;
    int m_busy = 0;
    uint64_t m_generation = 0;
    bool m_stop = false;
};

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }


    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

GSCpuBackend::GSCpuBackend()
{
    m_instanceId = g_nextBackendInstance.fetch_add(1u, std::memory_order_relaxed) & 0xFFFFu;
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

// ---------------------------------------------------------------------------------------------
// Pixel / texture pipelines (set up once per primitive, used per pixel)
// ---------------------------------------------------------------------------------------------
struct GSCpuBackend::PixelPipe
{
    uint8_t *vram = nullptr;
    ReadVramFunc frameRead = nullptr;
    WriteVramFunc frameWrite = nullptr;
    ReadVramFunc zRead = nullptr;
    WriteVramFunc zWrite = nullptr;
    int scx0 = 0;
    int scx1 = -1;
    int scy0 = 0;
    int scy1 = -1;
    uint32_t fbp = 0;
    uint32_t fbw = 1;
    uint32_t zbp = 0;
    uint32_t fbmsk = 0;
    uint32_t zmax = 0xFFFFFFFFu;
    uint64_t test = 0;
    uint8_t fpsm = 0;
    uint8_t zpsm = 0;
    uint8_t ztst = 1;
    bool frame16 = false;
    bool frame24 = false;
    bool zmask = false;
    bool alphaTest = false;
    bool dateRead = false;
    bool fge = false;
    uint8_t fogR = 0;
    uint8_t fogG = 0;
    uint8_t fogB = 0;
    bool abe = false;
    bool pabe = false;
    uint8_t asel = 0;
    uint8_t bsel = 0;
    uint8_t csel = 0;
    uint8_t dsel = 0;
    uint8_t fix = 0;
    bool fba = false;
};

struct GSCpuBackend::TexPipe
{
    uint8_t *vram = nullptr;
    ReadVramFunc read = nullptr;
    const uint16_t *clut = nullptr;
    const uint32_t *palette = nullptr; // expanded RGBA for indexed textures
    uint32_t paletteMask = 0xFFu;
    GSTexaReg texa{};
    GSTex0Reg tex0{};
    uint32_t levelTbp[7]{};
    uint32_t levelTbw[7]{};
    int texW = 1;
    int texH = 1;
    uint16_t minU = 0;
    uint16_t maxU = 0;
    uint16_t minV = 0;
    uint16_t maxV = 0;
    uint8_t wrapU = 0;
    uint8_t wrapV = 0;
    uint8_t psm = 0;
    uint8_t cpsm = 0;
    uint8_t csm = 0;
    uint8_t csa = 0;
    uint8_t kind = 3; // 0 direct 32-bit, 1 direct 16-bit, 2 indexed, 3 invalid
    uint8_t mmin = 0;
    bool magLinear = false;
    bool minLinear = false;
    bool lodActive = false;
    bool mipmap = false;
    bool lcm = false;
    uint32_t mxl = 0;
    float lodScale = 1.0f;
    float lodK = 0.0f;
};

namespace
{
    inline uint32_t depthToU32(double z)
    {
        if (!(z > 0.0))
            return 0u;
        if (z >= 4294967295.0)
            return 0xFFFFFFFFu;
        return static_cast<uint32_t>(z);
    }

    // PS2X_GS_TRACE_PIXEL="x,y[,fromVsync]" (frame coordinates): every write attempt to that pixel of any frame buffer, with the
    // incoming colour / depth, the blend selectors and the result or the reason it was dropped (`[gs-pixel]`). Bug hunt:
    // which pass of which primitive makes a pixel black (CLAUDE.md issue 6).
    struct GsPixelTrace
    {
        int x = -1, y = -1;
        unsigned long long fromVsync = 0u;
        uint32_t logged = 0u;
    };
    GsPixelTrace &gsPixelTrace()
    {
        static GsPixelTrace t = []
        {
            GsPixelTrace r;
            if (const char *env = std::getenv("PS2X_GS_TRACE_PIXEL"))
                std::sscanf(env, "%d,%d,%llu", &r.x, &r.y, &r.fromVsync);
            return r;
        }();
        return t;
    }

    inline void gsWritePixel(const GSCpuBackend::PixelPipe &p, int x, int y, uint32_t z,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
    {
        GsPixelTrace &trace = gsPixelTrace();
        const bool traced = x == trace.x && y == trace.y && trace.logged < 4000u && ps2xGuestVsync() >= trace.fromVsync;
        if (traced)
        {
            ++trace.logged;
            std::fprintf(stderr, "[gs-pixel] fbp %#x fbmsk %08x src %u,%u,%u,%u z %#x abe %d sel %u%u%u%u fix %u atst %d ztst %u zmask %d\n",
                         p.fbp, p.fbmsk, r, g, b, a, z, p.abe ? 1 : 0, p.asel, p.bsel, p.csel, p.dsel, p.fix, p.alphaTest ? 1 : 0,
                         p.ztst, p.zmask ? 1 : 0);
        }
        if (x < p.scx0 || x > p.scx1 || y < p.scy0 || y > p.scy1)
            return;

        if (p.fge)
        {
            const uint32_t inverseFog = 255u - fog;
            r = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * r) >> 8) + ((inverseFog * p.fogR) >> 8));
            g = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * g) >> 8) + ((inverseFog * p.fogG) >> 8));
            b = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * b) >> 8) + ((inverseFog * p.fogB) >> 8));
        }

        PixelWriteMask writeMask{};
        if (p.alphaTest)
        {
            writeMask = classifyAlphaTest(p.test, a, p.fpsm);
            if (!writeMask.writesAnything())
                return;
        }

        const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && p.fpsm == GS_PSM_CT32;
        const bool frmw = p.dateRead ||
                          (writeMask.writesFramebuffer() && (p.fbmsk != 0u || p.abe || preserveDestinationAlpha));

        uint32_t rawFramebufferPixel = 0u;
        uint32_t fbrgba = 0u;
        const uint32_t ux = static_cast<uint32_t>(x);
        const uint32_t uy = static_cast<uint32_t>(y);
        if (frmw)
        {
            rawFramebufferPixel = p.frameRead(p.vram, p.fbp, p.fbw, ux, uy);
            fbrgba = rawFramebufferPixel;
            if (p.frame16)
                fbrgba = Rgba5551ToRgba8888(static_cast<u16>(fbrgba));
            else if (p.frame24)
                fbrgba |= 0x80000000u; // the GS supplies 0x80 as destination alpha for RGB24
        }

        if (p.dateRead && !passesDestinationAlphaTest(p.test, p.fpsm, rawFramebufferPixel))
            return;

        if (z > p.zmax)
            z = p.zmax;

        switch (p.ztst)
        {
        case 0:
            return;
        case 1:
            break;
        case 2:
            if (z < p.zRead(p.vram, p.zbp, p.fbw, ux, uy))
            {
                if (traced)
                    std::fprintf(stderr, "[gs-pixel]    z test failed (buffer %#x)\n", p.zRead(p.vram, p.zbp, p.fbw, ux, uy));
                return;
            }
            break;
        default:
            if (z <= p.zRead(p.vram, p.zbp, p.fbw, ux, uy))
                return;
            break;
        }

        if (writeMask.writesFramebuffer())
        {
            if (p.abe && !(p.pabe && (a & 0x80u) == 0u))
            {
                const int dr = static_cast<int>(fbrgba & 0xFFu);
                const int dg = static_cast<int>((fbrgba >> 8) & 0xFFu);
                const int db = static_cast<int>((fbrgba >> 16) & 0xFFu);
                const int da = static_cast<int>((fbrgba >> 24) & 0xFFu);
                const int cAlpha = (p.csel == 0u) ? a : (p.csel == 1u) ? da : p.fix;
                auto pick = [](uint8_t sel, int cs, int cd) -> int
                {
                    return sel == 0u ? cs : (sel == 1u ? cd : 0);
                };
                const int sr = r;
                const int sg = g;
                const int sb = b;
                r = clampU8(((pick(p.asel, sr, dr) - pick(p.bsel, sr, dr)) * cAlpha >> 7) + pick(p.dsel, sr, dr));
                g = clampU8(((pick(p.asel, sg, dg) - pick(p.bsel, sg, dg)) * cAlpha >> 7) + pick(p.dsel, sg, dg));
                b = clampU8(((pick(p.asel, sb, db) - pick(p.bsel, sb, db)) * cAlpha >> 7) + pick(p.dsel, sb, db));
            }

            if (writeMask.writeAlpha && p.fba && p.fpsm != GS_PSM_CT24)
                a = static_cast<uint8_t>(a | 0x80u);

            uint32_t pixel = pack32(r, g, b, a);
            if (p.fbmsk != 0u)
                pixel = (pixel & ~p.fbmsk) | (fbrgba & p.fbmsk);
            if (preserveDestinationAlpha)
                pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
            if (p.frame16)
                pixel = Rgba8888ToRgba5551(pixel);
            p.frameWrite(p.vram, p.fbp, p.fbw, ux, uy, pixel);
            if (traced)
                std::fprintf(stderr, "[gs-pixel]    dst %08x -> %08x\n", fbrgba, pixel);
        }
        else if (traced)
            std::fprintf(stderr, "[gs-pixel]    no frame write (alpha test)\n");

        if (writeMask.writeDepth && !p.zmask)
            p.zWrite(p.vram, p.zbp, p.fbw, ux, uy, z);
    }

    inline uint32_t texLookupClut(const GSCpuBackend::TexPipe &tp, uint32_t index)
    {
        const uint8_t cpsm = tp.cpsm;
        const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t clutBase = (static_cast<uint32_t>(tp.csa) & csaMask) << 4u;
        const uint32_t sourceIndex = isFourBitIndexedPsm(tp.psm) ? (index & 0x0Fu) : (index & 0xFFu);

        uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (!sixteenBit && tp.csm == 0u && isEightBitIndexedPsm(tp.psm))
        {
            const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
            clutIndex = block + (sourceIndex & 0x0Fu);
        }

        const uint16_t *clut = tp.clut;
        switch (cpsm)
        {
        case GS_PSM_CT32:
        {
            const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
            return applyTexa(tp.texa, cpsm, raw);
        }
        case GS_PSM_CT24:
        {
            const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
            return applyTexa(tp.texa, cpsm, raw & 0x00FFFFFFu);
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return applyTexa(tp.texa, cpsm, Rgba5551ToRgba8888(clut[clutIndex]));
        default:
            break;
        }
        return 0xFFFF00FFu;
    }
}

namespace
{
    inline int wrapCoord(int c, int size, uint8_t mode, uint16_t regionMin, uint16_t regionMax)
    {
        switch (mode)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(c) & static_cast<uint32_t>(size - 1));
        case 1: // CLAMP
            return c < 0 ? 0 : (c >= size ? size - 1 : c);
        case 2: // REGION_CLAMP
            return std::min(std::max(c, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        default: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(c) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        }
    }

    inline uint32_t texFetch(const GSCpuBackend::TexPipe &tp, int level, int u, int v)
    {
        if (level == 0)
        {
            u = wrapCoord(u, tp.texW, tp.wrapU, tp.minU, tp.maxU);
            v = wrapCoord(v, tp.texH, tp.wrapV, tp.minV, tp.maxV);
        }
        else
        {
            u = wrapCoord(u, std::max(1, tp.texW >> level), tp.wrapU,
                          static_cast<uint16_t>(tp.minU >> level), static_cast<uint16_t>(tp.maxU >> level));
            v = wrapCoord(v, std::max(1, tp.texH >> level), tp.wrapV,
                          static_cast<uint16_t>(tp.minV >> level), static_cast<uint16_t>(tp.maxV >> level));
        }
        const uint32_t raw = tp.read(tp.vram, tp.levelTbp[level], tp.levelTbw[level],
                                     static_cast<uint32_t>(u), static_cast<uint32_t>(v));
        switch (tp.kind)
        {
        case 2:
            return tp.palette[raw & tp.paletteMask];
        case 0:
            return applyTexa(tp.texa, tp.psm, raw);
        case 1:
            return applyTexa(tp.texa, tp.psm, Rgba5551ToRgba8888(static_cast<u16>(raw)));
        default:
            return 0xFFFF00FFu;
        }
    }

    inline uint32_t lerpColor(uint32_t c0, uint32_t c1, uint32_t w) // w in [0,256]
    {
        const uint32_t iw = 256u - w;
        const uint32_t rb = ((c0 & 0x00FF00FFu) * iw + (c1 & 0x00FF00FFu) * w + 0x00800080u) >> 8u;
        const uint32_t ga = (((c0 >> 8u) & 0x00FF00FFu) * iw + ((c1 >> 8u) & 0x00FF00FFu) * w + 0x00800080u) >> 8u;
        return (rb & 0x00FF00FFu) | ((ga & 0x00FF00FFu) << 8u);
    }

    inline uint32_t texSampleLevel(const GSCpuBackend::TexPipe &tp, int level, float u, float v, bool linear)
    {
        if (!linear)
            return texFetch(tp, level, static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)));

        const float su = u - 0.5f;
        const float sv = v - 0.5f;
        const float fu = std::floor(su);
        const float fv = std::floor(sv);
        const int u0 = static_cast<int>(fu);
        const int v0 = static_cast<int>(fv);
        const uint32_t wx = static_cast<uint32_t>(std::min(256.0f, (su - fu) * 256.0f));
        const uint32_t wy = static_cast<uint32_t>(std::min(256.0f, (sv - fv) * 256.0f));

        const uint32_t c00 = texFetch(tp, level, u0, v0);
        const uint32_t c10 = texFetch(tp, level, u0 + 1, v0);
        const uint32_t c01 = texFetch(tp, level, u0, v0 + 1);
        const uint32_t c11 = texFetch(tp, level, u0 + 1, v0 + 1);
        return lerpColor(lerpColor(c00, c10, wx), lerpColor(c01, c11, wx), wy);
    }

    // u, v are texel coordinates of level 0; q is the interpolated Q used for the GS LOD.
    inline uint32_t texSample(const GSCpuBackend::TexPipe &tp, float u, float v, float q)
    {
        if (!tp.lodActive)
            return texSampleLevel(tp, 0, u, v, tp.magLinear);

        float lod = tp.lodK;
        if (!tp.lcm)
        {
            const float aq = std::fabs(q);
            lod = (aq > 1.0e-30f) ? (-std::log2(aq) * tp.lodScale + tp.lodK) : 64.0f;
        }
        if (!(lod > 0.0f))
            return texSampleLevel(tp, 0, u, v, tp.magLinear);
        if (!tp.mipmap)
            return texSampleLevel(tp, 0, u, v, tp.minLinear);

        const float maxLod = static_cast<float>(tp.mxl);
        if (lod > maxLod)
            lod = maxLod;
        const bool mipLinear = tp.mmin == 3u || tp.mmin == 5u;
        if (!mipLinear)
        {
            int level = static_cast<int>(std::floor(lod + 0.5f));
            level = std::min<int>(level, static_cast<int>(tp.mxl));
            const float scale = 1.0f / static_cast<float>(1 << level);
            return texSampleLevel(tp, level, u * scale, v * scale, tp.minLinear);
        }

        int level = static_cast<int>(std::floor(lod));
        float frac = lod - static_cast<float>(level);
        if (level >= static_cast<int>(tp.mxl))
        {
            level = static_cast<int>(tp.mxl);
            frac = 0.0f;
        }
        const float scale0 = 1.0f / static_cast<float>(1 << level);
        const uint32_t c0 = texSampleLevel(tp, level, u * scale0, v * scale0, tp.minLinear);
        const uint32_t w = static_cast<uint32_t>(frac * 256.0f);
        if (w == 0u)
            return c0;
        const float scale1 = scale0 * 0.5f;
        const uint32_t c1 = texSampleLevel(tp, level + 1, u * scale1, v * scale1, tp.minLinear);
        return lerpColor(c0, c1, w);
    }

    inline void texCombine(const GSTex0Reg &tex, uint32_t texel, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
    {
        const TextureCombineResult color = combineTexture(tex, r, g, b, a,
                                                          static_cast<uint8_t>(texel & 0xFFu),
                                                          static_cast<uint8_t>((texel >> 8) & 0xFFu),
                                                          static_cast<uint8_t>((texel >> 16) & 0xFFu),
                                                          static_cast<uint8_t>((texel >> 24) & 0xFFu));
        r = color.r;
        g = color.g;
        b = color.b;
        a = color.a;
    }
}

namespace
{
    struct PaletteCacheEntry
    {
        uint64_t generation = ~0ull;
        uint32_t slot = 0;
        uint32_t params = 0;
        uint32_t palette[256]{};
    };
    thread_local PaletteCacheEntry t_paletteCache[4];
    thread_local uint32_t t_paletteNext = 0;
}

void GSCpuBackend::SetupPixelPipe(PixelPipe &pp, const GSDrawState &state) const
{
    const GSContext &ctx = state.context;
    pp.vram = m_vram;
    pp.fpsm = ctx.frame.psm;
    pp.zpsm = ctx.zbuf.psm;
    pp.frameRead = m_readVramFuncs[pp.fpsm & 0x3Fu];
    pp.frameWrite = m_writeVramFuncs[pp.fpsm & 0x3Fu];
    pp.zRead = m_readVramFuncs[pp.zpsm & 0x3Fu];
    pp.zWrite = m_writeVramFuncs[pp.zpsm & 0x3Fu];
    pp.scx0 = ctx.scissor.x0;
    pp.scx1 = ctx.scissor.x1;
    pp.scy0 = ctx.scissor.y0;
    pp.scy1 = ctx.scissor.y1;
    pp.fbp = framePageBaseToBlock(ctx.frame.fbp);
    pp.fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    pp.zbp = framePageBaseToBlock(ctx.zbuf.zbp);
    pp.fbmsk = ctx.frame.fbmsk;
    pp.test = ctx.test;
    if (pp.zpsm == GS_PSM_Z24)
        pp.zmax = 0x00FFFFFFu;
    else if (pp.zpsm == GS_PSM_Z16 || pp.zpsm == GS_PSM_Z16S)
        pp.zmax = 0x0000FFFFu;
    else
        pp.zmax = 0xFFFFFFFFu;
    pp.ztst = static_cast<uint8_t>((ctx.test >> 17) & 0x3u);
    pp.frame16 = bitsPerPixel(pp.fpsm) == 16u;
    pp.frame24 = pp.fpsm == GS_PSM_CT24;
    pp.zmask = ctx.zbuf.zmask;
    pp.alphaTest = (ctx.test & 0x1u) != 0u;
    pp.dateRead = ((ctx.test >> 14) & 0x1u) != 0u &&
                  (pp.fpsm == GS_PSM_CT32 || pp.fpsm == GS_PSM_CT16 || pp.fpsm == GS_PSM_CT16S);
    pp.fge = state.prim.fge;
    pp.fogR = state.fogR;
    pp.fogG = state.fogG;
    pp.fogB = state.fogB;
    pp.abe = state.prim.abe;
    pp.pabe = state.pabe;
    pp.asel = static_cast<uint8_t>(ctx.alpha & 0x3u);
    pp.bsel = static_cast<uint8_t>((ctx.alpha >> 2) & 0x3u);
    pp.csel = static_cast<uint8_t>((ctx.alpha >> 4) & 0x3u);
    pp.dsel = static_cast<uint8_t>((ctx.alpha >> 6) & 0x3u);
    pp.fix = static_cast<uint8_t>((ctx.alpha >> 32) & 0xFFu);
    pp.fba = (ctx.fba & 0x1ull) != 0ull;
}

void GSCpuBackend::SetupTexPipe(TexPipe &tp, const GSDrawState &state, const ClutRef &clut) const
{
    const GSContext &ctx = state.context;
    const GSTex0Reg &tex = ctx.tex0;
    tp.vram = m_vram;
    tp.read = m_readVramFuncs[tex.psm & 0x3Fu];
    tp.clut = clut.data;
    tp.texa = state.texa;
    tp.tex0 = tex;
    tp.texW = state.textureWidth;
    tp.texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    tp.wrapU = static_cast<uint8_t>(clamp & 0x3u);
    tp.wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    tp.minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    tp.maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    tp.minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    tp.maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);
    tp.psm = tex.psm;
    tp.cpsm = tex.cpsm;
    tp.csm = tex.csm;
    tp.csa = tex.csa;
    switch (tex.psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
        tp.kind = 0u;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        tp.kind = 1u;
        break;
    case GS_PSM_T8:
    case GS_PSM_T8H:
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        tp.kind = 2u;
        break;
    default:
        tp.kind = 3u;
        break;
    }

    if (tp.kind == 2u)
    {
        // Expand the CLUT (with CSA/CSM/TEXA applied) once and reuse it across primitives.
        const bool fourBit = isFourBitIndexedPsm(tex.psm);
        const uint32_t params = static_cast<uint32_t>(tex.cpsm & 0x3Fu) |
                                (static_cast<uint32_t>(tex.csm & 1u) << 6u) |
                                (static_cast<uint32_t>(tex.csa & 0x1Fu) << 7u) |
                                (static_cast<uint32_t>(fourBit ? 1u : 0u) << 12u) |
                                (static_cast<uint32_t>(state.texa.ta0) << 13u) |
                                (static_cast<uint32_t>(state.texa.aem ? 1u : 0u) << 21u) |
                                (static_cast<uint32_t>(isEightBitIndexedPsm(tex.psm) ? 1u : 0u) << 22u);
        const uint32_t taggedSlot = (clut.slot & 0x00FFFFFFu) | (static_cast<uint32_t>(state.texa.ta1) << 24u);
        PaletteCacheEntry *entry = nullptr;
        for (PaletteCacheEntry &candidate : t_paletteCache)
        {
            if (candidate.generation == clut.generation && candidate.slot == taggedSlot && candidate.params == params)
            {
                entry = &candidate;
                break;
            }
        }
        if (!entry)
        {
            entry = &t_paletteCache[t_paletteNext++ & 3u];
            entry->generation = clut.generation;
            entry->slot = taggedSlot;
            entry->params = params;
            const uint32_t count = fourBit ? 16u : 256u;
            for (uint32_t i = 0; i < count; ++i)
                entry->palette[i] = texLookupClut(tp, i);
        }
        tp.palette = entry->palette;
        tp.paletteMask = fourBit ? 0x0Fu : 0xFFu;
    }

    const uint64_t tex1 = ctx.tex1;
    const uint32_t mmag = static_cast<uint32_t>((tex1 >> 5u) & 0x1u);
    tp.mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    tp.mxl = std::min<uint32_t>(static_cast<uint32_t>((tex1 >> 2u) & 0x7u), 6u);
    tp.lcm = (tex1 & 0x1u) != 0u;
    tp.lodScale = static_cast<float>(1u << static_cast<uint32_t>((tex1 >> 19u) & 0x3u));
    int32_t k = static_cast<int32_t>((tex1 >> 32u) & 0xFFFu);
    if ((k & 0x800) != 0)
        k -= 0x1000;
    tp.lodK = static_cast<float>(k) / 16.0f;
    tp.magLinear = mmag != 0u;
    tp.minLinear = tp.mmin == 1u || tp.mmin == 4u || tp.mmin == 5u;
    tp.mipmap = tp.mxl > 0u && tp.mmin >= 2u && tp.mmin <= 5u;
    tp.lodActive = tp.mipmap || (tp.magLinear != tp.minLinear);
    const uint32_t levels = tp.mipmap ? tp.mxl : 0u;
    for (uint32_t level = 0u; level <= levels; ++level)
        mipLevelBase(state, level, tp.levelTbp[level], tp.levelTbw[level]);
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch, const ClutRef &clut, int clipY0, int clipY1)
{
    PixelPipe pp;
    SetupPixelPipe(pp, batch.state);
    pp.scy0 = std::max(pp.scy0, clipY0);
    pp.scy1 = std::min(pp.scy1, clipY1);
    if (pp.scy1 < pp.scy0 || pp.scx1 < pp.scx0)
        return;

    const GSPrimType type = batch.state.prim.type;
    TexPipe tp;
    const TexPipe *tpp = nullptr;
    if (batch.state.prim.tme &&
        (type == GS_PRIM_SPRITE || type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN))
    {
        SetupTexPipe(tp, batch.state, clut);
        tpp = &tp;
    }

    switch (type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch, pp, tpp, clipY0, clipY1);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch, pp, tpp, clipY0, clipY1);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch, pp, clipY0, clipY1);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const GSContext &ctx = batch.state.context;
        const int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        const int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        gsWritePixel(pp, px, py, depthToU32(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch, const PixelPipe &pp, const TexPipe *tp, int, int)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSContext &ctx = state.context;

    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    const uint32_t z1 = depthToU32(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // pp's scissor already includes the band clip.
    if (unclippedX1 < pp.scx0 || unclippedX0 > pp.scx1 ||
        unclippedY1 < pp.scy0 || unclippedY0 > pp.scy1)
        return;

    const int drawX0 = clampInt(unclippedX0, pp.scx0, pp.scx1);
    const int drawY0 = clampInt(unclippedY0, pp.scy0, pp.scy1);
    const int drawX1 = clampInt(unclippedX1, pp.scx0, pp.scx1);
    const int drawY1 = clampInt(unclippedY1, pp.scy0, pp.scy1);

    const uint8_t r = v1.r;
    const uint8_t g = v1.g;
    const uint8_t b = v1.b;
    const uint8_t a = v1.a;

    if (!tp)
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                gsWritePixel(pp, x, y, z1, r, g, b, a, v1.fog);
        return;
    }

    const int texW = tp->texW;
    const int texH = tp->texH;
    const bool fst = state.prim.fst;
    float u0f, v0f, u1f, v1f;
    if (fst)
    {
        u0f = static_cast<float>(v0.u >> 4);
        v0f = static_cast<float>(v0.v >> 4);
        u1f = static_cast<float>(v1.u >> 4);
        v1f = static_cast<float>(v1.v >> 4);
    }
    else
    {
        const float q0 = fabsQ(v0.q);
        const float q1 = fabsQ(v1.q);
        u0f = (v0.s / q0) * static_cast<float>(texW);
        v0f = (v0.t / q0) * static_cast<float>(texH);
        u1f = (v1.s / q1) * static_cast<float>(texW);
        v1f = (v1.t / q1) * static_cast<float>(texH);
    }
    const float lodQ = v1.q;
    const float spriteW = static_cast<float>(spanX);
    const float spriteH = static_cast<float>(spanY);

    for (int y = drawY0; y <= drawY1; ++y)
    {
        const float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
        float texVf = v0f + (v1f - v0f) * ty;
        if (fst)
            texVf = static_cast<float>(clampInt(static_cast<int>((texVf * 16.0f) + 0.5f), 0, 0xFFFF)) / 16.0f;

        for (int x = drawX0; x <= drawX1; ++x)
        {
            const float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
            float texUf = u0f + (u1f - u0f) * tx;
            if (fst)
                texUf = static_cast<float>(clampInt(static_cast<int>((texUf * 16.0f) + 0.5f), 0, 0xFFFF)) / 16.0f;

            const uint32_t texel = texSample(*tp, texUf, texVf, lodQ);
            uint8_t cr = r;
            uint8_t cg = g;
            uint8_t cb = b;
            uint8_t ca = a;
            texCombine(tp->tex0, texel, cr, cg, cb, ca);
            gsWritePixel(pp, x, y, z1, cr, cg, cb, ca, v1.fog);
        }
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch, const PixelPipe &pp, const TexPipe *tp, int, int)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    const GSVertex &provoking = batch.vertices[2]; // flat shading uses the last (kicking) vertex

    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;

    const GSVertex *pv[3] = {&batch.vertices[0], &batch.vertices[1], &batch.vertices[2]};
    double vx[3];
    double vy[3];
    for (int i = 0; i < 3; ++i)
    {
        vx[i] = static_cast<double>(pv[i]->x) - static_cast<double>(ofx);
        vy[i] = static_cast<double>(pv[i]->y) - static_cast<double>(ofy);
    }

    // Twice the signed area. Vertex positions are multiples of 1/16 and pixel centres are at
    // +0.5, so every edge-function value below is exact in double precision.
    double area2 = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vy[1] - vy[0]) * (vx[2] - vx[0]);
    if (std::fabs(area2) < 1.0e-9)
        return;
    if (area2 < 0.0)
    {
        std::swap(pv[1], pv[2]);
        std::swap(vx[1], vx[2]);
        std::swap(vy[1], vy[2]);
        area2 = -area2;
    }
    const double invArea2 = 1.0 / area2;

    int minX = static_cast<int>(std::floor(std::min({vx[0], vx[1], vx[2]})));
    int maxX = static_cast<int>(std::ceil(std::max({vx[0], vx[1], vx[2]})));
    int minY = static_cast<int>(std::floor(std::min({vy[0], vy[1], vy[2]})));
    int maxY = static_cast<int>(std::ceil(std::max({vy[0], vy[1], vy[2]})));

    minX = std::max(minX, pp.scx0);
    maxX = std::min(maxX, pp.scx1);
    minY = std::max(minY, pp.scy0);
    maxY = std::min(maxY, pp.scy1);
    if (maxX < minX || maxY < minY)
        return;

    // Edge i is opposite vertex i: e0 = v1->v2, e1 = v2->v0, e2 = v0->v1.
    // E(p) = dx * (py - ay) - dy * (px - ax) is positive inside.
    struct Edge
    {
        double ax, ay, dx, dy;
        bool topLeft;
    };
    Edge edges[3];
    for (int i = 0; i < 3; ++i)
    {
        const int ia = (i + 1) % 3;
        const int ib = (i + 2) % 3;
        Edge &e = edges[i];
        e.ax = vx[ia];
        e.ay = vy[ia];
        e.dx = vx[ib] - vx[ia];
        e.dy = vy[ib] - vy[ia];
        // Top-left fill rule: every pixel on a shared edge belongs to exactly one triangle.
        e.topLeft = (e.dy < 0.0) || (e.dy == 0.0 && e.dx > 0.0);
    }

    const GSVertex &v0 = *pv[0];
    const GSVertex &v1 = *pv[1];
    const GSVertex &v2 = *pv[2];
    const bool gouraud = state.prim.iip;
    const bool fst = state.prim.fst;
    const float texWf = tp ? static_cast<float>(tp->texW) : 1.0f;
    const float texHf = tp ? static_cast<float>(tp->texH) : 1.0f;

    for (int y = minY; y <= maxY; ++y)
    {
        const double py = static_cast<double>(y) + 0.5;

        // Narrow the scan to the pixels the three edges can admit (with a 1-pixel margin; the
        // exact inside test below still decides every pixel).
        double lo = static_cast<double>(minX);
        double hi = static_cast<double>(maxX);
        bool rowEmpty = false;
        for (int i = 0; i < 3; ++i)
        {
            const Edge &e = edges[i];
            const double c = e.dx * (py - e.ay) + e.dy * e.ax; // E(px) = c - dy * px
            if (e.dy > 0.0)
                hi = std::min(hi, std::floor(c / e.dy - 0.5) + 1.0);
            else if (e.dy < 0.0)
                lo = std::max(lo, std::ceil(c / e.dy - 0.5) - 1.0);
            else
            {
                const double ev = e.dx * (py - e.ay);
                if (!(ev > 0.0 || (ev == 0.0 && e.topLeft)))
                    rowEmpty = true;
            }
        }
        if (rowEmpty || lo > hi)
            continue;
        const int xl = static_cast<int>(lo);
        const int xr = static_cast<int>(hi);

        const double px0 = static_cast<double>(xl) + 0.5;
        double e[3];
        for (int i = 0; i < 3; ++i)
            e[i] = edges[i].dx * (py - edges[i].ay) - edges[i].dy * (px0 - edges[i].ax);

        for (int x = xl; x <= xr; ++x)
        {
            const bool inside0 = e[0] > 0.0 || (e[0] == 0.0 && edges[0].topLeft);
            const bool inside1 = e[1] > 0.0 || (e[1] == 0.0 && edges[1].topLeft);
            const bool inside2 = e[2] > 0.0 || (e[2] == 0.0 && edges[2].topLeft);
            if (inside0 && inside1 && inside2)
            {
                const double dw0 = e[0] * invArea2;
                const double dw1 = e[1] * invArea2;
                const double dw2 = e[2] * invArea2;
                const float w0 = static_cast<float>(dw0);
                const float w1 = static_cast<float>(dw1);
                const float w2 = static_cast<float>(dw2);

                const double z = v0.z * dw0 + v1.z * dw1 + v2.z * dw2;

                uint8_t r, g, b, a;
                if (gouraud)
                {
                    r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                    g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                    b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                    a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
                }
                else
                {
                    r = provoking.r;
                    g = provoking.g;
                    b = provoking.b;
                    a = provoking.a;
                }

                if (tp)
                {
                    const float iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    float tu, tv;
                    if (fst)
                    {
                        tu = (v0.u * w0 + v1.u * w1 + v2.u * w2) / 16.0f;
                        tv = (v0.v * w0 + v1.v * w1 + v2.v * w2) / 16.0f;
                    }
                    else
                    {
                        // The GS interpolates S, T and Q and divides per pixel (perspective correct).
                        const float is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                        const float it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                        const float invQ = 1.0f / fabsQ(iq);
                        tu = is * invQ * texWf;
                        tv = it * invQ * texHf;
                    }
                    const uint32_t texel = texSample(*tp, tu, tv, iq);
                    texCombine(tp->tex0, texel, r, g, b, a);
                }

                const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
                gsWritePixel(pp, x, y, depthToU32(z + 0.5), r, g, b, a, fog);
            }
            e[0] -= edges[0].dy;
            e[1] -= edges[1].dy;
            e[2] -= edges[2].dy;
        }
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch, const PixelPipe &pp, int, int)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSContext &ctx = state.context;

    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    const int x1 = static_cast<int>(v1.x) - ofx;
    const int y1 = static_cast<int>(v1.y) - ofy;

    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        if (y0 >= pp.scy0 && y0 <= pp.scy1)
        {
            const float t = static_cast<float>(step) / static_cast<float>(totalSteps);
            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
                g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
                b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
                a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
            }
            else
            {
                r = v1.r;
                g = v1.g;
                b = v1.b;
                a = v1.a;
            }
            const double z = v0.z + (v1.z - v0.z) * t;
            const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
            gsWritePixel(pp, x0, y0, depthToU32(z), r, g, b, a, fog);
        }

        if (x0 == x1 && y0 == y1)
            break;

        const int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

// ---------------------------------------------------------------------------------------------
// Engine control
// ---------------------------------------------------------------------------------------------
GSCpuBackend::~GSCpuBackend()
{
    StopEngine();
    if (m_captureFile)
    {
        std::fclose(m_captureFile);
        m_captureFile = nullptr;
    }
}

namespace
{
    int decideEngineWorkers()
    {
        if (const char *env = std::getenv("PS2X_GS_THREADS"))
        {
            const int requested = std::atoi(env);
            return std::max(0, std::min(requested, 31));
        }
        const unsigned hc = std::thread::hardware_concurrency();
        if (hc <= 2u)
            return 0;
        const int workers = static_cast<int>(hc) - 3;
        return std::max(1, std::min(workers, 11));
    }
}

bool GSCpuBackend::EngineEnabled()
{
    if (m_engineMode < 0)
    {
        m_engineMode = decideEngineWorkers();
        std::cout << "[gs] renderer: " << (m_engineMode > 0 ? "deferred band-parallel" : "immediate")
                  << " workers=" << m_engineMode << std::endl;
    }
    if (m_engineMode > 0 && !m_dispatcher.joinable() && !m_engineStop)
        StartEngineUnlocked();
    return m_engineMode > 0;
}

void GSCpuBackend::StartEngineUnlocked()
{
    m_pool = std::make_unique<BandPool>(m_engineMode);
    m_dispatcher = std::thread([this]() { DispatcherLoop(); });
    g_statWorkers.store(static_cast<uint32_t>(m_engineMode + 1), std::memory_order_relaxed);
}

void GSCpuBackend::StopEngine()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_dispatcher.joinable())
            return;
        m_engineStop = true;
    }
    m_dispatchCv.notify_all();
    m_dispatcher.join();
    m_pool.reset();
}

GSCpuBackend::Segment &GSCpuBackend::OpenSegmentUnlocked()
{
    if (!m_open)
    {
        if (!m_freeSegments.empty())
        {
            m_open = std::move(m_freeSegments.back());
            m_freeSegments.pop_back();
        }
        else
        {
            m_open = std::make_unique<Segment>();
        }
    }
    return *m_open;
}

void GSCpuBackend::CloseOpenSegmentUnlocked()
{
    if (!m_open || m_open->empty())
        return;
    m_open->id = ++m_lastClosedId;
    m_queuedPrims += m_open->prims.size();
    m_queuedBytes += m_open->uploadData.size();
    m_fifo.push_back(std::move(m_open));
    g_statSegments.fetch_add(1u, std::memory_order_relaxed);
    m_dispatchCv.notify_one();
}

void GSCpuBackend::WaitForSpaceUnlocked(std::unique_lock<std::mutex> &lock)
{
    auto hasSpace = [this]()
    {
        return m_engineStop ||
               (m_fifo.size() < kMaxFifoSegments && m_queuedPrims < kMaxQueuedPrims && m_queuedBytes < kMaxQueuedBytes);
    };
    if (hasSpace())
        return;
    const auto t0 = std::chrono::steady_clock::now();
    m_spaceCv.wait(lock, hasSpace);
    g_statSpaceWaitUs.fetch_add(elapsedUs(t0), std::memory_order_relaxed);
}

void GSCpuBackend::CloseForHazardUnlocked(std::unique_lock<std::mutex> &lock)
{
    g_statHazardCloses.fetch_add(1u, std::memory_order_relaxed);
    CloseOpenSegmentUnlocked();
    WaitForSpaceUnlocked(lock);
}

// Runs fn with m_mutex held after every queued command has executed and while no segment runs.
// Must be entered without m_mutex held (lock order: m_drainMutex, then m_mutex).
template <typename Fn>
void GSCpuBackend::WithQuiescentVram(Fn &&fn) const
{
    GSCpuBackend *self = const_cast<GSCpuBackend *>(this);
    std::lock_guard<std::mutex> drainLock(m_drainMutex);
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!self->m_dispatcher.joinable())
    {
        fn();
        return;
    }
    self->CloseOpenSegmentUnlocked();
    const uint64_t target = self->m_lastClosedId;
    g_statDrains.fetch_add(1u, std::memory_order_relaxed);
    if (self->m_completedId < target)
    {
        const auto t0 = std::chrono::steady_clock::now();
        self->m_activationLimit = target;
        m_drainCv.wait(lock, [&]() { return self->m_completedId >= target || self->m_engineStop; });
        g_statDrainWaitUs.fetch_add(elapsedUs(t0), std::memory_order_relaxed);
    }
    fn();
    self->m_activationLimit = ~0ull;
    self->m_dispatchCv.notify_one();
}

void GSCpuBackend::DispatcherLoop()
{
    lowerCurrentThreadPriority();
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;)
    {
        m_dispatchCv.wait(lock, [this]()
                          { return m_engineStop || (!m_fifo.empty() && m_fifo.front()->id <= m_activationLimit); });
        if (m_engineStop)
            break;
        Segment *segment = m_fifo.front().get();
        lock.unlock();

        RunPreOps(*segment);
        if (!segment->prims.empty())
        {
            const int bands = segment->serial ? 1 : (segment->maxY - segment->minY) / kBandRows + 1;
            m_pool->run(bands, [this, segment](int band) { ExecuteBand(*segment, band); });
        }

        lock.lock();
        m_completedId = segment->id;
        m_queuedPrims -= std::min(m_queuedPrims, segment->prims.size());
        m_queuedBytes -= std::min(m_queuedBytes, segment->uploadData.size());
        std::unique_ptr<Segment> done = std::move(m_fifo.front());
        m_fifo.pop_front();
        done->reset();
        if (m_freeSegments.size() < 8u)
            m_freeSegments.push_back(std::move(done));
        m_drainCv.notify_all();
        m_spaceCv.notify_all();
    }
    m_drainCv.notify_all();
    m_spaceCv.notify_all();
}

void GSCpuBackend::RunPreOps(Segment &segment)
{
    segment.clutSlots.resize(static_cast<size_t>(segment.clutLoads) + 1u);
    segment.clutSlots[0] = m_execClut;
    size_t slot = 0;
    for (const PreOp &op : segment.preops)
    {
        switch (op.kind)
        {
        case PreOp::LoadClut:
            segment.clutSlots[slot + 1u] = segment.clutSlots[slot];
            ++slot;
            LoadClutInto(segment.clutSlots[slot], op.tex0, op.texclut);
            break;
        case PreOp::Upload:
            ExecuteUpload(op, segment.uploadData.data() + op.dataOffset);
            break;
        case PreOp::LocalCopy:
            ExecuteLocalCopy(op.transfer);
            break;
        }
    }
    m_execClut = segment.clutSlots[slot];
}

void GSCpuBackend::ExecuteBand(const Segment &segment, int band)
{
    const int by0 = segment.minY + band * kBandRows;
    const int by1 = segment.serial ? segment.maxY : by0 + kBandRows - 1;
    const size_t count = segment.prims.size();
    const int32_t *rows = segment.primRows.data();
    for (size_t i = 0; i < count; ++i)
    {
        const int y0 = rows[i * 2u];
        const int y1 = rows[i * 2u + 1u];
        if (y1 < by0 || y0 > by1)
            continue;
        const QueuedPrim &qp = segment.prims[i];
        ClutRef clut;
        clut.data = segment.clutSlots[qp.clutSlot].data();
        clut.generation = (m_instanceId << 48u) | (segment.id & 0x0000FFFFFFFFFFFFull);
        clut.slot = qp.clutSlot;
        DrawPrimitive(qp.batch, clut, std::max(by0, y0), std::min(by1, y1));
    }
}

// ---------------------------------------------------------------------------------------------
// Producer API
// ---------------------------------------------------------------------------------------------
void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    WithQuiescentVram([&]()
                      {
                          m_vram = vram;
                          m_vramSize = vramSize;
                          ResetUnlocked(); });
}

void GSCpuBackend::Reset()
{
    WithQuiescentVram([&]()
                      {
                          if (m_captureFile)
                              CaptureRecord(kCapReset, nullptr, 0u, nullptr, 0u);
                          ResetUnlocked(); });
}

void GSCpuBackend::ResetUnlocked()
{
    m_execClut.fill(0u);
    ++m_immClutVersion;
    m_clutCbp.fill(0u);
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (g_captureRequested.load(std::memory_order_relaxed))
    {
        g_captureRequested.store(false, std::memory_order_relaxed);
        WithQuiescentVram([&]()
                          {
                              if (m_captureFile || !m_vram)
                                  return;
                              m_captureFile = std::fopen("out/gscapture.bin", "wb");
                              if (!m_captureFile)
                              {
                                  std::cout << "[gs] capture: cannot open out/gscapture.bin" << std::endl;
                                  return;
                              }
                              const char magic[8] = {'G', 'S', 'C', 'A', 'P', '0', '0', '1'};
                              const uint32_t header[8] = {
                                  1u,
                                  static_cast<uint32_t>(sizeof(GSPrimitiveBatch)),
                                  static_cast<uint32_t>(sizeof(GSTex0Reg)),
                                  static_cast<uint32_t>(sizeof(GSTexClutReg)),
                                  static_cast<uint32_t>(sizeof(GSTransferCommand)),
                                  static_cast<uint32_t>(sizeof(GSContext)),
                                  static_cast<uint32_t>(sizeof(GSVertex)),
                                  static_cast<uint32_t>(sizeof(GSDrawState))};
                              std::fwrite(magic, 1, sizeof(magic), m_captureFile);
                              std::fwrite(header, 1, sizeof(header), m_captureFile);
                              std::fwrite(m_vram, 1, kVramBytes, m_captureFile);
                              std::fwrite(m_execClut.data(), 1, sizeof(m_execClut), m_captureFile);
                              std::fwrite(m_clutCbp.data(), 1, sizeof(m_clutCbp), m_captureFile);
                              m_captureRecords = 0u;
                              std::cout << "[gs] capture: started" << std::endl; });
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    g_statPrims.fetch_add(1u, std::memory_order_relaxed);
    if (m_captureFile)
        CaptureRecord(kCapPrim, &batch, sizeof(batch), nullptr, 0u);

    PrimFootprint fp;
    if (!computeFootprint(batch, fp))
        return;

    if (!EngineEnabled())
    {
        ClutRef clut;
        clut.data = m_execClut.data();
        clut.generation = (m_instanceId << 48u) | (1ull << 47u) | (m_immClutVersion & 0x00007FFFFFFFFFFFull);
        DrawPrimitive(batch, clut, fp.y0, fp.y1);
        return;
    }

    Segment *segment = &OpenSegmentUnlocked();
    const int bufferWidth = static_cast<int>(std::max<uint32_t>(batch.state.context.frame.fbw, 1u) * 64u);
    bool selfOverlap = false;
    for (int i = 0; i < fp.targetCount && !selfOverlap; ++i)
        selfOverlap = !fp.reads.empty() && fp.reads.overlaps(fp.targets[i].range);
    if (fp.x1 >= bufferWidth || selfOverlap)
    {
        // Pixels past the buffer width wrap onto lower rows' memory, and a primitive sampling
        // its own target depends on drawing order; both break the band partition. Draw such a
        // primitive on its own, in one band, exactly like the immediate renderer.
        if (!segment->empty())
        {
            CloseForHazardUnlocked(lock);
            segment = &OpenSegmentUnlocked();
        }
        segment->addPrim(batch, fp);
        segment->serial = true;
        CloseOpenSegmentUnlocked();
        WaitForSpaceUnlocked(lock);
        return;
    }
    if (segment->primHazard(fp))
    {
        CloseForHazardUnlocked(lock);
        segment = &OpenSegmentUnlocked();
    }
    segment->addPrim(batch, fp);

    if (segment->prims.size() >= kMaxSegmentPrims)
    {
        CloseOpenSegmentUnlocked();
        WaitForSpaceUnlocked(lock);
    }
    else if (m_fifo.empty() && segment->prims.size() >= kKickPrims)
    {
        // The executor is idle: hand it work now instead of waiting for a full segment.
        CloseOpenSegmentUnlocked();
    }
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    g_statClutLoads.fetch_add(1u, std::memory_order_relaxed);
    if (m_captureFile)
        CaptureRecord(kCapClutLoad, &tex0, sizeof(tex0), &texclut, sizeof(texclut));

    if (!EngineEnabled())
    {
        LoadClutInto(m_execClut, tex0, texclut);
        ++m_immClutVersion;
        return;
    }

    RangeSet source;
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    if (tex0.csm == 0u)
    {
        addRectRange(source, tex0.cbp, 1u, tex0.cpsm, 0u, 0u, fourBit ? 7u : 15u, fourBit ? 1u : 15u);
    }
    else
    {
        const uint32_t x0 = static_cast<uint32_t>(texclut.cou) << 4u;
        const uint32_t y0 = texclut.cov;
        addRectRange(source, tex0.cbp, std::max<uint32_t>(texclut.cbw, 1u), tex0.cpsm,
                     x0, y0, x0 + (fourBit ? 15u : 255u), y0);
    }

    Segment *segment = &OpenSegmentUnlocked();
    if ((!segment->prims.empty() && segment->writes.overlaps(source)) || segment->clutLoads >= 0x00FFFF00u)
    {
        CloseForHazardUnlocked(lock);
        segment = &OpenSegmentUnlocked();
    }
    PreOp op;
    op.kind = PreOp::LoadClut;
    op.tex0 = tex0;
    op.texclut = texclut;
    segment->preops.push_back(op);
    ++segment->clutLoads;
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    auto setupState = [&]()
    {
        m_transfer = command;
        m_transferState.x = command.trxpos.dsax;
        m_transferState.y = command.trxpos.dsay;
        m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
        m_transferState.copiedPixels = 0u;
        m_transferState.direction = command.direction;
        m_transferState.localToHostPendingBytes = 0u;
    };

    if (command.direction == 1u)
    {
        // Local -> host needs the finished picture.
        WithQuiescentVram([&]()
                          {
                              setupState();
                              if (m_captureFile)
                                  CaptureRecord(kCapLocalToHost, &command, sizeof(command), nullptr, 0u);
                              PerformLocalToHostTransfer(); });
        return;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    setupState();
    if (command.direction != 2u)
        return;

    if (m_captureFile)
        CaptureRecord(kCapLocalCopy, &command, sizeof(command), nullptr, 0u);

    if (!m_vram)
    {
        m_transferState.direction = 3u;
        return;
    }

    if (!EngineEnabled())
    {
        ExecuteLocalCopy(command);
    }
    else
    {
        const uint32_t rrw = command.trxreg.rrw;
        const uint32_t rrh = command.trxreg.rrh;
        if (rrw != 0u && rrh != 0u)
        {
            RangeSet src;
            RangeSet dst;
            addRectRange(src, command.bitbltbuf.sbp, command.bitbltbuf.sbw, command.bitbltbuf.spsm,
                         command.trxpos.ssax, command.trxpos.ssay,
                         command.trxpos.ssax + rrw - 1u, command.trxpos.ssay + rrh - 1u);
            addRectRange(dst, command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.bitbltbuf.dpsm,
                         command.trxpos.dsax, command.trxpos.dsay,
                         command.trxpos.dsax + rrw - 1u, command.trxpos.dsay + rrh - 1u);
            Segment *segment = &OpenSegmentUnlocked();
            if (!segment->prims.empty() &&
                (segment->writes.overlaps(src) || segment->reads.overlaps(dst) || segment->writes.overlaps(dst)))
            {
                CloseForHazardUnlocked(lock);
                segment = &OpenSegmentUnlocked();
            }
            PreOp op;
            op.kind = PreOp::LocalCopy;
            op.transfer = command;
            segment->preops.push_back(op);
        }
    }
    m_transferState.copiedPixels = m_transferState.totalPixels;
    m_transferState.direction = 3u;
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t start = m_transferState.copiedPixels;
    const uint32_t remaining = m_transferState.totalPixels - std::min(start, m_transferState.totalPixels);
    const uint32_t pixels = std::min(remaining, uploadPixelsForBytes(dpsm, sizeBytes));
    if (pixels == 0u)
        return;
    const uint32_t bytesUsed = std::min(sizeBytes, uploadBytesForPixels(dpsm, pixels));

    PreOp op;
    op.kind = PreOp::Upload;
    op.transfer = m_transfer;
    op.startPixel = start;
    op.pixelCount = pixels;
    op.dataSize = bytesUsed;
    g_statUploads.fetch_add(1u, std::memory_order_relaxed);
    if (m_captureFile)
        CaptureRecord(kCapUpload, &op, sizeof(op), data, bytesUsed);

    if (!EngineEnabled())
    {
        ExecuteUpload(op, data);
    }
    else
    {
        const uint32_t firstRow = m_transfer.trxpos.dsay + start / rrw;
        const uint32_t lastRow = m_transfer.trxpos.dsay + (start + pixels - 1u) / rrw;
        RangeSet dst;
        addRectRange(dst, m_transfer.bitbltbuf.dbp, m_transfer.bitbltbuf.dbw, dpsm,
                     m_transfer.trxpos.dsax, firstRow, m_transfer.trxpos.dsax + rrw - 1u, lastRow);
        Segment *segment = &OpenSegmentUnlocked();
        if (!segment->prims.empty() && (segment->reads.overlaps(dst) || segment->writes.overlaps(dst)))
        {
            CloseForHazardUnlocked(lock);
            segment = &OpenSegmentUnlocked();
        }
        op.dataOffset = segment->uploadData.size();
        segment->uploadData.insert(segment->uploadData.end(), data, data + bytesUsed);
        segment->preops.push_back(op);
        if (segment->uploadData.size() >= kMaxSegmentUploadBytes)
        {
            CloseOpenSegmentUnlocked();
            WaitForSpaceUnlocked(lock);
        }
    }

    const uint32_t copied = start + pixels;
    m_transferState.copiedPixels = copied;
    if (copied >= m_transferState.totalPixels)
    {
        m_transferState.direction = 3u;
        m_transferState.totalPixels = 0u;
    }
    else
    {
        m_transferState.x = m_transfer.trxpos.dsax + (copied % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (copied / rrw);
    }
}

void GSCpuBackend::Flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Non-blocking: hand everything recorded so far to the executor.
    CloseOpenSegmentUnlocked();
}

void GSCpuBackend::TextureFlush()
{
    // No texture cache: texels are always read from local memory.
}

void GSCpuBackend::Sync(GSSyncReason reason)
{
    switch (reason)
    {
    case GSSyncReason::Finish:
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_captureFile)
        {
            CaptureRecord(kCapFinish, nullptr, 0u, nullptr, 0u);
            ++m_captureFinishes;
            if (m_captureFinishes >= 3u)
                CaptureFinishUnlocked("finish");
        }
        break;
    }
    case GSSyncReason::LocalToHost:
    case GSSyncReason::Reset:
        WithQuiescentVram([]() {});
        break;
    default:
        break;
    }
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t value = 0u;
    WithQuiescentVram([&]() { value = ReadVramUnlocked(psm, base, bw, x, y); });
    return value;
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    WithQuiescentVram([&]()
                      {
                          if (m_captureFile)
                          {
                              const uint32_t args[6] = {psm, base, bw, x, y, value};
                              CaptureRecord(kCapWriteVram, args, sizeof(args), nullptr, 0u);
                          }
                          WriteVramUnlocked(psm, base, bw, x, y, value); });
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    WithQuiescentVram([&]()
                      {
                          if (!m_vram || m_vramSize == 0u)
                          {
                              out.clear();
                              return;
                          }
                          out.resize(m_vramSize);
                          std::memcpy(out.data(), m_vram, m_vramSize); });
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    bool result = false;
    WithQuiescentVram([&]()
                      {
                          if (m_captureFile)
                              CaptureRecord(kCapClear, &context, sizeof(context), &rgba, sizeof(rgba));
                          result = ClearFramebufferUnlocked(context, rgba); });
    return result;
}

// ---------------------------------------------------------------------------------------------
// Execution helpers
// ---------------------------------------------------------------------------------------------
uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::LoadClutInto(std::array<uint16_t, 512> &clut, const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;
    const ReadVramFunc read = m_readVramFuncs[tex0.cpsm & 0x3Fu];

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = m_vram ? read(m_vram, tex0.cbp, sourceWidth, sourceX, sourceY) : 0u;
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
}

void GSCpuBackend::ExecuteUpload(const PreOp &op, const uint8_t *data)
{
    if (!m_vram || !data)
        return;
    const GSTransferCommand &t = op.transfer;
    const uint32_t dbp = t.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(t.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = t.bitbltbuf.dpsm;
    const uint32_t rrw = std::max<uint32_t>(t.trxreg.rrw, 1u);
    const uint32_t dsax = t.trxpos.dsax;
    const uint32_t dsay = t.trxpos.dsay;
    const WriteVramFunc write = m_writeVramFuncs[dpsm & 0x3Fu];

    uint32_t col = op.startPixel % rrw;
    uint32_t row = op.startPixel / rrw;
    auto advance = [&]()
    {
        if (++col >= rrw)
        {
            col = 0u;
            ++row;
        }
    };

    uint32_t offset = 0u;
    const uint32_t size = op.dataSize;
    uint32_t i = 0u;
    while (i < op.pixelCount)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (size - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            write(m_vram, dbp, dbw, dsax + col, dsay + row, value);
            offset += 4u;
            ++i;
            advance();
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (size - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            write(m_vram, dbp, dbw, dsax + col, dsay + row, value);
            offset += 3u;
            ++i;
            advance();
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (size - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            write(m_vram, dbp, dbw, dsax + col, dsay + row, value);
            offset += 2u;
            ++i;
            advance();
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
        {
            if (offset >= size)
                return;
            write(m_vram, dbp, dbw, dsax + col, dsay + row, data[offset++]);
            ++i;
            advance();
            break;
        }
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            if (offset >= size)
                return;
            const uint8_t packed = data[offset++];
            write(m_vram, dbp, dbw, dsax + col, dsay + row, packed & 0x0Fu);
            ++i;
            advance();
            if (i < op.pixelCount)
            {
                write(m_vram, dbp, dbw, dsax + col, dsay + row, (packed >> 4u) & 0x0Fu);
                ++i;
                advance();
            }
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::ExecuteLocalCopy(const GSTransferCommand &transfer)
{
    if (!m_vram)
        return;
    const uint32_t rrw = transfer.trxreg.rrw;
    const uint32_t rrh = transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
        return;
    const uint32_t sbw = std::max<uint32_t>(transfer.bitbltbuf.sbw, 1u);
    const uint32_t dbw = std::max<uint32_t>(transfer.bitbltbuf.dbw, 1u);
    const ReadVramFunc read = m_readVramFuncs[transfer.bitbltbuf.spsm & 0x3Fu];
    const WriteVramFunc write = m_writeVramFuncs[transfer.bitbltbuf.dpsm & 0x3Fu];

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = read(m_vram, transfer.bitbltbuf.sbp, sbw,
                                    x + transfer.trxpos.ssax, y + transfer.trxpos.ssay);
        write(m_vram, transfer.bitbltbuf.dbp, dbw, x + transfer.trxpos.dsax, y + transfer.trxpos.dsay, value);
    }
}

// ---------------------------------------------------------------------------------------------
// Capture / stats
// ---------------------------------------------------------------------------------------------
void GSCpuBackend::RequestCapture()
{
    g_captureRequested.store(true, std::memory_order_relaxed);
}

GSCpuBackend::Stats GSCpuBackend::ReadAndResetStats()
{
    Stats s;
    s.prims = g_statPrims.exchange(0u, std::memory_order_relaxed);
    s.segments = g_statSegments.exchange(0u, std::memory_order_relaxed);
    s.hazardCloses = g_statHazardCloses.exchange(0u, std::memory_order_relaxed);
    s.drains = g_statDrains.exchange(0u, std::memory_order_relaxed);
    s.drainWaitUs = g_statDrainWaitUs.exchange(0u, std::memory_order_relaxed);
    s.spaceWaitUs = g_statSpaceWaitUs.exchange(0u, std::memory_order_relaxed);
    s.uploads = g_statUploads.exchange(0u, std::memory_order_relaxed);
    s.clutLoads = g_statClutLoads.exchange(0u, std::memory_order_relaxed);
    s.workers = g_statWorkers.load(std::memory_order_relaxed);
    return s;
}

void GSCpuBackend::CaptureRecord(uint32_t kind, const void *a, uint32_t aSize, const void *b, uint32_t bSize)
{
    if (!m_captureFile)
        return;
    const uint32_t head[3] = {kind, aSize, bSize};
    std::fwrite(head, 1, sizeof(head), m_captureFile);
    if (aSize)
        std::fwrite(a, 1, aSize, m_captureFile);
    if (bSize)
        std::fwrite(b, 1, bSize, m_captureFile);
    ++m_captureRecords;
    if (std::ftell(m_captureFile) > 96L * 1024L * 1024L)
        CaptureFinishUnlocked("size limit");
}

void GSCpuBackend::CaptureFinishUnlocked(const char *reason)
{
    if (!m_captureFile)
        return;
    const uint32_t head[3] = {kCapEnd, 0u, 0u};
    std::fwrite(head, 1, sizeof(head), m_captureFile);
    const long size = std::ftell(m_captureFile);
    std::fclose(m_captureFile);
    m_captureFile = nullptr;
    m_captureFinishes = 0u;
    std::cout << "[gs] capture: written out/gscapture.bin records=" << m_captureRecords
              << " bytes=" << size << " (" << reason << ")" << std::endl;
}

extern "C" void ps2xRequestGsCapture()
{
    GSCpuBackend::RequestCapture();
}

extern "C" int ps2xGsStatsLine(char *buffer, int size)
{
    if (!buffer || size <= 0)
        return 0;
    const GSCpuBackend::Stats s = GSCpuBackend::ReadAndResetStats();
    return std::snprintf(buffer, static_cast<size_t>(size),
                         "gs: workers=%u prims=%llu segs=%llu hazard=%llu drains=%llu drainWait=%llums spaceWait=%llums uploads=%llu cluts=%llu",
                         s.workers,
                         static_cast<unsigned long long>(s.prims),
                         static_cast<unsigned long long>(s.segments),
                         static_cast<unsigned long long>(s.hazardCloses),
                         static_cast<unsigned long long>(s.drains),
                         static_cast<unsigned long long>(s.drainWaitUs / 1000u),
                         static_cast<unsigned long long>(s.spaceWaitUs / 1000u),
                         static_cast<unsigned long long>(s.uploads),
                         static_cast<unsigned long long>(s.clutLoads));
}
void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

bool GSCpuBackend::ClearFramebufferUnlocked(const GSContext &context, uint32_t rgba)
{
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
