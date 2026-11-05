#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <ps2_log.h>

extern "C" uint64_t ps2xGuestVsync(); // ps2_test_harness.cpp

namespace
{
    bool g_vu0TraceThisRun = false; // PS2X_VU0_TRACE: this VU0 run matched the filter
}

namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // Rayman bring-up (speed): per-interpreter side state kept outside the class so the shared
    // header (and with it every generated translation unit) does not change. nextCommit is the
    // earliest readyCycle of any queued pipeline entry, so commitReadyPipelines() can return
    // immediately on the (very common) cycles where nothing becomes visible.
    struct VuSideState
    {
        const void *owner = nullptr;
        uint64_t nextCommit = 0;
        // Bit i set = entry i of that pipeline is valid (iterated in index order, like the scans).
        uint32_t flagMask = 0;
        uint32_t efuMask = 0;
        uint32_t storeMask = 0;
        uint32_t vfMask = 0;
        uint32_t viMask = 0;
        uint32_t accMask = 0;
        bool masksValid = false;
        // Set by rebuildDecodedCodeCache when no instruction of the loaded microcode reads the MAC or status flags
        // (FSEQ / FSAND / FSOR / FMEQ / FMAND / FMOR): updateFmacFlags then skips its pipeline entry (~5% of VU1 time
        // plus the commits). Clip flags (CLIP / FC*) are not affected.
        bool macStatusUnread = false;
    };
    VuSideState g_vuSide[4];

    inline VuSideState *vuSide(const void *owner)
    {
        for (VuSideState &side : g_vuSide)
        {
            if (side.owner == owner)
                return &side;
        }
        for (VuSideState &side : g_vuSide)
        {
            if (side.owner == nullptr)
            {
                side.owner = owner;
                side.nextCommit = 0;
                side.masksValid = false; // unknown until the next scheduler reset
                return &side;
            }
        }
        return nullptr; // no slot: always scan (correct, just slower)
    }

    inline void vuNoteReady(const void *owner, uint64_t readyCycle)
    {
        if (VuSideState *side = vuSide(owner))
            side->nextCommit = std::min(side->nextCommit, readyCycle);
    }

    inline void vuNoteQueued(const void *owner, uint64_t readyCycle, uint32_t VuSideState::*mask, uint32_t index)
    {
        if (VuSideState *side = vuSide(owner))
        {
            side->nextCommit = std::min(side->nextCommit, readyCycle);
            side->*mask |= 1u << index;
        }
    }

    // Set by applyFmacDest*: a product of the current product-sum op overflowed in some lane, so
    // the float result may be saturated and the exact path must classify it.
    thread_local bool t_fmacProductOverflow = false;
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    // Do not zero the 64 KiB packet buffer on every MSCAL; only the control fields matter.
    m_xgkick.sourceAddress = 0;
    m_xgkick.totalBytes = 0;
    m_xgkick.copiedBytes = 0;
    m_xgkick.currentTagEnd = 0;
    m_xgkick.cycleCredit = 0;
    m_xgkick.issueCycle = 0;
    m_xgkick.active = false;
    m_xgkick.currentTagEop = false;
    if (VuSideState *side = vuSide(this))
    {
        side->nextCommit = ~0ull;
        side->flagMask = side->efuMask = side->storeMask = 0u;
        side->vfMask = side->viMask = side->accMask = 0u;
        side->masksValid = true;
    }
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    return vuNormalizeOperand(value);
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
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

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    // Decode the operation once (calculateFmacExactResult decoded it again for every lane).
    enum ExactKind : uint8_t
    {
        KindNone,
        KindAdd,
        KindSub,
        KindMul,
        KindMAdd,
        KindMSub,
        KindOpMsub,
        KindOpMula
    };
    enum YSource : uint8_t
    {
        YBc,
        YQ,
        YI,
        YVec,
        YCross
    };
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const bool special = op >= 0x3Cu;
    const uint8_t cls = special ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
    ExactKind kind = KindNone;
    YSource ySource = YBc;
    if (cls <= 0x03u)
        kind = KindAdd;
    else if (cls <= 0x07u)
        kind = KindSub;
    else if (cls <= 0x0Bu)
        kind = KindMAdd;
    else if (cls <= 0x0Fu)
        kind = KindMSub;
    else if (cls >= 0x18u && cls <= 0x1Bu)
        kind = KindMul;
    else
    {
        switch (cls)
        {
        case 0x1Cu: kind = KindMul; ySource = YQ; break;
        case 0x1Eu: kind = KindMul; ySource = YI; break;
        case 0x20u: kind = KindAdd; ySource = YQ; break;
        case 0x21u: kind = KindMAdd; ySource = YQ; break;
        case 0x22u: kind = KindAdd; ySource = YI; break;
        case 0x23u: kind = KindMAdd; ySource = YI; break;
        case 0x24u: kind = KindSub; ySource = YQ; break;
        case 0x25u: kind = KindMSub; ySource = YQ; break;
        case 0x26u: kind = KindSub; ySource = YI; break;
        case 0x27u: kind = KindMSub; ySource = YI; break;
        case 0x28u: kind = KindAdd; ySource = YVec; break;
        case 0x29u: kind = KindMAdd; ySource = YVec; break;
        case 0x2Au: kind = KindMul; ySource = YVec; break;
        case 0x2Cu: kind = KindSub; ySource = YVec; break;
        case 0x2Du: kind = KindMSub; ySource = YVec; break;
        case 0x2Eu: kind = special ? KindOpMula : KindOpMsub; ySource = YCross; break;
        default: break;
        }
    }
    const bool productSum = kind == KindMAdd || kind == KindMSub;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint32_t bcLane = cls & 3u;
    static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
    static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};

    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        // Fast path: a normal float result proves the exact result is non-zero, in range and of
        // the same sign, so the flags are just the sign and the value stays. Zero/denormal/huge
        // results and possible cancellation in product-sum ops take the exact path.
        if (kind != KindOpMsub && kind != KindOpMula)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &result[component], sizeof(bits));
            const uint32_t exponent = (bits >> 23) & 0xFFu;
            bool fast = exponent >= 1u && exponent <= 253u;
            if (fast && productSum)
                fast = !t_fmacProductOverflow && exponent >= 2u &&
                       exponent + 20u >= vuFloatExponent(vuNormalizeOperand(m_state.acc[component]));
            if (fast)
            {
                laneFlags[component] = (bits & 0x80000000u) != 0u ? 0x2u : 0u;
                continue;
            }
        }

        if (kind == KindNone)
        {
            uint32_t flags = 0u;
            result[component] = normalizeResult(result[component], flags);
            laneFlags[component] = static_cast<uint8_t>(flags);
            continue;
        }

        // Exact result, same expressions and precision as calculateFmacExactResult().
        const bool cross = ySource == YCross;
        const long double x = static_cast<long double>(vuNormalizeOperand(m_state.vf[fs][cross ? crossLeft[component] : component]));
        long double y = 0.0L;
        switch (ySource)
        {
        case YBc: y = static_cast<long double>(vuNormalizeOperand(m_state.vf[ft][bcLane])); break;
        case YQ: y = static_cast<long double>(vuNormalizeOperand(m_state.q)); break;
        case YI: y = static_cast<long double>(vuNormalizeOperand(m_state.i)); break;
        case YVec: y = static_cast<long double>(vuNormalizeOperand(m_state.vf[ft][component])); break;
        case YCross: y = static_cast<long double>(vuNormalizeOperand(m_state.vf[ft][crossRight[component]])); break;
        }
        long double exactResult = 0.0L;
        switch (kind)
        {
        case KindAdd: exactResult = x + y; break;
        case KindSub: exactResult = x - y; break;
        case KindMul: exactResult = x * y; break;
        case KindMAdd: exactResult = static_cast<long double>(vuNormalizeOperand(m_state.acc[component])) + x * y; break;
        case KindMSub: exactResult = static_cast<long double>(vuNormalizeOperand(m_state.acc[component])) - x * y; break;
        case KindOpMsub:
            exactResult = component == 3u ? 0.0L : static_cast<long double>(vuNormalizeOperand(m_state.acc[component])) - x * y;
            break;
        case KindOpMula: exactResult = component == 3u ? 0.0L : x * y; break;
        default: break;
        }
        laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
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

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    // Which value the product's right operand is (decided once, not per lane).
    enum RightKind
    {
        RightBroadcast,
        RightQ,
        RightI,
        RightCross,
        RightLane
    };
    RightKind kind = RightLane;
    uint32_t broadcastLane = 0u;
    if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
    {
        kind = RightBroadcast;
        broadcastLane = (op >= 0x08u && op <= 0x0Fu ? op : special) & 3u;
    }
    else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        kind = RightQ;
    else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        kind = RightI;
    else if (op == 0x2Eu)
        kind = RightCross;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
    static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
    const float maximum = std::numeric_limits<float>::max();
    const float minimum = std::numeric_limits<float>::min();
    uint32_t extraSticky = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = vuNormalizeOperand(m_state.vf[fs][leftComponent]);
        float right;
        switch (kind)
        {
        case RightBroadcast:
            right = vuNormalizeOperand(m_state.vf[ft][broadcastLane]);
            break;
        case RightQ:
            right = vuNormalizeOperand(m_state.q);
            break;
        case RightI:
            right = vuNormalizeOperand(m_state.i);
            break;
        case RightCross:
            right = vuNormalizeOperand(m_state.vf[ft][crossRight[component]]);
            break;
        default:
            right = vuNormalizeOperand(m_state.vf[ft][component]);
            break;
        }

        // The product of two floats is exact in double precision.
        const double exactProduct = static_cast<double>(left) * static_cast<double>(right);
        const double magnitude = std::fabs(exactProduct);
        uint32_t flags = std::signbit(exactProduct) ? 0x2u : 0u;
        if (magnitude == 0.0)
            flags |= 0x1u;
        else if (magnitude > static_cast<double>(maximum))
            flags |= 0x8u;
        else if (magnitude < static_cast<double>(minimum))
            flags |= 0x5u;
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= flags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;
    if (m_decodedCodeCacheValid)
    {
        const VuSideState *flagSide = vuSide(this);
        if (flagSide && flagSide->macStatusUnread)
            return;
    }

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    FlagPipelineEntry *entry = nullptr;
    const VuSideState *side = vuSide(this);
    if (side && side->masksValid)
    {
        const uint32_t freeMask = ~side->flagMask & ((1u << kMaxFlagEntries) - 1u);
        if (freeMask != 0u)
            entry = &m_flagPipeline[static_cast<size_t>(std::countr_zero(freeMask))];
    }
    else
    {
        for (FlagPipelineEntry &candidate : m_flagPipeline)
        {
            if (!candidate.valid)
            {
                entry = &candidate;
                break;
            }
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
    vuNoteQueued(this, entry->readyCycle, &VuSideState::flagMask, static_cast<uint32_t>(entry - m_flagPipeline.data()));
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    const uint32_t productSticky = calculateFmacProductSticky(dest);
    t_fmacProductOverflow = (productSticky & 0x8u) != 0u;
    normalizeFmacResult(result, dest, laneFlags);
    t_fmacProductOverflow = false;
    updateFmacFlags(laneFlags, dest, productSticky);
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    const uint32_t productSticky = calculateFmacProductSticky(dest);
    t_fmacProductOverflow = (productSticky & 0x8u) != 0u;
    normalizeFmacResult(result, dest, laneFlags);
    t_fmacProductOverflow = false;
    updateFmacFlags(laneFlags, dest, productSticky);
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            vuNoteQueued(this, entry.readyCycle, &VuSideState::flagMask, static_cast<uint32_t>(&entry - m_flagPipeline.data()));
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            vuNoteQueued(this, entry.readyCycle, &VuSideState::flagMask, static_cast<uint32_t>(&entry - m_flagPipeline.data()));
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            vuNoteQueued(this, entry.readyCycle, &VuSideState::flagMask, static_cast<uint32_t>(&entry - m_flagPipeline.data()));
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
    vuNoteReady(this, m_fdiv.readyCycle);
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            vuNoteQueued(this, entry.readyCycle, &VuSideState::efuMask, static_cast<uint32_t>(&entry - m_efu.data()));
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            store.readyCycle = m_cycle + 1u;
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            vuNoteQueued(this, store.readyCycle, &VuSideState::storeMask, static_cast<uint32_t>(&store - m_storePipeline.data()));
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    PendingVfWrite *slot = nullptr;
    const VuSideState *side = vuSide(this);
    if (side && side->masksValid)
    {
        const uint32_t freeMask = ~side->vfMask & ((1u << kMaxPendingVfWrites) - 1u);
        if (freeMask != 0u)
            slot = &m_vfWritePipeline[static_cast<size_t>(std::countr_zero(freeMask))];
    }
    else
    {
        for (PendingVfWrite &write : m_vfWritePipeline)
        {
            if (!write.valid)
            {
                slot = &write;
                break;
            }
        }
    }
    if (!slot)
    {
        reportReservedInstruction(false, 0xFFFFFFF7u);
        return;
    }

    PendingVfWrite &write = *slot;
    write.valid = true;
    write.readyCycle = m_cycle + latency;
    write.sequence = ++m_nextWriteSequence;
    write.reg = reg;
    write.laneMask = laneMask;
    write.value[0] = value[0];
    write.value[1] = value[1];
    write.value[2] = value[2];
    write.value[3] = value[3];
    std::array<uint64_t, 4> &latest = m_vfLatestWrite[reg];
    if ((laneMask & 0x8u) != 0u)
        latest[0] = write.sequence;
    if ((laneMask & 0x4u) != 0u)
        latest[1] = write.sequence;
    if ((laneMask & 0x2u) != 0u)
        latest[2] = write.sequence;
    if ((laneMask & 0x1u) != 0u)
        latest[3] = write.sequence;
    vuNoteQueued(this, write.readyCycle, &VuSideState::vfMask, static_cast<uint32_t>(&write - m_vfWritePipeline.data()));
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            vuNoteQueued(this, write.readyCycle, &VuSideState::viMask, static_cast<uint32_t>(&write - m_viWritePipeline.data()));
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            vuNoteQueued(this, write.readyCycle, &VuSideState::accMask, static_cast<uint32_t>(&write - m_accWritePipeline.data()));
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelines()
{
    VuSideState *side = vuSide(this);
    if (side && m_cycle < side->nextCommit)
        return;

    if (side && side->masksValid)
    {
        // Same commits, same order as the full scan below, but only over valid entries.
        uint64_t nextReady = ~0ull;
        for (uint32_t mask = side->flagMask; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            FlagPipelineEntry &entry = m_flagPipeline[index];
            if (entry.readyCycle > m_cycle)
            {
                nextReady = std::min(nextReady, entry.readyCycle);
                continue;
            }
            if (entry.writesMac)
                m_state.mac = entry.mac;
            if (entry.writesStatus)
            {
                const uint32_t current = entry.status & 0xFu;
                m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
            }
            if (entry.writesSticky)
                m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            if (entry.writesClip)
                m_state.clip = entry.clip;
            entry = {};
            side->flagMask &= ~(1u << index);
        }

        if (m_fdiv.valid)
        {
            if (m_fdiv.readyCycle <= m_cycle)
            {
                m_state.q = m_fdiv.value;
                const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
                m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
                m_fdiv = {};
            }
            else
            {
                nextReady = std::min(nextReady, m_fdiv.readyCycle);
            }
        }

        for (uint32_t mask = side->efuMask; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            ScalarPipelineEntry &entry = m_efu[index];
            if (entry.readyCycle <= m_cycle)
            {
                m_state.p = entry.value;
                entry = {};
                side->efuMask &= ~(1u << index);
            }
            else
            {
                nextReady = std::min(nextReady, entry.readyCycle);
            }
        }

        for (uint32_t mask = side->storeMask; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            PendingStore &store = m_storePipeline[index];
            if (store.readyCycle > m_cycle)
            {
                nextReady = std::min(nextReady, store.readyCycle);
                continue;
            }
            if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
            {
                uint32_t oldWords[4]{};
                std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
                for (uint32_t component = 0; component < 4u; ++component)
                {
                    if ((store.laneMask & laneForComponent(component)) != 0u)
                        oldWords[component] = store.words[component];
                }
                std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
            }
            store = {};
            side->storeMask &= ~(1u << index);
        }

        for (uint32_t mask = side->vfMask; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            PendingVfWrite &write = m_vfWritePipeline[index];
            if (write.readyCycle > m_cycle)
            {
                nextReady = std::min(nextReady, write.readyCycle);
                continue;
            }
            const std::array<uint64_t, 4> &latest = m_vfLatestWrite[write.reg];
            float *reg = m_state.vf[write.reg];
            if ((write.laneMask & 0x8u) != 0u && latest[0] == write.sequence)
                reg[0] = write.value[0];
            if ((write.laneMask & 0x4u) != 0u && latest[1] == write.sequence)
                reg[1] = write.value[1];
            if ((write.laneMask & 0x2u) != 0u && latest[2] == write.sequence)
                reg[2] = write.value[2];
            if ((write.laneMask & 0x1u) != 0u && latest[3] == write.sequence)
                reg[3] = write.value[3];
            write.valid = false;
            side->vfMask &= ~(1u << index);
        }

        for (uint32_t mask = side->viMask; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            PendingViWrite &write = m_viWritePipeline[index];
            if (write.readyCycle > m_cycle)
            {
                nextReady = std::min(nextReady, write.readyCycle);
                continue;
            }
            if (m_viLatestWrite[write.reg] == write.sequence)
                m_state.vi[write.reg] = static_cast<int16_t>(write.value);
            write.valid = false;
            side->viMask &= ~(1u << index);
        }

        for (uint32_t mask = side->accMask; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            PendingAccWrite &write = m_accWritePipeline[index];
            if (write.readyCycle > m_cycle)
            {
                nextReady = std::min(nextReady, write.readyCycle);
                continue;
            }
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((write.laneMask & laneForComponent(component)) != 0u &&
                    m_accLatestWrite[component] == write.sequence)
                {
                    m_state.acc[component] = write.value[component];
                }
            }
            write.valid = false;
            side->accMask &= ~(1u << index);
        }

        side->nextCommit = nextReady;
        return;
    }

    uint64_t next = ~0ull;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
            continue;
        if (entry.readyCycle > m_cycle)
        {
            next = std::min(next, entry.readyCycle);
            continue;
        }

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
    }

    if (m_fdiv.valid)
    {
        if (m_fdiv.readyCycle <= m_cycle)
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
        }
        else
        {
            next = std::min(next, m_fdiv.readyCycle);
        }
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
            continue;
        if (entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
        else
        {
            next = std::min(next, entry.readyCycle);
        }
    }

    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
            continue;
        if (store.readyCycle > m_cycle)
        {
            next = std::min(next, store.readyCycle);
            continue;
        }
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
    }

    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
            continue;
        if (write.readyCycle > m_cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
    }

    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
            continue;
        if (write.readyCycle > m_cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
    }

    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
            continue;
        if (write.readyCycle > m_cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
    }

    if (side)
        side->nextCommit = next;
}

// Rayman bring-up: explain why an XGKICK (VU1 -> GS packet) was rejected.
extern const uint8_t *g_ps2xDbgVif1Stream;
extern uint32_t g_ps2xDbgVif1StreamSize;
extern uint32_t g_ps2xDbgVif1MscalPos;
extern uint32_t g_ps2xDbgVif1MscalCount;
extern VIFRegisters g_ps2xDbgVif1RegsAtStreamStart;
extern VIFRegisters g_ps2xDbgVif1RegsAtMscal;
extern uint8_t g_ps2xDbgVu1DataAtMscal[PS2_VU1_DATA_SIZE];
extern int g_ps2xDbgVu1DumpsRemaining;

namespace
{
    int g_xgkickDiagCount = 0;
    const uint8_t *g_dbgVuCode = nullptr;
    uint32_t g_dbgVuCodeSize = 0u;
    uint32_t g_dbgProgramStart = 0u;
    uint32_t g_dbgKickIssuePc = 0u;
    uint64_t g_dbgKickIssueCycle = 0u;
    int g_dbgDumpIndex = 0;

    void writeBinary(const char *path, const void *data, size_t size)
    {
        if (!data || size == 0u)
            return;
        if (std::FILE *f = std::fopen(path, "wb"))
        {
            std::fwrite(data, 1, size, f);
            std::fclose(f);
        }
    }

    void writeVifRegs(std::FILE *f, const char *label, const VIFRegisters &r)
    {
        std::fprintf(f, "%s: stat=0x%X cycle=0x%X mode=%u mask=0x%08X base=0x%X ofst=0x%X tops=0x%X top=0x%X itops=0x%X itop=0x%X\n",
                     label, r.stat, r.cycle, r.mode, r.mask, r.base, r.ofst, r.tops, r.top, r.itops, r.itop);
        std::fprintf(f, "%s: row=%08X %08X %08X %08X col=%08X %08X %08X %08X\n", label,
                     r.row[0], r.row[1], r.row[2], r.row[3], r.col[0], r.col[1], r.col[2], r.col[3]);
    }

    void dumpVu1Snapshot(const char *why, const VU1State &st, uint64_t cycle, uint32_t kickSrc, uint32_t copied,
                         const uint8_t *packet, const uint8_t *vuData, uint32_t vuDataSize)
    {
        if (g_ps2xDbgVu1DumpsRemaining <= 0)
            return;
        --g_ps2xDbgVu1DumpsRemaining;
        const int n = g_dbgDumpIndex++;
        char path[256];
        std::snprintf(path, sizeof(path), "out/vu1dump%d_code.bin", n);
        writeBinary(path, g_dbgVuCode, g_dbgVuCodeSize);
        std::snprintf(path, sizeof(path), "out/vu1dump%d_data_after.bin", n);
        writeBinary(path, vuData, vuDataSize);
        std::snprintf(path, sizeof(path), "out/vu1dump%d_data_at_mscal.bin", n);
        writeBinary(path, g_ps2xDbgVu1DataAtMscal, sizeof(g_ps2xDbgVu1DataAtMscal));
        std::snprintf(path, sizeof(path), "out/vu1dump%d_vif_stream.bin", n);
        writeBinary(path, g_ps2xDbgVif1Stream, g_ps2xDbgVif1StreamSize);
        std::snprintf(path, sizeof(path), "out/vu1dump%d_packet.bin", n);
        writeBinary(path, packet, copied);
        std::snprintf(path, sizeof(path), "out/vu1dump%d_info.txt", n);
        if (std::FILE *f = std::fopen(path, "w"))
        {
            std::fprintf(f, "why=%s\n", why);
            std::fprintf(f, "programStart=0x%X pcNow=0x%X cycle=%llu kickIssuePc=0x%X kickIssueCycle=%llu kickSrc=0x%X copied=0x%X\n",
                         g_dbgProgramStart, st.pc, static_cast<unsigned long long>(cycle), g_dbgKickIssuePc,
                         static_cast<unsigned long long>(g_dbgKickIssueCycle), kickSrc, copied);
            std::fprintf(f, "top=0x%X itop=0x%X\n", st.top, st.itop);
            std::fprintf(f, "vifStreamSize=0x%X mscalPos=0x%X mscalIndexInStream=%u\n", g_ps2xDbgVif1StreamSize,
                         g_ps2xDbgVif1MscalPos, g_ps2xDbgVif1MscalCount);
            writeVifRegs(f, "vifAtStreamStart", g_ps2xDbgVif1RegsAtStreamStart);
            writeVifRegs(f, "vifAtMscal", g_ps2xDbgVif1RegsAtMscal);
            for (int r = 0; r < 16; ++r)
                std::fprintf(f, "vi%d=0x%X\n", r, static_cast<uint32_t>(st.vi[r]) & 0xFFFFu);
            for (int r = 0; r < 32; ++r)
            {
                uint32_t w[4];
                std::memcpy(w, st.vf[r], sizeof(w));
                std::fprintf(f, "vf%d=%08X %08X %08X %08X\n", r, w[0], w[1], w[2], w[3]);
            }
            uint32_t accw[4], qw = 0, pw = 0, iw = 0;
            std::memcpy(accw, st.acc, sizeof(accw));
            std::memcpy(&qw, &st.q, 4);
            std::memcpy(&pw, &st.p, 4);
            std::memcpy(&iw, &st.i, 4);
            std::fprintf(f, "acc=%08X %08X %08X %08X q=%08X p=%08X i=%08X\n", accw[0], accw[1], accw[2], accw[3], qw, pw, iw);
            std::fclose(f);
        }
        std::fprintf(stderr, "[xgkick-diag] wrote out/vu1dump%d_* (program 0x%X, XGKICK at 0x%X)\n", n, g_dbgProgramStart,
                     g_dbgKickIssuePc);
    }
}

static void ps2xDumpXgkickOverflow(const char *why, uint32_t pc, uint64_t cycle, uint32_t top, uint32_t itop,
                                   uint32_t sourceAddress, uint32_t copiedBytes, const uint8_t *packet,
                                   const uint8_t *vuData, uint32_t vuDataSize, const int32_t *vi)
{
    if (g_xgkickDiagCount >= 6)
        return;
    ++g_xgkickDiagCount;
    std::fprintf(stderr, "[xgkick-diag] %s pc=0x%X cycle=%llu src=0x%X (qw 0x%X) copied=0x%X top=0x%X itop=0x%X\n",
                 why, pc, static_cast<unsigned long long>(cycle), sourceAddress, sourceAddress / 16u, copiedBytes, top, itop);
    std::fprintf(stderr, "[xgkick-diag]   vi:");
    for (int r = 0; r < 16; ++r)
        std::fprintf(stderr, " %d=0x%X", r, static_cast<uint32_t>(vi[r]) & 0xFFFFu);
    std::fprintf(stderr, "\n");
    // Walk the GIF tags that were assembled so far.
    uint32_t off = 0;
    for (int t = 0; t < 12 && off + 16u <= copiedBytes; ++t)
    {
        uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, packet + off, 8);
        std::memcpy(&hi, packet + off + 8, 8);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t eop = static_cast<uint32_t>((lo >> 15) & 1u);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        std::fprintf(stderr, "[xgkick-diag]   tag@+0x%X lo=%016llX hi=%016llX nloop=%u eop=%u flg=%u nreg=%u\n", off,
                     static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), nloop, eop, flg, nreg);
        uint64_t bytes = 16u;
        if (flg == 0u)
            bytes += static_cast<uint64_t>(nloop) * nreg * 16u;
        else if (flg == 1u)
            bytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
        else
            bytes += static_cast<uint64_t>(nloop) * 16u;
        if (eop || bytes > 0x10000u)
            break;
        off += static_cast<uint32_t>(bytes);
    }
    // Raw VU1 data around the kick address.
    if (vuData && vuDataSize >= 16u)
    {
        for (uint32_t q = 0; q < 6u; ++q)
        {
            const uint32_t a = (sourceAddress + q * 16u) % vuDataSize;
            uint32_t words[4];
            std::memcpy(words, vuData + a, 16);
            std::fprintf(stderr, "[xgkick-diag]   vu[0x%04X] %08X %08X %08X %08X\n", a, words[0], words[1], words[2], words[3]);
        }
    }
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            ps2xDumpXgkickOverflow("packet never reached EOP", m_state.pc, m_cycle, m_state.top, m_state.itop,
                                   m_xgkick.sourceAddress, m_xgkick.copiedBytes, m_xgkick.packet.data(),
                                   m_activeVuData, m_activeVuDataSize, m_state.vi);
            dumpVu1Snapshot("packet never reached EOP", m_state, m_cycle, m_xgkick.sourceAddress, m_xgkick.copiedBytes,
                            m_xgkick.packet.data(), m_activeVuData, m_activeVuDataSize);
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        const uint32_t firstSource = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (firstSource + 16u <= m_activeVuDataSize)
        {
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes, m_activeVuData + firstSource, 16u);
        }
        else
        {
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
            }
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u || format == 3u) // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                ps2xDumpXgkickOverflow("GIF tag larger than buffer", m_state.pc, m_cycle, m_state.top, m_state.itop,
                                       m_xgkick.sourceAddress, m_xgkick.copiedBytes, m_xgkick.packet.data(),
                                       m_activeVuData, m_activeVuDataSize, m_state.vi);
                dumpVu1Snapshot("GIF tag larger than buffer", m_state, m_cycle, m_xgkick.sourceAddress, m_xgkick.copiedBytes,
                                m_xgkick.packet.data(), m_activeVuData, m_activeVuDataSize);
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    // PS2X_VU1_DUMP_RGBA=<A<<24 | B<<16 | G<<8 | R>: VU1 snapshot (dumpVu1Snapshot: microcode, data memory at MSCAL,
    // VIF stream) at the first XGKICK whose packet sets that colour in a PACKED RGBAQ. Bug hunt: which microprogram
    // and which input produce a given primitive.
    static const char *dumpRgbaEnv = std::getenv("PS2X_VU1_DUMP_RGBA");
    static const uint32_t dumpRgba = dumpRgbaEnv ? static_cast<uint32_t>(std::strtoul(dumpRgbaEnv, nullptr, 0)) : 0u;
    static bool dumpedRgba = false;
    if (dumpRgbaEnv && !dumpedRgba)
    {
        const uint8_t *p = m_xgkick.packet.data();
        for (uint32_t off = 0u; off + 16u <= m_xgkick.totalBytes && !dumpedRgba;)
        {
            uint64_t tagLo = 0u, regs = 0u;
            std::memcpy(&tagLo, p + off, 8u);
            std::memcpy(&regs, p + off + 8u, 8u);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3u);
            const uint32_t nreg = ((tagLo >> 60) & 0xFu) ? static_cast<uint32_t>((tagLo >> 60) & 0xFu) : 16u;
            off += 16u;
            if (flg != 0u)
            {
                off += flg == 1u ? ((nloop * nreg + 1u) & ~1u) * 8u : nloop * 16u;
                continue;
            }
            for (uint32_t i = 0u; i < nloop * nreg && off + 16u <= m_xgkick.totalBytes; ++i, off += 16u)
            {
                if (((regs >> (4u * (i % nreg))) & 0xFu) != 1u)
                    continue;
                uint32_t w[4];
                std::memcpy(w, p + off, 16u);
                const uint32_t rgba = (w[0] & 0xFFu) | ((w[1] & 0xFFu) << 8) | ((w[2] & 0xFFu) << 16) | ((w[3] & 0xFFu) << 24);
                if (rgba == dumpRgba)
                {
                    dumpedRgba = true;
                    g_ps2xDbgVu1DumpsRemaining = std::max(g_ps2xDbgVu1DumpsRemaining, 1);
                    dumpVu1Snapshot("PS2X_VU1_DUMP_RGBA match", m_state, m_cycle, m_xgkick.sourceAddress, m_xgkick.totalBytes,
                                    m_xgkick.packet.data(), m_activeVuData, m_activeVuDataSize);
                    std::fprintf(stderr, "[vu1-dump] RGBA %08x found in an XGKICK packet: out/vu1dump*_info.txt\n", rgba);
                    break;
                }
            }
        }
    }

    // PS2X_VU1_KICK_HASH=1: a running FNV-1a hash of every XGKICK packet, printed every 600 guest VSyncs
    // (`[vu1-hash]`). Two builds that print the same hashes sent the GS the same bytes: an equivalence check for
    // VU1 interpreter changes.
    static const bool kickHash = []
    {
        const char *env = std::getenv("PS2X_VU1_KICK_HASH");
        return env && *env && *env != '0';
    }();
    if (kickHash)
    {
        static uint64_t hash = 1469598103934665603ull, kicks = 0, nextReport = 600;
        for (uint32_t i = 0; i < m_xgkick.totalBytes; ++i)
            hash = (hash ^ m_xgkick.packet[i]) * 1099511628211ull;
        ++kicks;
        const uint64_t vsync = ps2xGuestVsync();
        if (vsync >= nextReport)
        {
            std::fprintf(stderr, "[vu1-hash] vsync %llu kicks %llu hash %016llx\n", static_cast<unsigned long long>(vsync),
                         static_cast<unsigned long long>(kicks), static_cast<unsigned long long>(hash));
            nextReport = (vsync / 600u + 1u) * 600u;
        }
    }

    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    g_dbgKickIssuePc = m_state.pc;
    g_dbgKickIssueCycle = m_cycle;
    // Field-wise reset: zeroing the 64 KiB packet buffer per XGKICK was a large cost.
    m_xgkick.totalBytes = 0;
    m_xgkick.copiedBytes = 0;
    m_xgkick.currentTagEnd = 0;
    m_xgkick.currentTagEop = false;
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            const std::array<uint64_t, 4> &regReady = m_vfReady[access.reg];
            if ((access.lanes & 0x8u) != 0u)
                ready = std::max(ready, regReady[0]);
            if ((access.lanes & 0x4u) != 0u)
                ready = std::max(ready, regReady[1]);
            if ((access.lanes & 0x2u) != 0u)
                ready = std::max(ready, regReady[2]);
            if ((access.lanes & 0x1u) != 0u)
                ready = std::max(ready, regReady[3]);
        }
        for (uint32_t mask = static_cast<uint32_t>(usage->viRead) & 0xFFFEu; mask != 0u; mask &= mask - 1u)
            ready = std::max(ready, m_viReady[static_cast<size_t>(std::countr_zero(mask))]);
        const uint8_t accLanes = usage->accRead;
        if (accLanes != 0u)
        {
            if ((accLanes & 0x8u) != 0u)
                ready = std::max(ready, m_accReady[0]);
            if ((accLanes & 0x4u) != 0u)
                ready = std::max(ready, m_accReady[1]);
            if ((accLanes & 0x2u) != 0u)
                ready = std::max(ready, m_accReady[2]);
            if ((accLanes & 0x1u) != 0u)
                ready = std::max(ready, m_accReady[3]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    auto markVf = [this](uint8_t reg, uint8_t lanes, uint64_t ready)
    {
        std::array<uint64_t, 4> &regReady = m_vfReady[reg];
        if ((lanes & 0x8u) != 0u)
            regReady[0] = ready;
        if ((lanes & 0x4u) != 0u)
            regReady[1] = ready;
        if ((lanes & 0x2u) != 0u)
            regReady[2] = ready;
        if ((lanes & 0x1u) != 0u)
            regReady[3] = ready;
    };

    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        markVf(lowerWrite.reg, lowerWrite.lanes, m_cycle + latency);
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        markVf(upperWrite.reg, upperWrite.lanes, m_cycle + latency);
    }

    for (uint32_t mask = static_cast<uint32_t>(decoded.lowerUsage.viWrite) & 0xFFFEu; mask != 0u; mask &= mask - 1u)
        m_viReady[static_cast<size_t>(std::countr_zero(mask))] =
            m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    const uint8_t accLanes = decoded.upperUsage.accWrite;
    if (accLanes != 0u)
    {
        const uint64_t ready = m_cycle + kAccForwardLatency;
        if ((accLanes & 0x8u) != 0u)
            m_accReady[0] = ready;
        if ((accLanes & 0x4u) != 0u)
            m_accReady[1] = ready;
        if ((accLanes & 0x2u) != 0u)
            m_accReady[2] = ready;
        if ((accLanes & 0x1u) != 0u)
            m_accReady[3] = ready;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    bool readsMacStatus = codeSize / 8u > kMaxDecodedPairs; // code past the cache: unknown, keep the flags
    for (uint32_t i = 0; i < pairCount; ++i)
    {
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);
        const DecodedInstructionPair &pair = m_decodedCodeCache[i];
        if (!pair.iBit)
        {
            const uint32_t op = pair.lower >> 25;
            if (op == 0x14u || op == 0x16u || op == 0x17u || op == 0x18u || op == 0x1Au || op == 0x1Bu)
                readsMacStatus = true;
        }
    }
    if (VuSideState *side = vuSide(this))
        side->macStatusUnread = !readsMacStatus;

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    g_dbgProgramStart = startPC & microAddressMask();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    static const uint64_t vu0TraceStartVsync = []
    {
        const char *env = std::getenv("PS2X_VU0_TRACE");
        return (env && *env) ? std::strtoull(env, nullptr, 10) : ~0ull;
    }();
    g_vu0TraceThisRun = false;
    if (m_unit == Unit::VU0 && vu0TraceStartVsync != ~0ull)
    {
        // PS2X_VU0_TRACE_MATCH=<value>: only runs with a VF input lane within 0.01 of that value are traced.
        static const char *matchEnv = std::getenv("PS2X_VU0_TRACE_MATCH");
        static const float matchValue = (matchEnv && *matchEnv) ? std::strtof(matchEnv, nullptr) : 0.0f;
        bool matches = !(matchEnv && *matchEnv);
        for (uint32_t r = 1; r < 32u && !matches; ++r)
            for (uint32_t lane = 0; lane < 4u; ++lane)
                if (std::fabs(m_state.vf[r][lane] - matchValue) < 0.01f)
                    matches = true;
        if (ps2xGuestVsync() == vu0TraceStartVsync && matches)
        {
            g_vu0TraceThisRun = true;
            static uint32_t starts = 0u;
            if (starts == 0u)
            {
                if (FILE *dump = std::fopen("out/vu0_micro_trace.bin", "wb"))
                {
                    std::fwrite(vuCode, 1u, codeSize, dump);
                    std::fclose(dump);
                    std::fprintf(stderr, "[vu0-trace] VU0 micro memory written to out/vu0_micro_trace.bin\n");
                }
            }
            if (starts++ < 3000u)
            {
                std::fprintf(stderr, "[vu0-trace] start pc %04x status=%03x mac=%04x clip=%06x q=%g", m_state.pc, m_state.status,
                             m_state.mac, m_state.clip, m_state.q);
                for (uint32_t r = 1; r < 32u; ++r)
                    if (m_state.vf[r][0] != 0.0f || m_state.vf[r][1] != 0.0f || m_state.vf[r][2] != 0.0f || m_state.vf[r][3] != 0.0f)
                        std::fprintf(stderr, " vf%u=%g,%g,%g,%g", r, m_state.vf[r][0], m_state.vf[r][1], m_state.vf[r][2], m_state.vf[r][3]);
                for (uint32_t r = 1; r < 16u; ++r)
                    if (m_state.vi[r] != 0)
                        std::fprintf(stderr, " vi%u=%d", r, m_state.vi[r]);
                std::fprintf(stderr, "\n");
            }
        }
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    g_dbgVuCode = vuCode;
    g_dbgVuCodeSize = codeSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    // VF results are written at once (readers stall on m_vfReady anyway). PS2X_VU1_QUEUED_VF=1 restores the queued
    // writes (commit at the ready cycle), for A/B checks with PS2X_VU1_KICK_HASH.
    static const bool immediateVf = []
    {
        const char *env = std::getenv("PS2X_VU1_QUEUED_VF");
        return !(env && *env && *env != '0');
    }();
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        // Use the decoded-code cache entry in place when it is current (avoids a ~100-byte copy).
        DecodedInstructionPair decodedCopy;
        const DecodedInstructionPair *decodedPtr = nullptr;
        if (memory && (m_state.pc & 7u) == 0u && m_decodedCodeCacheValid && m_cachedVuCode == vuCode &&
            m_cachedMemory == memory && m_cachedCodeSize == codeSize && (m_state.pc / 8u) < kMaxDecodedPairs &&
            m_cachedCodeGeneration == (m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration()))
        {
            decodedPtr = &m_decodedCodeCache[m_state.pc / 8u];
        }
        else
        {
            decodedCopy = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
            decodedPtr = &decodedCopy;
        }
        const DecodedInstructionPair &decoded = *decodedPtr;
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        {
            const uint32_t viMask = static_cast<uint32_t>(decoded.lowerUsage.viWrite) & 0xFFFEu;
            if (viMask != 0u)
            {
                writtenVi = static_cast<uint8_t>(std::countr_zero(viMask));
                oldVi = m_state.vi[writtenVi];
            }
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (!immediateVf && hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (!immediateVf && hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        // A taken branch whose delay slot is this pair: the jump happens after this pair. If this pair issues another
        // taken branch ("branch in a branch delay slot"), the hardware jumps to the first target, runs that one
        // instruction as the second branch's delay slot, then jumps to the second target (PCSX2: takedelaybranch).
        // Rayman's VU0 collision microcode relies on it (0x0718 IBEQ with B 0x0438 in its delay slot: the first
        // target 0x0C58 clears vi13 = "no contact"); without it every triangle edge reported a contact.
        const bool inTakenDelaySlot = m_state.branchPending && m_state.branchDelay == 0u;
        const uint32_t pendingFirstTarget = m_state.branchTarget;

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

        if (immediateVf)
        {
            // The result stays in the register now. Readers still stall until the write's ready cycle
            // (markPairWrites -> m_vfReady), and the VU interlocks VF reads, so they see the same value as with the
            // queued write; the new sequence number makes older queued writes to these lanes stale.
            auto noteImmediate = [this](uint8_t reg, uint8_t lanes)
            {
                const uint64_t sequence = ++m_nextWriteSequence;
                std::array<uint64_t, 4> &latest = m_vfLatestWrite[reg];
                if ((lanes & 0x8u) != 0u)
                    latest[0] = sequence;
                if ((lanes & 0x4u) != 0u)
                    latest[1] = sequence;
                if ((lanes & 0x2u) != 0u)
                    latest[2] = sequence;
                if ((lanes & 0x1u) != 0u)
                    latest[3] = sequence;
            };
            if (hasUpperWrite)
                noteImmediate(upperWrite.reg, upperWrite.lanes);
            if (hasDistinctLowerWrite)
                noteImmediate(lowerWrite.reg, lowerWrite.lanes);
        }
        if (!immediateVf && hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (!immediateVf && hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (decoded.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        // PS2X_VU0_TRACE=<guest vsync>: every VU0 microprogram pair executed during that VSync (`[vu0-trace]`: pc,
        // cycle, raw upper / lower, the VF / VI written with their new values, ACC, Q and the flags as FSxx / FMxx read
        // them now). Decode the words with tools/vudis.py. Bug hunt: Rayman's collision runs on VU0 (vcallms 0).
        static const uint64_t vu0TraceVsync = []
        {
            const char *env = std::getenv("PS2X_VU0_TRACE");
            return (env && *env) ? std::strtoull(env, nullptr, 10) : ~0ull;
        }();
        if (m_unit == Unit::VU0 && vu0TraceVsync != ~0ull && g_vu0TraceThisRun && ps2xGuestVsync() == vu0TraceVsync)
        {
            static uint32_t traced = 0u;
            if (traced++ < 60000u)
            {
                std::fprintf(stderr, "[vu0-trace] pc %04x cyc %llu %08x %08x", m_state.pc, static_cast<unsigned long long>(m_cycle),
                             decoded.upper, decoded.lower);
                if (hasUpperWrite)
                    std::fprintf(stderr, " | vf%u=%g %g %g %g", upperWrite.reg, m_state.vf[upperWrite.reg][0],
                                 m_state.vf[upperWrite.reg][1], m_state.vf[upperWrite.reg][2], m_state.vf[upperWrite.reg][3]);
                if (hasDistinctLowerWrite)
                    std::fprintf(stderr, " | vf%u=%g %g %g %g", lowerWrite.reg, m_state.vf[lowerWrite.reg][0],
                                 m_state.vf[lowerWrite.reg][1], m_state.vf[lowerWrite.reg][2], m_state.vf[lowerWrite.reg][3]);
                if (writtenVi != 0u)
                    std::fprintf(stderr, " | vi%u=%d", writtenVi, m_state.vi[writtenVi]);
                if (decoded.upperUsage.accWrite != 0u)
                    std::fprintf(stderr, " | acc=%g %g %g %g", newAcc[0], newAcc[1], newAcc[2], newAcc[3]);
                std::fprintf(stderr, " | q=%g status=%03x mac=%04x\n", m_state.q, m_state.status, m_state.mac);
            }
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (inTakenDelaySlot && m_state.branchPending && m_state.branchDelay != 0u)
        {
            // Branch in the delay slot of a taken branch: go to the first target now; the second branch (just
            // issued, delay 1) fires after the instruction there.
            m_state.pc = pendingFirstTarget & microAddressMask();
            m_state.branchDelay = 0u;
        }
        else if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}
