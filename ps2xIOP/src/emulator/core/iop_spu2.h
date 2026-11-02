#pragma once

// Rayman bring-up: SPU2 (PS2 sound processor) emulation for the IOP.
//
// Implements the parts games rely on: 2 MiB sound RAM, both cores' register files, 48 voices
// with VAG/ADPCM decoding, loop flags, ADSR envelopes, ENDX/ENVX/NAX feedback, the IRQA address
// interrupt, IOP DMA channels 4/7 (normal and AutoDMA streaming into the core input buffers),
// the MMIX/VMIX routing between voices, input, core 0 -> core 1, and master volumes.
// Not emulated: reverb (wet path), noise, pitch modulation, volume sweeps.
//
// Time base: one output sample every 768 IOP cycles (36.864 MHz / 48 kHz). Mixed stereo samples
// go to a ring buffer that the host audio callback drains (ps2xSpu2ReadSamples).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace ps2x::iop::detail
{
    class Spu2
    {
    public:
        static constexpr uint32_t kRamWords = 1024u * 1024u; // 2 MiB
        static constexpr uint32_t kCyclesPerSample = 768u;
        static constexpr uint32_t kVoices = 24u;

        enum Event : uint32_t
        {
            EventAdmaDone0 = 1u << 0,
            EventAdmaDone1 = 1u << 1,
            EventIrq = 1u << 2,
        };

        Spu2() : m_ram(kRamWords, 0u) { reset(); }

        // Diagnostics (read and cleared by the owner): voices keyed on, bytes DMA'd into sound RAM,
        // register writes.
        uint32_t statKeyOn = 0u;
        uint32_t statDmaBytes = 0u;
        uint32_t statRegWrites = 0u;
        // Diagnostics: voices sounding now, and core 1 (final output) master volume and routing.
        uint32_t activeVoices() const
        {
            uint32_t n = 0u;
            for (const Core &core : m_cores)
                for (const Voice &v : core.voices)
                    n += v.active && v.envelope > 0 ? 1u : 0u;
            return n;
        }
        int32_t outputVolume() const { return std::max(m_cores[1].mvolL, m_cores[1].mvolR); }
        uint16_t routing(uint32_t coreIndex) const { return m_cores[coreIndex & 1u].mmix; }

        void reset()
        {
            std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
            m_regs.fill(0u);
            for (Core &core : m_cores)
                core = Core{};
            m_cycleCarry = 0;
            m_irqInfo = 0;
            m_events = 0;
        }

        // ---------------------------------------------------------------- registers
        static bool owns(uint32_t phys) { return phys >= 0x1F900000u && phys < 0x1F900800u; }

        uint16_t read16(uint32_t phys)
        {
            const uint32_t off = phys & 0x7FEu;
            if (off >= 0x760u)
                return readGlobal(off);
            const uint32_t coreIndex = off >= 0x400u ? 1u : 0u;
            const uint32_t r = off & 0x3FEu;
            Core &core = m_cores[coreIndex];
            if (r < 0x180u)
            {
                Voice &v = core.voices[r >> 4];
                switch (r & 0xFu)
                {
                case 0xA: return static_cast<uint16_t>(v.envelope);
                case 0xC: return static_cast<uint16_t>(v.volL);
                case 0xE: return static_cast<uint16_t>(v.volR);
                default: return m_regs[off >> 1];
                }
            }
            if (r >= 0x1C0u && r < 0x2E0u)
            {
                const uint32_t idx = (r - 0x1C0u) / 0xCu;
                const uint32_t field = (r - 0x1C0u) % 0xCu;
                const Voice &v = core.voices[idx];
                switch (field)
                {
                case 0x0: return static_cast<uint16_t>((v.ssa >> 16) & 0xFu);
                case 0x2: return static_cast<uint16_t>(v.ssa);
                case 0x4: return static_cast<uint16_t>((v.lsa >> 16) & 0xFu);
                case 0x6: return static_cast<uint16_t>(v.lsa);
                case 0x8: return static_cast<uint16_t>((v.nax >> 16) & 0xFu);
                case 0xA: return static_cast<uint16_t>(v.nax);
                default: return 0u;
                }
            }
            switch (r)
            {
            case 0x19Cu: return static_cast<uint16_t>((core.irqa >> 16) & 0xFu);
            case 0x19Eu: return static_cast<uint16_t>(core.irqa);
            case 0x1A8u: return static_cast<uint16_t>((core.tsa >> 16) & 0xFu);
            case 0x1AAu: return static_cast<uint16_t>(core.tsa);
            case 0x1A0u: case 0x1A2u: case 0x1A4u: case 0x1A6u: return 0u; // KON/KOFF read back as 0
            case 0x340u: return static_cast<uint16_t>(core.endx);
            case 0x342u: return static_cast<uint16_t>(core.endx >> 16);
            case 0x344u: return core.statx;
            case 0x1B0u: return core.admas;
            default: return m_regs[off >> 1];
            }
        }

        void write16(uint32_t phys, uint16_t value)
        {
            const uint32_t off = phys & 0x7FEu;
            ++statRegWrites;
            m_regs[off >> 1] = value;
            if (off >= 0x760u)
            {
                writeGlobal(off, value);
                return;
            }
            const uint32_t coreIndex = off >= 0x400u ? 1u : 0u;
            const uint32_t r = off & 0x3FEu;
            Core &core = m_cores[coreIndex];
            if (r < 0x180u)
            {
                Voice &v = core.voices[r >> 4];
                switch (r & 0xFu)
                {
                case 0x0: v.volLReg = value; v.volL = fixedVolume(value, v.volL); break;
                case 0x2: v.volRReg = value; v.volR = fixedVolume(value, v.volR); break;
                case 0x4: v.pitch = std::min<uint32_t>(value, 0x3FFFu); break;
                case 0x6: v.adsr1 = value; break;
                case 0x8: v.adsr2 = value; break;
                case 0xA: v.envelope = static_cast<int32_t>(value & 0x7FFFu); break;
                default: break;
                }
                return;
            }
            if (r >= 0x1C0u && r < 0x2E0u)
            {
                const uint32_t idx = (r - 0x1C0u) / 0xCu;
                const uint32_t field = (r - 0x1C0u) % 0xCu;
                Voice &v = core.voices[idx];
                switch (field)
                {
                case 0x0: v.ssa = ((static_cast<uint32_t>(value) & 0xFu) << 16) | (v.ssa & 0xFFFFu); break;
                case 0x2: v.ssa = (v.ssa & 0xF0000u) | value; break;
                case 0x4: v.lsa = ((static_cast<uint32_t>(value) & 0xFu) << 16) | (v.lsa & 0xFFFFu); v.lsaSetByCpu = true; break;
                case 0x6: v.lsa = (v.lsa & 0xF0000u) | value; v.lsaSetByCpu = true; break;
                case 0x8: v.nax = ((static_cast<uint32_t>(value) & 0xFu) << 16) | (v.nax & 0xFFFFu); break;
                case 0xA: v.nax = (v.nax & 0xF0000u) | value; break;
                default: break;
                }
                return;
            }
            switch (r)
            {
            case 0x188u: core.vmixl = (core.vmixl & 0xFF0000u) | value; break;
            case 0x18Au: core.vmixl = (core.vmixl & 0xFFFFu) | ((static_cast<uint32_t>(value) & 0xFFu) << 16); break;
            case 0x190u: core.vmixr = (core.vmixr & 0xFF0000u) | value; break;
            case 0x192u: core.vmixr = (core.vmixr & 0xFFFFu) | ((static_cast<uint32_t>(value) & 0xFFu) << 16); break;
            case 0x198u: core.mmix = value; break;
            case 0x19Au:
            {
                const uint16_t oldMode = static_cast<uint16_t>((core.attr >> 4) & 3u);
                core.attr = value;
                const uint16_t mode = static_cast<uint16_t>((value >> 4) & 3u);
                if ((value & 0x8000u) != 0u)
                    core.statx = 0u;
                if (mode == 0u)
                    core.statx = static_cast<uint16_t>(core.statx & ~0x80u);
                else if (oldMode == 0u)
                    core.statx = static_cast<uint16_t>(core.statx | 0x80u);
                break;
            }
            case 0x19Cu: core.irqa = ((static_cast<uint32_t>(value) & 0xFu) << 16) | (core.irqa & 0xFFFFu); break;
            case 0x19Eu: core.irqa = (core.irqa & 0xF0000u) | value; break;
            case 0x1A0u: keyOn(core, value); break;
            case 0x1A2u: keyOn(core, (static_cast<uint32_t>(value) & 0xFFu) << 16); break;
            case 0x1A4u: keyOff(core, value); break;
            case 0x1A6u: keyOff(core, (static_cast<uint32_t>(value) & 0xFFu) << 16); break;
            case 0x1A8u: core.tsa = ((static_cast<uint32_t>(value) & 0xFu) << 16) | (core.tsa & 0xFFFFu); break;
            case 0x1AAu: core.tsa = (core.tsa & 0xF0000u) | value; break;
            case 0x1ACu: // manual transfer data port
                checkIrqRange(core.tsa, 1u);
                m_ram[core.tsa & (kRamWords - 1u)] = value;
                core.tsa = (core.tsa + 1u) & (kRamWords - 1u);
                break;
            case 0x1B0u:
                core.admas = value;
                if ((value & 3u) == 0u)
                    core.adma = Adma{};
                break;
            case 0x340u: core.endx &= 0xFF0000u; break;
            case 0x342u: core.endx &= 0x00FFFFu; break;
            default: break;
            }
        }

        // ---------------------------------------------------------------- DMA (IOP channels 4 and 7)
        bool admaEnabled(uint32_t coreIndex) const { return (m_cores[coreIndex & 1u].admas & (1u << (coreIndex & 1u))) != 0u; }

        void dmaWrite(uint32_t coreIndex, const uint8_t *src, uint32_t bytes)
        {
            statDmaBytes += bytes;
            Core &core = m_cores[coreIndex & 1u];
            const uint32_t words = bytes / 2u;
            checkIrqRange(core.tsa, words);
            for (uint32_t i = 0; i < words; ++i)
            {
                uint16_t w;
                std::memcpy(&w, src + i * 2u, 2u);
                m_ram[(core.tsa + i) & (kRamWords - 1u)] = w;
            }
            core.tsa = (core.tsa + words) & (kRamWords - 1u);
        }

        void dmaRead(uint32_t coreIndex, uint8_t *dst, uint32_t bytes)
        {
            Core &core = m_cores[coreIndex & 1u];
            const uint32_t words = bytes / 2u;
            checkIrqRange(core.tsa, words);
            for (uint32_t i = 0; i < words; ++i)
            {
                const uint16_t w = m_ram[(core.tsa + i) & (kRamWords - 1u)];
                std::memcpy(dst + i * 2u, &w, 2u);
            }
            core.tsa = (core.tsa + words) & (kRamWords - 1u);
        }

        // AutoDMA: the data (256 L samples, 256 R samples, repeated) is pulled into the core's
        // sound-data-input ring as the core consumes it. Completion is reported by advance().
        void startAdma(uint32_t coreIndex, uint32_t sourceAddress, uint32_t bytes)
        {
            Core &core = m_cores[coreIndex & 1u];
            core.adma.active = true;
            core.adma.source = sourceAddress;
            core.adma.remaining = bytes;
            core.statx = static_cast<uint16_t>(core.statx | 0x80u);
            // Prime the half the core is not reading right now.
            if (core.adma.needFill[0] || core.adma.needFill[1])
                fillAdmaHalf(core, coreIndex & 1u, core.inputPos < 0x100u ? 1u : 0u);
        }

        void setIopRam(const uint8_t *ram, uint32_t size)
        {
            m_iopRam = ram;
            m_iopRamSize = size;
        }

        void completeDma(uint32_t coreIndex)
        {
            // STATX bit 7 stays set: LIBSD treats it as "DMA ready".
            Core &core = m_cores[coreIndex & 1u];
            core.statx = static_cast<uint16_t>(core.statx | 0x80u);
        }

        void markDmaStarted(uint32_t coreIndex)
        {
            Core &core = m_cores[coreIndex & 1u];
            core.statx = static_cast<uint16_t>(core.statx | 0x80u);
        }

        // ---------------------------------------------------------------- time
        // Advances by IOP cycles, mixing into `out` (interleaved stereo). Returns Event bits.
        uint32_t advance(uint64_t iopCycles, std::vector<int16_t> &out)
        {
            m_cycleCarry += iopCycles;
            const uint64_t samples = m_cycleCarry / kCyclesPerSample;
            m_cycleCarry %= kCyclesPerSample;
            for (uint64_t i = 0; i < samples; ++i)
            {
                int32_t l = 0;
                int32_t r = 0;
                tick(l, r);
                out.push_back(clamp16(l));
                out.push_back(clamp16(r));
            }
            const uint32_t events = m_events;
            m_events = 0;
            return events;
        }

        uint16_t irqInfo() const { return m_irqInfo; }

    private:
        struct Voice
        {
            uint32_t ssa = 0, lsa = 0, nax = 0;
            bool lsaSetByCpu = false;
            uint16_t volLReg = 0, volRReg = 0;
            int32_t volL = 0, volR = 0;
            uint32_t pitch = 0;
            uint16_t adsr1 = 0, adsr2 = 0;
            int32_t envelope = 0;
            int phase = 0; // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
            uint32_t envCounter = 0;
            bool active = false;
            uint32_t counter = 0;
            int32_t prev = 0, cur = 0;
            int32_t s1 = 0, s2 = 0;
            int16_t block[28]{};
            uint32_t blockIndex = 28;
            uint32_t blockAddr = 0;
            uint8_t blockFlags = 0;
        };

        struct Adma
        {
            bool active = false;
            uint32_t source = 0;
            uint32_t remaining = 0;
            bool needFill[2] = {true, true};
        };

        struct Core
        {
            std::array<Voice, kVoices> voices{};
            uint32_t vmixl = 0, vmixr = 0;
            uint16_t mmix = 0;
            uint16_t attr = 0;
            uint16_t statx = 0;
            uint16_t admas = 0;
            uint32_t irqa = 0;
            uint32_t tsa = 0;
            uint32_t endx = 0;
            uint32_t inputPos = 0;
            int32_t mvolL = 0, mvolR = 0;
            int32_t avolL = 0, avolR = 0;
            int32_t bvolL = 0, bvolR = 0;
            Adma adma{};
        };

        static int16_t clamp16(int32_t v)
        {
            return static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }

        // Volume registers: bit 15 clear = fixed volume (15-bit signed). Sweeps keep the current volume.
        static int32_t fixedVolume(uint16_t value, int32_t current)
        {
            if ((value & 0x8000u) != 0u)
            {
                // Volume sweep (bit 13 = decrease). Sweeps are not stepped: jump to where the sweep ends
                // (full volume or silence) so a fade-in does not leave the channel muted forever.
                (void)current;
                return (value & 0x2000u) != 0u ? 0 : 0x7FFF;
            }
            return static_cast<int16_t>(static_cast<uint16_t>(value << 1));
        }


        uint16_t readGlobal(uint32_t off)
        {
            if (off == 0x7C2u)
                return m_irqInfo;
            return m_regs[off >> 1];
        }

        void writeGlobal(uint32_t off, uint16_t value)
        {
            if (off == 0x7C2u)
            {
                m_irqInfo = static_cast<uint16_t>(m_irqInfo & value); // write-to-acknowledge style
                return;
            }
            if (off >= 0x760u && off < 0x7B0u)
            {
                const uint32_t coreIndex = off >= 0x788u ? 1u : 0u;
                const uint32_t r = off - (coreIndex ? 0x788u : 0x760u);
                Core &core = m_cores[coreIndex];
                switch (r)
                {
                case 0x00: core.mvolL = fixedVolume(value, core.mvolL); break;
                case 0x02: core.mvolR = fixedVolume(value, core.mvolR); break;
                case 0x08: core.avolL = static_cast<int16_t>(value); break;
                case 0x0A: core.avolR = static_cast<int16_t>(value); break;
                case 0x0C: core.bvolL = static_cast<int16_t>(value); break;
                case 0x0E: core.bvolR = static_cast<int16_t>(value); break;
                default: break;
                }
            }
        }

        void raiseIrq(uint32_t coreIndex)
        {
            m_irqInfo = static_cast<uint16_t>(m_irqInfo | (4u << coreIndex));
            m_events |= EventIrq;
        }

        void checkIrqRange(uint32_t start, uint32_t words)
        {
            for (uint32_t c = 0; c < 2u; ++c)
            {
                const Core &core = m_cores[c];
                if ((core.attr & 0x40u) == 0u)
                    continue;
                const uint32_t rel = (core.irqa - start) & (kRamWords - 1u);
                if (rel < words)
                    raiseIrq(c);
            }
        }

        void keyOn(Core &core, uint32_t mask)
        {
            for (uint32_t i = 0; i < kVoices; ++i)
            {
                if ((mask & (1u << i)) == 0u)
                    continue;
                ++statKeyOn;
                Voice &v = core.voices[i];
                v.active = true;
                v.phase = 1;
                v.envelope = 0;
                v.envCounter = 0;
                v.nax = v.ssa;
                v.counter = 0;
                v.prev = v.cur = 0;
                v.s1 = v.s2 = 0;
                v.blockIndex = 28;
                v.blockFlags = 0;
                v.lsaSetByCpu = false;
                v.volL = fixedVolume(v.volLReg, v.volL);
                v.volR = fixedVolume(v.volRReg, v.volR);
                core.endx &= ~(1u << i);
                decodeBlock(core, v, true);
            }
        }

        static void keyOff(Core &core, uint32_t mask)
        {
            for (uint32_t i = 0; i < kVoices; ++i)
            {
                if ((mask & (1u << i)) != 0u && core.voices[i].active)
                {
                    core.voices[i].phase = 4;
                    core.voices[i].envCounter = 0;
                }
            }
        }

        // Decodes the ADPCM block at v.nax. `first` = right after key-on (no end handling yet).
        void decodeBlock(Core &core, Voice &v, bool first)
        {
            if (!first)
            {
                // Finished the previous block: apply its end/loop flags.
                const uint32_t voiceIndex = static_cast<uint32_t>(&v - core.voices.data());
                if ((v.blockFlags & 1u) != 0u)
                {
                    core.endx |= 1u << voiceIndex;
                    if ((v.blockFlags & 2u) != 0u)
                    {
                        v.nax = v.lsa;
                    }
                    else
                    {
                        // End without repeat: the voice is muted.
                        v.active = false;
                        v.phase = 0;
                        v.envelope = 0;
                        v.nax = v.lsa;
                        std::fill(std::begin(v.block), std::end(v.block), int16_t{0});
                        v.blockIndex = 0;
                        return;
                    }
                }
                else
                {
                    v.nax = (v.blockAddr + 8u) & (kRamWords - 1u);
                }
            }
            v.blockAddr = v.nax & (kRamWords - 1u);
            checkIrqRange(v.blockAddr, 8u);
            const uint16_t header = m_ram[v.blockAddr];
            const uint32_t shift = header & 0xFu;
            const uint32_t filter = std::min<uint32_t>((header >> 4) & 0xFu, 4u);
            v.blockFlags = static_cast<uint8_t>(header >> 8);
            if ((v.blockFlags & 4u) != 0u && !v.lsaSetByCpu)
                v.lsa = v.blockAddr;
            static constexpr int32_t f0[5] = {0, 60, 115, 98, 122};
            static constexpr int32_t f1[5] = {0, 0, -52, -55, -60};
            for (uint32_t i = 0; i < 28u; ++i)
            {
                const uint16_t word = m_ram[(v.blockAddr + 1u + i / 4u) & (kRamWords - 1u)];
                const int32_t nibble = (word >> ((i & 3u) * 4u)) & 0xF;
                int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> std::min<uint32_t>(shift, 12u);
                sample += (v.s1 * f0[filter] + v.s2 * f1[filter]) >> 6;
                sample = std::clamp(sample, -32768, 32767);
                v.s2 = v.s1;
                v.s1 = sample;
                v.block[i] = static_cast<int16_t>(sample);
            }
            v.blockIndex = 0;
        }

        int32_t nextSample(Core &core, Voice &v)
        {
            if (v.blockIndex >= 28u)
                decodeBlock(core, v, false);
            return v.block[v.blockIndex++];
        }

        // ADSR envelope step (PSX/PS2 rules: rate = shift + step, exponential variants).
        static void envelopeStep(Voice &v, bool increase, bool exponential, uint32_t shift, int32_t step)
        {
            uint32_t cycles = 1u << (shift > 11u ? shift - 11u : 0u);
            int32_t delta = step << (shift < 11u ? 11u - shift : 0u);
            if (exponential && increase && v.envelope > 0x6000)
                cycles *= 4u;
            if (exponential && !increase)
                delta = (delta * v.envelope) >> 15;
            if (++v.envCounter < cycles)
                return;
            v.envCounter = 0;
            v.envelope = std::clamp(v.envelope + delta, 0, 0x7FFF);
        }

        static void updateEnvelope(Voice &v)
        {
            switch (v.phase)
            {
            case 1: // attack
            {
                const uint32_t rate = (v.adsr1 >> 8) & 0x7Fu;
                envelopeStep(v, true, (v.adsr1 & 0x8000u) != 0u, rate >> 2, 7 - static_cast<int32_t>(rate & 3u));
                if (v.envelope >= 0x7FFF)
                {
                    v.envelope = 0x7FFF;
                    v.phase = 2;
                    v.envCounter = 0;
                }
                break;
            }
            case 2: // decay
            {
                const uint32_t shift = (v.adsr1 >> 4) & 0xFu; // decay shift (step fixed at -8)
                envelopeStep(v, false, true, shift, -8);
                const int32_t sustain = ((v.adsr1 & 0xF) + 1) * 0x800;
                if (v.envelope <= sustain)
                {
                    v.phase = 3;
                    v.envCounter = 0;
                }
                break;
            }
            case 3: // sustain
            {
                const uint32_t rate = (v.adsr2 >> 6) & 0x7Fu;
                const bool decrease = (v.adsr2 & 0x4000u) != 0u;
                const int32_t step = decrease ? -8 + static_cast<int32_t>(rate & 3u) : 7 - static_cast<int32_t>(rate & 3u);
                envelopeStep(v, !decrease, (v.adsr2 & 0x8000u) != 0u, rate >> 2, step);
                break;
            }
            case 4: // release
            {
                const uint32_t shift = v.adsr2 & 0x1Fu; // release shift (step fixed at -8)
                envelopeStep(v, false, (v.adsr2 & 0x20u) != 0u, shift, -8);
                if (v.envelope <= 0)
                {
                    v.envelope = 0;
                    v.phase = 0;
                    v.active = false;
                }
                break;
            }
            default:
                break;
            }
        }

        void fillAdmaHalf(Core &core, uint32_t coreIndex, uint32_t half)
        {
            Adma &a = core.adma;
            if (!a.active || a.remaining == 0u || !m_iopRam)
                return;
            const uint32_t chunk = std::min<uint32_t>(a.remaining, 0x400u);
            const uint32_t base = 0x2000u + coreIndex * 0x400u;
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint16_t l = 0, r = 0;
                const uint32_t lo = a.source + i * 2u;
                const uint32_t ro = a.source + 0x200u + i * 2u;
                if (i * 2u + 1u < chunk && lo + 1u < m_iopRamSize)
                    std::memcpy(&l, m_iopRam + lo, 2u);
                if (0x200u + i * 2u + 1u < chunk && ro + 1u < m_iopRamSize)
                    std::memcpy(&r, m_iopRam + ro, 2u);
                m_ram[base + half * 0x100u + i] = l;
                m_ram[base + 0x200u + half * 0x100u + i] = r;
            }
            a.source += chunk;
            a.remaining -= chunk;
            a.needFill[half] = false;
            if (a.remaining == 0u)
            {
                a.active = false;
                completeDma(coreIndex);
                m_events |= coreIndex ? EventAdmaDone1 : EventAdmaDone0;
            }
        }

        void mixCore(uint32_t coreIndex, int32_t extL, int32_t extR, int32_t &outL, int32_t &outR)
        {
            Core &core = m_cores[coreIndex];
            int32_t dryL = 0, dryR = 0;
            for (uint32_t i = 0; i < kVoices; ++i)
            {
                Voice &v = core.voices[i];
                if (!v.active)
                    continue;
                updateEnvelope(v);
                if (!v.active)
                    continue;
                // linear interpolation between the last two decoded samples
                const int32_t sample = v.prev + (((v.cur - v.prev) * static_cast<int32_t>(v.counter & 0xFFFu)) >> 12);
                const int32_t enveloped = (sample * v.envelope) >> 15;
                if ((core.vmixl & (1u << i)) != 0u)
                    dryL += (enveloped * v.volL) >> 15;
                if ((core.vmixr & (1u << i)) != 0u)
                    dryR += (enveloped * v.volR) >> 15;
                v.counter += v.pitch;
                while (v.counter >= 0x1000u && v.active)
                {
                    v.counter -= 0x1000u;
                    v.prev = v.cur;
                    v.cur = nextSample(core, v);
                }
            }

            // Sound data input (core input ring at 0x2000 + core * 0x400), fed by AutoDMA.
            const uint32_t base = 0x2000u + coreIndex * 0x400u;
            const int32_t inL = static_cast<int16_t>(m_ram[base + core.inputPos]);
            const int32_t inR = static_cast<int16_t>(m_ram[base + 0x200u + core.inputPos]);
            if ((m_cores[0].attr & 0x40u) != 0u || (m_cores[1].attr & 0x40u) != 0u)
            {
                checkIrqRange(base + core.inputPos, 1u);
                checkIrqRange(base + 0x200u + core.inputPos, 1u);
            }
            const uint32_t oldHalf = core.inputPos >> 8;
            core.inputPos = (core.inputPos + 1u) & 0x1FFu;
            const uint32_t newHalf = core.inputPos >> 8;
            if (newHalf != oldHalf)
            {
                core.adma.needFill[oldHalf] = true;
                if (core.adma.active)
                    fillAdmaHalf(core, coreIndex, oldHalf);
            }
            const bool admaPlaying = (core.admas & (1u << coreIndex)) != 0u;

            const uint16_t mmix = core.mmix;
            int32_t mixL = 0, mixR = 0;
            if (mmix & 0x800u) mixL += dryL;
            if (mmix & 0x400u) mixR += dryR;
            if (admaPlaying)
            {
                if (mmix & 0x080u) mixL += (inL * core.avolL) >> 15;
                if (mmix & 0x040u) mixR += (inR * core.avolR) >> 15;
            }
            if (mmix & 0x008u) mixL += (extL * core.bvolL) >> 15;
            if (mmix & 0x004u) mixR += (extR * core.bvolR) >> 15;

            outL = (std::clamp(mixL, -32768, 32767) * core.mvolL) >> 15;
            outR = (std::clamp(mixR, -32768, 32767) * core.mvolR) >> 15;
        }

        void tick(int32_t &l, int32_t &r)
        {
            int32_t c0L = 0, c0R = 0;
            mixCore(0u, 0, 0, c0L, c0R);
            mixCore(1u, c0L, c0R, l, r);
        }

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, 0x400> m_regs{};
        std::array<Core, 2> m_cores{};
        uint64_t m_cycleCarry = 0;
        uint16_t m_irqInfo = 0;
        uint32_t m_events = 0;
        const uint8_t *m_iopRam = nullptr;
        uint32_t m_iopRamSize = 0;
    };
}
