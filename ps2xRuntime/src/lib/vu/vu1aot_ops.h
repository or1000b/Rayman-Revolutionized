// VU1 ahead-of-time translation: instruction templates for the generated programs. Same expressions, same order of
// reads and writes as vu1aot.cpp (upper / fmacFinish / lowerPlain), which copy the interpreter; only the decoding
// moves to compile time (the instruction word is a template argument), so the compiler keeps just the code of that
// one instruction: no switch, no loop over lanes the destination mask leaves out, no unused operand. Checked against
// the interpreter with PS2X_VU1AOT=diff (docs/vu1_aot.md).
#pragma once

#include "vu1aot.h"

namespace vu1aot
{
    namespace ops
    {
        VU1AOT_INLINE float nz(float v) { return vuNormalizeOperand(v); }

        constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};

        // The FMAC family (execUpper cases that end in applyFmacDest / applyFmacDestAcc / applyDest for MAX / MINI).
        enum class Op : uint8_t
        {
            Add,
            Sub,
            MAdd,
            MSub,
            Mul,
            Max,
            Min,
            OpMsub,
            OpMula
        };

        constexpr Kind kindOf(Op op)
        {
            return op == Op::Add ? KindAdd : op == Op::Sub ? KindSub : op == Op::MAdd ? KindMAdd : op == Op::MSub ? KindMSub
                   : op == Op::Mul ? KindMul : op == Op::OpMsub ? KindOpMsub : op == Op::OpMula ? KindOpMula : KindNone;
        }

        // The second operand of lane k (before the operation), as execUpper reads it.
        template <YSource Y, uint8_t FT, uint8_t BC>
        VU1AOT_INLINE float yOperand(const Ctx &c, uint32_t k)
        {
            if constexpr (Y == YBc)
                return nz(c.s.vf[FT][BC]);
            else if constexpr (Y == YQ)
                return nz(c.s.q);
            else if constexpr (Y == YI)
                return nz(c.s.i);
            else if constexpr (Y == YVec)
                return nz(c.s.vf[FT][k]);
            else
                return nz(c.s.vf[FT][crossRight[k]]);
        }

        // fmacFinish with the instruction known.
        template <Kind KIND, YSource Y, uint8_t DEST, uint8_t FS, uint8_t FT, uint8_t BC, uint32_t INSTR>
        VU1AOT_INLINE void fmacFinishT(Ctx &c, float *result, float *dst, FlagResult &flagsOut)
        {
            constexpr bool productSum = KIND == KindMAdd || KIND == KindMSub || KIND == KindOpMsub;
            constexpr bool productSumFast = KIND == KindMAdd || KIND == KindMSub;
            constexpr bool cross = Y == YCross;
            constexpr bool opmsubOp = (INSTR & 0x3Fu) == 0x2Eu;
            uint32_t productSticky = 0u;
            if constexpr (productSum)
            {
                const float maximum = std::numeric_limits<float>::max();
                const float minimum = std::numeric_limits<float>::min();
                for (uint32_t k = 0; k < 4u; ++k)
                {
                    if ((DEST & laneBit(k)) == 0u)
                        continue;
                    const float left = nz(c.s.vf[FS][opmsubOp ? crossLeft[k] : k]);
                    const float right = yOperand<Y, FT, BC>(c, k);
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
            uint8_t laneFlags[4] = {0, 0, 0, 0};
            for (uint32_t k = 0; k < 4u; ++k)
            {
                if ((DEST & laneBit(k)) == 0u)
                    continue;
                if constexpr (KIND != KindOpMsub && KIND != KindOpMula)
                {
                    uint32_t bits = 0;
                    std::memcpy(&bits, &result[k], sizeof(bits));
                    const uint32_t exponent = (bits >> 23) & 0xFFu;
                    bool fast = exponent >= 1u && exponent <= 253u;
                    if constexpr (productSumFast)
                    {
                        if (fast)
                            fast = !productOverflow && exponent >= 2u && exponent + 20u >= vuFloatExponent(nz(c.s.acc[k]));
                    }
                    if (fast)
                    {
                        laneFlags[k] = (bits & 0x80000000u) != 0u ? 0x2u : 0u;
                        continue;
                    }
                }
                const long double x = static_cast<long double>(nz(c.s.vf[FS][cross ? crossLeft[k] : k]));
                const long double y = static_cast<long double>(yOperand<Y, FT, BC>(c, k));
                long double exactResult = 0.0L;
                if constexpr (KIND == KindAdd)
                    exactResult = x + y;
                else if constexpr (KIND == KindSub)
                    exactResult = x - y;
                else if constexpr (KIND == KindMul)
                    exactResult = x * y;
                else if constexpr (KIND == KindMAdd)
                    exactResult = static_cast<long double>(nz(c.s.acc[k])) + x * y;
                else if constexpr (KIND == KindMSub)
                    exactResult = static_cast<long double>(nz(c.s.acc[k])) - x * y;
                else if constexpr (KIND == KindOpMsub)
                    exactResult = k == 3u ? 0.0L : static_cast<long double>(nz(c.s.acc[k])) - x * y;
                else
                    exactResult = k == 3u ? 0.0L : x * y;
                laneFlags[k] = normalizeExact(result[k], exactResult);
            }
            flagsOut = {};
            if (DEST != 0u && c.flagsLive)
            {
                uint32_t mac = 0u, status = 0u;
                for (uint32_t k = 0; k < 4u; ++k)
                {
                    const uint8_t lane = laneBit(k);
                    if ((DEST & lane) == 0u)
                        continue;
                    const uint32_t f = laneFlags[k];
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
            applyDest(dst, result, DEST);
        }

        template <Op OP, YSource Y, uint8_t DEST, uint8_t FS, uint8_t FT, uint8_t FD, uint8_t BC, bool TOACC, uint32_t INSTR>
        VU1AOT_INLINE void fmac(Ctx &c, FlagResult &flags)
        {
            float result[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            constexpr bool cross = Y == YCross;
            for (uint32_t k = 0; k < 4u; ++k)
            {
                if ((DEST & laneBit(k)) == 0u)
                    continue;
                const float vs = nz(c.s.vf[FS][cross ? crossLeft[k] : k]);
                const float y = yOperand<Y, FT, BC>(c, k);
                if constexpr (OP == Op::Add)
                    result[k] = vs + y;
                else if constexpr (OP == Op::Sub)
                    result[k] = vs - y;
                else if constexpr (OP == Op::MAdd)
                    result[k] = nz(c.s.acc[k]) + vs * y;
                else if constexpr (OP == Op::MSub)
                    result[k] = nz(c.s.acc[k]) - vs * y;
                else if constexpr (OP == Op::Mul)
                    result[k] = vs * y;
                else if constexpr (OP == Op::Max)
                    result[k] = (vs > y) ? vs : y;
                else if constexpr (OP == Op::Min)
                    result[k] = (vs < y) ? vs : y;
                else if constexpr (OP == Op::OpMsub)
                    result[k] = k == 3u ? 0.0f : nz(c.s.acc[k]) - vs * y;
                else
                    result[k] = k == 3u ? 0.0f : vs * y;
            }
            float *dst = TOACC ? c.s.acc : c.s.vf[FD];
            if constexpr (OP == Op::Max || OP == Op::Min)
            {
                flags = {};
                applyDest(dst, result, DEST);
            }
            else
                fmacFinishT<kindOf(OP), Y, DEST, FS, FT, BC, INSTR>(c, result, dst, flags);
        }

        // ---------------------------------------------------------------- lower (lowerPlain with the word known)

        template <uint32_t I>
        VU1AOT_INLINE uint32_t qwordAddress(int32_t vi, const Ctx &c)
        {
            return ((uint32_t)(int32_t)(vi + IMM11(I))) * 16u & (c.dataSize - 1);
        }

        template <uint32_t I>
        VU1AOT_INLINE void lower(Ctx &c)
        {
            constexpr uint8_t opHi = (I >> 25) & 0x7F;
            constexpr uint8_t vfT = (I >> 16) & 0x1F, vfS = (I >> 11) & 0x1F;
            constexpr uint8_t viT = (I >> 16) & 0xF, viS = (I >> 11) & 0xF, viD = (I >> 6) & 0xF;
            constexpr uint8_t dest = (I >> 21) & 0xF;
            VU1State &s = c.s;
            if constexpr (opHi == 0x00) // LQ
            {
                const uint32_t addr = qwordAddress<I>(s.vi[viS], c);
                if (addr + 16 <= c.dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, c.data + addr, 16);
                    applyDest(s.vf[vfT], tmp, dest);
                }
            }
            else if constexpr (opHi == 0x01) // SQ
            {
                const uint32_t addr = qwordAddress<I>(s.vi[viT], c);
                if (addr + 16 <= c.dataSize)
                {
                    uint32_t words[4];
                    std::memcpy(words, s.vf[vfS], sizeof(words));
                    storeQword(c, addr, words, dest);
                }
            }
            else if constexpr (opHi == 0x04) // ILW
            {
                const uint32_t addr = qwordAddress<I>(s.vi[viS], c);
                if (addr + 16 <= c.dataSize)
                {
                    constexpr int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                    uint32_t v;
                    std::memcpy(&v, c.data + addr + comp * 4, 4);
                    if constexpr (viT != 0)
                        s.vi[viT] = (int32_t)(int16_t)(v & 0xFFFF);
                }
            }
            else if constexpr (opHi == 0x05) // ISW
            {
                const uint32_t addr = qwordAddress<I>(s.vi[viS], c);
                if (addr + 16 <= c.dataSize)
                {
                    const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viT] & 0xFFFF));
                    const uint32_t words[4] = {val, val, val, val};
                    storeQword(c, addr, words, dest);
                }
            }
            else if constexpr (opHi == 0x08 || opHi == 0x09) // IADDIU / ISUBIU
            {
                constexpr int16_t imm = (int16_t)(I & 0x7FF) | ((I >> 10) & 0x7800);
                if constexpr (viT != 0)
                    s.vi[viT] = opHi == 0x08 ? (int16_t)(s.vi[viS] + imm) : (int16_t)(s.vi[viS] - imm);
            }
            else if constexpr (opHi == 0x40 && ((I & 0x3F) == 0x30 || (I & 0x3F) == 0x31)) // IADD / ISUB
            {
                if constexpr (viD != 0)
                    s.vi[viD] = (I & 0x3F) == 0x30 ? (int16_t)(s.vi[viS] + s.vi[viT]) : (int16_t)(s.vi[viS] - s.vi[viT]);
            }
            else if constexpr (opHi == 0x40 && (I & 0x3F) == 0x32) // IADDI
            {
                constexpr int16_t imm5 = (int16_t)((int32_t)((I >> 6) & 0x1F) << 27 >> 27);
                if constexpr (viT != 0)
                    s.vi[viT] = (int16_t)(s.vi[viS] + imm5);
            }
            else if constexpr (opHi == 0x40 && ((I & 0x3F) == 0x34 || (I & 0x3F) == 0x35)) // IAND / IOR
            {
                if constexpr (viD != 0)
                    s.vi[viD] = (I & 0x3F) == 0x34 ? (s.vi[viS] & s.vi[viT]) : (s.vi[viS] | s.vi[viT]);
            }
            else if constexpr (opHi == 0x40 && (I & 0x3F) >= 0x3C && ((I & 0x3u) | ((I >> 4) & 0x7Cu)) == 0x30) // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, s.vf[vfS], 16);
                applyDest(s.vf[vfT], tmp, dest);
            }
            else if constexpr (opHi == 0x40 && (I & 0x3F) >= 0x3C && ((I & 0x3u) | ((I >> 4) & 0x7Cu)) == 0x31) // MR32
            {
                float tmp[4] = {s.vf[vfS][1], s.vf[vfS][2], s.vf[vfS][3], s.vf[vfS][0]};
                applyDest(s.vf[vfT], tmp, dest);
            }
            else if constexpr (opHi == 0x40 && (I & 0x3F) >= 0x3C && ((I & 0x3u) | ((I >> 4) & 0x7Cu)) == 0x34) // LQI
            {
                const uint32_t addr = ((uint32_t)(uint16_t)s.vi[viS]) * 16u & (c.dataSize - 1);
                if (addr + 16 <= c.dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, c.data + addr, 16);
                    applyDest(s.vf[vfT], tmp, dest);
                }
                if constexpr (viS != 0)
                    s.vi[viS] = (int16_t)(s.vi[viS] + 1);
            }
            else if constexpr (opHi == 0x40 && (I & 0x3F) >= 0x3C && ((I & 0x3u) | ((I >> 4) & 0x7Cu)) == 0x35) // SQI
            {
                const uint32_t addr = ((uint32_t)(uint16_t)s.vi[viT]) * 16u & (c.dataSize - 1);
                if (addr + 16 <= c.dataSize)
                {
                    uint32_t words[4];
                    std::memcpy(words, s.vf[vfS], sizeof(words));
                    storeQword(c, addr, words, dest);
                }
                if constexpr (viT != 0)
                    s.vi[viT] = (int16_t)(s.vi[viT] + 1);
            }
            else
                lowerPlain(c, I); // the rest: the generic copy
        }
    }
}
