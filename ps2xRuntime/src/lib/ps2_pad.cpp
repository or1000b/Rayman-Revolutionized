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

    // ------------------------------------------------------------------ the player's controls (2026-11-19)
    // The launcher's Controls tab writes controls.ini in the port's folder (PS2X_CONTROLS=<file> for another one):
    // one line per action, "name=key1,key2,pad". key: a raylib KeyboardKey, 1000 + n = mouse button n, 0 = none;
    // pad: a raylib GamepadButton (Xbox layout), 0 = none, -1 = not mappable (the sticks). Without the file every
    // control is as before. Move = the left stick and the D-pad (the game moves Rayman with the stick, its menus use
    // the D-pad); Camera = the right stick. A controller's sticks and D-pad always work as themselves.
    enum ControlAction
    {
        kMoveUp, kMoveDown, kMoveLeft, kMoveRight,
        kCamUp, kCamDown, kCamLeft, kCamRight,
        kCross, kSquare, kCircle, kTriangle, kL1, kR1, kL2, kR2, kStart, kSelect, kL3, kR3,
        kActionCount
    };
    constexpr const char *kActionNames[kActionCount] = {
        "move_up", "move_down", "move_left", "move_right", "camera_up", "camera_down", "camera_left", "camera_right",
        "cross", "square", "circle", "triangle", "l1", "r1", "l2", "r2", "start", "select", "l3", "r3"};
    constexpr uint16_t kActionBits[kActionCount] = {
        0x0010u, 0x0040u, 0x0080u, 0x0020u, 0u, 0u, 0u, 0u,
        0x4000u, 0x8000u, 0x2000u, 0x1000u, 0x0400u, 0x0800u, 0x0100u, 0x0200u, 0x0008u, 0x0001u, 0x0002u, 0x0004u};
    constexpr int kMouseBase = 1000;
    constexpr int kKeysPerAction = 3;

    struct ControlBinding
    {
        int keys[kKeysPerAction];
        int pad;
    };

    struct ControlTable
    {
        ControlBinding action[kActionCount];
    };

    // The controls before the Controls tab (the user's mapping, 2026-11-11): arrows move, A jumps, Space shoots.
    ControlTable defaultControls()
    {
        ControlTable t{};
        auto set = [&](ControlAction a, int k1, int k2, int k3, int pad) { t.action[a] = {{k1, k2, k3}, pad}; };
        set(kMoveUp, KEY_UP, 0, 0, -1);
        set(kMoveDown, KEY_DOWN, 0, 0, -1);
        set(kMoveLeft, KEY_LEFT, 0, 0, -1);
        set(kMoveRight, KEY_RIGHT, 0, 0, -1);
        set(kCamUp, 0, 0, 0, -1);
        set(kCamDown, 0, 0, 0, -1);
        set(kCamLeft, 0, 0, 0, -1);
        set(kCamRight, 0, 0, 0, -1);
        set(kCross, KEY_A, KEY_X, 0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        set(kSquare, KEY_SPACE, KEY_Z, KEY_KP_0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
        set(kCircle, KEY_C, KEY_ESCAPE, 0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
        set(kTriangle, KEY_V, KEY_KP_1, 0, GAMEPAD_BUTTON_RIGHT_FACE_UP);
        set(kL1, KEY_Q, 0, 0, GAMEPAD_BUTTON_LEFT_TRIGGER_1);
        set(kR1, KEY_E, 0, 0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
        set(kL2, KEY_ONE, 0, 0, GAMEPAD_BUTTON_LEFT_TRIGGER_2);
        set(kR2, KEY_THREE, 0, 0, GAMEPAD_BUTTON_RIGHT_TRIGGER_2);
        set(kStart, KEY_ENTER, 0, 0, GAMEPAD_BUTTON_MIDDLE_RIGHT);
        set(kSelect, KEY_TAB, KEY_RIGHT_SHIFT, 0, GAMEPAD_BUTTON_MIDDLE_LEFT);
        set(kL3, 0, 0, 0, GAMEPAD_BUTTON_LEFT_THUMB);
        set(kR3, 0, 0, 0, GAMEPAD_BUTTON_RIGHT_THUMB);
        return t;
    }

    ControlTable loadControls()
    {
        ControlTable t = defaultControls();
        const char *env = std::getenv("PS2X_CONTROLS");
        const char *path = (env && *env) ? env : "controls.ini";
        std::FILE *f = std::fopen(path, "r");
        if (!f)
            return t;
        char line[256];
        int read = 0;
        while (std::fgets(line, sizeof(line), f))
        {
            char *eq = std::strchr(line, '=');
            if (!eq || line[0] == '#')
                continue;
            *eq = '\0';
            for (int a = 0; a < kActionCount; ++a)
            {
                if (std::strcmp(line, kActionNames[a]) != 0)
                    continue;
                int k1 = 0, k2 = 0, pad = 0;
                if (std::sscanf(eq + 1, "%d,%d,%d", &k1, &k2, &pad) >= 2)
                {
                    t.action[a] = {{k1, k2, 0}, kActionBits[a] ? pad : -1};
                    ++read;
                }
            }
        }
        std::fclose(f);
        std::fprintf(stderr, "[pad] controls from %s (%d actions)\n", path, read);
        return t;
    }

    const ControlTable &controls()
    {
        static const ControlTable t = loadControls();
        return t;
    }

    bool controlDown(const ControlBinding &b)
    {
        for (int code : b.keys)
        {
            if (code >= kMouseBase && code < kMouseBase + 8 ? IsMouseButtonDown(code - kMouseBase) : (code > 0 && IsKeyDown(code)))
                return true;
        }
        return false;
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
    static constexpr ButtonMap kDpad[] = {
        {GAMEPAD_BUTTON_LEFT_FACE_UP, 0x0010u},    // Up
        {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, 0x0020u}, // Right
        {GAMEPAD_BUTTON_LEFT_FACE_DOWN, 0x0040u},  // Down
        {GAMEPAD_BUTTON_LEFT_FACE_LEFT, 0x0080u},  // Left
    };
    uint16_t pressed = 0u;
    for (const ButtonMap &m : kDpad)
        if (IsGamepadButtonDown(pad, m.raylib))
            pressed |= m.ps2;
    // The other buttons as the player mapped them (controls.ini; by default A / Cross, B / Circle, X / Square,
    // Y / Triangle, LB / L1, RB / R1, LT / L2 (raylib: trigger past 10%), RT / R2, Back / Select, Start, stick clicks).
    const ControlTable &table = controls();
    for (int a = kCross; a < kActionCount; ++a)
        if (table.action[a].pad > 0 && IsGamepadButtonDown(pad, table.action[a].pad))
            pressed |= kActionBits[a];
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
    // Keyboard and mouse are always read too (not only when no gamepad is detected), so a connected-but-idle
    // controller does not silently disable them. As the player mapped them (controls.ini, the launcher's Controls
    // tab); by default the user's mapping of 2026-11-11: arrows = move, A = jump (Cross), Space = shoot (Square),
    // Z / X / C / V = Square / Cross / Circle / Triangle. The game moves Rayman with the left analog stick only (the
    // D-pad does not move him in a level) and its menus use the D-pad, so Move drives both.
    {
        const ControlTable &table = controls();
        const bool up = controlDown(table.action[kMoveUp]), down = controlDown(table.action[kMoveDown]);
        const bool left = controlDown(table.action[kMoveLeft]), right = controlDown(table.action[kMoveRight]);
        if (up)
            clearBit(PAD_UP);
        if (down)
            clearBit(PAD_DOWN);
        if (left)
            clearBit(PAD_LEFT);
        if (right)
            clearBit(PAD_RIGHT);
        // Move = left analog stick (full deflection) as well
        {
            const int ax = (right ? 1 : 0) - (left ? 1 : 0);
            const int ay = (down ? 1 : 0) - (up ? 1 : 0);
            if (ax != 0)
                data[6] = static_cast<uint8_t>(ax > 0 ? 0xFF : 0x00);
            if (ay != 0)
                data[7] = static_cast<uint8_t>(ay > 0 ? 0xFF : 0x00);
        }
        // Camera = right analog stick (full deflection)
        {
            const int cx = (controlDown(table.action[kCamRight]) ? 1 : 0) - (controlDown(table.action[kCamLeft]) ? 1 : 0);
            const int cy = (controlDown(table.action[kCamDown]) ? 1 : 0) - (controlDown(table.action[kCamUp]) ? 1 : 0);
            if (cx != 0)
                data[4] = static_cast<uint8_t>(cx > 0 ? 0xFF : 0x00);
            if (cy != 0)
                data[5] = static_cast<uint8_t>(cy > 0 ? 0xFF : 0x00);
        }
        for (int a = kCross; a < kActionCount; ++a)
            if (controlDown(table.action[a]))
                clearBit(kActionBits[a]);
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
