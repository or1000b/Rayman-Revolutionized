#include "runtime/gs/ps2_gif_arbiter.h"
#include "pgs/gs_pgs_bridge.h"
#include <algorithm>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
    return flg == 2u || flg == 3u;
}

namespace
{
    // Packet buffers of drained packets, reused by the next submits on the same thread. Every VU1 XGKICK packet goes
    // through submit + drain: a new vector each time was ~5% of the graphics thread in malloc / free (2026-11-16).
    thread_local std::vector<std::vector<uint8_t>> t_freeBuffers;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    if (!t_freeBuffers.empty())
    {
        pkt.data.swap(t_freeBuffers.back());
        t_freeBuffers.pop_back();
    }
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            ps2x::pgs::setCurrentPath(static_cast<uint32_t>(pkt.pathId));
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            ps2x::pgs::setCurrentPath(3u);
        }
    }
    for (auto &pkt : m_queue)
    {
        // Small buffers only (a big image upload keeps its memory otherwise), and a bounded pool.
        if (t_freeBuffers.size() < 64u && pkt.data.capacity() <= (256u << 10))
        {
            pkt.data.clear();
            t_freeBuffers.push_back(std::move(pkt.data));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
