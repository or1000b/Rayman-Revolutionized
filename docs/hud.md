# HUD layer: the 2D drawn by the window (2026-11-15)

Goal (user): a sharp HUD (text, icons, menus), not stretched in 16:9. Chosen approach: the port draws the HUD itself
at screen resolution, with AI-upscaled textures.

## How the game draws its 2D

- Captures (`PS2X_PGS_CAPTURE_VSYNC=<vsync>`, new `PS2X_PGS_CAPTURE_FRAMES=<n>`; `tools/gifgroups.py` lists draw groups
  with path, primitive, texture, screen box and Z) of the language menu, the main menu and the swim level's Murfy text
  box: the whole 2D layer comes from VU1 (PATH1) in small packets (two triangle fans of 3 vertices per quad, ST /
  RGBAQ / XYZF2) with **every vertex at Z = 0xFFFF**; the 3D never is. It is drawn **after the 3D** of each frame
  (no colour-writing 3D draw follows it in any capture). In between the game clears the Z buffer with a full-screen
  sprite at Z = 0 and FRAME.FBMSK = 0xFFFFFFFF (writes no colour).
- Each letter is its own 64 x 64 PSMT8 texture (white glyph, stored upside down; the colour comes from the vertices),
  uploaded host -> local at (0, 0) of its block with a 16 x 16 PSMCT32 CLUT (CSM1). The text box backdrop is an
  untextured quad, black at alpha 0x60, ALPHA 0x44. Menus also have full-screen black quads (darkening, fades).

## The port

- `gs_hud.h` / `gs_hud.cpp` (in the GS bridge library), on the GS thread before paraLLEl-GS (`forwardNow`): every GIF
  packet (EOP-delimited) is walked for the GS state (PRIM, TEX0 with CLD, ALPHA, FRAME, XYOFFSET, uploads: BITBLTBUF /
  TRXPOS / TRXREG / TRXDIR + IMAGE data). A packet whose drawing vertices all have Z = 0xFFFF becomes triangles in the
  game's 512 x 512 screen space, with its textures decoded on the CPU (PSMT8 / PSMT4 + PSMCT32 CLUT, PSMCT32) and keyed
  by an FNV-1a hash of the decoded RGBA; paraLLEl-GS gets a copy where those vertices have ADC = 1 (no drawing kick;
  A+D XYZ2 -> XYZ3). A texture that cannot be decoded leaves the packet with paraLLEl-GS (`kept` in the stats).
- Frames: the 2D is kept per frame buffer (FRAME.FBP). Switching from a displayed buffer to another starts a new frame
  there; a colour-writing non-2D draw into a buffer that has 2D also starts over (`late` in the stats: seen when a
  frame starts after an offscreen draw, harmless). At each scanout the displayed buffer's 2D (DISPFB of the enabled
  circuit, the display registers as of the GS stream position) is published for the window.
- Window (`ps2_runtime.cpp` `drawHudOverlay`): after the picture, the published triangles with rlgl (modulate:
  vertex colour x 2, PS2 alpha 0x80 = 255; ALPHA 0x44 -> alpha blending, Cs * As + Cd -> additive). Game x 0..512 and
  y 0..512 span the picture's rectangle; wider than 4:3 the 2D keeps its 4:3 shape around the centre, except quads
  spanning the full width (fades), which stay full width. Textures: `hud_textures/<key>.png` when present (any size),
  else the decoded original, bilinear.
- Settings: `PS2X_HUD` (unset: on in normal play, off in test mode, where run_tests compares the GS picture; 1 / 0),
  `PS2X_HUD_DUMP=1` (originals to `hud_dump/<key>.png`), `PS2X_HUD_LOG=<n>` (the first n published frames, batch by
  batch), `PS2X_WINDOW_SHOTS=1` (with every test_shot also the window as drawn, `out/window_shot_<tick>.png`).
  `run_rayman_hd.bat` sets PS2X_HUD=1 and PS2X_HUD_DUMP=1.
- Upscaling: `upscale_hud.bat` -> `tools/hud_upscale.py`: Real-ESRGAN (realesrgan-ncnn-vulkan, model
  realesrgan-x4plus-anime) on the dumped textures not done yet, x4 into `hud_textures/`. The tool lives in
  `tools/realesrgan/` (the Windows build of release v0.2.5.0 from github.com/xinntao/Real-ESRGAN); it, `hud_dump/` and
  `hud_textures/` are not committed (.gitignore).

## Checked

- shoot_minisaurus at 1920 x 1080, SSAA x4, 16:9 (`out/window_shot_7652.png`): Murfy, the backdrop and the text drawn
  by the window at 4:3 proportions; the GS picture has no HUD. Main menu (choose_english, 640 x 448): the same as the
  GS drawing. 104704 packets taken, 51 textures, 0 kept.
- Not yet: the upscaler's output (running the downloaded tool needs the user's go-ahead), other HUD elements (lums,
  health), videos with subtitles, the pause menu.
