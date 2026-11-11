#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // PS2X_PAD_DEADZONE (0 .. 0.9, default 0.15): stick travel ignored around the centre (radial). Worn sticks rest a
    // little off centre, which would walk Rayman by himself.
    float padDeadZone()
    {
        static const float zone = []
        {
            const char *env = std::getenv("PS2X_PAD_DEADZONE");
            const float value = env ? static_cast<float>(std::atof(env)) : 0.15f;
            return std::clamp(value, 0.0f, 0.9f);
        }();
        return zone;
    }

    // A stick (x, y in -1 .. 1) with the dead zone, rescaled so full travel stays full, as DualShock bytes
    // (0x00 left / up, 0x80 centre, 0xFF right / down).
    void stickToBytes(float x, float y, uint8_t &outX, uint8_t &outY)
    {
        const float zone = padDeadZone();
        const float length = std::sqrt(x * x + y * y);
        if (length <= zone || length <= 0.0f)
        {
            x = y = 0.0f;
        }
        else
        {
            const float scale = std::min(1.0f, (length - zone) / (1.0f - zone)) / length;
            x *= scale;
            y *= scale;
        }
        auto toByte = [](float v)
        { return static_cast<uint8_t>(std::lround((std::clamp(v, -1.0f, 1.0f) + 1.0f) * 127.5f)); };
        outX = toByte(x);
        outY = toByte(y);
    }
}

// Xbox controllers (XInput) and PlayStation 3 / 4 / 5 controllers (DirectInput, through GLFW's built-in SDL
// GameControllerDB mappings; ps2_runtime.cpp adds the DualSense Edge and PS2X_PAD_MAPPINGS) via raylib. Every pad is
// seen in the Xbox layout, and the DualShock 2's buttons go by position: A / Cross, B / Circle, X / Square,
// Y / Triangle, LB / L1, RB / R1, LT / L2, RT / R2, Back (Share, Create) / Select, Start (Options) / Start, stick
// clicks / L3 / R3, D-pad, both sticks. The first connected pad answers (plugging one in or out while playing works).
// buttons: active low, DualShock bit order; axes: rx, ry, lx, ly (0x80 = centre). False when no pad is connected.
extern "C" bool ps2xHostGamepadRead(uint16_t *buttons, uint8_t axes[4])
{
    static int s_lastPad = -2;
    int pad = -1;
    for (int i = 0; i < 4 && pad < 0; ++i)
        if (IsGamepadAvailable(i))
            pad = i;
    if (pad != s_lastPad)
    {
        if (pad >= 0)
            std::fprintf(stderr, "[pad] controller %d connected: %s\n", pad, GetGamepadName(pad));
        else if (s_lastPad >= 0)
            std::fprintf(stderr, "[pad] controller disconnected\n");
        s_lastPad = pad;
    }
    if (pad < 0)
        return false;

    struct ButtonMap
    {
        int raylib;
        uint16_t ps2;
    };
    static constexpr ButtonMap kMap[] = {
        {GAMEPAD_BUTTON_LEFT_FACE_UP, 0x0010u},     // Up
        {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, 0x0020u},  // Right
        {GAMEPAD_BUTTON_LEFT_FACE_DOWN, 0x0040u},   // Down
        {GAMEPAD_BUTTON_LEFT_FACE_LEFT, 0x0080u},   // Left
        {GAMEPAD_BUTTON_RIGHT_FACE_UP, 0x1000u},    // Triangle (Y)
        {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, 0x2000u}, // Circle (B)
        {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, 0x4000u},  // Cross (A)
        {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, 0x8000u},  // Square (X)
        {GAMEPAD_BUTTON_LEFT_TRIGGER_1, 0x0400u},   // L1
        {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, 0x0800u},  // R1
        {GAMEPAD_BUTTON_LEFT_TRIGGER_2, 0x0100u},   // L2 (raylib: trigger past 10%)
        {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, 0x0200u},  // R2
        {GAMEPAD_BUTTON_MIDDLE_LEFT, 0x0001u},      // Select
        {GAMEPAD_BUTTON_MIDDLE_RIGHT, 0x0008u},     // Start
        {GAMEPAD_BUTTON_LEFT_THUMB, 0x0002u},       // L3
        {GAMEPAD_BUTTON_RIGHT_THUMB, 0x0004u},      // R3
    };
    uint16_t pressed = 0u;
    for (const ButtonMap &m : kMap)
        if (IsGamepadButtonDown(pad, m.raylib))
            pressed |= m.ps2;
    *buttons = static_cast<uint16_t>(~pressed);
    stickToBytes(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y),
                 axes[0], axes[1]);
    stickToBytes(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y),
                 axes[2], axes[3]);
    return true;
}

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // Xbox / PlayStation controller (ps2xHostGamepadRead above): buttons, both sticks with a dead zone.
    {
        uint16_t padButtons = 0xFFFFu;
        uint8_t axes[4];
        if (ps2xHostGamepadRead(&padButtons, axes))
        {
            btns &= padButtons;
            data[4] = axes[0];
            data[5] = axes[1];
            data[6] = axes[2];
            data[7] = axes[3];
        }
    }
    // Keyboard is always read too (not only when no gamepad is detected), so a
    // connected-but-idle controller does not silently disable the keyboard.
    // Rayman Revolution (user's mapping, 2026-11-11): arrows = move, A = jump (Cross), Space = shoot (Square).
    // The game moves Rayman with the left analog stick only (the D-pad does not move him in a level) and its
    // menus use the D-pad, so the arrows drive both. Z / X / C / V stay Square / Cross / Circle / Triangle.
    {
        const bool up = IsKeyDown(KEY_UP), down = IsKeyDown(KEY_DOWN);
        const bool left = IsKeyDown(KEY_LEFT), right = IsKeyDown(KEY_RIGHT);
        if (up)
            clearBit(PAD_UP);
        if (down)
            clearBit(PAD_DOWN);
        if (left)
            clearBit(PAD_LEFT);
        if (right)
            clearBit(PAD_RIGHT);
        // Arrows = left analog stick (full deflection) as well
        {
            const int ax = (right ? 1 : 0) - (left ? 1 : 0);
            const int ay = (down ? 1 : 0) - (up ? 1 : 0);
            if (ax != 0)
                data[6] = static_cast<uint8_t>(ax > 0 ? 0xFF : 0x00);
            if (ay != 0)
                data[7] = static_cast<uint8_t>(ay > 0 ? 0xFF : 0x00);
        }
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_A))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_ONE))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_THREE))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_RIGHT_SHIFT) || IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
