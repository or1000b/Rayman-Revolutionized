#pragma once
#include <cstdint>

constexpr uint32_t kThreadReturnSentinel = 0x1FFFFF00u;
constexpr uint32_t kCallReturnSentinel = 0x1FFFFF04u;
constexpr uint64_t kIopClockHz = 36'864'000ull;
// NTSC field cadence (approximately 59.94 Hz).  VBlank imports are
// scheduler waits, not no-op timing hints: returning immediately lets
// high-priority IRX threads busy-loop and starve RPC server threads.
constexpr uint64_t kVblankPeriodCycles = (kIopClockHz * 1001ull + 30'000ull) / 60'000ull;
constexpr uint64_t kVblankEndPhaseCycles = kVblankPeriodCycles / 16ull;
constexpr uint32_t kDefaultSlice = 256u;
constexpr uint32_t kMaxCallInstructions = 2'000'000u;
// RPC server functions run to completion inside one host call (the EE side waits for the reply). Some take long:
// UBISOUND's bank load (module "load", command 2) writes a 1.8 MiB bank to SPU2 RAM halfword by halfword. Cut off
// early, the handler leaves the bank partly loaded and the reply unwritten (the game then plays every sound from
// SPU2 address 0).
constexpr uint32_t kMaxRpcCallInstructions = 400'000'000u;
constexpr uint32_t kModuleLoadBase = 0x00010000u;
constexpr uint32_t kStackGuardBytes = 64u;
// Module start routines run in a kernel thread at MODLOAD's priority, bounded by this budget.
constexpr uint32_t kModuleStartPriority = 8u;
constexpr uint64_t kModuleStartBudgetCycles = kIopClockHz * 3ull;
