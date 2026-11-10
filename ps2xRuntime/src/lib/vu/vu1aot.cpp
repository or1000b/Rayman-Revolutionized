// VU1 ahead-of-time translation: value helpers (copies of the interpreter's cases). See vu1aot.h / docs/vu1_aot.md.
#include "vu1aot.h"

#include <algorithm>

namespace vu1aot
{
    namespace
    {
        int32_t vuFloatToInt(float value, float scale) // ps2_vu1_upper.cpp
        {
            const double scaled = static_cast<double>(value) * static_cast<double>(scale);
            if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
                return std::numeric_limits<int32_t>::max();
            if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
                return std::numeric_limits<int32_t>::min();
            return static_cast<int32_t>(scaled);
        }

        float vuEatan(float value) // ps2_vu1_lower.cpp
        {
            constexpr float coefficients[] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.13085337519646f,
                                              0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f};
            constexpr float quarterPi = 0.785398185253143f;
            const float squared = value * value;
            float polynomial = coefficients[7];
            for (int index = 6; index >= 0; --index)
                polynomial = coefficients[index] + squared * polynomial;
            return quarterPi + value * polynomial;
        }

        float vuEsin(float value)
        {
            constexpr float coefficients[] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
            const float squared = value * value;
            float polynomial = coefficients[4];
            for (int index = 3; index >= 0; --index)
                polynomial = coefficients[index] + squared * polynomial;
            return value * polynomial;
        }

        float vuEexp(float value)
        {
            constexpr float coefficients[] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                                              0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
            float polynomial = coefficients[5];
            for (int index = 4; index >= 0; --index)
                polynomial = coefficients[index] + value * polynomial;
            polynomial = 1.0f + value * polynomial;
            polynomial *= polynomial;
            polynomial *= polynomial;
            return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
        }
    }

    float normalizeResult(float value, uint32_t &laneFlags) // VU1Interpreter::normalizeResult
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = bits & 0x80000000u;
        const uint32_t magnitude = bits & 0x7FFFFFFFu;
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        laneFlags = sign != 0u ? 0x2u : 0u;
        if (magnitude == 0u)
            laneFlags |= 0x1u;
        else if (exponent == 0u)
        {
            laneFlags |= 0x5u;
            bits = sign;
        }
        else if (exponent == 0xFFu)
        {
            laneFlags |= 0x8u;
            bits = sign | 0x7F7FFFFFu;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    uint8_t normalizeExact(float &value, long double exactResult) // VU1Interpreter::normalizeFmacExactResult
    {
        const bool negative = std::signbit(exactResult);
        const long double magnitude = std::fabs(exactResult);
        const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
        const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
        uint8_t flags = negative ? 0x2u : 0u;
        uint32_t bits = negative ? 0x80000000u : 0u;
        if (magnitude == 0.0L)
        {
            flags |= 0x1u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude > maximum)
        {
            flags |= 0x8u;
            bits |= 0x7F7FFFFFu;
            std::memcpy(&value, &bits, sizeof(value));
        }
        else if (magnitude < minimum)
        {
            flags |= 0x5u;
            std::memcpy(&value, &bits, sizeof(value));
        }
        return flags;
    }

    void fmacFinish(Ctx &c, uint32_t upper, float *result, float *dst, uint8_t dest, Kind kind, YSource ySource,
                    FlagResult &flagsOut)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const bool special = op >= 0x3Cu;
        const uint8_t cls = special ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
        const uint8_t fs = FS(upper);
        const uint8_t ft = FT(upper);
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const bool productSum = kind == KindMAdd || kind == KindMSub || kind == KindOpMsub;

        // calculateFmacProductSticky (OPMSUB counts as a product-sum there; OPMULA does not).
        uint32_t productSticky = 0u;
        if (productSum)
        {
            const float maximum = std::numeric_limits<float>::max();
            const float minimum = std::numeric_limits<float>::min();
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((dest & laneBit(component)) == 0u)
                    continue;
                const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
                const float left = vuNormalizeOperand(c.s.vf[fs][leftComponent]);
                float right;
                switch (ySource)
                {
                case YBc: right = vuNormalizeOperand(c.s.vf[ft][cls & 3u]); break;
                case YQ: right = vuNormalizeOperand(c.s.q); break;
                case YI: right = vuNormalizeOperand(c.s.i); break;
                case YCross: right = vuNormalizeOperand(c.s.vf[ft][crossRight[component]]); break;
                default: right = vuNormalizeOperand(c.s.vf[ft][component]); break;
                }
                const double exactProduct = static_cast<double>(left) * static_cast<double>(right);
                const double magnitude = std::fabs(exactProduct);
                uint32_t f = std::signbit(exactProduct) ? 0x2u : 0u;
                if (magnitude == 0.0)
                    f |= 0x1u;
                else if (magnitude > static_cast<double>(maximum))
                    f |= 0x8u;
                else if (magnitude < static_cast<double>(minimum))
                    f |= 0x5u;
                productSticky |= f & 0xFu;
            }
        }
        const bool productOverflow = (productSticky & 0x8u) != 0u;
        const bool productSumFast = kind == KindMAdd || kind == KindMSub;

        // normalizeFmacResult.
        uint8_t laneFlags[4]{};
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((dest & laneBit(component)) == 0u)
                continue;
            if (kind != KindOpMsub && kind != KindOpMula)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &result[component], sizeof(bits));
                const uint32_t exponent = (bits >> 23) & 0xFFu;
                bool fast = exponent >= 1u && exponent <= 253u;
                if (fast && productSumFast)
                    fast = !productOverflow && exponent >= 2u &&
                           exponent + 20u >= vuFloatExponent(vuNormalizeOperand(c.s.acc[component]));
                if (fast)
                {
                    laneFlags[component] = (bits & 0x80000000u) != 0u ? 0x2u : 0u;
                    continue;
                }
            }
            if (kind == KindNone)
            {
                uint32_t f = 0u;
                result[component] = normalizeResult(result[component], f);
                laneFlags[component] = static_cast<uint8_t>(f);
                continue;
            }
            const bool cross = ySource == YCross;
            const long double x = static_cast<long double>(vuNormalizeOperand(c.s.vf[fs][cross ? crossLeft[component] : component]));
            long double y = 0.0L;
            switch (ySource)
            {
            case YBc: y = static_cast<long double>(vuNormalizeOperand(c.s.vf[ft][cls & 3u])); break;
            case YQ: y = static_cast<long double>(vuNormalizeOperand(c.s.q)); break;
            case YI: y = static_cast<long double>(vuNormalizeOperand(c.s.i)); break;
            case YVec: y = static_cast<long double>(vuNormalizeOperand(c.s.vf[ft][component])); break;
            case YCross: y = static_cast<long double>(vuNormalizeOperand(c.s.vf[ft][crossRight[component]])); break;
            }
            long double exactResult = 0.0L;
            switch (kind)
            {
            case KindAdd: exactResult = x + y; break;
            case KindSub: exactResult = x - y; break;
            case KindMul: exactResult = x * y; break;
            case KindMAdd: exactResult = static_cast<long double>(vuNormalizeOperand(c.s.acc[component])) + x * y; break;
            case KindMSub: exactResult = static_cast<long double>(vuNormalizeOperand(c.s.acc[component])) - x * y; break;
            case KindOpMsub:
                exactResult = component == 3u ? 0.0L : static_cast<long double>(vuNormalizeOperand(c.s.acc[component])) - x * y;
                break;
            case KindOpMula: exactResult = component == 3u ? 0.0L : x * y; break;
            default: break;
            }
            laneFlags[component] = normalizeExact(result[component], exactResult);
        }

        // updateFmacFlags (the values; the generated code queues them).
        flagsOut = {};
        if (dest != 0u && c.flagsLive)
        {
            uint32_t mac = 0u, status = 0u;
            for (uint32_t component = 0; component < 4u; ++component)
            {
                const uint8_t lane = laneBit(component);
                if ((dest & lane) == 0u)
                    continue;
                const uint32_t f = laneFlags[component];
                if (f & 0x1u)
                    mac |= lane;
                if (f & 0x2u)
                    mac |= static_cast<uint32_t>(lane) << 4;
                if (f & 0x4u)
                    mac |= static_cast<uint32_t>(lane) << 8;
                if (f & 0x8u)
                    mac |= static_cast<uint32_t>(lane) << 12;
                status |= f;
            }
            flagsOut.queued = true;
            flagsOut.mac = mac;
            flagsOut.status = status;
            flagsOut.extraSticky = productSticky;
        }
        applyDest(dst, result, dest);
    }

    void upper(Ctx &c, uint32_t instr, FlagResult &flags, bool &clipQueued, uint32_t &clip)
    {
        flags = {};
        clipQueued = false;
        const uint8_t dest = DEST(instr);
        const uint8_t ft = FT(instr);
        const uint8_t fs = FS(instr);
        const uint8_t fd = FD(instr);
        const uint8_t op = instr & 0x3F;
        float *vd = c.s.vf[fd];
        float vs[4], vt[4], acc[4];
        for (uint32_t component = 0; component < 4u; ++component)
        {
            vs[component] = vuNormalizeOperand(c.s.vf[fs][component]);
            vt[component] = vuNormalizeOperand(c.s.vf[ft][component]);
            acc[component] = vuNormalizeOperand(c.s.acc[component]);
        }
        const float q = vuNormalizeOperand(c.s.q);
        const float i = vuNormalizeOperand(c.s.i);
        float result[4];
        auto toVd = [&](Kind kind, YSource y) { fmacFinish(c, instr, result, vd, dest, kind, y, flags); };
        auto toAcc = [&](Kind kind, YSource y) { fmacFinish(c, instr, result, c.s.acc, dest, kind, y, flags); };

        if (op <= 0x1Bu || (op >= 0x1Cu && op <= 0x2Fu))
        {
            switch (op)
            {
            case 0x00: case 0x01: case 0x02: case 0x03: { const float bc = vt[op & 3]; for (int k = 0; k < 4; k++) result[k] = vs[k] + bc; toVd(KindAdd, YBc); return; }
            case 0x04: case 0x05: case 0x06: case 0x07: { const float bc = vt[op & 3]; for (int k = 0; k < 4; k++) result[k] = vs[k] - bc; toVd(KindSub, YBc); return; }
            case 0x08: case 0x09: case 0x0A: case 0x0B: { const float bc = vt[op & 3]; for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * bc; toVd(KindMAdd, YBc); return; }
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: { const float bc = vt[op & 3]; for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * bc; toVd(KindMSub, YBc); return; }
            case 0x10: case 0x11: case 0x12: case 0x13: { const float bc = vt[op & 3]; for (int k = 0; k < 4; k++) result[k] = (vs[k] > bc) ? vs[k] : bc; applyDest(vd, result, dest); return; }
            case 0x14: case 0x15: case 0x16: case 0x17: { const float bc = vt[op & 3]; for (int k = 0; k < 4; k++) result[k] = (vs[k] < bc) ? vs[k] : bc; applyDest(vd, result, dest); return; }
            case 0x18: case 0x19: case 0x1A: case 0x1B: { const float bc = vt[op & 3]; for (int k = 0; k < 4; k++) result[k] = vs[k] * bc; toVd(KindMul, YBc); return; }
            case 0x1C: for (int k = 0; k < 4; k++) result[k] = vs[k] * q; toVd(KindMul, YQ); return;
            case 0x1D: for (int k = 0; k < 4; k++) result[k] = (vs[k] > i) ? vs[k] : i; applyDest(vd, result, dest); return;
            case 0x1E: for (int k = 0; k < 4; k++) result[k] = vs[k] * i; toVd(KindMul, YI); return;
            case 0x1F: for (int k = 0; k < 4; k++) result[k] = (vs[k] < i) ? vs[k] : i; applyDest(vd, result, dest); return;
            case 0x20: for (int k = 0; k < 4; k++) result[k] = vs[k] + q; toVd(KindAdd, YQ); return;
            case 0x21: for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * q; toVd(KindMAdd, YQ); return;
            case 0x22: for (int k = 0; k < 4; k++) result[k] = vs[k] + i; toVd(KindAdd, YI); return;
            case 0x23: for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * i; toVd(KindMAdd, YI); return;
            case 0x24: for (int k = 0; k < 4; k++) result[k] = vs[k] - q; toVd(KindSub, YQ); return;
            case 0x25: for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * q; toVd(KindMSub, YQ); return;
            case 0x26: for (int k = 0; k < 4; k++) result[k] = vs[k] - i; toVd(KindSub, YI); return;
            case 0x27: for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * i; toVd(KindMSub, YI); return;
            case 0x28: for (int k = 0; k < 4; k++) result[k] = vs[k] + vt[k]; toVd(KindAdd, YVec); return;
            case 0x29: for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * vt[k]; toVd(KindMAdd, YVec); return;
            case 0x2A: for (int k = 0; k < 4; k++) result[k] = vs[k] * vt[k]; toVd(KindMul, YVec); return;
            case 0x2B: for (int k = 0; k < 4; k++) result[k] = (vs[k] > vt[k]) ? vs[k] : vt[k]; applyDest(vd, result, dest); return;
            case 0x2C: for (int k = 0; k < 4; k++) result[k] = vs[k] - vt[k]; toVd(KindSub, YVec); return;
            case 0x2D: for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * vt[k]; toVd(KindMSub, YVec); return;
            case 0x2E:
                result[0] = acc[0] - vs[1] * vt[2];
                result[1] = acc[1] - vs[2] * vt[0];
                result[2] = acc[2] - vs[0] * vt[1];
                result[3] = 0.0f;
                toVd(KindOpMsub, YCross);
                return;
            case 0x2F: for (int k = 0; k < 4; k++) result[k] = (vs[k] < vt[k]) ? vs[k] : vt[k]; applyDest(vd, result, dest); return;
            default: break;
            }
        }
        if (op < 0x3Cu)
        {
            c.bail = BailUnsupported;
            return;
        }
        const uint8_t sp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        float *vtDest = c.s.vf[ft];
        switch (sp)
        {
        case 0x00: case 0x01: case 0x02: case 0x03: { const float bc = vt[sp & 3]; for (int k = 0; k < 4; k++) result[k] = vs[k] + bc; toAcc(KindAdd, YBc); return; }
        case 0x04: case 0x05: case 0x06: case 0x07: { const float bc = vt[sp & 3]; for (int k = 0; k < 4; k++) result[k] = vs[k] - bc; toAcc(KindSub, YBc); return; }
        case 0x08: case 0x09: case 0x0A: case 0x0B: { const float bc = vt[sp & 3]; for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * bc; toAcc(KindMAdd, YBc); return; }
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: { const float bc = vt[sp & 3]; for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * bc; toAcc(KindMSub, YBc); return; }
        case 0x10: case 0x11: case 0x12: case 0x13:
        {
            static constexpr float divisor[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            for (int k = 0; k < 4; k++)
            {
                int32_t iv;
                std::memcpy(&iv, &c.s.vf[fs][k], 4);
                result[k] = sp == 0x10 ? static_cast<float>(iv) : static_cast<float>(iv) / divisor[sp & 3];
            }
            applyDest(vtDest, result, dest);
            return;
        }
        case 0x14: case 0x15: case 0x16: case 0x17:
        {
            static constexpr float scale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            for (int k = 0; k < 4; k++)
            {
                int32_t iv = vuFloatToInt(vs[k], scale[sp & 3]);
                std::memcpy(&result[k], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        }
        case 0x18: case 0x19: case 0x1A: case 0x1B: { const float bc = vt[sp & 3]; for (int k = 0; k < 4; k++) result[k] = vs[k] * bc; toAcc(KindMul, YBc); return; }
        case 0x1C: for (int k = 0; k < 4; k++) result[k] = vs[k] * q; toAcc(KindMul, YQ); return;
        case 0x1D: for (int k = 0; k < 4; k++) result[k] = std::fabs(vs[k]); applyDest(vtDest, result, dest); return;
        case 0x1E: for (int k = 0; k < 4; k++) result[k] = vs[k] * i; toAcc(KindMul, YI); return;
        case 0x1F: // CLIP
        {
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &c.s.vf[ft][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
            const auto exceeds = [limit](float value, uint32_t signMask)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                bits ^= signMask;
                int32_t orderedBits = 0;
                std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
                return orderedBits > limit;
            };
            uint32_t f = 0u;
            if (exceeds(c.s.vf[fs][0], 0x00000000u)) f |= 0x01u;
            if (exceeds(c.s.vf[fs][0], 0x80000000u)) f |= 0x02u;
            if (exceeds(c.s.vf[fs][1], 0x00000000u)) f |= 0x04u;
            if (exceeds(c.s.vf[fs][1], 0x80000000u)) f |= 0x08u;
            if (exceeds(c.s.vf[fs][2], 0x00000000u)) f |= 0x10u;
            if (exceeds(c.s.vf[fs][2], 0x80000000u)) f |= 0x20u;
            // queueClip: the working clip shifts at issue; the entry carries it.
            c.workingClip = ((c.workingClip << 6) | (f & 0x3Fu)) & 0xFFFFFFu;
            clip = c.workingClip;
            clipQueued = true;
            return;
        }
        case 0x20: for (int k = 0; k < 4; k++) result[k] = vs[k] + q; toAcc(KindAdd, YQ); return;
        case 0x21: for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * q; toAcc(KindMAdd, YQ); return;
        case 0x22: for (int k = 0; k < 4; k++) result[k] = vs[k] + i; toAcc(KindAdd, YI); return;
        case 0x23: for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * i; toAcc(KindMAdd, YI); return;
        case 0x24: for (int k = 0; k < 4; k++) result[k] = vs[k] - q; toAcc(KindSub, YQ); return;
        case 0x25: for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * q; toAcc(KindMSub, YQ); return;
        case 0x26: for (int k = 0; k < 4; k++) result[k] = vs[k] - i; toAcc(KindSub, YI); return;
        case 0x27: for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * i; toAcc(KindMSub, YI); return;
        case 0x28: for (int k = 0; k < 4; k++) result[k] = vs[k] + vt[k]; toAcc(KindAdd, YVec); return;
        case 0x29: for (int k = 0; k < 4; k++) result[k] = acc[k] + vs[k] * vt[k]; toAcc(KindMAdd, YVec); return;
        case 0x2A: for (int k = 0; k < 4; k++) result[k] = vs[k] * vt[k]; toAcc(KindMul, YVec); return;
        case 0x2C: for (int k = 0; k < 4; k++) result[k] = vs[k] - vt[k]; toAcc(KindSub, YVec); return;
        case 0x2D: for (int k = 0; k < 4; k++) result[k] = acc[k] - vs[k] * vt[k]; toAcc(KindMSub, YVec); return;
        case 0x2E:
            result[0] = vs[1] * vt[2];
            result[1] = vs[2] * vt[0];
            result[2] = vs[0] * vt[1];
            result[3] = 0.0f;
            toAcc(KindOpMula, YCross);
            return;
        case 0x2F:
        case 0x30:
            return;
        default:
            c.bail = BailUnsupported;
            return;
        }
    }

    bool lowerPlain(Ctx &c, uint32_t instr)
    {
        if (instr == 0x00000000u || instr == 0x8000033Cu)
            return true;
        const uint8_t opHi = (instr >> 25) & 0x7F;
        const uint32_t dataSize = c.dataSize;
        uint8_t *vuData = c.data;
        VU1State &s = c.s;
        switch (opHi)
        {
        case 0x00: // LQ
        {
            const uint8_t it = FT(instr), is = VIS(instr), dest = (instr >> 21) & 0xF;
            uint32_t addr = ((uint32_t)(int32_t)(s.vi[is] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                float tmp[4];
                std::memcpy(tmp, vuData + addr, 16);
                applyDest(s.vf[it], tmp, dest);
            }
            return true;
        }
        case 0x01: // SQ
        {
            const uint8_t is = FS(instr), it = VIT(instr), dest = (instr >> 21) & 0xF;
            uint32_t addr = ((uint32_t)(int32_t)(s.vi[it] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                uint32_t words[4];
                std::memcpy(words, s.vf[is], sizeof(words));
                storeQword(c, addr, words, dest);
            }
            return true;
        }
        case 0x04: // ILW
        {
            const uint8_t it = VIT(instr), is = VIS(instr), dest = (instr >> 21) & 0xF;
            uint32_t addr = ((uint32_t)(int32_t)(s.vi[is] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                const int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                uint32_t v;
                std::memcpy(&v, vuData + addr + comp * 4, 4);
                if (it != 0)
                    s.vi[it] = (int32_t)(int16_t)(v & 0xFFFF);
            }
            return true;
        }
        case 0x05: // ISW
        {
            const uint8_t it = VIT(instr), is = VIS(instr), dest = (instr >> 21) & 0xF;
            uint32_t addr = ((uint32_t)(int32_t)(s.vi[is] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[it] & 0xFFFF));
                const uint32_t words[4] = {val, val, val, val};
                storeQword(c, addr, words, dest);
            }
            return true;
        }
        case 0x08: // IADDIU
        {
            const uint8_t it = VIT(instr), is = VIS(instr);
            const int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
            if (it != 0)
                s.vi[it] = (int16_t)(s.vi[is] + imm);
            return true;
        }
        case 0x09: // ISUBIU
        {
            const uint8_t it = VIT(instr), is = VIS(instr);
            const int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
            if (it != 0)
                s.vi[it] = (int16_t)(s.vi[is] - imm);
            return true;
        }
        case 0x10: s.vi[1] = ((s.clip & 0xFFFFFF) == (instr & 0xFFFFFF)) ? 1 : 0; return true;            // FCEQ
        case 0x12: s.vi[1] = ((s.clip & (instr & 0xFFFFFF)) != 0) ? 1 : 0; return true;                  // FCAND
        case 0x13: s.vi[1] = ((s.clip | (instr & 0xFFFFFF)) == 0xFFFFFF) ? 1 : 0; return true;           // FCOR
        case 0x14: // FSEQ
        {
            const uint8_t it = VIT(instr);
            const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
            if (it != 0)
                s.vi[it] = ((s.status & 0xFFFu) == imm12) ? 1 : 0;
            return true;
        }
        case 0x16: // FSAND
        {
            const uint8_t it = VIT(instr);
            const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
            if (it != 0)
                s.vi[it] = static_cast<int32_t>((s.status & 0xFFFu) & imm12);
            return true;
        }
        case 0x17: // FSOR
        {
            const uint8_t it = VIT(instr);
            const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
            if (it != 0)
                s.vi[it] = static_cast<int32_t>((s.status & 0xFFFu) | imm12);
            return true;
        }
        case 0x18: { const uint8_t it = VIT(instr), is = VIS(instr); if (it != 0) s.vi[it] = ((s.mac & 0xFFFF) == (uint32_t)(uint16_t)s.vi[is]) ? 1 : 0; return true; } // FMEQ
        case 0x1A: { const uint8_t it = VIT(instr), is = VIS(instr); if (it != 0) s.vi[it] = (int32_t)(s.mac & (uint32_t)(uint16_t)s.vi[is]); return true; }              // FMAND
        case 0x1B: { const uint8_t it = VIT(instr), is = VIS(instr); if (it != 0) s.vi[it] = (int32_t)(s.mac | (uint32_t)(uint16_t)s.vi[is]); return true; }              // FMOR
        case 0x1C: { const uint8_t it = VIT(instr); if (it != 0) s.vi[it] = static_cast<int32_t>(s.clip & 0x0FFFu); return true; }                                      // FCGET
        case 0x40:
            break;
        default:
            return false;
        }

        const uint8_t funct = instr & 0x3Fu;
        const uint8_t vfT = FT(instr), vfS = FS(instr), viT = VIT(instr), viS = VIS(instr), viD = VID(instr);
        const uint8_t dest = (instr >> 21) & 0xF;
        switch (funct)
        {
        case 0x30: if (viD != 0) s.vi[viD] = (int16_t)(s.vi[viS] + s.vi[viT]); return true; // IADD
        case 0x31: if (viD != 0) s.vi[viD] = (int16_t)(s.vi[viS] - s.vi[viT]); return true; // ISUB
        case 0x32: // IADDI
        {
            const int16_t imm5 = (int16_t)((int32_t)((instr >> 6) & 0x1F) << 27 >> 27);
            if (viT != 0)
                s.vi[viT] = (int16_t)(s.vi[viS] + imm5);
            return true;
        }
        case 0x34: if (viD != 0) s.vi[viD] = s.vi[viS] & s.vi[viT]; return true; // IAND
        case 0x35: if (viD != 0) s.vi[viD] = s.vi[viS] | s.vi[viT]; return true; // IOR
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            break;
        default:
            return false;
        }
        const uint8_t f2 = (uint8_t)((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        switch (f2)
        {
        case 0x30: { float tmp[4]; std::memcpy(tmp, s.vf[vfS], 16); applyDest(s.vf[vfT], tmp, dest); return true; } // MOVE
        case 0x31: { float tmp[4] = {s.vf[vfS][1], s.vf[vfS][2], s.vf[vfS][3], s.vf[vfS][0]}; applyDest(s.vf[vfT], tmp, dest); return true; } // MR32
        case 0x34: // LQI
        {
            uint32_t addr = ((uint32_t)(uint16_t)s.vi[viS]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                float tmp[4];
                std::memcpy(tmp, vuData + addr, 16);
                applyDest(s.vf[vfT], tmp, dest);
            }
            if (viS != 0)
                s.vi[viS] = (int16_t)(s.vi[viS] + 1);
            return true;
        }
        case 0x35: // SQI
        {
            uint32_t addr = ((uint32_t)(uint16_t)s.vi[viT]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                uint32_t words[4];
                std::memcpy(words, s.vf[vfS], sizeof(words));
                storeQword(c, addr, words, dest);
            }
            if (viT != 0)
                s.vi[viT] = (int16_t)(s.vi[viT] + 1);
            return true;
        }
        case 0x36: // LQD
        {
            if (viS != 0)
                s.vi[viS] = (int16_t)(s.vi[viS] - 1);
            uint32_t addr = ((uint32_t)(uint16_t)s.vi[viS]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                float tmp[4];
                std::memcpy(tmp, vuData + addr, 16);
                applyDest(s.vf[vfT], tmp, dest);
            }
            return true;
        }
        case 0x37: // SQD
        {
            if (viT != 0)
                s.vi[viT] = (int16_t)(s.vi[viT] - 1);
            uint32_t addr = ((uint32_t)(uint16_t)s.vi[viT]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                uint32_t words[4];
                std::memcpy(words, s.vf[vfS], sizeof(words));
                storeQword(c, addr, words, dest);
            }
            return true;
        }
        case 0x3B: // WAITQ (the stall is the generator's)
            return true;
        case 0x3C: // MTIR
        {
            const uint32_t comp = (instr >> 21) & 0x3u;
            uint32_t fval;
            std::memcpy(&fval, &s.vf[vfS][comp], 4);
            if (viT != 0)
                s.vi[viT] = (int32_t)(int16_t)(fval & 0xFFFF);
            return true;
        }
        case 0x3D: // MFIR
        {
            float result[4];
            const int32_t val = (int32_t)(int16_t)(s.vi[viS] & 0xFFFF);
            std::memcpy(&result[0], &val, 4);
            result[1] = result[0];
            result[2] = result[0];
            result[3] = result[0];
            applyDest(s.vf[vfT], result, dest);
            return true;
        }
        case 0x3E: // ILWR
        {
            uint32_t addr = ((uint32_t)(uint16_t)s.vi[viS]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                const int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                uint32_t v;
                std::memcpy(&v, vuData + addr + comp * 4, 4);
                if (viT != 0)
                    s.vi[viT] = (int32_t)(int16_t)(v & 0xFFFF);
            }
            return true;
        }
        case 0x3F: // ISWR
        {
            uint32_t addr = ((uint32_t)(uint16_t)s.vi[viS]) * 16u;
            addr &= (dataSize - 1);
            if (addr + 16 <= dataSize)
            {
                const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viT] & 0xFFFF));
                const uint32_t words[4] = {val, val, val, val};
                storeQword(c, addr, words, dest);
            }
            return true;
        }
        case 0x40: // RNEXT
        {
            const uint32_t x = (s.r >> 4) & 1u;
            const uint32_t y = (s.r >> 22) & 1u;
            s.r = ((s.r << 1) ^ x ^ y) & 0x007FFFFFu;
            s.r |= 0x3F800000u;
            float value = 0.0f;
            std::memcpy(&value, &s.r, sizeof(value));
            const float result[4] = {value, value, value, value};
            applyDest(s.vf[vfT], result, dest);
            return true;
        }
        case 0x41: // RGET
        {
            float value = 0.0f;
            std::memcpy(&value, &s.r, sizeof(value));
            const float result[4] = {value, value, value, value};
            applyDest(s.vf[vfT], result, dest);
            return true;
        }
        case 0x42: // RINIT
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &s.vf[vfS][(instr >> 21) & 3u], sizeof(bits));
            s.r = 0x3F800000u | (bits & 0x007FFFFFu);
            return true;
        }
        case 0x43: // RXOR
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &s.vf[vfS][(instr >> 21) & 3u], sizeof(bits));
            s.r = 0x3F800000u | ((s.r ^ bits) & 0x007FFFFFu);
            return true;
        }
        case 0x64: { const float result[4] = {s.p, s.p, s.p, s.p}; applyDest(s.vf[vfT], result, dest); return true; } // MFP
        case 0x68: if (viT != 0) s.vi[viT] = (int32_t)(s.top & 0x3FFu); return true;   // XTOP
        case 0x69: if (viT != 0) s.vi[viT] = (int32_t)(s.itop & 0x3FFu); return true;  // XITOP
        case 0x7B: // WAITP (the stall is the generator's)
            return true;
        default:
            return false;
        }
    }

    void fdiv(Ctx &c, uint32_t instr, float &value, uint32_t &statusDi, uint32_t &latency)
    {
        const VU1State &s = c.s;
        const uint8_t vfT = FT(instr), vfS = FS(instr);
        const uint8_t f2 = (uint8_t)((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        uint32_t ignoredFlags = 0u;
        statusDi = 0u;
        if (f2 == 0x38) // DIV
        {
            const float num = vuNormalizeOperand(s.vf[vfS][(instr >> 21) & 0x3]);
            const float den = vuNormalizeOperand(s.vf[vfT][(instr >> 23) & 0x3]);
            float result = 0.0f;
            if (den == 0.0f)
            {
                statusDi = num == 0.0f ? 0x10u : 0x20u;
                result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            }
            else
                result = num / den;
            result = normalizeResult(result, ignoredFlags);
            value = normalizeResult(result, ignoredFlags); // queueQ normalizes again
            statusDi &= 0x30u;
            latency = 7u;
            return;
        }
        if (f2 == 0x39) // SQRT
        {
            const float val = vuNormalizeOperand(s.vf[vfT][(instr >> 23) & 0x3]);
            value = normalizeResult(std::sqrt(std::fabs(val)), ignoredFlags);
            statusDi = (val < 0.0f ? 0x10u : 0u) & 0x30u;
            latency = 7u;
            return;
        }
        // RSQRT
        const float num = vuNormalizeOperand(s.vf[vfS][(instr >> 21) & 0x3]);
        const float radicand = vuNormalizeOperand(s.vf[vfT][(instr >> 23) & 0x3]);
        const float den = std::sqrt(std::fabs(radicand));
        statusDi = radicand < 0.0f ? 0x10u : 0u;
        float result = 0.0f;
        if (den != 0.0f)
            result = num / den;
        else
        {
            statusDi = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        result = normalizeResult(result, ignoredFlags);
        value = normalizeResult(result, ignoredFlags);
        statusDi &= 0x30u;
        latency = 13u;
    }

    void efu(Ctx &c, uint32_t instr, float &value, uint32_t &latency)
    {
        const VU1State &s = c.s;
        const uint8_t vfS = FS(instr);
        const uint8_t f2 = (uint8_t)((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        const uint32_t component = (instr >> 21) & 3u;
        float v = 0.0f;
        switch (f2)
        {
        case 0x70: { const float x = vuNormalizeOperand(s.vf[vfS][0]), y = vuNormalizeOperand(s.vf[vfS][1]), z = vuNormalizeOperand(s.vf[vfS][2]); v = x * x + y * y + z * z; latency = 11u; break; }
        case 0x71: { const float x = vuNormalizeOperand(s.vf[vfS][0]), y = vuNormalizeOperand(s.vf[vfS][1]), z = vuNormalizeOperand(s.vf[vfS][2]); const float sum = x * x + y * y + z * z; v = sum != 0.0f ? 1.0f / sum : sum; latency = 18u; break; }
        case 0x72: { const float x = vuNormalizeOperand(s.vf[vfS][0]), y = vuNormalizeOperand(s.vf[vfS][1]), z = vuNormalizeOperand(s.vf[vfS][2]); v = std::sqrt(x * x + y * y + z * z); latency = 18u; break; }
        case 0x73: { const float x = vuNormalizeOperand(s.vf[vfS][0]), y = vuNormalizeOperand(s.vf[vfS][1]), z = vuNormalizeOperand(s.vf[vfS][2]); const float len = std::sqrt(x * x + y * y + z * z); v = len != 0.0f ? 1.0f / len : len; latency = 24u; break; }
        case 0x74: { const float x = vuNormalizeOperand(s.vf[vfS][0]), y = vuNormalizeOperand(s.vf[vfS][1]); v = x != 0.0f ? vuEatan(y / x) : 0.0f; latency = 54u; break; }
        case 0x75: { const float x = vuNormalizeOperand(s.vf[vfS][0]), z = vuNormalizeOperand(s.vf[vfS][2]); v = x != 0.0f ? vuEatan(z / x) : 0.0f; latency = 54u; break; }
        case 0x76: { float sum = 0.0f; for (uint32_t k = 0; k < 4u; ++k) sum += vuNormalizeOperand(s.vf[vfS][k]); v = sum; latency = 12u; break; }
        case 0x78: { const float x = vuNormalizeOperand(s.vf[vfS][component]); v = x >= 0.0f ? std::sqrt(x) : x; latency = 12u; break; }
        case 0x79: { const float x = vuNormalizeOperand(s.vf[vfS][component]); float r = x; if (r >= 0.0f) { r = std::sqrt(r); if (r != 0.0f) r = 1.0f / r; } v = r; latency = 18u; break; }
        case 0x7A: { const float x = vuNormalizeOperand(s.vf[vfS][component]); v = x != 0.0f ? 1.0f / x : x; latency = 12u; break; }
        case 0x7C: v = vuEsin(vuNormalizeOperand(s.vf[vfS][component])); latency = 29u; break;
        case 0x7D: v = vuEatan(vuNormalizeOperand(s.vf[vfS][component])); latency = 54u; break;
        case 0x7E: v = vuEexp(vuNormalizeOperand(s.vf[vfS][component])); latency = 44u; break;
        default: c.bail = BailUnsupported; latency = 1u; break;
        }
        uint32_t ignoredFlags = 0u;
        value = normalizeResult(v, ignoredFlags); // queueP
    }
}
