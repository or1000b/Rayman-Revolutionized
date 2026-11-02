#pragma once

// GPU GS renderer: the game's GS command stream is drawn by paraLLEl-GS (Vulkan compute shaders,
// https://github.com/Arntzen-Software/parallel-gs, LGPL-3.0-or-later) instead of the CPU rasteriser.
//
// This header has no Vulkan / Granite types so the runtime does not see them. The bridge library is
// built only when PS2X_ENABLE_PARALLEL_GS is on; otherwise every call reports "not active" and the
// CPU renderer stays in charge.
//
// Threading: the bridge is not thread-safe. The GS frontend calls it under its own state mutex.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::pgs
{
    // GS privileged registers at vsync time (same order as the PS2 register file at 0x12000000).
    struct PrivRegs
    {
        uint64_t pmode = 0;
        uint64_t smode1 = 0;
        uint64_t smode2 = 0;
        uint64_t srfsh = 0;
        uint64_t synch1 = 0;
        uint64_t synch2 = 0;
        uint64_t syncv = 0;
        uint64_t dispfb1 = 0;
        uint64_t display1 = 0;
        uint64_t dispfb2 = 0;
        uint64_t display2 = 0;
        uint64_t extbuf = 0;
        uint64_t extdata = 0;
        uint64_t extwrite = 0;
        uint64_t bgcolor = 0;
    };

    // SIGNAL / FINISH / LABEL register writes seen in the command stream (the CSR side effects).
    class SignalSink
    {
    public:
        virtual ~SignalSink() = default;
        virtual void signal(uint64_t payload) = 0;
        virtual void finish(uint64_t payload) = 0;
        virtual void label(uint64_t payload) = 0;
    };

    // True when the bridge was compiled in (PS2X_HAS_PARALLEL_GS).
    bool available();

    // Creates the Vulkan device and the GS. Returns false (and a reason) when it cannot run here; the
    // caller then keeps the CPU renderer.
    bool start(SignalSink *sink, std::string &error);
    bool active();
    void shutdown();

    // A GIF transfer (whole qwords) on PATH1..3 (path 1..3; 0 is accepted as PATH3).
    void gifTransfer(uint32_t path, const void *data, size_t bytes);
    // A+D style register write (PRIM, XYZ2, TEX0_1, ... TRXDIR, SIGNAL, FINISH ...).
    void writeRegister(uint32_t reg, uint64_t value);
    // GS -> host image transfer (TRXDIR = 1): reads `bytes` (rounded up to qwords) from the FIFO.
    size_t readTransfer(void *dst, size_t bytes);

    // Ends the frame: scans out what the CRTC shows for `regs` and reads it back as tightly packed
    // RGBA8 rows (alpha forced opaque). Returns false when nothing is displayed.
    bool scanout(const PrivRegs &regs, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);

    // One line for the periodic log ("pgs: <gpu name> frames=N ...").
    std::string statsLine();

    // GIF path (1..3) of the packet the GIF arbiter is handing to the GS frontend right now, so each
    // path keeps its own GIF tag state on the GPU renderer (a PATH3 image can be split across DMA tags).
    void setCurrentPath(uint32_t path);
    uint32_t currentPath();
}
