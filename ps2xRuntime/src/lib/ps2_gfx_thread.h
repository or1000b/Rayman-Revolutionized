// The graphics thread (PS2X_GFX_THREAD, 2026-11-12; code in ps2_memory.cpp, notes in docs/framerate.md).
//
// DMA transfers to VIF1 and the GIF were run to the end on the game thread when the game started them: VIF1
// unpacks, the VU1 programs (40+% of the game thread at 120 FPS) and the packets to the GS. With the graphics
// thread the game thread copies a transfer's data and posts it; one worker runs the posts in order, so the stream
// the GS gets is the same. Everything the game can read back waits for the worker first (sync): VIF1 / GIF
// registers, VU1 memory, GS -> host transfers. The game still sees a transfer complete at once, as before.
//
// Off in deterministic runs (run_tests) unless PS2X_GFX_THREAD=1: GS SIGNAL / FINISH then reach the game when the
// worker gets there, not at a fixed guest cycle.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

struct GSRegisters;

namespace ps2x::gfx
{
    bool enabled();    // decided at the first call
    void forceOff();   // before the first post (the CPU renderer reads GS memory from the window thread)
    bool onWorker();   // the calling thread is the graphics thread
    // enabled() and not on the worker: the caller posts its work instead of doing it.
    bool shouldPost();
    // Debug: PS2X_GFX_MASK bits (default 7): 1 = DMA transfers, 2 = GIF packets from stubs, 4 = GS register writes.
    bool shouldPost(unsigned kind);
    // Runs task on the worker, after everything posted before. bytes: the data it carries (the game thread waits
    // when the worker is more than a few MiB behind). priv: the GS privileged registers; their display fields as
    // of now become the picture's once the worker reaches this task (see shadowDisplay).
    void post(std::function<void()> task, size_t bytes, const GSRegisters *priv);
    // Waits until the worker has run everything posted (no-op when off or on the worker).
    void sync();
    // The display registers (pmode, dispfb1, display1, dispfb2, display2) as of the task the worker is running.
    // False when the worker is idle: the live registers are right. A buffer flip the game made after posting its
    // frame must not show before the worker has drawn that frame.
    bool shadowDisplay(uint64_t out[5]);
    void stop();
}
