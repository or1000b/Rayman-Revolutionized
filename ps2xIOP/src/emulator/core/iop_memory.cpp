#include "iop_memory.h"
#include "iop_spu2.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        // Single-producer (IOP emulation) / single-consumer (host audio callback) stereo ring.
        constexpr uint32_t kAudioRingFrames = 1u << 15; // ~0.68 s at 48 kHz
        struct AudioRing
        {
            int16_t data[kAudioRingFrames * 2u]{};
            std::atomic<uint64_t> writeFrame{0};
            std::atomic<uint64_t> readFrame{0};
            double readFraction = 0.0;
            double rate = 1.0;
        };
        AudioRing g_audioRing;

        // Diagnostics for the periodic log line (ps2xSpu2StatsLine).
        std::atomic<uint32_t> g_statKeyOn{0};
        std::atomic<uint32_t> g_statDmaBytes{0};
        std::atomic<uint32_t> g_statRegWrites{0};
        std::atomic<uint32_t> g_statFrames{0};
        std::atomic<int32_t> g_statPeak{0};
        std::atomic<uint32_t> g_statUnderruns{0};
        std::atomic<uint32_t> g_statVoices{0};
        std::atomic<int32_t> g_statMvol{0};
        std::atomic<uint32_t> g_statMmix{0};

        void pushAudio(const std::vector<int16_t> &samples)
        {
            const uint64_t frames = samples.size() / 2u;
            uint64_t w = g_audioRing.writeFrame.load(std::memory_order_relaxed);
            const uint64_t r = g_audioRing.readFrame.load(std::memory_order_acquire);
            for (uint64_t i = 0; i < frames; ++i)
            {
                if (w - r >= kAudioRingFrames - 1u)
                    break; // consumer is not draining (no audio device): drop
                const uint32_t slot = static_cast<uint32_t>(w & (kAudioRingFrames - 1u));
                g_audioRing.data[slot * 2u] = samples[i * 2u];
                g_audioRing.data[slot * 2u + 1u] = samples[i * 2u + 1u];
                ++w;
            }
            g_audioRing.writeFrame.store(w, std::memory_order_release);
        }
    }

    IopMemory::~IopMemory() = default;

    uint32_t IopMemory::advanceSpu2(uint64_t iopCycles)
    {
        if (!m_spu2 || iopCycles == 0u)
            return 0u;
        m_spu2->setIopRam(m_ram.data(), static_cast<uint32_t>(m_ram.size()));
        m_spu2Samples.clear();
        const uint32_t events = m_spu2->advance(iopCycles, m_spu2Samples);
        if (!m_spu2Samples.empty())
        {
            pushAudio(m_spu2Samples);
            int32_t peak = 0;
            for (const int16_t sample : m_spu2Samples)
                peak = std::max<int32_t>(peak, sample < 0 ? -static_cast<int32_t>(sample) : sample);
            int32_t previous = g_statPeak.load(std::memory_order_relaxed);
            while (peak > previous && !g_statPeak.compare_exchange_weak(previous, peak, std::memory_order_relaxed))
            {
            }
            g_statFrames.fetch_add(static_cast<uint32_t>(m_spu2Samples.size() / 2u), std::memory_order_relaxed);
        }
        g_statKeyOn.fetch_add(m_spu2->statKeyOn, std::memory_order_relaxed);
        g_statDmaBytes.fetch_add(m_spu2->statDmaBytes, std::memory_order_relaxed);
        g_statRegWrites.fetch_add(m_spu2->statRegWrites, std::memory_order_relaxed);
        m_spu2->statKeyOn = m_spu2->statDmaBytes = m_spu2->statRegWrites = 0u;
        const uint32_t voices = m_spu2->activeVoices();
        uint32_t previousVoices = g_statVoices.load(std::memory_order_relaxed);
        while (voices > previousVoices && !g_statVoices.compare_exchange_weak(previousVoices, voices, std::memory_order_relaxed))
        {
        }
        g_statMvol.store(m_spu2->outputVolume(), std::memory_order_relaxed);
        g_statMmix.store((static_cast<uint32_t>(m_spu2->routing(0)) << 16) | m_spu2->routing(1), std::memory_order_relaxed);
        return events;
    }

    // The SPU2 of the (single) IOP, for ps2xSpu2DebugDump.
    const Spu2 *g_debugSpu2 = nullptr;

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize), m_spu2(std::make_unique<Spu2>())
    {
        g_debugSpu2 = m_spu2.get();
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_allocations.clear();
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
        if (m_spu2)
            m_spu2->reset();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (Spu2::owns(phys))
            return static_cast<uint8_t>(m_spu2->read16(phys & ~1u) >> ((phys & 1u) * 8u));
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 1u) == 0u && Spu2::owns(phys))
            return m_spu2->read16(phys);
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && Spu2::owns(phys))
            return static_cast<uint32_t>(m_spu2->read16(phys)) | (static_cast<uint32_t>(m_spu2->read16(phys + 2u)) << 16u);
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (Spu2::owns(phys))
        {
            const uint32_t half = phys & ~1u;
            uint16_t current = m_spu2->read16(half);
            const uint32_t shift = (phys & 1u) * 8u;
            current = static_cast<uint16_t>((current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift));
            m_spu2->write16(half, current);
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 1u) == 0u && Spu2::owns(phys))
        {
            m_spu2->write16(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && Spu2::owns(phys))
        {
            m_spu2->write16(phys, static_cast<uint16_t>(value));
            m_spu2->write16(phys + 2u, static_cast<uint16_t>(value >> 16u));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        const uint32_t coreIndex = secondCore ? 1u : 0u;
        m_hardware[address] = value & ~kDmaStart;
        m_spu2->markDmaStarted(coreIndex);

        const uint32_t blockControlAddress = address - sizeof(uint32_t);
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(blockControlAddress); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t transferWords = static_cast<uint64_t>(wordsPerBlock) * blockCount;

        // Move the data now (SPU2 RAM <-> IOP RAM). Direction: CHCR bit 0 = RAM -> SPU2.
        const uint32_t madrAddress = address - 8u;
        uint32_t madr = 0u;
        if (const auto current = m_hardware.find(madrAddress); current != m_hardware.end())
            madr = current->second;
        const uint32_t source = physicalAddress(madr) & (RamSize - 1u);
        const uint32_t bytes = static_cast<uint32_t>(std::min<uint64_t>(transferWords * 4u, RamSize - source));
        const bool toSpu = (value & 1u) != 0u;
        m_spu2->setIopRam(m_ram.data(), static_cast<uint32_t>(m_ram.size()));
        if (toSpu && m_spu2->admaEnabled(coreIndex))
        {
            // AutoDMA streaming: completion is raised by the SPU2 as it consumes the data.
            m_spu2->startAdma(coreIndex, source, bytes);
            return;
        }
        if (toSpu)
            m_spu2->dmaWrite(coreIndex, m_ram.data() + source, bytes);
        else
        {
            m_spu2->dmaRead(coreIndex, m_ram.data() + source, bytes);
            markOwned(source, bytes);
        }
        m_hardware[madrAddress] = madr + bytes;

        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            std::max<uint64_t>(transferWords * 2u, 64u),
        };
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    void IopMemory::insertAllocation(uint32_t address, uint32_t size)
    {
        // Kept sorted by address: the free gaps are the spaces between neighbours.
        const auto at = std::lower_bound(m_allocations.begin(), m_allocations.end(), address,
                                         [](const Allocation &block, uint32_t value)
                                         { return block.address < value; });
        m_allocations.insert(at, Allocation{address, size});
        markOwned(address, size);
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address > HeapLimit || size > HeapLimit - address)
                return 0u;
            for (const auto &block : m_allocations)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            insertAllocation(address, size);
            return address;
        }

        uint32_t candidate = alignUp(HeapBase, alignment);
        for (const auto &block : m_allocations)
        {
            if (block.address + block.size <= candidate)
                continue;
            if (block.address >= candidate && block.address - candidate >= size)
                break;
            candidate = alignUp(block.address + block.size, alignment);
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        insertAllocation(candidate, size);
        return candidate;
    }

    uint32_t IopMemory::allocateHigh(uint32_t size, uint32_t alignment)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        uint32_t gapEnd = HeapLimit;
        for (auto block = m_allocations.rbegin();; ++block)
        {
            const bool last = block == m_allocations.rend();
            const uint32_t gapStart = last ? HeapBase : block->address + block->size;
            if (gapEnd >= gapStart && gapEnd - gapStart >= size)
            {
                const uint32_t address = (gapEnd - size) & ~(alignment - 1u);
                if (address >= gapStart)
                {
                    insertAllocation(address, size);
                    return address;
                }
            }
            if (last)
                return 0u;
            gapEnd = std::min(gapEnd, block->address);
        }
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        uint32_t best = 0u;
        uint32_t cursor = HeapBase;
        for (const auto &block : m_allocations)
        {
            if (block.address > cursor)
                best = std::max(best, block.address - cursor);
            cursor = std::max(cursor, block.address + block.size);
        }
        return cursor < HeapLimit ? std::max(best, HeapLimit - cursor) : best;
    }

    uint32_t IopMemory::totalFreeMemory() const
    {
        uint32_t total = 0u;
        uint32_t cursor = HeapBase;
        for (const auto &block : m_allocations)
        {
            if (block.address > cursor)
                total += block.address - cursor;
            cursor = std::max(cursor, block.address + block.size);
        }
        return cursor < HeapLimit ? total + (HeapLimit - cursor) : total;
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}

// Host audio: fill `frames` interleaved stereo int16 frames from the SPU2 output ring. When the
// emulation runs slower than real time the ring drains; playback then slows down smoothly
// (pitch drops) instead of stuttering. Returns the number of frames produced from SPU2 output.
extern "C" int ps2xSpu2ReadSamples(int16_t *out, int frames)
{
    using namespace ps2x::iop::detail;
    if (!out || frames <= 0)
        return 0;
    auto &ring = g_audioRing;
    const uint64_t w = ring.writeFrame.load(std::memory_order_acquire);
    uint64_t r = ring.readFrame.load(std::memory_order_relaxed);
    constexpr double kTargetFill = 4800.0; // 100 ms
    const double fill = static_cast<double>(w - r);
    double target = 1.0 + (fill - kTargetFill) / (4.0 * kTargetFill);
    target = std::clamp(target, 0.90, 1.10); // small drift correction only: underruns go silent, never slow-motion
    ring.rate += (target - ring.rate) * 0.05;
    int produced = 0;
    bool starved = false;
    // Last output sample, faded out when the emulator falls behind so a starved buffer does not click.
    static int32_t s_lastL = 0;
    static int32_t s_lastR = 0;
    for (int i = 0; i < frames; ++i)
    {
        if (r + 1u >= w)
        {
            s_lastL = (s_lastL * 31) / 32;
            s_lastR = (s_lastR * 31) / 32;
            out[i * 2] = static_cast<int16_t>(s_lastL);
            out[i * 2 + 1] = static_cast<int16_t>(s_lastR);
            starved = true;
            continue;
        }
        const uint32_t a = static_cast<uint32_t>(r & (kAudioRingFrames - 1u));
        const uint32_t b = static_cast<uint32_t>((r + 1u) & (kAudioRingFrames - 1u));
        const double f = ring.readFraction;
        out[i * 2] = static_cast<int16_t>(ring.data[a * 2u] + (ring.data[b * 2u] - ring.data[a * 2u]) * f);
        out[i * 2 + 1] = static_cast<int16_t>(ring.data[a * 2u + 1u] + (ring.data[b * 2u + 1u] - ring.data[a * 2u + 1u]) * f);
        s_lastL = out[i * 2];
        s_lastR = out[i * 2 + 1];
        ring.readFraction += ring.rate;
        const uint64_t whole = static_cast<uint64_t>(ring.readFraction);
        ring.readFraction -= static_cast<double>(whole);
        r += whole;
        ++produced;
    }
    if (r > w)
        r = w;
    ring.readFrame.store(r, std::memory_order_release);
    if (starved)
        g_statUnderruns.fetch_add(1u, std::memory_order_relaxed);
    return produced;
}

// One-line SPU2 summary since the previous call, e.g. "spu2 kon=12 dma=64K regs=900 out=480000 peak=8123 ring=4790 underruns=0".
extern "C" void ps2xIopTakeCdStats(uint32_t *out); // imports/iop_cdvd.cpp

extern "C" int ps2xSpu2StatsLine(char *buffer, int size)
{
    using namespace ps2x::iop::detail;
    if (!buffer || size <= 0)
        return 0;
    const uint64_t fill = g_audioRing.writeFrame.load(std::memory_order_acquire) - g_audioRing.readFrame.load(std::memory_order_acquire);
    const uint32_t mmix = g_statMmix.load();
    uint32_t cd[4] = {};
    ps2xIopTakeCdStats(cd);
    const int written = std::snprintf(buffer, static_cast<size_t>(size),
                                      "spu2 kon=%u voices=%u dma=%uK regs=%u out=%u peak=%d mvol=%d mmix=%04x/%04x ring=%u underruns=%u"
                                      " | iopcd reads=%u sectors=%u host=%u fail=%u",
                                      g_statKeyOn.exchange(0u), g_statVoices.exchange(0u), g_statDmaBytes.exchange(0u) / 1024u,
                                      g_statRegWrites.exchange(0u), g_statFrames.exchange(0u), g_statPeak.exchange(0), g_statMvol.load(),
                                      mmix >> 16, mmix & 0xFFFFu, static_cast<unsigned>(fill), g_statUnderruns.exchange(0u),
                                      cd[0], cd[1], cd[2], cd[3]);
    return written < 0 ? 0 : std::min(written, size - 1);
}

// Full SPU2 mixer state to stderr ([spu2-dump] lines). Call on the thread that runs the IOP.
extern "C" void ps2xSpu2DebugDump()
{
    if (ps2x::iop::detail::g_debugSpu2)
        ps2x::iop::detail::g_debugSpu2->debugDump(stderr);
}
