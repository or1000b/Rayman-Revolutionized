// Rayman Revolution - PC port launcher (the user's specification, 2026-11-19): install the port from the player's own
// PAL disc image, choose the graphics settings, play, and update from GitHub. English only. The look follows the game
// (a night jungle with floating lums, a golden title, the green-to-white lettering of its menus) with original
// drawings: nothing of the game's own art is used or shipped.
#include "raylib.h"
#include "rlgl.h"

#include "iso9660.h"
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace launcher;

namespace
{
    constexpr int kWidth = 1120;
    constexpr int kHeight = 700;

    // ---------------------------------------------------------------- palette
    const Color kNightTop{8, 22, 56, 255};
    const Color kNightBottom{52, 22, 96, 255};
    const Color kGoldLight{255, 238, 140, 255};
    const Color kGold{255, 196, 52, 255};
    const Color kGoldDeep{226, 124, 22, 255};
    const Color kTitleOutline{120, 22, 14, 255};
    const Color kBrownText{72, 28, 8, 255};
    const Color kPanelFill{26, 14, 56, 236};
    const Color kPanelBorder{238, 186, 68, 255};
    const Color kPurple{78, 44, 142, 255};
    const Color kPurpleLight{112, 74, 186, 255};
    const Color kPillFill{36, 20, 74, 255};
    const Color kLumCore{255, 248, 190, 255};
    const Color kLumGlow{255, 206, 56, 255};
    const Color kMenuGreenTop{238, 255, 226, 255}; // the game's lettering: white-green at the top...
    const Color kMenuGreenBottom{86, 232, 64, 255}; // ...bright green at the bottom
    const Color kText{238, 232, 255, 255};
    const Color kTextDim{176, 164, 214, 255};
    const Color kOk{120, 236, 96, 255};
    const Color kWarn{255, 176, 72, 255};
    const Color kError{255, 110, 96, 255};

    // ---------------------------------------------------------------- fonts and drawing helpers
    struct Fonts
    {
        Font title, black, blackSmall, brush, label, body;
    } g_font;

    Font loadFont(const char *file, int size)
    {
        const std::string path = std::string("C:/Windows/Fonts/") + file;
        Font font = FileExists(path.c_str()) ? LoadFontEx(path.c_str(), size, nullptr, 0) : GetFontDefault();
        SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        return font;
    }

    // Vertical colour gradient for text: mixes two colours by the pixel's height inside [y0, y1] on screen.
    Shader g_gradient;
    int g_locTop = -1, g_locBottom = -1, g_locRange = -1;
    const char *kGradientFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec4 colTop;
uniform vec4 colBottom;
uniform vec2 yRange;
void main()
{
    float a = texture(texture0, fragTexCoord).a * fragColor.a;
    float t = clamp((yRange.x - gl_FragCoord.y) / max(1.0, yRange.x - yRange.y), 0.0, 1.0);
    vec4 c = mix(colTop, colBottom, t);
    finalColor = vec4(c.rgb, c.a * a);
}
)";

    Vector4 norm(Color c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}; }

    void textGradient(const Font &font, const char *text, Vector2 pos, float size, float spacing, Color top, Color bottom)
    {
        const float h = static_cast<float>(GetScreenHeight());
        const Vector4 t = norm(top), b = norm(bottom);
        const float range[2] = {h - pos.y - size * 0.18f, h - pos.y - size * 0.92f};
        BeginShaderMode(g_gradient);
        SetShaderValue(g_gradient, g_locTop, &t, SHADER_UNIFORM_VEC4);
        SetShaderValue(g_gradient, g_locBottom, &b, SHADER_UNIFORM_VEC4);
        SetShaderValue(g_gradient, g_locRange, range, SHADER_UNIFORM_VEC2);
        DrawTextEx(font, text, pos, size, spacing, WHITE);
        EndShaderMode();
    }

    void textOutline(const Font &font, const char *text, Vector2 pos, float size, float spacing, Color outline, float thick)
    {
        for (int i = 0; i < 16; ++i)
        {
            const float a = static_cast<float>(i) * PI / 8.0f;
            DrawTextEx(font, text, {pos.x + std::cos(a) * thick, pos.y + std::sin(a) * thick}, size, spacing, outline);
        }
    }

    float textWidth(const Font &font, const char *text, float size, float spacing) { return MeasureTextEx(font, text, size, spacing).x; }

    float roundness(Rectangle r, float radius) { return std::min(1.0f, 2.0f * radius / std::min(r.width, r.height)); }

    void panel(Rectangle r)
    {
        DrawRectangleRounded({r.x + 6, r.y + 9, r.width, r.height}, roundness(r, 18), 12, Fade(BLACK, 0.35f));
        DrawRectangleRounded(r, roundness(r, 18), 12, kPanelFill);
        DrawRectangleRoundedLinesEx(r, roundness(r, 18), 12, 3.0f, kPanelBorder);
        const Rectangle inner{r.x + 7, r.y + 7, r.width - 14, r.height - 14};
        DrawRectangleRoundedLinesEx(inner, roundness(inner, 13), 12, 1.0f, Fade(kPanelBorder, 0.35f));
    }

    // ---------------------------------------------------------------- the night jungle background
    struct Lum
    {
        float x, y, r, phase, speed;
    };
    std::vector<Lum> g_lums;
    struct Star
    {
        float x, y, size, phase, speed;
    };
    std::vector<Star> g_stars;

    float rnd(float a, float b) { return a + (b - a) * static_cast<float>(GetRandomValue(0, 10000)) / 10000.0f; }

    void initBackground()
    {
        SetRandomSeed(1999); // the same sky every time
        for (int i = 0; i < 14; ++i)
            g_lums.push_back({rnd(30, kWidth - 30), rnd(60, kHeight - 120), rnd(5.5f, 10.0f), rnd(0, 6.28f), rnd(0.6f, 1.4f)});
        for (int i = 0; i < 90; ++i)
            g_stars.push_back({rnd(0, kWidth), rnd(0, kHeight * 0.62f), rnd(0.8f, 2.2f), rnd(0, 6.28f), rnd(1.0f, 3.0f)});
    }

    void hill(float baseY, float amp, float freq, float phase, Color color)
    {
        const float h = static_cast<float>(kHeight);
        auto y = [&](float x) { return baseY + amp * std::sin(x * freq + phase) + amp * 0.45f * std::sin(x * freq * 2.3f + phase * 1.7f); };
        for (float x = 0; x < kWidth; x += 8)
        {
            const float x1 = x + 8;
            DrawTriangle({x, y(x)}, {x, h}, {x1, y(x1)}, color);
            DrawTriangle({x1, y(x1)}, {x, h}, {x1, h}, color);
        }
    }

    // A big jungle leaf from base along angle (radians), drawn as a strip with a lighter midrib.
    void leaf(Vector2 base, float angle, float length, float width, Color color, float sway)
    {
        const float a = angle + sway;
        const Vector2 dir{std::cos(a), std::sin(a)}, nrm{-dir.y, dir.x};
        constexpr int kSteps = 18;
        Vector2 prevL = base, prevR = base;
        for (int i = 1; i <= kSteps; ++i)
        {
            const float t = static_cast<float>(i) / kSteps;
            const float w = width * std::pow(std::sin(PI * t), 0.75f) * (1.0f - 0.25f * t);
            const Vector2 c{base.x + dir.x * length * t, base.y + dir.y * length * t + 30.0f * t * t};
            const Vector2 l{c.x + nrm.x * w, c.y + nrm.y * w}, r{c.x - nrm.x * w, c.y - nrm.y * w};
            DrawTriangle(prevL, prevR, l, color);
            DrawTriangle(l, prevR, r, color);
            prevL = l;
            prevR = r;
        }
        DrawLineBezier(base, {base.x + dir.x * length, base.y + dir.y * length + 30.0f}, 2.0f, Fade(WHITE, 0.10f));
    }

    void drawLum(float x, float y, float r, float pulse)
    {
        BeginBlendMode(BLEND_ADDITIVE);
        DrawCircleGradient(static_cast<int>(x), static_cast<int>(y), r * (4.0f + pulse), Fade(kLumGlow, 0.32f), Fade(kLumGlow, 0.0f));
        EndBlendMode();
        DrawCircleV({x, y}, r, kLumGlow);
        DrawCircleV({x - r * 0.18f, y - r * 0.18f}, r * 0.72f, kGoldLight);
        DrawCircleV({x - r * 0.35f, y - r * 0.38f}, r * 0.28f, kLumCore);
    }

    void background(float t)
    {
        DrawRectangleGradientV(0, 0, kWidth, kHeight, kNightTop, kNightBottom);
        // moonlight
        BeginBlendMode(BLEND_ADDITIVE);
        DrawCircleGradient(520, -40, 420, Fade({120, 170, 255, 255}, 0.18f), Fade(BLACK, 0.0f));
        EndBlendMode();
        for (const Star &s : g_stars)
        {
            const float a = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * s.speed + s.phase));
            DrawCircleV({s.x, s.y}, s.size, Fade(WHITE, a * 0.8f));
        }
        hill(470, 26, 0.006f, 0.4f, {24, 46, 92, 255});
        hill(520, 30, 0.009f, 2.1f, {16, 58, 74, 255});
        hill(585, 22, 0.012f, 4.0f, {10, 52, 48, 255});
        hill(640, 16, 0.016f, 1.0f, {6, 34, 30, 255});
        // leaves in the corners, swaying a little
        const float sway = std::sin(t * 0.8f) * 0.025f;
        leaf({-20, 720}, -1.05f, 300, 52, {12, 74, 44, 255}, sway);
        leaf({-30, 640}, -0.55f, 260, 44, {18, 92, 52, 255}, -sway);
        leaf({40, 740}, -1.45f, 240, 40, {10, 60, 38, 255}, sway * 0.6f);
        leaf({1140, 720}, -2.10f, 300, 52, {12, 74, 44, 255}, -sway);
        leaf({1150, 630}, -2.60f, 250, 42, {18, 92, 52, 255}, sway);
        leaf({1080, 745}, -1.75f, 230, 40, {10, 60, 38, 255}, -sway * 0.6f);
        for (const Lum &lum : g_lums)
        {
            const float x = lum.x + std::sin(t * 0.25f * lum.speed + lum.phase) * 26.0f;
            const float y = lum.y + std::sin(t * lum.speed + lum.phase) * 9.0f;
            drawLum(x, y, lum.r, 0.6f * std::sin(t * 2.0f * lum.speed + lum.phase));
        }
    }

    // ---------------------------------------------------------------- widgets
    bool g_handCursor = false;

    bool hovered(Rectangle r) { return CheckCollisionPointRec(GetMousePosition(), r); }

    enum class Style
    {
        Gold,
        Purple
    };

    bool button(Rectangle r, const char *label, Style style, bool enabled, const Font &font, float size)
    {
        const bool over = enabled && hovered(r);
        const bool down = over && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        if (over)
            g_handCursor = true;
        Rectangle b = r;
        if (over && !down)
            b = {r.x - 3, r.y - 2, r.width + 6, r.height + 4};
        if (down)
            b.y += 2;
        const float rad = style == Style::Gold ? 26.0f : 16.0f;
        if (over)
        {
            BeginBlendMode(BLEND_ADDITIVE);
            for (int i = 1; i <= 4; ++i)
            {
                const Rectangle g{b.x - i * 5.0f, b.y - i * 5.0f, b.width + i * 10.0f, b.height + i * 10.0f};
                DrawRectangleRounded(g, roundness(g, rad + i * 5.0f), 12, Fade(style == Style::Gold ? kGold : kPurpleLight, 0.07f));
            }
            EndBlendMode();
        }
        DrawRectangleRounded({b.x + 4, b.y + 6, b.width, b.height}, roundness(b, rad), 12, Fade(BLACK, 0.35f));
        Color base = style == Style::Gold ? kGoldDeep : kPurple;
        Color light = style == Style::Gold ? kGold : kPurpleLight;
        Color border = style == Style::Gold ? Color{110, 46, 10, 255} : kPanelBorder;
        if (!enabled)
        {
            base = {70, 62, 92, 255};
            light = {92, 84, 116, 255};
            border = {120, 110, 140, 255};
        }
        DrawRectangleRounded(b, roundness(b, rad), 12, base);
        const Rectangle top{b.x + 4, b.y + 4, b.width - 8, b.height * 0.52f};
        DrawRectangleRounded(top, roundness(top, rad - 4), 12, light);
        const Rectangle gloss{b.x + 14, b.y + 7, b.width - 28, b.height * 0.16f};
        DrawRectangleRounded(gloss, 1.0f, 8, Fade(WHITE, enabled ? 0.30f : 0.10f));
        DrawRectangleRoundedLinesEx(b, roundness(b, rad), 12, 3.0f, border);
        const float w = textWidth(font, label, size, 2);
        const Vector2 p{b.x + (b.width - w) / 2.0f, b.y + (b.height - size) / 2.0f - 1.0f};
        if (style == Style::Gold && enabled)
        {
            textOutline(font, label, p, size, 2, Fade(kGoldLight, 0.85f), 2.0f);
            DrawTextEx(font, label, p, size, 2, kBrownText);
        }
        else
        {
            textOutline(font, label, p, size, 2, Fade(BLACK, 0.6f), 2.0f);
            DrawTextEx(font, label, p, size, 2, enabled ? kText : kTextDim);
        }
        return over && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    }

    // A row of choices; returns the chosen index (clicks only when enabled).
    int pills(Rectangle area, const std::vector<const char *> &items, int value, bool enabled)
    {
        const float gap = 10.0f;
        const float w = (area.width - gap * static_cast<float>(items.size() - 1)) / static_cast<float>(items.size());
        int result = value;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const Rectangle r{area.x + static_cast<float>(i) * (w + gap), area.y, w, area.height};
            const bool active = static_cast<int>(i) == value;
            const bool over = enabled && hovered(r);
            if (over)
                g_handCursor = true;
            if (active)
            {
                DrawRectangleRounded(r, roundness(r, 14), 10, enabled ? kGoldDeep : Color{120, 96, 70, 255});
                const Rectangle top{r.x + 3, r.y + 3, r.width - 6, r.height * 0.5f};
                DrawRectangleRounded(top, roundness(top, 11), 10, enabled ? kGold : Color{150, 126, 96, 255});
                DrawRectangleRoundedLinesEx(r, roundness(r, 14), 10, 2.0f, enabled ? Color{110, 46, 10, 255} : Color{90, 80, 70, 255});
            }
            else
            {
                DrawRectangleRounded(r, roundness(r, 14), 10, over ? Color{58, 34, 112, 255} : kPillFill);
                DrawRectangleRoundedLinesEx(r, roundness(r, 14), 10, over ? 2.0f : 1.5f, Fade(kPanelBorder, over ? 0.9f : (enabled ? 0.45f : 0.2f)));
            }
            const float size = 22.0f;
            const float tw = textWidth(g_font.blackSmall, items[i], size, 1);
            const Vector2 p{r.x + (r.width - tw) / 2.0f, r.y + (r.height - size) / 2.0f};
            if (active)
            {
                DrawTextEx(g_font.blackSmall, items[i], p, size, 1, enabled ? kBrownText : Color{70, 60, 50, 255});
            }
            else if (enabled)
            {
                textOutline(g_font.blackSmall, items[i], p, size, 1, Fade(BLACK, 0.55f), 1.5f);
                textGradient(g_font.blackSmall, items[i], p, size, 1, kMenuGreenTop, kMenuGreenBottom);
            }
            else
            {
                DrawTextEx(g_font.blackSmall, items[i], p, size, 1, Fade(kTextDim, 0.45f));
            }
            if (over && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                result = static_cast<int>(i);
        }
        return result;
    }

    // The brush lettering of the game's menus: green-to-white with a dark outline.
    void heading(const char *text, Vector2 pos, float size)
    {
        textOutline(g_font.brush, text, pos, size, 1, Fade({8, 30, 6, 255}, 0.9f), 3.0f);
        textGradient(g_font.brush, text, pos, size, 1, kMenuGreenTop, kMenuGreenBottom);
    }

    // Wraps text into lines that fit width (word by word).
    std::vector<std::string> wrap(const Font &font, const std::string &text, float size, float width)
    {
        std::vector<std::string> lines;
        std::string line, word;
        auto flushWord = [&]() {
            if (word.empty())
                return;
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(font, candidate.c_str(), size, 0) > width)
            {
                lines.push_back(line);
                line = word;
            }
            else
                line = candidate;
            word.clear();
        };
        for (char c : text)
        {
            if (c == ' ')
                flushWord();
            else if (c == '\n')
            {
                flushWord();
                lines.push_back(line);
                line.clear();
            }
            else
                word += c;
        }
        flushWord();
        if (!line.empty())
            lines.push_back(line);
        return lines;
    }

    // ---------------------------------------------------------------- settings
    const std::vector<const char *> kResolutions{"480p", "720p", "1080p", "1440p"};
    const char *kWindowSizes[] = {"854x480", "1280x720", "1920x1080", "2560x1440"};
    const std::vector<const char *> kDisplays{"Window", "Full screen"};
    const std::vector<const char *> kRates{"25 FPS", "50 FPS", "60 FPS", "120 FPS"};
    const char *kRateValues[] = {"", "50", "60", "120"}; // "" = the original 25 FPS
    const std::vector<const char *> kSsaa{"x2", "x4", "x8", "x16"};
    const char *kSsaaValues[] = {"2", "4", "8", "16"};
    const std::vector<const char *> kOnOff{"OFF", "ON"};

    struct Settings
    {
        int resolution = 3; // 1440p
        int display = 1;    // full screen
        int rate = 3;       // 120 FPS
        int ssaa = 3;       // x16
        int smooth = 1;     // on
    };

    std::string g_root;
    std::string settingsPath() { return g_root + "\\launcher.ini"; }

    Settings loadSettings()
    {
        Settings s;
        const std::string text = readTextFile(settingsPath());
        auto get = [&](const char *key, int &value, int max) {
            const std::string k = std::string(key) + "=";
            const size_t at = text.find(k);
            if (at != std::string::npos)
            {
                const int v = std::atoi(text.c_str() + at + k.size());
                if (v >= 0 && v <= max)
                    value = v;
            }
        };
        get("resolution", s.resolution, 3);
        get("display", s.display, 1);
        get("rate", s.rate, 3);
        get("ssaa", s.ssaa, 3);
        get("smooth", s.smooth, 1);
        return s;
    }

    void saveSettings(const Settings &s)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "# Rayman Revolution launcher settings (indexes into the launcher's choices)\nresolution=%d\ndisplay=%d\nrate=%d\nssaa=%d\nsmooth=%d\n",
                      s.resolution, s.display, s.rate, s.ssaa, s.smooth);
        writeTextFile(settingsPath(), buf);
    }

    // What run_rayman_hd.bat and run_rayman.bat set, from the launcher's choices.
    std::vector<std::pair<std::string, std::string>> gameEnvironment(const Settings &s, const std::string &stamp)
    {
        const bool smooth = s.smooth == 1 && s.rate == 3;
        return {
            {"PS2X_GS_SSAA", kSsaaValues[s.ssaa]},
            {"PS2X_WINDOW", kWindowSizes[s.resolution]},
            {"PS2X_FULLSCREEN", s.display == 1 ? "1" : "0"},
            {"PS2X_FRAMERATE", kRateValues[s.rate]},
            {"PS2X_RAYMAN_LOGIC_HZ", smooth ? "25" : ""},
            {"PS2X_RAYMAN_INTERP", smooth ? "1" : ""},
            {"PS2X_WIDESCREEN", "16:9"},
            {"PS2X_FILTER", "bilinear"},
            {"PS2X_HUD", "1"},
            {"PS2X_HUD_DUMP", "1"},
            {"PS2X_SHOW_FPS", "1"},
            {"PS2X_AUTOSAVE", "1"},
            {"PS2X_VU1_PROFILE", "1"},
            {"PS2X_VU0_PROFILE", "1"},
            {"PS2X_RUN_STAMP", stamp},
        };
    }

    std::string gitVersion()
    {
        const std::string head = readTextFile(g_root + "\\.git\\HEAD");
        if (head.rfind("ref: ", 0) == 0)
        {
            std::string ref = head.substr(5);
            while (!ref.empty() && (ref.back() == '\n' || ref.back() == '\r'))
                ref.pop_back();
            std::string hash = readTextFile(g_root + "\\.git\\" + ref);
            if (hash.empty())
            {
                const std::string packed = readTextFile(g_root + "\\.git\\packed-refs");
                const size_t at = packed.find(" " + ref);
                if (at != std::string::npos && at >= 40)
                    hash = packed.substr(at - 40, 40);
            }
            return hash.size() >= 7 ? hash.substr(0, 7) : "unknown";
        }
        return head.size() >= 7 ? head.substr(0, 7) : "unknown";
    }

    // ---------------------------------------------------------------- the launcher's state
    enum class Busy
    {
        None,
        Installing,
        Building,
        Updating,
        Playing
    };

    struct App
    {
        Settings settings;
        Busy busy = Busy::None;
        std::thread worker;
        IsoProgress progress;
        std::atomic<bool> workerDone{false};
        bool workerOk = false;
        std::mutex mutex;
        std::string message;
        Color messageColor = kText;
        Process process;
        std::string playLog;
        double reinstallArmedUntil = 0.0;
        std::string version;

        void say(const std::string &text, Color color = kText)
        {
            std::lock_guard<std::mutex> lock(mutex);
            message = text;
            messageColor = color;
        }
        std::pair<std::string, Color> said()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return {message, messageColor};
        }
    } g_app;

    std::string gameFolder() { return g_root + "\\Rayman revolution files"; }
    std::string runnerPath() { return g_root + "\\out\\game\\ps2xRuntime\\ps2EntryRunner.exe"; }
    bool gameInstalled() { return fileExists(gameFolder() + "\\SLES_500.44") && fileExists(gameFolder() + "\\GAME.BF"); }
    bool portBuilt() { return fileExists(runnerPath()); }

    void idleMessage()
    {
        if (!gameInstalled())
            g_app.say("Choose INSTALL FROM ISO and pick your own Rayman Revolution (PAL) disc image.", kWarn);
        else if (!portBuilt())
            g_app.say("The game files are installed but the port is not built yet: INSTALL FROM ISO builds it.", kWarn);
        else
            g_app.say("Ready. Your settings are saved when you press PLAY.", kText);
    }

    void startInstall(const std::string &iso)
    {
        g_app.busy = Busy::Installing;
        g_app.workerDone = false;
        g_app.progress.bytesDone = 0;
        g_app.progress.bytesTotal = 0;
        g_app.say("Checking the disc image...");
        g_app.worker = std::thread([iso]() {
            std::vector<IsoFile> files;
            std::string err;
            bool ok = isoListFiles(iso, files, err) && isoIsRaymanRevolutionPal(iso, files, err);
            if (ok)
            {
                g_app.say("Copying the game files from your disc image...");
                ok = isoExtractAll(iso, files, gameFolder(), g_app.progress, err);
            }
            g_app.workerOk = ok;
            g_app.say(ok ? "The game files are installed from your disc image (your saves are kept)." : err, ok ? kOk : kError);
            g_app.workerDone = true;
        });
    }

    void startBuild()
    {
        if (startProcess("cmd.exe /c \"\"" + g_root + "\\build_rayman.bat\"\"", g_root, {}, "", false, true, g_app.process))
        {
            g_app.busy = Busy::Building;
            g_app.say("Building the port for your PC. A console window shows the progress (this takes a while).");
        }
        else
            g_app.say("Could not start build_rayman.bat.", kError);
    }

    void startUpdate()
    {
        if (!fileExists(g_root + "\\.git"))
        {
            g_app.say("Updates come from the port's GitHub repository: this copy is not a Git checkout.", kError);
            return;
        }
        if (startProcess("cmd.exe /c \"\"" + g_root + "\\update_rayman.bat\"\"", g_root, {}, g_root + "\\out\\update.log", true, false, g_app.process))
        {
            g_app.busy = Busy::Updating;
            g_app.say("Updating from GitHub, then rebuilding what changed...");
        }
        else
            g_app.say("Could not start update_rayman.bat.", kError);
    }

    void startGame()
    {
        saveSettings(g_app.settings);
        const std::string stamp = timeStamp();
        makeDirs(g_root + "\\out\\logs");
        if (fileExists(g_root + "\\out\\run.log"))
            copyFile(g_root + "\\out\\run.log", g_root + "\\out\\run_prev.log");
        g_app.playLog = g_root + "\\out\\logs\\run_" + stamp + ".log";
        const std::string cmd = "\"" + runnerPath() + "\" \"" + gameFolder() + "\\SLES_500.44\"";
        if (startProcess(cmd, g_root, gameEnvironment(g_app.settings, stamp), g_app.playLog, true, false, g_app.process))
        {
            g_app.busy = Busy::Playing;
            g_app.say("Rayman Revolution is running.", kOk);
            MinimizeWindow();
        }
        else
            g_app.say("The game could not be started.", kError);
    }

    void pollWork()
    {
        int code = 0;
        switch (g_app.busy)
        {
        case Busy::Installing:
            if (g_app.workerDone)
            {
                g_app.worker.join();
                g_app.busy = Busy::None;
                if (g_app.workerOk && !portBuilt())
                    startBuild();
            }
            break;
        case Busy::Building:
            if (!processRunning(g_app.process, code))
            {
                g_app.busy = Busy::None;
                if (portBuilt())
                    g_app.say("The port is built. Ready to play!", kOk);
                else
                    g_app.say("The build did not finish: see out\\build_game.log (Visual Studio with C++ is needed).", kError);
            }
            break;
        case Busy::Updating:
            if (!processRunning(g_app.process, code))
            {
                g_app.busy = Busy::None;
                const std::string log = readTextFile(g_root + "\\out\\update.log");
                if (code == 0 && log.find("Already up to date") != std::string::npos)
                    g_app.say("The port is already up to date.", kOk);
                else if (code == 0)
                    g_app.say("Updated from GitHub and rebuilt. Restart the launcher to see its own changes.", kOk);
                else if (code == 1)
                    g_app.say("Getting the update from GitHub failed: see out\\update.log.", kError);
                else
                    g_app.say("The update was downloaded but the build failed: see out\\build_game.log.", kError);
                g_app.version = gitVersion();
            }
            break;
        case Busy::Playing:
            if (!processRunning(g_app.process, code))
            {
                g_app.busy = Busy::None;
                copyFile(g_app.playLog, g_root + "\\out\\run.log");
                keepNewestFiles(g_root + "\\out\\logs", "run_", 30);
                RestoreWindow();
                char buf[512];
                std::snprintf(buf, sizeof(buf), "The game closed (code %d). Its log: %s", code, g_app.playLog.c_str());
                g_app.say(buf, code == 0 ? kText : kWarn);
            }
            break;
        case Busy::None:
            break;
        }
    }

    // ---------------------------------------------------------------- the screen
    void drawTitle()
    {
        const Vector2 p1{58, 30};
        textOutline(g_font.title, "RAYMAN", {p1.x + 4, p1.y + 7}, 108, 3, Fade(BLACK, 0.45f), 6.0f);
        textOutline(g_font.title, "RAYMAN", p1, 108, 3, kTitleOutline, 6.0f);
        textGradient(g_font.title, "RAYMAN", p1, 108, 3, kGoldLight, kGoldDeep);
        const Vector2 p2{64, 140};
        textOutline(g_font.black, "REVOLUTION", p2, 44, 9, kTitleOutline, 3.5f);
        textGradient(g_font.black, "REVOLUTION", p2, 44, 9, {255, 214, 120, 255}, {236, 84, 30, 255});
        heading("PC Port Launcher", {66, 196}, 30);
    }

    void drawStatus(Rectangle r, float t)
    {
        panel(r);
        heading("Status", {r.x + 24, r.y + 14}, 28);
        auto row = [&](float y, const char *name, const std::string &value, Color color) {
            DrawTextEx(g_font.label, name, {r.x + 26, y}, 21, 0, kTextDim);
            DrawTextEx(g_font.label, value.c_str(), {r.x + 150, y}, 21, 0, color);
        };
        const bool installed = gameInstalled();
        row(r.y + 58, "Game files", installed ? "Installed (PAL)" : "Not installed", installed ? kOk : kWarn);
        row(r.y + 84, "Port", portBuilt() ? "Built" : "Not built", portBuilt() ? kOk : kWarn);
        row(r.y + 110, "Version", g_app.version, kText);

        const auto [msg, color] = g_app.said();
        float y = r.y + 142;
        if (g_app.busy == Busy::Installing && g_app.progress.bytesTotal > 0)
        {
            const float f = static_cast<float>(g_app.progress.bytesDone) / static_cast<float>(g_app.progress.bytesTotal);
            const Rectangle bar{r.x + 26, y, r.width - 52, 16};
            DrawRectangleRounded(bar, 1.0f, 8, kPillFill);
            DrawRectangleRounded({bar.x, bar.y, std::max(16.0f, bar.width * f), bar.height}, 1.0f, 8, kGold);
            drawLum(bar.x + std::max(8.0f, bar.width * f - 8.0f), bar.y + 8, 7, 0.4f * std::sin(t * 6));
            char pct[64];
            std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(f * 100.0f));
            DrawTextEx(g_font.label, pct, {bar.x + bar.width - 46, bar.y - 24}, 19, 0, kGoldLight);
            y += 26;
        }
        else if (g_app.busy == Busy::Updating || g_app.busy == Busy::Building)
        {
            const Rectangle bar{r.x + 26, y, r.width - 52, 16};
            DrawRectangleRounded(bar, 1.0f, 8, kPillFill);
            const float x = bar.x + (bar.width - 60) * (0.5f + 0.5f * std::sin(t * 2.2f));
            drawLum(x + 30, bar.y + 8, 7, 0.4f * std::sin(t * 6));
            y += 26;
        }
        for (const std::string &line : wrap(g_font.body, msg, 19, r.width - 52))
        {
            DrawTextEx(g_font.body, line.c_str(), {r.x + 26, y}, 19, 0, color);
            y += 23;
        }
    }

    void drawGraphics(Rectangle r)
    {
        panel(r);
        heading("Graphics", {r.x + 26, r.y + 14}, 32);
        Settings &s = g_app.settings;
        const bool locked = g_app.busy != Busy::None;
        float y = r.y + 66;
        auto row = [&](const char *name, const char *hint, const std::vector<const char *> &items, int &value, bool enabled) {
            DrawTextEx(g_font.label, name, {r.x + 28, y}, 23, 0, enabled ? kGoldLight : kTextDim);
            DrawTextEx(g_font.body, hint, {r.x + 28, y + 26}, 16, 0, kTextDim);
            value = pills({r.x + 28, y + 48, r.width - 56, 42}, items, value, enabled && !locked);
            y += 100;
        };
        row("Resolution", "The game window's size (full screen uses your monitor's).", kResolutions, s.resolution, true);
        row("Display", "Full screen sets your monitor to an even refresh rate for the game.", kDisplays, s.display, true);
        row("Frame rate", "25 FPS is the original. 120 FPS is the smoothest.", kRates, s.rate, true);
        row("Supersampling (SSAA)", "x16 is the sharpest (real 1440p) and needs a strong graphics card.", kSsaa, s.ssaa, true);
        const bool smoothAvailable = s.rate == 3;
        int smooth = smoothAvailable ? s.smooth : 0;
        DrawTextEx(g_font.label, "Smooth mode", {r.x + 28, y}, 23, 0, smoothAvailable ? kGoldLight : kTextDim);
        DrawTextEx(g_font.body, "Game logic at the original 25 steps a second, pictures in between.", {r.x + 28, y + 26}, 16, 0, kTextDim);
        const int chosen = pills({r.x + 28, y + 48, r.width - 56, 42}, kOnOff, smooth, smoothAvailable && !locked);
        if (smoothAvailable)
            s.smooth = chosen;
        const char *note = "Smooth mode is available only at 120 FPS.";
        const Color noteColor = smoothAvailable ? Fade(kTextDim, 0.8f) : kWarn;
        DrawTextEx(g_font.label, note, {r.x + 28, y + 98}, 19, 0, noteColor);
    }
}

int main(int argc, char **argv)
{
    std::string screenshot;
    bool playNow = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshot = argv[i + 1];
        else if (std::strcmp(argv[i], "--play") == 0) // start the game at once with the saved settings
            playNow = true;
        else if ((std::strcmp(argv[i], "--check-iso") == 0 || std::strcmp(argv[i], "--extract") == 0) && i + 1 < argc)
        {
            // Without a window (tests): list and check a disc image, and with --extract <iso> <folder> copy it.
            std::vector<IsoFile> files;
            std::string err;
            const bool ok = isoListFiles(argv[i + 1], files, err) && isoIsRaymanRevolutionPal(argv[i + 1], files, err);
            uint64_t total = 0;
            for (const IsoFile &f : files)
                total += f.size;
            std::printf("%s: %zu files, %llu bytes, %s\n", argv[i + 1], files.size(), static_cast<unsigned long long>(total),
                        ok ? "Rayman Revolution PAL" : err.c_str());
            if (ok && std::strcmp(argv[i], "--extract") == 0 && i + 2 < argc)
            {
                IsoProgress progress;
                const bool done = isoExtractAll(argv[i + 1], files, argv[i + 2], progress, err);
                std::printf("extract: %s (%llu bytes)\n", done ? "done" : err.c_str(), static_cast<unsigned long long>(progress.bytesDone.load()));
                return done ? 0 : 2;
            }
            return ok ? 0 : 1;
        }
    }

    g_root = portRoot();
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_ALWAYS_RUN | FLAG_VSYNC_HINT);
    InitWindow(kWidth, kHeight, "Rayman Revolution - PC Port Launcher");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    rlDisableBackfaceCulling();

    // The window's icon: a lum.
    Image icon = GenImageColor(64, 64, BLANK);
    for (int r = 31; r >= 1; --r)
    {
        const float f = static_cast<float>(r) / 31.0f;
        const Color c = r > 18 ? Fade(kLumGlow, (1.0f - f) * 1.6f) : (r > 7 ? kLumGlow : kLumCore);
        ImageDrawCircle(&icon, 32, 32, r, c);
    }
    SetWindowIcon(icon);
    UnloadImage(icon);

    g_font.title = loadFont("seguibl.ttf", 108);
    g_font.black = loadFont("seguibl.ttf", 54);
    g_font.blackSmall = loadFont("seguibl.ttf", 24);
    g_font.brush = loadFont("segoeprb.ttf", 34);
    g_font.label = loadFont("seguisb.ttf", 24);
    g_font.body = loadFont("segoeui.ttf", 20);
    g_gradient = LoadShaderFromMemory(nullptr, kGradientFs);
    g_locTop = GetShaderLocation(g_gradient, "colTop");
    g_locBottom = GetShaderLocation(g_gradient, "colBottom");
    g_locRange = GetShaderLocation(g_gradient, "yRange");

    initBackground();
    g_app.settings = loadSettings();
    g_app.version = gitVersion();
    idleMessage();
    if (playNow && gameInstalled() && portBuilt())
        startGame();

    int frames = 0;
    while (!WindowShouldClose())
    {
        pollWork();
        const float t = static_cast<float>(GetTime());
        g_handCursor = false;
        BeginDrawing();
        background(t);
        drawTitle();

        const bool idle = g_app.busy == Busy::None;
        const bool ready = gameInstalled() && portBuilt();
        if (button({58, 250, 442, 98}, ready ? "PLAY" : "INSTALL FIRST", Style::Gold, idle && ready, g_font.black, ready ? 54 : 40))
            startGame();
        if (button({58, 364, 213, 58}, gameInstalled() ? "REINSTALL" : "INSTALL FROM ISO", Style::Purple, idle, g_font.blackSmall, 22))
        {
            if (gameInstalled() && GetTime() > g_app.reinstallArmedUntil)
            {
                g_app.reinstallArmedUntil = GetTime() + 6.0;
                g_app.say("The game is already installed. Press REINSTALL again to copy it from a disc image again (your saves are kept).", kWarn);
            }
            else
            {
                const std::string iso = pickDiscImage(GetWindowHandle());
                if (!iso.empty())
                    startInstall(iso);
                else
                    idleMessage();
            }
        }
        if (button({287, 364, 213, 58}, "UPDATE", Style::Purple, idle, g_font.blackSmall, 22))
            startUpdate();
        drawStatus({58, 440, 442, 196}, t);
        drawGraphics({536, 34, 526, 602});

        const char *footer = "Needs your own Rayman Revolution PAL disc image. Nothing of the game is included.";
        const float fw = textWidth(g_font.body, footer, 16, 0);
        DrawTextEx(g_font.body, footer, {(kWidth - fw) / 2.0f, 668}, 16, 0, Fade(kTextDim, 0.85f));

        SetMouseCursor(g_handCursor ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
        EndDrawing();

        if (!screenshot.empty() && ++frames == 45)
        {
            Image shot = LoadImageFromScreen();
            ExportImage(shot, screenshot.c_str());
            UnloadImage(shot);
            break;
        }
    }

    if (g_app.worker.joinable())
        g_app.worker.join();
    saveSettings(g_app.settings);
    UnloadShader(g_gradient);
    CloseWindow();
    return 0;
}
