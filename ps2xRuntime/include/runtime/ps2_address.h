#ifndef PS2_ADDRESS_H
#define PS2_ADDRESS_H

#include <cstdint>

#include "runtime/ps2_memory.h"

// The guest memory helpers run for every load and store of the generated code (millions per frame). MSVC did not
// inline them into the generated functions (each READ / WRITE was two calls: the address check and the RAM access),
// so they are forced (2026-11-16, docs/framerate.md).
#ifndef PS2_FORCE_INLINE
#if defined(_MSC_VER)
#define PS2_FORCE_INLINE __forceinline
#define PS2_NOINLINE __declspec(noinline)
#else
#define PS2_FORCE_INLINE inline __attribute__((always_inline))
#define PS2_NOINLINE __attribute__((noinline))
#endif
#endif

static inline constexpr uint32_t PS2_EE_UNCACHED_RAM_MIRROR_BASE = 0x20000000u;
static inline constexpr uint32_t PS2_EE_UNCACHED_RAM_MIRROR_SIZE = 0x20000000u;
static inline constexpr uint32_t PS2_KSEG0_BASE = 0x80000000u;
static inline constexpr uint32_t PS2_KSEG0_KSEG1_SIZE = 0x40000000u;
static inline constexpr uint32_t PS2_KSEG2_BASE = 0xC0000000u;

static PS2_FORCE_INLINE constexpr bool Ps2AddressInRange(uint32_t value, uint32_t base, uint32_t size)
{
    return (value - base) < size;
}

static PS2_FORCE_INLINE constexpr bool Ps2IsUncachedRamMirrorAddress(uint32_t addr)
{
    return Ps2AddressInRange(addr, PS2_EE_UNCACHED_RAM_MIRROR_BASE, PS2_EE_UNCACHED_RAM_MIRROR_SIZE);
}

static PS2_FORCE_INLINE constexpr bool Ps2IsKseg01Address(uint32_t addr)
{
    return Ps2AddressInRange(addr, PS2_KSEG0_BASE, PS2_KSEG0_KSEG1_SIZE);
}

static PS2_FORCE_INLINE constexpr bool Ps2IsKseg23Address(uint32_t addr)
{
    return addr >= PS2_KSEG2_BASE;
}

static PS2_FORCE_INLINE constexpr uint32_t Ps2DirectMappedPhysicalAddress(uint32_t addr)
{
    return addr & 0x1FFFFFFFu;
}

static PS2_FORCE_INLINE constexpr uint32_t Ps2PhysicalAddress(uint32_t addr)
{
    return (addr >= PS2_KSEG0_BASE) ? Ps2DirectMappedPhysicalAddress(addr) : addr;
}

static PS2_FORCE_INLINE constexpr bool Ps2IsPhysicalSpecialAddress(uint32_t physAddr)
{
    return Ps2AddressInRange(physAddr, PS2_BIOS_BASE, PS2_BIOS_SIZE) ||
           Ps2AddressInRange(physAddr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE) ||
           Ps2AddressInRange(physAddr, PS2_IO_BASE, PS2_IO_SIZE) ||
           Ps2AddressInRange(physAddr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE) ||
           (physAddr >= PS2_VU0_DATA_BASE && physAddr < (PS2_VU1_CODE_BASE + PS2_VU1_CODE_SIZE));
}

// The full check, out of line: only addresses outside main RAM get here (and then the access is the slow one anyway).
static PS2_NOINLINE constexpr bool Ps2IsSpecialAddressOutsideRam(uint32_t addr)
{
    if (Ps2IsKseg23Address(addr))
        return true;

    return Ps2IsPhysicalSpecialAddress(Ps2PhysicalAddress(addr));
}

static PS2_FORCE_INLINE constexpr bool Ps2IsSpecialAddress(uint32_t addr)
{
    // Main RAM through any of its views (kuseg, the uncached 0x20000000 / 0x30000000 mirrors, kseg0 / kseg1) is
    // never special: every special range lies at 32 MiB or above in the low 29 bits.
    if (addr < PS2_KSEG2_BASE && (addr & 0x1FFFFFFFu) < PS2_RAM_SIZE)
        return false;
    return Ps2IsSpecialAddressOutsideRam(addr);
}

#endif // PS2_ADDRESS_H
