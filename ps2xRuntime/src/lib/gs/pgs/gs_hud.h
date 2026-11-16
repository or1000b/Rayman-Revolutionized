#pragma once

// The HUD layer (PS2X_HUD, 2026-11-15; docs/hud.md).
//
// Rayman Revolution draws its whole 2D layer (text, Murfy's face, menu items, the text box, fades) through VU1 with
// every vertex at Z = 0xFFFF, after the 3D of each frame (checked in captures of the menus and a level: no 3D is
// drawn after it). The GS thread takes those packets out of what paraLLEl-GS draws (their vertices become
// non-drawing: ADC = 1) and records them as triangles, with their textures decoded on the CPU from the uploads seen in
// the stream (PSMT8 / PSMT4 with a PSMCT32 CLUT, PSMCT32). The window draws them over the picture at screen
// resolution, with a 4x texture from hud_textures/<key>.png when there is one (tools/hud_upscale.py), and in 16:9 it
// keeps their 4:3 shape (full-width quads such as fades stay full width). A packet whose texture cannot be decoded
// stays with paraLLEl-GS.
//
// PS2X_HUD: unset = on in normal play, off in test mode (run_tests compares the GS picture); 1 / 0 force it.
// PS2X_HUD_DUMP=1: the window writes every texture it meets to hud_dump/<key>.png (the input of hud_upscale.py).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::hud
{
    // A vertex in the game's screen space (the 512 x 512 area the game draws into), texture coordinates normalized
    // to the texture, colour ready for the window's modulate (255 = 1.0, alpha 255 = opaque).
    struct Vertex
    {
        float x, y, u, v;
        uint8_t r, g, b, a;
    };

    enum Blend : uint8_t
    {
        BlendAlpha,
        BlendAdd
    };

    // Triangles (3 vertices each) with one texture (0 = none) and one blend.
    struct Batch
    {
        uint64_t texture = 0;
        Blend blend = BlendAlpha;
        bool fullWidth = false; // spans the whole width (a fade, a backdrop): stretched in 16:9, not kept 4:3
        std::vector<Vertex> vertices;
    };

    struct Frame
    {
        std::vector<Batch> batches;
        bool drawOnly = false; // drawn by a frame without game logic (PS2X_RAYMAN_LOGIC_HZ): see markFrame
    };

    struct Texture
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    bool enabled();

    // GS thread, with the bridge's lock: GIF packets about to go to paraLLEl-GS. The HUD drawing in them is recorded;
    // returns data, or scratch holding a copy where those vertices no longer draw. Register writes outside packets go
    // to registerWrite.
    const uint8_t *filterPacket(const uint8_t *data, size_t size, std::vector<uint8_t> &scratch);
    void registerWrite(uint32_t reg, uint64_t value);
    // At each scanout (GS thread): the HUD of the displayed buffer becomes the one latest() returns.
    void publish(uint64_t pmode, uint64_t dispfb1, uint64_t dispfb2);
    // GS thread, in stream order: the frame the game starts drawing now has no game logic (drawOnly) or has it. With
    // the logic at the console's rate (main.cpp PS2X_RAYMAN_LOGIC_HZ) some 2D is drawn only by the logic (the box
    // behind Murfy's text, parts of the HUD): a draw-only frame's 2D is not published, the last logic frame's 2D stays
    // (the 2D then changes 25 times a second, as on the console). Frames without a mark count as logic frames.
    void markFrame(bool drawOnly);

    // Window thread: the HUD of the last scanout, and a texture by key.
    void latest(Frame &out);
    bool texture(uint64_t key, Texture &out);

    // " | hud packets=N frames=N textures=N ..." for the periodic log line (empty when off).
    std::string statsLine();
}
