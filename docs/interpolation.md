# In-between pictures: the game step at its own rate, drawing at 120 (research, 2026-11-15 ..)

Goal (user's choice, 2026-11-15, "option 2"): like rac-rs, run the game's logic at its original rate (25 or 50 steps
per second: no frame-rate bugs such as issue 22, less CPU) and draw in-between pictures up to 120 FPS. Today
PS2X_FRAMERATE=120 runs the whole game 120 times a second (option 1).

This file collects what the research found, step by step. Nothing here changes how the game runs yet.

## 1. One game frame (measured)

- 0x1007F8 is one frame: the **game step** 0x100CD0 (sector change 0x101828, the actors and their AI 0x10C0D8 /
  0x106680, collision, camera, ...; it also builds the display lists, 0x145EC0), then **0x1010F8** (draws the views
  the step queued, the 2D, waits for the next VBlank, the frame clock 0x14B9A0), then it jumps to 0x1028C8.
- So logic and drawing are separate calls in one fixed order: a first condition for in-between pictures.
- `PS2X_RAYMAN_PHASES=1` (main.cpp) times the parts: from one part's start to the next part's start (a part can end
  in a guest thread switch, so timing a call from outside misses its work). The native profiler (`PS2X_NATIVE_PROF`)
  cannot split the guest call tree: after a thread switch the guest code runs from the dispatcher, not under its
  caller, and some guest functions get the name of a neighbour.

Fairy Glade at 120 FPS (scenario glade_120_hd, Rayman standing), microseconds per frame (8333 per frame):

| part | us |
|---|---|
| game step 0x100CD0 | ~4400 |
| - 0x145EC0 (display lists) | ~2000 |
| - 0x10C0D8 (actors) | ~1000 |
| - 0x106680 | ~570 |
| - 0x141D20 | ~350 |
| - 0x147C30 | ~310 |
| 0x1010F8: drawing on the game thread | ~330 |
| 0x1010F8: waiting for the next VBlank | ~3600 |

The 3D itself (VU1 programs, the GS) runs on the graphics thread (`PS2X_GFX_THREAD`).

## 2. What drawing reads

- Display lists ("render command stream"): the step writes them through a pointer at gp-0x711C (0x14B100 / 0x14B110
  / 0x14B130 / 0x14B150 select the list, at ~0x355530 / 0x355740), 0x14B348 sends them. Entries such as
  `03000000 <object> <mask> 1.0 3`, `04000000 <sector>`.
- An object in the list is a superobject: +4 geometry, +8 / +0xC first / last child, +0x10 child count, +0x14 /
  +0x18 next / previous, +0x1C parent, +0x20 local matrix, +0x24 global matrix (rotation rows, translation at +0x30).
  An animated character's parts (type 8) are made each frame from a pool (0x440000-0x470000, frame number at +0x28),
  so their addresses change every frame: to pair a part with the same part one step earlier, use its parent and its
  geometry (and its place among the parent's children), not its address.
- The views queue (0x36ED50, 8-byte entries, counts gp-0x6FBC / gp-0x6FB8) is drawn and emptied by 0x152928 in
  0x1010F8. In the Fairy Glade it is empty there (0x152928 takes 1 us).

## 3. The 3D drawing work is inside the step

`PS2X_RAYMAN_PHASES` one level deeper: 0x145EC0 = 0x1474D0 (~2.1 ms): writing the lists (0x1467B8, 0x147128,
0x1473D8) ~0.1 ms; sending them, 0x14B348 (called twice: a0 = the view, a1 = 1 then 0), ~2.0 ms. So per frame at 120
in the Fairy Glade: ~2.1 ms drawing work (lists + send + 0x1010F8) and ~2.4 ms logic (actors, AI, collision, ...).

The runtime returns a long guest function to the scheduler in the middle (a deterministic time slice, or a thread
switch) and runs its rest later from the scheduler: almost every long part above does so in every frame. So a hook
cannot call a guest function and then act "after" it: every change must be made when a function is entered.

## 4. Experiment: the logic at the console's rate, drawing at 120 (`PS2X_RAYMAN_LOGIC_HZ=25`)

main.cpp `installRaymanLogicSplit`, with PS2X_FRAMERATE=120:
- The logic runs 25 times per second exactly as on the console: the clock unit stays 0.02 s, every step is n = 2
  units. Steps are placed on the 25 Hz grid of guest VBlanks (at 120 Hz: every 5, 5, 5, 5, 4 VBlanks).
- The frames between only draw: the step 0x100CD0 still runs, but its logic calls return at once (0x10C0D8 /
  0x106680 actors and AI, 0x101828 sector change, 0x11A2D0 animation, 0x147C30, 0x141D20, ...: 16 calls whose result
  the step does not use). The display lists 0x145EC0 and the camera calls run every frame.
- The frame clock 0x14B9A0 and the frame's tail 0x1028C8 run only before a logic step. The tail updates the input
  (0x11ECC0: previous = current, read the pads, "just pressed" = the difference) and handles pause: run every frame,
  a press became "just pressed" in a draw-only frame and was lost (first try: stuck in the language menu, X never
  seen).
- A first try with the logic every 5th frame and the clock unit 1/120 (24 steps of 1/24 s) was close but not the
  console (helicopter -3.50, Rayman stuck in the air for 7 s on the way to the water): unit-based timers changed.

Scenario `rayman_physics_split` (PASS), against the original 25 FPS:

| | original 25 | PS2X_FRAMERATE=120 | split: logic 25, drawing 120 |
|---|---|---|---|
| game ms per real second | 1000 | 1000 | 1000 |
| standing jump: airtime | 0.92 s | 0.88 | 0.920 |
| standing jump: height | 3.12 | 3.25 | 3.120 |
| helicopter fall speed | -3.16 | -3.43 | -3.154 |
| walking (best 10%) | 9.38 | 8.78 | 9.44 |
| surface swimming | 10.15 | 10.19 | 9.67 (another route) |

Pictures of the draw-only frames are complete (Rayman, Murfy, texts, water). The routes differ a little from the
original run because the pad is read on another grid. Without in-between pictures the motion is still 25 steps per
second, drawn 120 times.

## 5. In-between pictures (`PS2X_RAYMAN_INTERP=1`, with `PS2X_RAYMAN_LOGIC_HZ=25`)

main.cpp `installRaymanInterp`. Where the picture's matrices come from:
- The camera: near the end of every step, **0x1174E0** builds RenderWare's camera (pointer at 0x393134; its frame
  holds the camera placement plus a fixed offset of ~17856 per axis; RwCameraBeginUpdate 0x18C450 then makes the
  RwCamera view matrix). It takes them from the game's camera [[0x252278 + 0x68]], whose view matrix (row vectors:
  rows 0-2 rotation, row 3 translation) is at +4; 0x116308 builds the frustum from the same matrix. The logic writes
  it in 0x106680 (0x118C88). Found with `PS2X_EE_WATCH` on the three translations.
- The objects: the display lists read the global matrices ([so + 0x24]) of the dynamic world: the main character's
  superobject [0x252398], its parent (type 1, ~184 persos) and everything under it (~250 superobjects: persos type 2,
  their parts type 8, made again each step from a pool).

So, when 0x1174E0 is entered from the step (it is also called by 0x1109E0), every object's global matrix gets a value
interpolated between the last two steps, and when RwCameraBeginUpdate (0x18C450) is entered from 0x1174E0, so does
RenderWare's camera frame (its modelling matrix, just set from the game camera by 0x14FE90). Time: N-1 at the logic
step's frame, moving to N until the next step (one step behind the logic, as in rac-rs). Interpolation: translation
linear, rotation rows blended and made perpendicular again (Gram-Schmidt). Pairing: persos by address, parts by
parent + data + count of earlier siblings with the same data (the parts' order changes between steps: pairing by
place made Rayman's limbs jump). Only types 2 and 8. No interpolation for a move of more than 6 units per step, a turn
of 90 degrees or more, or a size change of more than 25%.
- Snapshots hold each step's FINAL matrices, taken on the first draw-only frame after the logic step: some matrices
  are only finished while the step's frame draws (0x14AF88 updates a part's global matrix lazily, when its stamp at
  +0x28 differs from the counter gp-0x7120, which 0x14AC40 advances once per step). Writes are undone (at 0x141D20,
  before a logic step) only where the memory still holds the value written. The first version snapshot the matrices
  before the drawing and put them all back after it: lazily finished matrices were overwritten with stale ones and
  Murfy vanished on the draw-only frames (only his sparkles stayed: the "bright flash" at Rayman's shots).
- The first camera version interpolated the game camera's view matrix ([[0x252278 + 0x68]] + 4): the game uses that
  camera for more than the picture, and Murfy vanished on the draw-only frames too. RenderWare's camera frame only
  changes the picture (`PS2X_RAYMAN_INTERP_PARTS`: 1 objects, 2 game camera, 4 RenderWare camera; default 5).

Results (scenario `rayman_interp`): the physics stay the console's (the same values as `rayman_physics_split`: the
true matrices are back before the logic runs). Consecutive pictures 20 ms apart now all differ (before: alternately
~10 and ~0.3: the camera moved only with the steps).

Cost (`glade_split_hd`: Fairy Glade, 120 FPS, SSAA x4, 16:9; `PS2X_RAYMAN_PHASES`): 120.1 game FPS (check: 110+).
The game step takes ~2.9 ms per frame instead of ~4.4 (logic now on 1 frame in 4.8: actors ~0.26 ms per frame
instead of ~1.0); most of the rest is the drawing (0x14B348 ~2.0 ms) that every picture needs. Interpolation itself
(~250 matrices) ~0.04 ms. Waiting for the VBlank: ~5.1 ms instead of ~3.6.

Flicker (things drawn only on logic frames):
- Shadows: each frame the display lists add a draw request (0x1473D8 -> 0x10C9F0) to a list at 0x354200 (count,
  up to 20 requests of 0xD4 bytes; the AI can add more); in the next step 0x147C30 tests their visibility, turns
  them into entries of a draw list (count at gp-0x71A0 = 0x393250, entries of 0xCC bytes at 0x3311D8) and empties
  the requests; 0x141D20 (after the display lists) computes each entry's geometry (collision queries) and draws it.
  With 0x147C30 / 0x141D20 skipped on draw-only frames, every shadow showed only on logic frames. Now 0x147C30 and
  0x141D20 run on every frame, with the calls before 0x147C30 they need (0x147968, 0x154700, 0x14C2B8: without them
  0x147C30 crashed the game thread in RenderWare's camera update), and a draw-only frame gets the logic frame's
  requests back (saved / put back when 0x147C30 is entered).
- Wrong first fix (commit 3c4e73d): saving the draw list itself and drawing it again with the geometry from the logic
  frame. The geometry buffers are temporary: the draw-only frames drew stale geometry (the dark area it put in the
  measured box was that, not Murfy's text box), and on the user's hub recording the Vulkan device was lost / the game
  hung at the level change (scenario `hub_split_off`).
- A gap without steps (loading, a menu loop) now forgets the saved requests and the interpolation snapshots (their
  objects may be gone); the next step is a logic step. The frame clock follows the 25 Hz grid also in frames without
  a step (loading screens, some menus): with the unit 0.02 s an update every 120 Hz frame counted 20 ms per 8.3 ms.
- 2D drawn only by the logic (the dark box behind Murfy's text, parts of the HUD): the HUD layer (gs_hud, on in
  normal play) holds it. At each game step main.cpp sends a frame mark (`ps2xHudFrameMark`, gs_frontend.cpp: a write
  to register 0x7F, not a GS register, through the graphics thread like the game's GS writes, so it reaches the GS
  thread in order); frames without a step send "logic" at their frame clock. A draw-only frame's 2D is not published:
  the window keeps showing the last logic frame's 2D (it changes 25 times a second, as on the console). The mark must
  stay valid until the next one (a frame's first drawing can go to an off-screen buffer: consumed there, the mark got
  lost and no 2D showed at all). Checked with window shots (`PS2X_HUD=1 PS2X_WINDOW_SHOTS=1`, scenario
  `shoot_split`): the box shows on every picture. With the HUD layer off (test mode) the GS picture still shows the
  box only on logic frames.
- Check: with no interpolation two pictures of the same step must be identical: `hub_split_off` (the user's hub
  recording, level change included): 0.00 (0.01 twice); `rayman_interp_off`: identical except the text box.

## Moving objects flickered: the camera was not interpolated (issue 24, 2026-11-17)

Tool: `PS2X_TEST_BURST="<first VSync>,<count>"` (ps2_test_harness.cpp) saves every picture the window fetches in that
VSync range to `out/burst_<vsync>.bmp` (the window runs uncapped then; at 640 x 512 every VSync is caught, with SSAA x4
some are missed or late), and main.cpp logs `[split] vsync N step logic|draw-only` for the same range. A picture shows
the step of one VSync earlier (two with SSAA x4). Use `PS2X_HUD=1`, so the GS picture has no 2D (the logic draws it only
on its own frames; in normal play the HUD layer holds it). Compare runs with and without `PS2X_RAYMAN_INTERP_PARTS`
(0 = nothing interpolated: then every picture of a step is the logic picture).

Found: Rayman walking in `rayman_interp`, the whole picture changed by ~14.5 (mean per channel) at each logic picture
and by ~0.3 in between, as without interpolation: the camera stood still for 4-5 frames and jumped 25 times a second,
while the objects, interpolated one step behind, moved smoothly. On the screen every moving object then slid back at
each step and crept forward until the next: the user's "moving objects flicker". Cause: 0x1174E0 runs twice per frame
(from the step and from 0x1109E0), and each time 0x18C450 is entered for another camera (frames 0xFD1110, the 3D view,
and 0xFD11C0, a camera at the fixed offset). One shared "last two steps" pair was overwritten by the second camera, so
the two steps never belonged to the same camera and nothing was interpolated (since 565e88f). Now each camera keeps
its own pair. After the fix: ~4.5 on every picture, the background moves a little each frame and Rayman stays steady.
Also checked:
- Rayman's limbs are interpolated (an experiment that lifted every part by 2 units on the draw-only frames lifted them
  in the picture; the per-object log `PS2X_RAYMAN_INTERP_LOG=<vsync>` shows them all as "blend").
- Still at the logic rate, as on the console: particles (the sparkles around a lum: frozen for a step, then new ones)
  and texture animation (water, waterfalls).

## Still a little flicker: the picture followed the last gap, not the 25 Hz grid (2026-11-17)

The user, after the camera fix: "there is still a little bit flikering in the smooth mode although its much better".
The logic steps sit on the 25 Hz grid of guest VBlanks, at 120 Hz every 5, 5, 5, 5, 4 VBlanks. The in-between
pictures measured their place by the gap between the last two steps: t = (v - step) / (last gap). After a gap of 5
followed by 4, the last draw-only picture was at t = 0.6 and the logic picture jumped to 1.0 (twice the usual
step); after a gap of 4 followed by 5, t reached 1.0 one picture early and the logic picture showed the same place
again. A hitch 5 times a second, everything on the screen (camera and objects alike).

Measured with `PS2X_TEST_BURST=18440,100 PS2X_HUD=1` on `rayman_interp` (Rayman walking): the mean change between
consecutive pictures (scratch tool: mean absolute difference, capture misses left out) was ~4.5 on most pictures,
but 10.8 on the logic picture after a 4-gap and 0.6-0.9 on the next logic picture (cv 0.32). Now (main.cpp
`raymanInterpGridT`): the picture follows the grid itself. On the frame of VBlank v after the logic step of slot s,
x = (v * L - s * R) / R steps have passed; the picture shows the logic's time minus 1 + d steps, d = (L - gcd(L, R)) /
R = the most x can be on a logic frame (1/6 step = 7 ms at 120 Hz). A logic frame k (its own step is not final yet)
shows k-2..k-1 at t = 1 + x - d (the two steps saved before it; it showed k-1 before), the draw-only frames after it
show k-1..k at t = x - d. Snapshots are paired by logic step number (`g_raymanLogicSteps`), not by VBlank. After:
logic pictures 4.1-6.5, draw-only 3.2-6.5, cv 0.14. RenderWare's camera frames get the same treatment (each keeps
the steps of its two pictures; a logic frame blends the two before its own).

Also checked (normal play, real-time clock, window at 120, 1440p; `PS2X_PRESENT_STATS=1` in ps2_runtime.cpp
UploadFrame counts window frames that found no new game VSync or more than one): during play every game frame was
shown exactly once (0 of 600 in most 5 s windows); repeats only while booting and loading. Objects kept as they are
(`PS2X_RAYMAN_INTERP_KEPT=<n>` logs them with the reason): ~0.6 a frame in `rayman_interp`, nearly all one effect
shrinking to nothing (size ratio over 1.25 per step, the guard against an effect appearing), a few turns over 90
degrees; ~0.5 a frame unpaired (new parts).

## Rayman, the energy ball and the characters still stuttered (2026-11-18)

The user, after the grid fix: "most noticeable with Rayman when he's given a command to move. You can also see it
slightly with the energy ball Rayman shoots, and with the characters he talks to, like Sssam. On the other hand, the
environment looks good." The camera and Rayman's own place were smooth (`PS2X_RAYMAN_INTERP_TRACE="<vsync>,<count>"`
logs, per frame, the main character's translation as written and as drawn: 0.078 units every picture while walking).
Three things were not:

1. **Objects the drawing finishes.** A superobject's global matrix is stale when its stamp (+0x28) differs from the step
   counter gp-0x7120 (moved once per logic step by 0x14AC40). The drawing finishes stale objects itself: 0x14ACB8 while
   it walks the scene (global = local x the parent's matrix on its matrix stack 0x355448) and 0x14AF88 (an object and
   everything under it) from the shadow requests. On a logic frame that came after the in-between matrices were written,
   so those objects showed the newest step and went back on the next picture: 13-16 matrices per logic frame in
   `rayman_interp` (a character flying beside Rayman and its parts, another one), the energy ball in `shoot_split`
   (tracked on burst pictures: ~14 pixels forward on every logic picture, ~9 back on the next). Fix (main.cpp
   `raymanLazyStart`): on a logic frame, before the in-between matrices, 0x14AF88 runs for the whole dynamic world like a
   guest call (the step's registers kept, returning to the unused word 0x14AF84) and gives every stale object's final
   matrix with the game's own code; each gets the counter as its stamp, so the drawing keeps what is written. At 0x141D20
   each one gets what the game would have had: the final matrix and the counter where the drawing asked for it (0x14ACB8
   or 0x14AF88 entered for it or a parent), else its old matrix and stamp. Checked with `PS2X_RAYMAN_INTERP_LAZY=2`
   (the logic frame drawn from the true matrices, the drawing's own results compared with the early ones): 90114 of
   90114 identical. After: 0 matrices replaced on logic frames (3957 before in 1300 pictures); the ball moves the same
   ~2.5 pixels on every picture.
2. **Parts that change shape.** Rayman's hands and feet get another geometry every step or two while he walks (the
   animation swaps the object of a bone; the parts are made again each step from a pool). The pairing key held the
   geometry, and the parent's (so every part under a swapped one got a new key too), so such a part had no pair: it was
   drawn at the newest step, standing still in the world while the body caught up (768 such parts in 1029 walking
   pictures). Now (`raymanInterpPair`) a part without a pair by key is paired with a part of the same perso that has
   none either: first one with the same geometry within 0.5 units (the same part under a changed parent: measured nearly
   all within 0.05), else one at the same place of the skeleton (same parent and depth) within 1 unit (the same bone in
   another shape). Burst pictures decided the second rule: with it a foot rises over the 4-5 pictures of a step; without
   it the foot stood still and jumped at each logic picture. Parts of other shapes under other parents measured 0.4-1
   units apart: different parts, not blended. A part still without a pair, or one kept by the checks (a turn of 90
   degrees, a size jump), now goes along with its perso (`raymanInterpAttach`: part x inverse(perso true) x perso drawn)
   instead of standing at the newest step ahead of the body.
3. **Shadows.** A shadow is a draw request the AI makes in the logic step (0x147AE8, +0 the superobject); 0x147C30 turns
   it into an entry of the shadows' draw list (count gp-0x71A0, 0xCC bytes each at 0x3311D8: +0x00 a world -> shadow
   transform with its translation at +0x30, +0x84 the world centre, then sizes) from the step's bounding box, and
   0x141D20 builds the shadow from the entry. Draw-only frames replay the step's requests, so the entries were the same
   on every picture of a step: under a walking Rayman the shadow slid back ~2.5 pixels (640 x 512) a picture and jumped
   6-14 forward at each logic picture (a saw-tooth 25 times a second, right under the character). Writing the in-between
   matrix while 0x147C30 ran changed nothing (it uses the bounding box). Now (`raymanInterpShadows`), when 0x141D20 is
   entered, each entry moves with the written perso nearest its centre by that perso's in-between move (centre + d,
   translation - d x rows); the entries go back when the next step is entered. After: the shadow's movement per picture
   std 4.5 -> 1.6 pixels (what remains: its size and shape change with the animation at the steps).

Measured on Rayman's parts relative to his body (`[interp-parts]` lines, scratch tool): per-picture jumps over 0.2
units 388 -> about 100 in 1300 pictures; the rest are shape swaps of other parts, which a matrix measure counts as jumps
whatever the picture shows. Research switches: `PS2X_RAYMAN_INTERP_LAZY` (0 off, 2 compare), `PS2X_RAYMAN_INTERP_PAIR=0`,
`PS2X_RAYMAN_INTERP_SWAP=<units>`, `PS2X_RAYMAN_INTERP_ATTACH=0`, `PS2X_RAYMAN_INTERP_SHADOWS=0`,
`PS2X_RAYMAN_INTERP_PAIRLOG=1`, `PS2X_RAYMAN_INTERP_TRACE`.

Still at the logic rate, as on the console: particles (Murfy's sparkles: the picture around him changes twice as much on
logic pictures), texture animation, a part's change of shape itself (it happens at the step).

## Next steps

1. More flicker checks (effects, other levels); the user's impression.
2. Check menus, cutscenes, the shop (issue 22 should be as on the console), loading, pause, particles, the HUD layer.
3. Then decide with the user: make this the 120 FPS mode (a setting in the launcher), or keep option 1.
