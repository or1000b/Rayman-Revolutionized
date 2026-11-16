// The HUD layer: see gs_hud.h.
#include "gs_hud.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace ps2x::hud
{
    namespace
    {
        constexpr uint32_t kHudZ = 0xFFFFu;
        constexpr uint32_t kNoFbp = 0xFFFFFFFFu;

        // ------------------------------------------------------------ GS state seen in the stream (GS thread)
        uint64_t g_regs[256] = {};
        uint32_t g_prim = 0;
        float g_s = 0.0f, g_t = 0.0f, g_q = 1.0f;
        uint32_t g_u = 0, g_v = 0;
        uint8_t g_rgba[4] = {0x80, 0x80, 0x80, 0x80};

        // Host -> local uploads at (0, 0) of PSMT8 / PSMT4 / PSMCT32 data, by destination block (DBP): how the game's
        // texture cache loads textures and CLUTs. The texels are in raster order, as the transfer carries them.
        struct Upload
        {
            uint32_t psm = 0, dbw = 0, w = 0, h = 0, id = 0;
            std::vector<uint8_t> bytes;
        };
        std::unordered_map<uint32_t, Upload> g_uploads;
        size_t g_uploadBytes = 0;
        uint32_t g_uploadIds = 0;
        struct Transfer
        {
            bool active = false;
            uint32_t dbp = 0, dbw = 0, psm = 0, w = 0, h = 0;
            size_t need = 0;
            std::vector<uint8_t> bytes;
        };
        Transfer g_xfer;

        // The CLUT buffer (RGBA8, alpha scaled from the GS's 0..0x80 to 0..255), loaded by TEX0's CLD.
        uint32_t g_clut[256] = {};
        bool g_clutValid = false;
        uint32_t g_clutId = 0;
        // The last CLUT load decoded (tex0Written). Busy scenes reload the same CLUT ~136,000 times a second; rebuilding
        // the table each time was ~11% of the GS thread, and the new id made the decoded-texture memo miss (2026-11-16).
        struct ClutLoad
        {
            uint32_t uploadId = 0, cbp = 0, psm = 0, csa = 0;
        } g_lastClut;

        // Decode inputs -> texture key, and the decoded textures (read by the window thread).
        std::unordered_map<uint64_t, uint64_t> g_texMemo;
        std::mutex g_texMutex;
        std::unordered_map<uint64_t, Texture> g_textures;

        // The HUD drawn into each frame buffer (FRAME.FBP) in its current frame, and the buffers seen on screen.
        std::unordered_map<uint32_t, Frame> g_targets;
        std::unordered_set<uint32_t> g_displayFbps;
        uint32_t g_lastDrawFbp = kNoFbp;

        std::mutex g_pubMutex;
        Frame g_published;

        uint64_t g_packets = 0, g_frames = 0, g_undecodable = 0, g_lateDraws = 0, g_held = 0;
        bool g_markDrawOnly = false; // the last markFrame (GS thread): the drawing since then belongs to that frame

        // The vertex queue of the primitive being built (GS thread).
        struct QVertex
        {
            float x, y, u, v;
            uint8_t r, g, b, a;
        };
        QVertex g_queue[3];
        uint32_t g_queueCount = 0; // vertices since the last PRIM
        QVertex g_fanFirst{};

        uint8_t clamp255(uint32_t v)
        {
            return static_cast<uint8_t>(std::min<uint32_t>(255u, v));
        }

        uint32_t psmBits(uint32_t psm)
        {
            switch (psm)
            {
            case 0x00: case 0x30: return 32u;
            case 0x01: case 0x31: return 24u;
            case 0x02: case 0x0A: case 0x32: case 0x3A: return 16u;
            case 0x13: case 0x1B: return 8u;
            case 0x14: case 0x24: case 0x2C: return 4u;
            default: return 0u;
            }
        }

        float floatBits(uint32_t bits)
        {
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }

        uint32_t contextOf(uint32_t prim)
        {
            return (prim >> 9) & 1u;
        }

        uint32_t currentFbp()
        {
            return static_cast<uint32_t>(g_regs[0x4C + contextOf(g_prim)] & 0x1FFu);
        }

        // FRAME.FBMSK all ones: the draw writes no colour (the game clears the Z buffer that way between its 2D
        // elements, with a full-screen sprite at Z = 0): it does not count as a visible draw.
        bool colourWritten()
        {
            return (g_regs[0x4C + contextOf(g_prim)] >> 32) != 0xFFFFFFFFull;
        }

        uint64_t fnv(uint64_t h, const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < size; ++i)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }

        // ------------------------------------------------------------ uploads and the CLUT

        void startTransfer()
        {
            const uint64_t bb = g_regs[0x50], pos = g_regs[0x51], reg = g_regs[0x52];
            const uint32_t dbp = static_cast<uint32_t>((bb >> 32) & 0x3FFFu);
            const uint32_t dbw = static_cast<uint32_t>((bb >> 48) & 0x3Fu);
            const uint32_t psm = static_cast<uint32_t>((bb >> 56) & 0x3Fu);
            const uint32_t dsax = static_cast<uint32_t>((pos >> 32) & 0x7FFu), dsay = static_cast<uint32_t>((pos >> 48) & 0x7FFu);
            const uint32_t w = static_cast<uint32_t>(reg & 0xFFFu), h = static_cast<uint32_t>((reg >> 32) & 0xFFFu);
            g_xfer.active = false;
            g_xfer.bytes.clear();
            if (dsax != 0u || dsay != 0u || !(psm == 0x00u || psm == 0x13u || psm == 0x14u) || w == 0u || h == 0u)
                return;
            const size_t need = static_cast<size_t>(w) * h * psmBits(psm) / 8u;
            if (need == 0u || need > (4u << 20))
                return;
            g_xfer.active = true;
            g_xfer.dbp = dbp;
            g_xfer.dbw = dbw;
            g_xfer.psm = psm;
            g_xfer.w = w;
            g_xfer.h = h;
            g_xfer.need = need;
            g_xfer.bytes.reserve(need);
        }

        void imageData(const uint8_t *data, size_t bytes)
        {
            if (!g_xfer.active)
                return;
            const size_t take = std::min(bytes, g_xfer.need - g_xfer.bytes.size());
            g_xfer.bytes.insert(g_xfer.bytes.end(), data, data + take);
            if (g_xfer.bytes.size() < g_xfer.need)
                return;
            if (g_uploadBytes > (48u << 20))
            {
                // Bounded: forget everything (textures in use get uploaded again by the game's cache or stay with
                // paraLLEl-GS until then).
                g_uploads.clear();
                g_uploadBytes = 0;
            }
            Upload &u = g_uploads[g_xfer.dbp];
            g_uploadBytes -= u.bytes.size();
            u.psm = g_xfer.psm;
            u.dbw = g_xfer.dbw;
            u.w = g_xfer.w;
            u.h = g_xfer.h;
            u.id = ++g_uploadIds;
            u.bytes.swap(g_xfer.bytes);
            g_uploadBytes += u.bytes.size();
            g_xfer.active = false;
            g_xfer.bytes.clear();
        }

        uint32_t clutEntry(const Upload &u, uint32_t x, uint32_t y)
        {
            uint32_t c = 0;
            std::memcpy(&c, u.bytes.data() + (static_cast<size_t>(y) * u.w + x) * 4u, 4u);
            const uint32_t a = std::min<uint32_t>(255u, (c >> 24) * 2u);
            return (c & 0x00FFFFFFu) | (a << 24);
        }

        void tex0Written(uint64_t v)
        {
            const uint32_t cld = static_cast<uint32_t>((v >> 61) & 7u);
            if (cld == 0u)
                return;
            const uint32_t psm = static_cast<uint32_t>((v >> 20) & 0x3Fu);
            const uint32_t cbp = static_cast<uint32_t>((v >> 37) & 0x3FFFu);
            const uint32_t cpsm = static_cast<uint32_t>((v >> 51) & 0xFu);
            const uint32_t csm = static_cast<uint32_t>((v >> 55) & 1u);
            const uint32_t csa = static_cast<uint32_t>((v >> 56) & 0x1Fu);
            if (psm != 0x13u && psm != 0x14u)
                return; // not an indexed texture: the CLUT is not used
            const auto it = g_uploads.find(cbp);
            const bool decodable = it != g_uploads.end() && cpsm == 0u && csm == 0u && it->second.psm == 0x00u;
            const ClutLoad load{decodable ? it->second.id : 0u, cbp, psm, psm == 0x14u ? csa : 0u};
            if (decodable && g_clutValid && load.uploadId == g_lastClut.uploadId && load.cbp == g_lastClut.cbp &&
                load.psm == g_lastClut.psm && load.csa == g_lastClut.csa)
                return; // the same CLUT as the last load: the table and its id stay as they are
            ++g_clutId;
            g_clutValid = false;
            g_lastClut = ClutLoad{};
            if (!decodable)
                return;
            const Upload &u = it->second;
            if (psm == 0x13u)
            {
                if (u.w < 16u || u.h < 16u)
                    return;
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    // CSM1: bits 3 and 4 of the index are swapped in the 16 x 16 CLUT image.
                    const uint32_t pos = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
                    g_clut[i] = clutEntry(u, pos & 15u, pos >> 4);
                }
            }
            else
            {
                if (u.w < 8u || u.h < 2u)
                    return;
                for (uint32_t i = 0; i < 16u; ++i)
                    g_clut[(csa * 16u + i) & 255u] = clutEntry(u, i & 7u, i >> 3);
            }
            g_clutValid = true;
            g_lastClut = load;
        }

        // The current TEX0 as a decoded texture's key (0 = cannot decode it).
        uint64_t textureKey(uint64_t tex0)
        {
            const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
            const uint32_t tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
            const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
            const uint32_t tw = static_cast<uint32_t>((tex0 >> 26) & 0xFu), th = static_cast<uint32_t>((tex0 >> 30) & 0xFu);
            const bool tcc = ((tex0 >> 34) & 1u) != 0u;
            const uint32_t csa = static_cast<uint32_t>((tex0 >> 56) & 0x1Fu);
            const auto it = g_uploads.find(tbp);
            if (it == g_uploads.end() || tw > 10u || th > 10u)
                return 0u;
            const Upload &u = it->second;
            if (u.psm != psm || u.dbw != tbw)
                return 0u;
            const bool indexed = psm == 0x13u || psm == 0x14u;
            if (indexed && !g_clutValid)
                return 0u;
            uint64_t memoKey = 1469598103934665603ull;
            const uint64_t fields[4] = {tex0 & 0x1F00000FFFFFFFFFull, u.id, indexed ? g_clutId : 0u, csa};
            memoKey = fnv(memoKey, fields, sizeof(fields));
            const auto memo = g_texMemo.find(memoKey);
            if (memo != g_texMemo.end())
                return memo->second;

            Texture t;
            t.width = 1u << tw;
            t.height = 1u << th;
            t.rgba.assign(static_cast<size_t>(t.width) * t.height * 4u, 0u);
            const uint32_t w = std::min(t.width, u.w), h = std::min(t.height, u.h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const size_t index = static_cast<size_t>(y) * u.w + x;
                    uint32_t c = 0;
                    if (psm == 0x13u)
                        c = g_clut[u.bytes[index]];
                    else if (psm == 0x14u)
                    {
                        const uint8_t pair = u.bytes[index >> 1];
                        c = g_clut[(csa * 16u + ((index & 1u) ? (pair >> 4) : (pair & 15u))) & 255u];
                    }
                    else
                        c = clutEntry(u, x, y);
                    if (!tcc)
                        c |= 0xFF000000u;
                    std::memcpy(t.rgba.data() + (static_cast<size_t>(y) * t.width + x) * 4u, &c, 4u);
                }
            uint64_t key = fnv(1469598103934665603ull, &t.width, sizeof(t.width));
            key = fnv(key, &t.height, sizeof(t.height));
            key = fnv(key, t.rgba.data(), t.rgba.size()) | 1u;
            {
                std::lock_guard<std::mutex> lock(g_texMutex);
                if (g_textures.find(key) == g_textures.end())
                    g_textures.emplace(key, std::move(t));
            }
            if (g_texMemo.size() > 65536u)
                g_texMemo.clear();
            g_texMemo.emplace(memoKey, key);
            return key;
        }

        // ------------------------------------------------------------ registers

        void resetQueue()
        {
            g_queueCount = 0;
        }

        void writeReg(uint32_t reg, uint64_t v)
        {
            reg &= 0xFFu;
            g_regs[reg] = v;
            switch (reg)
            {
            case 0x00: // PRIM
                g_prim = static_cast<uint32_t>(v & 0x7FFu);
                resetQueue();
                break;
            case 0x01: // RGBAQ
                g_rgba[0] = static_cast<uint8_t>(v);
                g_rgba[1] = static_cast<uint8_t>(v >> 8);
                g_rgba[2] = static_cast<uint8_t>(v >> 16);
                g_rgba[3] = static_cast<uint8_t>(v >> 24);
                g_q = floatBits(static_cast<uint32_t>(v >> 32));
                break;
            case 0x02: // ST
                g_s = floatBits(static_cast<uint32_t>(v));
                g_t = floatBits(static_cast<uint32_t>(v >> 32));
                break;
            case 0x03: // UV
                g_u = static_cast<uint32_t>(v & 0x3FFFu);
                g_v = static_cast<uint32_t>((v >> 16) & 0x3FFFu);
                break;
            case 0x06: // TEX0_1
            case 0x07: // TEX0_2
                tex0Written(v);
                break;
            case 0x53: // TRXDIR
                if ((v & 3u) == 0u)
                    startTransfer();
                else
                    g_xfer.active = false;
                break;
            default:
                break;
            }
        }

        // ------------------------------------------------------------ frames

        bool isDisplay(uint32_t fbp)
        {
            return g_displayFbps.empty() || g_displayFbps.count(fbp) != 0u;
        }

        // A draw into frame buffer fbp. Switching from a displayed buffer to another starts a new frame there (its
        // old HUD is not on screen: the game draws into the back buffer). 3D drawn into a buffer that already has HUD
        // in this frame (not seen in Rayman: its 2D layer comes last) also starts over.
        Frame *g_lastFrame = nullptr; // g_targets[g_lastDrawFbp] (element pointers survive rehashing)

        void noteDraw(uint32_t fbp, bool hud)
        {
            if (fbp == g_lastDrawFbp && g_lastFrame)
            {
                // The common case: more drawing into the same buffer.
                if (!hud && !g_lastFrame->batches.empty())
                {
                    ++g_lateDraws;
                    g_lastFrame->batches.clear();
                }
                return;
            }
            Frame &f = g_targets[fbp];
            g_lastFrame = &f;
            if (fbp != g_lastDrawFbp)
            {
                const bool flip = g_lastDrawFbp == kNoFbp || isDisplay(g_lastDrawFbp);
                g_lastDrawFbp = fbp;
                if (flip)
                {
                    f.batches.clear();
                    f.drawOnly = g_markDrawOnly; // a new frame: it takes the mark sent before its drawing
                    return;
                }
            }
            if (!hud && !f.batches.empty())
            {
                ++g_lateDraws;
                f.batches.clear();
            }
        }

        // ------------------------------------------------------------ packets

        // Pass 1: does the packet draw, and only at Z = 0xFFFF (PACKED vertices or A+D)?
        bool isHudPacket(const uint8_t *data, size_t size)
        {
            size_t p = 0;
            uint32_t drawing = 0;
            while (p + 16u <= size)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, data + p, 8u);
                std::memcpy(&hi, data + p + 8u, 8u);
                p += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                const uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
                if (flg == 0u)
                {
                    for (uint32_t l = 0; l < nloop; ++l)
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            if (p + 16u > size)
                                return false;
                            const uint32_t desc = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                            uint64_t qlo, qhi;
                            std::memcpy(&qlo, data + p, 8u);
                            std::memcpy(&qhi, data + p + 8u, 8u);
                            p += 16u;
                            uint32_t z = 0;
                            bool kick = false;
                            if (desc == 0x4u || desc == 0x5u)
                            {
                                kick = ((qhi >> 47) & 1u) == 0u;
                                z = desc == 0x4u ? static_cast<uint32_t>((qhi >> 4) & 0xFFFFFFu) : static_cast<uint32_t>(qhi);
                            }
                            else if (desc == 0xEu && ((qhi & 0xFFu) == 0x04u || (qhi & 0xFFu) == 0x05u))
                            {
                                kick = true;
                                z = (qhi & 0xFFu) == 0x04u ? static_cast<uint32_t>((qlo >> 32) & 0xFFFFFFu) : static_cast<uint32_t>(qlo >> 32);
                            }
                            if (kick)
                            {
                                if (z != kHudZ)
                                    return false;
                                ++drawing;
                            }
                        }
                }
                else if (flg == 1u)
                {
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        const uint32_t desc = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                        if (nloop != 0u && (desc == 0x4u || desc == 0x5u))
                            return false; // REGLIST vertices: not taken (cannot be made non-drawing one by one)
                    }
                    p += ((static_cast<size_t>(nloop) * nreg + 1u) / 2u) * 16u;
                }
                else
                    p += static_cast<size_t>(nloop) * 16u;
            }
            return drawing != 0u;
        }

        // The colour a vertex gets in the window (raylib modulates texel * colour / 255).
        Vertex finishVertex(const QVertex &q, const QVertex &colour, bool tme, uint32_t tfx, bool abe)
        {
            Vertex v{q.x, q.y, q.u, q.v, 0, 0, 0, 0};
            if (!tme)
            {
                v.r = colour.r;
                v.g = colour.g;
                v.b = colour.b;
            }
            else if (tfx == 1u) // DECAL
            {
                v.r = v.g = v.b = 255u;
            }
            else // MODULATE (and the highlights, close enough for the 2D layer): texel * vertex / 0x80
            {
                v.r = clamp255(colour.r * 2u);
                v.g = clamp255(colour.g * 2u);
                v.b = clamp255(colour.b * 2u);
            }
            v.a = (abe && !(tme && tfx == 1u)) ? clamp255(colour.a * 2u) : 255u;
            return v;
        }

        struct PacketHud
        {
            std::vector<Batch> batches;
            std::vector<size_t> patchAdc; // offsets of PACKED XYZ qwords' high halves
            std::vector<size_t> patchAd;  // offsets of A+D XYZ qwords' high halves
            float x0 = 1e9f, x1 = -1e9f;
            bool ok = true;
            uint32_t fbp = kNoFbp;
        };

        void emitTriangle(PacketHud &out, const QVertex &a, const QVertex &b, const QVertex &c)
        {
            const uint32_t ctx = contextOf(g_prim);
            const bool tme = ((g_prim >> 4) & 1u) != 0u, abe = ((g_prim >> 6) & 1u) != 0u, iip = ((g_prim >> 3) & 1u) != 0u;
            const uint64_t tex0 = g_regs[0x06 + ctx];
            uint64_t key = 0;
            if (tme)
            {
                key = textureKey(tex0);
                if (key == 0u)
                {
                    out.ok = false;
                    return;
                }
            }
            Blend blend = BlendAlpha;
            if (abe)
            {
                const uint64_t alpha = g_regs[0x42 + ctx];
                const uint32_t sa = alpha & 3u, sb = (alpha >> 2) & 3u, sd = (alpha >> 6) & 3u;
                if (sa == 0u && sb == 2u && sd == 1u)
                    blend = BlendAdd; // Cs * As + Cd
            }
            const uint32_t tfx = static_cast<uint32_t>((tex0 >> 35) & 3u);
            if (out.batches.empty() || out.batches.back().texture != key || out.batches.back().blend != blend)
            {
                out.batches.emplace_back();
                out.batches.back().texture = key;
                out.batches.back().blend = blend;
            }
            std::vector<Vertex> &dst = out.batches.back().vertices;
            const QVertex *vs[3] = {&a, &b, &c};
            for (const QVertex *q : vs)
            {
                dst.push_back(finishVertex(*q, iip ? *q : c, tme, tfx, abe));
                out.x0 = std::min(out.x0, q->x);
                out.x1 = std::max(out.x1, q->x);
            }
        }

        // A vertex (XYZ written). kick: it draws (XYZ2 / XYZF2 with ADC = 0).
        void vertex(PacketHud *out, uint32_t x, uint32_t y, bool kick)
        {
            const uint32_t ctx = contextOf(g_prim);
            const uint64_t offset = g_regs[0x18 + ctx];
            QVertex q;
            q.x = (static_cast<float>(x) - static_cast<float>(offset & 0xFFFFu)) / 16.0f;
            q.y = (static_cast<float>(y) - static_cast<float>((offset >> 32) & 0xFFFFu)) / 16.0f;
            if (((g_prim >> 8) & 1u) != 0u) // FST: UV in texels (10.4)
            {
                const uint64_t tex0 = g_regs[0x06 + ctx];
                q.u = (static_cast<float>(g_u) / 16.0f) / static_cast<float>(1u << ((tex0 >> 26) & 0xFu));
                q.v = (static_cast<float>(g_v) / 16.0f) / static_cast<float>(1u << ((tex0 >> 30) & 0xFu));
            }
            else
            {
                const float qq = (g_q != 0.0f) ? g_q : 1.0f;
                q.u = g_s / qq;
                q.v = g_t / qq;
            }
            q.r = g_rgba[0];
            q.g = g_rgba[1];
            q.b = g_rgba[2];
            q.a = g_rgba[3];

            const uint32_t type = g_prim & 7u;
            if (kick && !out && colourWritten())
                noteDraw(currentFbp(), false);
            if (kick && out)
                out->fbp = currentFbp();
            switch (type)
            {
            case 3: // triangle list
                g_queue[g_queueCount % 3u] = q;
                if (++g_queueCount % 3u == 0u && kick && out)
                    emitTriangle(*out, g_queue[0], g_queue[1], g_queue[2]);
                break;
            case 4: // strip
                g_queue[0] = g_queue[1];
                g_queue[1] = g_queue[2];
                g_queue[2] = q;
                if (++g_queueCount >= 3u && kick && out)
                    emitTriangle(*out, g_queue[0], g_queue[1], g_queue[2]);
                break;
            case 5: // fan
                if (g_queueCount == 0u)
                    g_fanFirst = q;
                g_queue[1] = g_queue[2];
                g_queue[2] = q;
                if (++g_queueCount >= 3u && kick && out)
                    emitTriangle(*out, g_fanFirst, g_queue[1], g_queue[2]);
                break;
            case 6: // sprite: two corners
                g_queue[g_queueCount & 1u] = q;
                if ((++g_queueCount & 1u) == 0u && kick && out)
                {
                    const QVertex &a = g_queue[0], &c = q;
                    QVertex b = a, d = a;
                    b.x = c.x;
                    b.u = c.u;
                    d.y = c.y;
                    d.v = c.v;
                    b.r = d.r = c.r;
                    b.g = d.g = c.g;
                    b.b = d.b = c.b;
                    b.a = d.a = c.a;
                    QVertex a2 = a;
                    a2.r = c.r;
                    a2.g = c.g;
                    a2.b = c.b;
                    a2.a = c.a;
                    emitTriangle(*out, a2, b, c);
                    emitTriangle(*out, a2, c, d);
                }
                break;
            default: // points and lines: not in the 2D layer we take
                if (kick && out)
                    out->ok = false;
                break;
            }
        }
    }

    bool enabled()
    {
        static const bool on = []
        {
            const char *env = std::getenv("PS2X_HUD");
            if (env && *env)
                return env[0] != '0';
            const char *test = std::getenv("PS2X_TEST_MODE");
            return !(test && std::atoi(test) != 0);
        }();
        return on;
    }

    namespace
    {
    // One GIF packet (tags up to EOP) at data[0, size), base = its offset in the buffer: state, frame bookkeeping, and
    // when it is 2D the window can draw, its triangles and the offsets to make non-drawing (adc / ad).
    void processPacket(const uint8_t *data, size_t size, size_t base, std::vector<size_t> &adc, std::vector<size_t> &ad)
    {
        const bool candidate = isHudPacket(data, size);
        PacketHud hud;
        PacketHud *out = candidate ? &hud : nullptr;
        size_t p = 0;
        while (p + 16u <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + p, 8u);
            std::memcpy(&hi, data + p + 8u, 8u);
            p += 16u;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            const uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
            if (flg == 0u)
            {
                if ((lo >> 46) & 1u)
                    writeReg(0x00, (lo >> 47) & 0x7FFu);
                // A 3D tag without register writes: only its first drawing vertex matters (the frame bookkeeping).
                bool needWalk = candidate;
                bool draws = false;
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint32_t desc = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                    needWalk = needWalk || desc == 0x0u || desc == 0x6u || desc == 0x7u || desc == 0xEu;
                    draws = draws || desc == 0x4u || desc == 0x5u;
                }
                if (!needWalk)
                {
                    if (draws && nloop != 0u && colourWritten())
                        noteDraw(currentFbp(), false);
                    p += static_cast<size_t>(nloop) * nreg * 16u;
                    continue;
                }
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        if (p + 16u > size)
                            break;
                        const uint32_t desc = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                        uint64_t qlo, qhi;
                        std::memcpy(&qlo, data + p, 8u);
                        std::memcpy(&qhi, data + p + 8u, 8u);
                        switch (desc)
                        {
                        case 0x0:
                            writeReg(0x00, qlo);
                            break;
                        case 0x1: // RGBAQ (Q from the last ST)
                            g_rgba[0] = static_cast<uint8_t>(qlo);
                            g_rgba[1] = static_cast<uint8_t>(qlo >> 32);
                            g_rgba[2] = static_cast<uint8_t>(qhi);
                            g_rgba[3] = static_cast<uint8_t>(qhi >> 32);
                            break;
                        case 0x2: // ST, with Q
                            g_s = floatBits(static_cast<uint32_t>(qlo));
                            g_t = floatBits(static_cast<uint32_t>(qlo >> 32));
                            g_q = floatBits(static_cast<uint32_t>(qhi));
                            break;
                        case 0x3:
                            g_u = static_cast<uint32_t>(qlo & 0x3FFFu);
                            g_v = static_cast<uint32_t>((qlo >> 32) & 0x3FFFu);
                            break;
                        case 0x4: // XYZF2
                        case 0x5: // XYZ2
                        {
                            const bool kick = ((qhi >> 47) & 1u) == 0u;
                            if (kick && out)
                                hud.patchAdc.push_back(base + p + 8u);
                            vertex(out, static_cast<uint32_t>(qlo & 0xFFFFu), static_cast<uint32_t>((qlo >> 32) & 0xFFFFu), kick);
                            break;
                        }
                        case 0x6:
                        case 0x7:
                            writeReg(desc, qlo);
                            break;
                        case 0xC:
                        case 0xD:
                            vertex(out, static_cast<uint32_t>(qlo & 0xFFFFu), static_cast<uint32_t>((qlo >> 32) & 0xFFFFu), false);
                            break;
                        case 0xE: // A+D
                        {
                            const uint32_t reg = static_cast<uint32_t>(qhi & 0xFFu);
                            if (reg == 0x04u || reg == 0x05u || reg == 0x0Cu || reg == 0x0Du)
                            {
                                const bool kick = reg == 0x04u || reg == 0x05u;
                                if (kick && out)
                                    hud.patchAd.push_back(base + p + 8u);
                                vertex(out, static_cast<uint32_t>(qlo & 0xFFFFu), static_cast<uint32_t>((qlo >> 16) & 0xFFFFu), kick);
                            }
                            else
                                writeReg(reg, qlo);
                            break;
                        }
                        default:
                            break;
                        }
                        p += 16u;
                    }
            }
            else if (flg == 1u)
            {
                const size_t total = static_cast<size_t>(nloop) * nreg;
                for (size_t i = 0; i < total && p + 8u <= size; ++i, p += 8u)
                {
                    const uint32_t desc = static_cast<uint32_t>((hi >> (4u * (i % nreg))) & 0xFu);
                    uint64_t v;
                    std::memcpy(&v, data + p, 8u);
                    switch (desc)
                    {
                    case 0x0: writeReg(0x00, v); break;
                    case 0x1: writeReg(0x01, v); break;
                    case 0x2: writeReg(0x02, v); break;
                    case 0x3: writeReg(0x03, v); break;
                    case 0x4:
                    case 0x5:
                        vertex(nullptr, static_cast<uint32_t>(v & 0xFFFFu), static_cast<uint32_t>((v >> 16) & 0xFFFFu), true);
                        break;
                    case 0x6:
                    case 0x7:
                        writeReg(desc, v);
                        break;
                    case 0xC:
                    case 0xD:
                        vertex(nullptr, static_cast<uint32_t>(v & 0xFFFFu), static_cast<uint32_t>((v >> 16) & 0xFFFFu), false);
                        break;
                    default:
                        break;
                    }
                }
                if (total & 1u)
                    p += 8u;
            }
            else
            {
                const size_t bytes = std::min(static_cast<size_t>(nloop) * 16u, size - p);
                imageData(data + p, bytes);
                p += static_cast<size_t>(nloop) * 16u;
            }
        }
        if (!candidate)
            return;
        if (!hud.ok || hud.batches.empty() || hud.fbp == kNoFbp)
        {
            // 2D the window cannot draw (a texture not decoded): it stays with paraLLEl-GS.
            ++g_undecodable;
            if (hud.fbp != kNoFbp)
                noteDraw(hud.fbp, true);
            return;
        }
        adc.insert(adc.end(), hud.patchAdc.begin(), hud.patchAdc.end());
        ad.insert(ad.end(), hud.patchAd.begin(), hud.patchAd.end());
        const bool fullWidth = hud.x0 <= 1.0f && hud.x1 >= 511.0f;
        noteDraw(hud.fbp, true);
        Frame &frame = g_targets[hud.fbp];
        frame.drawOnly = g_markDrawOnly; // the 2D of the frame marked last (markFrame)
        for (Batch &b : hud.batches)
        {
            b.fullWidth = fullWidth;
            if (!frame.batches.empty() && frame.batches.back().texture == b.texture && frame.batches.back().blend == b.blend &&
                frame.batches.back().fullWidth == b.fullWidth)
                frame.batches.back().vertices.insert(frame.batches.back().vertices.end(), b.vertices.begin(), b.vertices.end());
            else
                frame.batches.push_back(std::move(b));
        }
        ++g_packets;
    }

    // The end of the GIF packet starting at start (after its EOP tag's data, or the buffer's end).
    size_t packetEnd(const uint8_t *data, size_t size, size_t start)
    {
        size_t p = start;
        while (p + 16u <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + p, 8u);
            std::memcpy(&hi, data + p + 8u, 8u);
            p += 16u;
            const size_t nloop = static_cast<size_t>(lo & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            const size_t nreg = static_cast<size_t>((lo >> 60) & 0xFu) ? static_cast<size_t>((lo >> 60) & 0xFu) : 16u;
            p += flg == 0u ? nloop * nreg * 16u : flg == 1u ? ((nloop * nreg + 1u) / 2u) * 16u : nloop * 16u;
            if ((lo >> 15) & 1u)
                break;
        }
        return std::min(p, size);
    }
    }

    const uint8_t *filterPacket(const uint8_t *data, size_t size, std::vector<uint8_t> &scratch)
    {
        static thread_local std::vector<size_t> adc, ad;
        adc.clear();
        ad.clear();
        for (size_t start = 0; start + 16u <= size;)
        {
            const size_t end = packetEnd(data, size, start);
            processPacket(data + start, end - start, start, adc, ad);
            start = end;
        }
        if (adc.empty() && ad.empty())
            return data;
        scratch.assign(data, data + size);
        for (const size_t at : adc)
            scratch[at + 5u] |= 0x80u; // ADC (bit 111): the vertex no longer draws
        for (const size_t at : ad)
            scratch[at] = static_cast<uint8_t>(scratch[at] | 0x08u); // XYZ2 / XYZF2 -> XYZ3 / XYZF3
        return scratch.data();
    }

    void registerWrite(uint32_t reg, uint64_t value)
    {
        reg &= 0xFFu;
        if (reg == 0x04u || reg == 0x05u)
            vertex(nullptr, static_cast<uint32_t>(value & 0xFFFFu), static_cast<uint32_t>((value >> 16) & 0xFFFFu), true);
        else if (reg == 0x0Cu || reg == 0x0Du)
            vertex(nullptr, static_cast<uint32_t>(value & 0xFFFFu), static_cast<uint32_t>((value >> 16) & 0xFFFFu), false);
        else
            writeReg(reg, value);
    }

    void publish(uint64_t pmode, uint64_t dispfb1, uint64_t dispfb2)
    {
        const uint32_t fbp = static_cast<uint32_t>(((pmode & 1u) ? dispfb1 : dispfb2) & 0x1FFu);
        if (g_displayFbps.size() > 8u)
            g_displayFbps.clear();
        g_displayFbps.insert(fbp);
        const auto it = g_targets.find(fbp);
        Frame copy = it != g_targets.end() ? it->second : Frame{};
        // PS2X_HUD_LOG=<n>: the first n published frames with 2D, batch by batch (texture, blend, triangles, first vertex).
        static int s_logLeft = []
        {
            const char *env = std::getenv("PS2X_HUD_LOG");
            return env ? std::atoi(env) : 0;
        }();
        if (s_logLeft > 0 && !copy.batches.empty())
        {
            --s_logLeft;
            std::fprintf(stderr, "[hud] publish fbp %u: %zu batches\n", fbp, copy.batches.size());
            for (const Batch &b : copy.batches)
            {
                const Vertex &v = b.vertices.front();
                std::fprintf(stderr, "[hud]   tex %016llx blend %d full %d tris %zu  v0 (%.1f %.1f) uv (%.3f %.3f) rgba %u %u %u %u\n",
                             static_cast<unsigned long long>(b.texture), b.blend, b.fullWidth ? 1 : 0, b.vertices.size() / 3u, v.x, v.y,
                             v.u, v.v, v.r, v.g, v.b, v.a);
            }
        }
        if (copy.drawOnly) // no game logic in that frame: keep the last logic frame's 2D (markFrame)
        {
            ++g_held;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pubMutex);
        g_published = std::move(copy);
        ++g_frames;
    }

    void markFrame(bool drawOnly)
    {
        g_markDrawOnly = drawOnly;
    }

    void latest(Frame &out)
    {
        std::lock_guard<std::mutex> lock(g_pubMutex);
        out = g_published;
    }

    bool texture(uint64_t key, Texture &out)
    {
        std::lock_guard<std::mutex> lock(g_texMutex);
        const auto it = g_textures.find(key);
        if (it == g_textures.end())
            return false;
        out = it->second;
        return true;
    }

    std::string statsLine()
    {
        if (!enabled())
            return {};
        size_t textures = 0;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            textures = g_textures.size();
        }
        char line[192];
        std::snprintf(line, sizeof(line), " | hud packets=%llu frames=%llu textures=%zu kept=%llu late=%llu held=%llu",
                      static_cast<unsigned long long>(g_packets), static_cast<unsigned long long>(g_frames), textures,
                      static_cast<unsigned long long>(g_undecodable), static_cast<unsigned long long>(g_lateDraws),
                      static_cast<unsigned long long>(g_held));
        return line;
    }
}
