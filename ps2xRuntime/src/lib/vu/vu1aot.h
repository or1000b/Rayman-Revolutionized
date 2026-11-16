// VU1 ahead-of-time translation: what the generated programs (src/lib/vu/aot/prog_*.cpp, written by
// tools/vu1aot/vu1gen.py) run on. Plan and rules: docs/vu1_aot.md.
//
// The native code must reproduce the interpreter (ps2_vu1_core.cpp / _upper.cpp / _lower.cpp) bit for bit and cycle
// for cycle. The generator works out the interpreter's timing at generation time; this header holds the value side:
// every helper below is a copy of one interpreter case, with the same expressions in the same order (the run's
// rounding mode is the interpreter's: round toward zero). `PS2X_VU1AOT=diff` compares the two on every run.
//
// Not included by ps2_runtime.h (changing it rebuilds only the VU files).
#pragma once

#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#if defined(_MSC_VER)
#define VU1AOT_INLINE __forceinline
#else
#define VU1AOT_INLINE inline __attribute__((always_inline))
#endif

namespace vu1aot
{
    // Why a native run gave up; the dispatcher then undoes it and the interpreter runs the whole program.
    enum Bail : int
    {
        BailNone = 0,
        BailBudget = 1,        // the run would pass the cycle budget (the interpreter stops mid-program there)
        BailKickBusy = 2,      // XGKICK while the previous packet is still transferring (data-dependent stall)
        BailJrTarget = 3,      // JR / JALR to an address the generator did not cover
        BailKickError = 4,     // XGKICK packet the interpreter would reject (bad tag / overflow)
        BailUnsupported = 5,   // a path the generator could not translate
        BailFlagOverflow = 6,  // more pending flag entries than the interpreter has slots
        BailRunaway = 7,
    };

    // PATH1 transfer of one XGKICK packet, as VU1Interpreter::progressXgkick (one qword per 2 cycles).
    struct Kick
    {
        bool active = false;
        bool tagEop = false;
        uint32_t source = 0;
        uint32_t copied = 0;
        uint32_t tagEnd = 0;
        uint32_t total = 0;
        uint32_t credit = 0;
    };

    struct Ctx
    {
        VU1State s{};                  // registers: a copy, written back when the run succeeds
        uint8_t *data = nullptr;       // VU1 data memory
        uint32_t dataSize = 0;
        uint64_t cycles = 0;           // cycles of this run so far (the interpreter's m_cycle - start)
        uint64_t budget = 0;           // the interpreter's maxCycles
        bool flagsLive = true;         // MAC / status flags computed (the interpreter's !macStatusUnread)
        uint32_t workingClip = 0;      // the interpreter's m_workingClip
        int bail = BailNone;
        uint32_t bailPc = 0;

        // XGKICK: the packet being transferred and the packets finished in this run (delivered after success).
        Kick kick;
        uint8_t *kickBuf = nullptr;    // 64 KiB
        std::vector<uint8_t> packets;
        std::vector<uint32_t> packetSizes;

        // Stores into data memory, oldest first, so a bailed run can be undone (addresses are qword aligned).
        bool logUndo = true;
        struct Undo
        {
            uint32_t address;
            uint8_t old[16];
        };
        std::vector<Undo> undo;

        // Pending results (the interpreter's queues), indexed by the generator.
        struct FlagEntry
        {
            uint32_t mac, status, extraSticky, clip;
            bool writesMac, writesStatus, writesSticky, writesClip;
            bool on; // the interpreter queued it (FMAC entries only when flags are live)
        };
        FlagEntry flags[8]{};
        float pendingQ = 0.0f;
        uint32_t pendingQDi = 0;
        float pendingP[2]{};

        // VF ready offsets after the run (cycles past the end; 0 = ready), for the interpreter's next MSCNT.
        uint8_t vfResidue[32][4]{};

        // The FMAC operands as execUpper reads them: vuNormalizeOperand of every lane of every VF register and of ACC
        // (vu1aot_ops.h, ops::nz4). The SSE templates read these instead of clamping a register on every use (~7
        // vector operations per operand). Set for all registers when a run starts (ops::initNv); every write of a
        // register in the generated code refreshes its copy before the next read. Scratch: never part of the state.
        alignas(16) float nv[32][4]{};
        alignas(16) float nacc[4]{};
        // The branch hazard the last pair leaves (the interpreter's m_viBranchBackup*).
        bool branchBackupValid = false;
        uint8_t branchBackupReg = 0;
        int32_t branchBackupValue = 0;
    };

    // ---------------------------------------------------------------- XGKICK

    // An address in data memory, wrapped. VU data memory is a power of two (16 / 4 KiB): a mask instead of the
    // division of `%` (kickQword ran it per qword of every XGKICK: ~5% of the graphics thread, 2026-11-16).
    inline uint32_t wrapData(const Ctx &c, uint32_t address)
    {
        return (c.dataSize & (c.dataSize - 1u)) == 0u ? (address & (c.dataSize - 1u)) : (address % c.dataSize);
    }

    VU1AOT_INLINE void kickStart(Ctx &c, uint32_t qwordAddress)
    {
        c.kick = {};
        c.kick.active = true;
        c.kick.source = wrapData(c, qwordAddress * 16u);
        c.kick.credit = 1u; // XGKICK's issue cycle counts toward PATH1 (startXgkick)
    }

    // One qword of PATH1 (progressXgkick: one per 2 VU cycles of credit). Returns false on a packet the interpreter
    // rejects.
    inline bool kickQword(Ctx &c)
    {
        Kick &k = c.kick;
        if (k.copied > 0x10000u - 16u)
            return false;
        const uint32_t qwordOffset = k.copied;
        const uint32_t firstSource = wrapData(c, k.source + k.copied);
        if (firstSource + 16u <= c.dataSize)
            std::memcpy(c.kickBuf + k.copied, c.data + firstSource, 16u);
        else
            for (uint32_t i = 0; i < 16u; ++i)
                c.kickBuf[k.copied + i] = c.data[wrapData(c, k.source + k.copied + i)];
        k.copied += 16u;
        if (k.tagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, c.kickBuf + qwordOffset, sizeof(tagLo));
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
            else
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            if (tagBytes > 0x10000u - qwordOffset)
                return false;
            k.tagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            k.tagEop = ((tagLo >> 15) & 1u) != 0u;
            if (k.tagEop)
                k.total = k.tagEnd;
        }
        if (k.copied >= k.tagEnd)
        {
            if (k.tagEop)
            {
                c.packets.insert(c.packets.end(), c.kickBuf, c.kickBuf + k.total);
                c.packetSizes.push_back(k.total);
                k.active = false;
            }
            else
            {
                k.tagEnd = 0u;
                k.tagEop = false;
            }
        }
        return true;
    }

    // n VU cycles pass (stall cycles or the cycle a pair takes). progressXgkick runs every cycle: +1 credit, then a
    // qword per 2 credits while the packet lasts. Nothing between those cycles looks at the transfer, so the n
    // credits go in at once (same qwords, same credit left; after the packet ends the credit no longer matters:
    // kickStart resets it). A rejected packet bails, and the interpreter redoes the run.
    VU1AOT_INLINE bool advance(Ctx &c, uint32_t n)
    {
        c.cycles += n;
        if (!c.kick.active)
            return true;
        c.kick.credit += n;
        while (c.kick.active && c.kick.credit >= 2u)
        {
            c.kick.credit -= 2u;
            if (!kickQword(c))
                return false;
        }
        return true;
    }

    // PATH1 qwords while the credit lasts (one per 2 credits). Inside a GIF tag's data every qword but the tag's last
    // goes in one copy: kickQword would only copy them (no tag to read, no end to handle; tagEnd <= 64 KiB, so no
    // overflow check can fail there). The tag, the last qword and a source that wraps go one by one.
    inline bool kickRun(Ctx &c)
    {
        Kick &k = c.kick;
        while (k.active && k.credit >= 2u)
        {
            if (k.tagEnd != 0u && k.tagEnd - k.copied > 32u)
            {
                const uint32_t source = wrapData(c, k.source + k.copied);
                uint32_t n = std::min(k.credit / 2u, (k.tagEnd - k.copied) / 16u - 1u);
                n = std::min(n, (c.dataSize - source) / 16u);
                if (n != 0u)
                {
                    std::memcpy(c.kickBuf + k.copied, c.data + source, n * 16u);
                    k.copied += n * 16u;
                    k.credit -= n * 2u;
                    continue;
                }
            }
            k.credit -= 2u;
            if (!kickQword(c))
                return false;
        }
        return true;
    }

    // The generated code counts cycles in a local only (`cyc`) and brings the XGKICK transfer up to that cycle where
    // its progress can matter: before a store into data memory, at an XGKICK and at the end of the run (`kc`: the
    // cycle it was brought to last). The same qwords from the same memory as advancing it every cycle (advance):
    // nothing between two such points writes data memory or looks at the transfer.
    VU1AOT_INLINE bool kickSync(Ctx &c, uint64_t cyc, uint64_t &kc)
    {
        const uint64_t n = cyc - kc;
        kc = cyc;
        if (!c.kick.active)
            return true;
        c.kick.credit += static_cast<uint32_t>(n);
        return c.kick.credit < 2u || kickRun(c);
    }

    // ---------------------------------------------------------------- commits (commitReadyPipelines)

    VU1AOT_INLINE void commitFlag(Ctx &c, const Ctx::FlagEntry &e)
    {
        if (e.writesMac)
            c.s.mac = e.mac;
        if (e.writesStatus)
        {
            const uint32_t current = e.status & 0xFu;
            c.s.status = (c.s.status & 0xFF0u) | current | ((current | e.extraSticky) << 6);
        }
        if (e.writesSticky)
            c.s.status = (c.s.status & 0x03Fu) | (e.status & 0xFC0u);
        if (e.writesClip)
            c.s.clip = e.clip;
    }

    VU1AOT_INLINE void commitQ(Ctx &c)
    {
        c.s.q = c.pendingQ;
        const uint32_t currentDi = c.pendingQDi & 0x30u;
        c.s.status = (c.s.status & 0xFCFu) | currentDi | (currentDi << 6);
    }

    // ---------------------------------------------------------------- data memory

    VU1AOT_INLINE void storeQword(Ctx &c, uint32_t address, const uint32_t words[4], uint8_t laneMask)
    {
        // queueStore + commitReadyPipelines: only lanes in the mask change.
        if (address + 16u > c.dataSize)
            return;
        if (c.logUndo)
        {
            c.undo.emplace_back();
            c.undo.back().address = address;
            std::memcpy(c.undo.back().old, c.data + address, 16u);
        }
        uint32_t old[4];
        std::memcpy(old, c.data + address, sizeof(old));
        if (laneMask & 0x8u)
            old[0] = words[0];
        if (laneMask & 0x4u)
            old[1] = words[1];
        if (laneMask & 0x2u)
            old[2] = words[2];
        if (laneMask & 0x1u)
            old[3] = words[3];
        std::memcpy(c.data + address, old, sizeof(old));
    }

    VU1AOT_INLINE void applyDest(float *dst, const float *result, uint8_t dest)
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

    // ---------------------------------------------------------------- FMAC (execUpper + applyFmacDest*)

    struct FlagResult
    {
        bool queued = false; // the interpreter queued a flag pipeline entry (dest != 0 and flags live)
        uint32_t mac = 0, status = 0, extraSticky = 0;
    };

    constexpr uint8_t laneBit(uint32_t component) { return static_cast<uint8_t>(1u << (3u - component)); }

    float normalizeResult(float value, uint32_t &laneFlags);
    uint8_t normalizeExact(float &value, long double exact);

    enum Kind : uint8_t
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

    // calculateFmacProductSticky + normalizeFmacResult + updateFmacFlags + applyDest, for one FMAC instruction whose
    // float result (computed like execUpper) is in `result`. `upper` is the instruction word.
    void fmacFinish(Ctx &c, uint32_t upper, float *result, float *dst, uint8_t dest, Kind kind, YSource ySource,
                    FlagResult &flags);

    // execUpper for one upper instruction word. Flag entries are not queued here: `flags` (FMAC) or `clipQueued`
    // (CLIP: the new 24-bit working clip in `clip`) tell the generated code what to queue.
    void upper(Ctx &c, uint32_t instr, FlagResult &flags, bool &clipQueued, uint32_t &clip);

    // ---------------------------------------------------------------- lower

    VU1AOT_INLINE int16_t imm11(uint32_t i) { return IMM11(i); }

    // execLower for the plain cases (loads / stores, integer ALU, MOVE / MR32 / MTIR / MFIR, R register, MFP,
    // XTOP / XITOP, flag reads). Returns false for anything the generator must handle itself (branches, XGKICK,
    // FDIV / EFU, FSSET / FCSET).
    bool lowerPlain(Ctx &c, uint32_t instr);

    // FDIV results (DIV / SQRT / RSQRT): the value and the status D / I bits queueQ would get.
    void fdiv(Ctx &c, uint32_t instr, float &value, uint32_t &statusDi, uint32_t &latency);
    // EFU results: the value and the latency queueP would get.
    void efu(Ctx &c, uint32_t instr, float &value, uint32_t &latency);

    // ---------------------------------------------------------------- programs

    using ProgramFn = bool (*)(Ctx &c, uint32_t entryPc);

    struct Program
    {
        const char *key;              // vu1reach.py program key
        const uint32_t *pcs;          // the pairs the program reaches ...
        const uint64_t *words;        // ... and their words as stored: lower | (upper << 32)
        uint32_t pairCount;
        const uint32_t *entries;      // entry PCs the native code covers
        uint32_t entryCount;
        ProgramFn run;
        bool flagsFree;               // generated without MAC / status flags: runs only when no instruction reads them
    };

    extern const Program kPrograms[];   // VU1 (aot/aot_programs.cpp, or vu1aot_none.cpp)
    extern const uint32_t kProgramCount;
    extern const Program kPrograms0[];  // VU0 (aot/aot_programs0.cpp, or vu0aot_none.cpp)
    extern const uint32_t kProgramCount0;
}
