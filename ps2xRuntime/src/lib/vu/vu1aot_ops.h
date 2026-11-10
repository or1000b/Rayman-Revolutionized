// VU1 ahead-of-time translation: instruction templates for the generated programs. Same expressions, same order of
// reads and writes as vu1aot.cpp (upper / fmacFinish / lowerPlain), which copy the interpreter; only the decoding
// moves to compile time (the instruction word is a template argument), so the compiler keeps just the code of that
// one instruction: no switch, no loop over lanes the destination mask leaves out, no unused operand. Checked against
// the interpreter with PS2X_VU1AOT=diff (docs/vu1_aot.md).
#pragma once

#include "vu1aot.h"

#include <immintrin.h>

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

        // ---------------------------------------------------------------- SSE fast path (flags not live)
        //
        // The same arithmetic on 4 lanes at once: SSE computes each lane exactly like the scalar instruction (same
        // rounding mode, round toward zero; operands clamped like vuNormalizeOperand; multiply and add rounded
        // separately). The interpreter keeps a lane's float result when its exponent is in 1..253 (and, for MADD /
        // MSUB, no product overflowed and the result is not much smaller than ACC: exponent + 20 >= ACC exponent);
        // then the MAC / status flags are only signs, and nothing reads them when flags are not live (Rayman's VU1
        // code never reads them). Any other lane: the exact scalar template does the whole instruction (registers are
        // untouched until the store). OPMSUB / OPMULA always take the interpreter's exact path, so they stay scalar.

        VU1AOT_INLINE __m128i nz4i(__m128i b)
        {
            const __m128i e = _mm_and_si128(b, _mm_set1_epi32(0x7F800000));
            const __m128i sign = _mm_and_si128(b, _mm_set1_epi32(static_cast<int>(0x80000000u)));
            b = _mm_blendv_epi8(b, sign, _mm_cmpeq_epi32(e, _mm_setzero_si128()));
            return _mm_blendv_epi8(b, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), _mm_cmpeq_epi32(e, _mm_set1_epi32(0x7F800000)));
        }

        VU1AOT_INLINE __m128 nz4(const float *v) { return _mm_castsi128_ps(nz4i(_mm_loadu_si128(reinterpret_cast<const __m128i *>(v)))); }

        VU1AOT_INLINE __m128i exp4(__m128 v) { return _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(v), 23), _mm_set1_epi32(0xFF)); }

        // DEST (0x8 = x) -> SSE lane bits (bit 0 = x), for movemask / blends.
        constexpr int laneMask(uint8_t dest) { return ((dest >> 3) & 1) | ((dest >> 1) & 2) | ((dest << 1) & 4) | ((dest << 3) & 8); }

        template <uint8_t DEST>
        VU1AOT_INLINE void store4(float *dst, __m128 r)
        {
            constexpr int m = laneMask(DEST);
            if constexpr (m == 0xF)
                _mm_storeu_ps(dst, r);
            else if constexpr (m != 0)
                _mm_storeu_ps(dst, _mm_blend_ps(_mm_loadu_ps(dst), r, m));
        }

        template <Op OP, YSource Y, uint8_t DEST, uint8_t FS, uint8_t FT, uint8_t FD, uint8_t BC, bool TOACC, uint32_t INSTR>
        VU1AOT_INLINE void fmacSse(Ctx &c, FlagResult &flags)
        {
            if constexpr (OP == Op::OpMsub || OP == Op::OpMula)
            {
                fmac<OP, Y, DEST, FS, FT, FD, BC, TOACC, INSTR>(c, flags);
            }
            else
            {
                if (c.flagsLive)
                {
                    fmac<OP, Y, DEST, FS, FT, FD, BC, TOACC, INSTR>(c, flags);
                    return;
                }
                flags = {};
                const __m128 vs = nz4(c.s.vf[FS]);
                __m128 y;
                if constexpr (Y == YBc)
                {
                    const __m128 t = nz4(c.s.vf[FT]);
                    y = _mm_shuffle_ps(t, t, _MM_SHUFFLE(BC, BC, BC, BC));
                }
                else if constexpr (Y == YQ)
                    y = _mm_set1_ps(nz(c.s.q));
                else if constexpr (Y == YI)
                    y = _mm_set1_ps(nz(c.s.i));
                else
                    y = nz4(c.s.vf[FT]);
                float *dst = TOACC ? c.s.acc : c.s.vf[FD];
                if constexpr (OP == Op::Max)
                    store4<DEST>(dst, _mm_max_ps(vs, y)); // (vs > y) ? vs : y
                else if constexpr (OP == Op::Min)
                    store4<DEST>(dst, _mm_min_ps(vs, y)); // (vs < y) ? vs : y
                else
                {
                    __m128 r;
                    __m128i ok;
                    if constexpr (OP == Op::Add || OP == Op::Sub || OP == Op::Mul)
                    {
                        if constexpr (OP == Op::Add)
                            r = _mm_add_ps(vs, y);
                        else if constexpr (OP == Op::Sub)
                            r = _mm_sub_ps(vs, y);
                        else
                            r = _mm_mul_ps(vs, y);
                        const __m128i e = exp4(r);
                        ok = _mm_and_si128(_mm_cmpgt_epi32(e, _mm_setzero_si128()), _mm_cmplt_epi32(e, _mm_set1_epi32(254)));
                    }
                    else
                    {
                        const __m128 acc = nz4(c.s.acc);
                        if constexpr (OP == Op::MAdd)
                            r = _mm_add_ps(acc, _mm_mul_ps(vs, y));
                        else
                            r = _mm_sub_ps(acc, _mm_mul_ps(vs, y));
                        // No product can overflow when the exponents sum to at most 379 (|vs * y| < 2^127).
                        const __m128i esum = _mm_add_epi32(exp4(vs), exp4(y));
                        if ((_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(esum, _mm_set1_epi32(379)))) & laneMask(DEST)) != 0)
                        {
                            fmac<OP, Y, DEST, FS, FT, FD, BC, TOACC, INSTR>(c, flags);
                            return;
                        }
                        const __m128i er = exp4(r);
                        ok = _mm_and_si128(_mm_cmpgt_epi32(er, _mm_set1_epi32(1)), _mm_cmplt_epi32(er, _mm_set1_epi32(254)));
                        ok = _mm_and_si128(ok, _mm_cmpgt_epi32(_mm_add_epi32(er, _mm_set1_epi32(21)), exp4(acc)));
                    }
                    if ((_mm_movemask_ps(_mm_castsi128_ps(ok)) & laneMask(DEST)) == laneMask(DEST))
                        store4<DEST>(dst, r);
                    else
                        fmac<OP, Y, DEST, FS, FT, FD, BC, TOACC, INSTR>(c, flags);
                }
            }
        }

        // ITOF0 / 4 / 12 / 15 (special 0x10..0x13): static_cast<float>(int) [/ 16 / 4096 / 32768] into FT.
        template <uint8_t SC, uint8_t DEST, uint8_t FS, uint8_t FT>
        VU1AOT_INLINE void itof(Ctx &c)
        {
            __m128 r = _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i *>(c.s.vf[FS])));
            if constexpr (SC == 1)
                r = _mm_div_ps(r, _mm_set1_ps(16.0f));
            else if constexpr (SC == 2)
                r = _mm_div_ps(r, _mm_set1_ps(4096.0f));
            else if constexpr (SC == 3)
                r = _mm_div_ps(r, _mm_set1_ps(32768.0f));
            store4<DEST>(c.s.vf[FT], r);
        }

        // ABS (special 0x1D): fabs of the clamped operand into FT.
        template <uint8_t DEST, uint8_t FS, uint8_t FT>
        VU1AOT_INLINE void absT(Ctx &c)
        {
            store4<DEST>(c.s.vf[FT], _mm_and_ps(nz4(c.s.vf[FS]), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))));
        }
    }
}
