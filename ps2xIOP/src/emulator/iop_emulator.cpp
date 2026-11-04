#include "iop_emulator.h"

#include <cstdio>
#include <cstdlib>
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
            parsePcWatches();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            alarms.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            spu2Cycle = 0;
            eeCycleCarry = 0;
            wallStarted = false;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            outsideSleepers = 0u;
            outsideWakeups = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                if (call.ordinal == 24u && kernel.currentThreadId() == 0 && callDepth > 0u)
                {
                    sleepOutsideThread(cpu); // SleepThread from an RPC handler run directly
                    return ImportDisposition::Handled;
                }
                if ((call.ordinal == 25u || call.ordinal == 26u) && a0 == 0u && outsideSleepers > 0u)
                {
                    // WakeupThread(0): GetThreadId() returned 0 to the sleeper above, so a helper
                    // thread it created wakes it with id 0 (illegal on hardware, unused otherwise).
                    ++outsideWakeups;
                    setV0(0);
                    return ImportDisposition::Handled;
                }
                if (call.ordinal >= 35u && call.ordinal <= 38u)
                {
                    dispatchAlarmImport(call.ordinal, cpu);
                    return ImportDisposition::Handled;
                }
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        // PS2X_IOP_WATCH="addr[:reg:bytes],..." (hex IOP addresses): log the registers, and optionally the
        // memory a register points to, each time execution reaches addr. PS2X_IOP_WATCH_MAX = hits per address
        // (default 40). Debug aid for the UBISOUND port.
        struct PcWatch
        {
            uint32_t pc = 0u;
            int reg = -1;
            uint32_t bytes = 0u;
            uint32_t hits = 0u;
        };
        std::vector<PcWatch> pcWatches;
        uint32_t pcWatchMax = 40u;

        static int parseRegisterName(const std::string &name)
        {
            static const char *const kNames[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                                                   "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                                   "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                                   "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
            for (int i = 0; i < 32; ++i)
                if (name == kNames[i])
                    return i;
            return -1;
        }

        void parsePcWatches()
        {
            const char *spec = std::getenv("PS2X_IOP_WATCH");
            if (!spec)
                return;
            if (const char *max = std::getenv("PS2X_IOP_WATCH_MAX"))
                pcWatchMax = static_cast<uint32_t>(std::strtoul(max, nullptr, 10));
            std::stringstream list(spec);
            std::string item;
            while (std::getline(list, item, ','))
            {
                PcWatch watch;
                std::stringstream parts(item);
                std::string field;
                if (!std::getline(parts, field, ':'))
                    continue;
                watch.pc = static_cast<uint32_t>(std::strtoul(field.c_str(), nullptr, 16));
                if (std::getline(parts, field, ':'))
                    watch.reg = parseRegisterName(field);
                if (std::getline(parts, field, ':'))
                    watch.bytes = std::min<uint32_t>(static_cast<uint32_t>(std::strtoul(field.c_str(), nullptr, 0)), 256u);
                pcWatches.push_back(watch);
            }
        }

        void checkPcWatch(const CpuState &cpu)
        {
            for (PcWatch &watch : pcWatches)
            {
                if (watch.pc != cpu.pc || watch.hits >= pcWatchMax)
                    continue;
                ++watch.hits;
                std::ostringstream out;
                out << "[iop-watch] pc=0x" << std::hex << cpu.pc << " ra=0x" << cpu.gpr[31]
                    << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5] << " a2=0x" << cpu.gpr[6]
                    << " a3=0x" << cpu.gpr[7] << " v0=0x" << cpu.gpr[2] << " s0=0x" << cpu.gpr[16]
                    << " s1=0x" << cpu.gpr[17] << " sp=0x" << cpu.gpr[29] << " cyc=" << std::dec << totalCycles;
                if (watch.reg >= 0 && watch.bytes)
                {
                    const uint32_t at = cpu.gpr[watch.reg];
                    out << " mem@0x" << std::hex << at << "=";
                    for (uint32_t i = 0; i + 4u <= watch.bytes; i += 4u)
                    {
                        char word[10];
                        std::snprintf(word, sizeof(word), "%08x ", read32(at + i));
                        out << word;
                    }
                }
                log(LogLevel::Info, out.str());
            }
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (!pcWatches.empty())
                checkPcWatch(cpu);
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (const auto import = imports.decode(cpu.pc))
            {
                static const bool traceImports = std::getenv("PS2X_IOP_TRACE") != nullptr;
                const uint32_t traceRa = cpu.gpr[31];
                const uint32_t traceA[4] = {cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7]};
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                if (traceImports)
                {
                    std::ostringstream out;
                    out << "[IOP trace] " << import->library << ':' << import->ordinal << std::hex
                        << " a0=0x" << traceA[0] << " a1=0x" << traceA[1] << " a2=0x" << traceA[2] << " a3=0x" << traceA[3]
                        << " ra=0x" << traceRa;
                    if (disposition == ImportDisposition::JumpToGuest)
                        out << " -> guest 0x" << cpu.pc;
                    else
                        out << " -> v0=0x" << cpu.gpr[2];
                    log(LogLevel::Info, out.str());
                }
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
                if (dirtyStackForNextCall)
                    fillStackGarbage(stackTop - kCallStackSize / 2u, kCallStackSize / 2u - 32u);
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            const uint32_t executed = runCpu(cpu, budget);
            if (cpu.pc != kCallReturnSentinel)
            {
                // The call was abandoned before it returned: its work is half done and v0 is not its result.
                static uint32_t s_reported = 0u;
                if (s_reported < 20u)
                {
                    ++s_reported;
                    std::ostringstream out;
                    out << "[IOP] guest call 0x" << std::hex << address << " did not return: pc=0x" << cpu.pc
                        << " ra=0x" << cpu.gpr[31] << std::dec << " executed=" << executed << " budget=" << budget
                        << (cpu.yielded ? " (blocked)" : "");
                    log(LogLevel::Warning, out.str());
                }
            }
            return cpu.gpr[2];
        }

        // IOP RAM starts zeroed here, unlike a real IOP whose stacks hold leftovers. Some IRX code
        // reads a stack slot before writing it (UBISOUND's voice init tests an uninitialised
        // "all ok" flag) and only works because that garbage is non-zero. RPC handlers therefore
        // start on a stack filled with self-pointers: non-zero, positive and pointing into RAM.
        void fillStackGarbage(uint32_t base, uint32_t size)
        {
            std::vector<uint32_t> words(size / 4u);
            for (size_t i = 0; i < words.size(); ++i)
                words[i] = base + static_cast<uint32_t>(i) * 4u;
            (void)memory.writeRam(base, words.data(), words.size() * 4u);
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            dirtyStackForNextCall = true;
            const uint32_t result = callFunction(address, a0, a1, a2, a3, gp, kMaxRpcCallInstructions);
            dirtyStackForNextCall = false;
            return result;
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        // thbase 35-38: SetAlarm, iSetAlarm, CancelAlarm, iCancelAlarm.
        void dispatchAlarmImport(uint16_t ordinal, CpuState &cpu)
        {
            constexpr int32_t kFoundHandler = -104; // KE_FOUND_HANDLER
            constexpr int32_t kNotFoundHandler = -105; // KE_NOTFOUND_HANDLER
            const auto find = [this](uint32_t handler, uint32_t common)
            {
                return std::find_if(alarms.begin(), alarms.end(), [&](const Alarm &alarm)
                                    { return alarm.handler == handler && alarm.common == common; });
            };
            int32_t result = 0;
            if (ordinal == 35u || ordinal == 36u)
            {
                const uint64_t clock = static_cast<uint64_t>(read32(cpu.gpr[4])) |
                                       (static_cast<uint64_t>(read32(cpu.gpr[4] + 4u)) << 32u);
                if (find(cpu.gpr[5], cpu.gpr[6]) != alarms.end())
                    result = kFoundHandler;
                else
                    alarms.push_back(Alarm{totalCycles + std::max<uint64_t>(clock, 1u), cpu.gpr[5], cpu.gpr[6], cpu.gpr[28]});
            }
            else
            {
                const auto it = find(cpu.gpr[4], cpu.gpr[5]);
                if (it == alarms.end())
                    result = kNotFoundHandler;
                else
                    alarms.erase(it);
            }
            cpu.gpr[2] = static_cast<uint32_t>(result);
        }

        void serviceAlarms()
        {
            if (servicingAlarms || alarms.empty())
                return;
            servicingAlarms = true;
            struct Guard
            {
                bool &flag;
                ~Guard() { flag = false; }
            } guard{servicingAlarms};
            for (size_t i = 0; i < alarms.size();)
            {
                if (alarms[i].due > totalCycles)
                {
                    ++i;
                    continue;
                }
                const Alarm alarm = alarms[i];
                const uint32_t next = callFunction(alarm.handler, alarm.common, 0u, 0u, 0u, alarm.gp, 100000u);
                // The handler may have cancelled or re-armed alarms; find this one again.
                const auto it = std::find_if(alarms.begin(), alarms.end(), [&](const Alarm &other)
                                             { return other.handler == alarm.handler && other.common == alarm.common &&
                                                      other.due == alarm.due; });
                if (it == alarms.end())
                {
                    i = 0;
                    continue;
                }
                if (next == 0u)
                {
                    alarms.erase(it);
                    i = 0;
                    continue;
                }
                // Periodic: keep the cadence of the original schedule.
                it->due = std::max(alarm.due + next, totalCycles + 1u);
                ++i;
            }
        }

        uint64_t nextAlarmCycle(uint64_t fallback) const noexcept
        {
            uint64_t next = fallback;
            for (const Alarm &alarm : alarms)
                next = std::min(next, alarm.due);
            return next;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        // SleepThread called by code running outside any thread, i.e. an RPC server function the
        // EE's call executes directly (callFunction). On hardware that code runs in the server thread,
        // which blocks here until a thread it just created wakes it (UBISOUND creates its IOP->EE
        // binder thread this way and reads the result from its own stack afterwards). Run the other
        // threads until the wakeup arrives, bounded so a missing wakeup cannot hang the EE's call.
        void sleepOutsideThread(CpuState &cpu)
        {
            if (outsideWakeups > 0u)
            {
                --outsideWakeups;
                cpu.gpr[2] = 0u;
                return;
            }
            ++outsideSleepers;
            CpuState *const caller = activeCpu;
            const uint64_t deadline = totalCycles + kIopClockHz / 2u;
            while (outsideWakeups == 0u && totalCycles < deadline)
                runCycles(std::min<uint64_t>(deadline - totalCycles, 4096u));
            activeCpu = caller;
            --outsideSleepers;
            if (outsideWakeups > 0u)
                --outsideWakeups;
            else
                log(LogLevel::Warning, "[IOP] SleepThread outside a thread was never woken; continuing");
            cpu.gpr[2] = 0u;
        }

        // Brings the SPU2 up to the current IOP time and turns its events into interrupts.
        void syncSpu2()
        {
            if (totalCycles <= spu2Cycle)
                return;
            const uint32_t events = memory.advanceSpu2(totalCycles - spu2Cycle);
            spu2Cycle = totalCycles;
            if (events & 1u) // AutoDMA core 0 finished
                pendingDmaInterrupts[0x24] = totalCycles;
            if (events & 2u) // AutoDMA core 1 finished
                pendingDmaInterrupts[0x28] = totalCycles;
            if (events & 4u) // SPU2 IRQA hit (dispatched to the registered handler like the DMA IRQs)
                pendingDmaInterrupts[9] = totalCycles;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    syncSpu2();
                    rpc.serviceEeCalls(totalCycles);
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    serviceAlarms();
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        nextWake = nextAlarmCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
                syncSpu2();
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            uint32_t startResult = 0u;
            bool startFinished = false;
            const int startThread = kernel.createStartThread(module.entry, argumentSize, args, module.gp,
                                                             0x4000u, kModuleStartPriority, kThreadReturnSentinel);
            if (startThread > 0)
            {
                // Run the scheduler until the start thread returns. Other threads (for example a worker
                // the module just created and is waiting for) run in between, as on hardware.
                const uint64_t deadline = totalCycles + kModuleStartBudgetCycles;
                while (totalCycles < deadline)
                {
                    const IopThread *thread = kernel.findThread(startThread);
                    if (!thread)
                        break;
                    if (thread->state == IopThreadState::Dormant || thread->state == IopThreadState::Dead)
                    {
                        startResult = thread->cpu.gpr[2];
                        startFinished = true;
                        break;
                    }
                    runCycles(std::min<uint64_t>(deadline - totalCycles, 20000u));
                }
                if (startFinished)
                    kernel.destroyThread(startThread);
                else
                    log(LogLevel::Warning, "[IOP] module start did not return within the budget; leaving it running");
            }
            else
            {
                startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
                startFinished = true;
            }
            if (args && startFinished)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            std::erase_if(alarms, [&](const Alarm &alarm)
                          { return alarm.handler - it->second.base < it->second.size; });
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            (void)memory.freeAllocation(it->second.base);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        // thbase alarms (SetAlarm / CancelAlarm): handler(common) runs in interrupt context when due; its return
        // value is the next interval in IOP cycles (0 = done).
        struct Alarm
        {
            uint64_t due = 0u;
            uint32_t handler = 0u;
            uint32_t common = 0u;
            uint32_t gp = 0u;
        };
        std::vector<Alarm> alarms;
        bool servicingAlarms = false;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t spu2Cycle = 0;
        uint64_t eeCycleCarry = 0;
        // Real-time floor for IOP time (see wallClockDeficit).
        std::chrono::steady_clock::time_point wallBase{};
        uint64_t wallBaseCycle = 0;
        bool wallStarted = false;

        // IOP time normally follows EE cycles / 8. When the host runs the EE slower than a PS2 that
        // drags the IOP along: the sound driver's timers, its streaming and the SPU2 output then run
        // in slow motion. This returns the cycles needed to keep the IOP at least at real-time pace,
        // a bounded amount per call; a backlog beyond 100 ms (host stalls, loading) is dropped.
        uint64_t wallClockDeficit(uint64_t planned)
        {
            using Clock = std::chrono::steady_clock;
            constexpr uint64_t kMaxLag = kIopClockHz / 10u;
            constexpr uint64_t kMaxPerCall = kIopClockHz / 500u; // 2 ms
            // Deterministic mode (PS2X_DETERMINISTIC=1, see ps2_test_harness.cpp): IOP time follows EE cycles only.
            static const bool deterministic = []()
            {
                const char *value = std::getenv("PS2X_DETERMINISTIC");
                return value && std::atoi(value) != 0;
            }();
            if (deterministic)
                return 0;
            const Clock::time_point now = Clock::now();
            const uint64_t projected = totalCycles + planned;
            if (!wallStarted)
            {
                wallStarted = true;
                wallBase = now;
                wallBaseCycle = projected;
                return 0;
            }
            const uint64_t elapsedUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - wallBase).count());
            const uint64_t wallCycle = wallBaseCycle + elapsedUs * (kIopClockHz / 1000u) / 1000u;
            if (wallCycle <= projected)
                return 0;
            uint64_t deficit = wallCycle - projected;
            if (deficit > kMaxLag)
            {
                wallBase = now;
                wallBaseCycle = projected + kMaxLag;
                deficit = kMaxLag;
            }
            return std::min(deficit, kMaxPerCall);
        }
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        uint32_t outsideSleepers = 0u;
        uint32_t outsideWakeups = 0u;
        bool dirtyStackForNextCall = false;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        iopCycles += m_impl->wallClockDeficit(iopCycles);
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
