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
constexpr uint32_t kModuleLoadBase = 0x00010000u;
constexpr uint32_t kStackGuardBytes = 64u;
// Module start routines run in a kernel thread at MODLOAD's priority, bounded by this budget.
constexpr uint32_t kModuleStartPriority = 8u;
constexpr uint64_t kModuleStartBudgetCycles = kIopClockHz * 3ull;
