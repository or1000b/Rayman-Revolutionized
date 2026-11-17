@echo off
rem Rayman Revolution in high resolution. Change the settings below (Notepad), save, then run this file.

rem Picture quality:
rem   1  = the PS2's own picture (640 x 512)
rem   2  = smoother edges
rem   4  = double resolution (1280 x 1024) and smooth edges
rem   8  = double resolution, even smoother edges
rem   16 = real 1440p: four times the PS2's resolution (2560 x 2048), shrunk to your screen's size by the graphics
rem        card (needs a strong graphics card; 120 FPS almost everywhere, ~113 at the busiest spot known)
set PS2X_GS_SSAA=16

rem Full screen at the monitor's resolution (720p, 1080p, 1440p...): 1 = yes, 0 = a window.
rem F10 switches between full screen and a window while playing.
set PS2X_FULLSCREEN=1

rem The window's size when not full screen: 1280x720, 1920x1080, 2560x1440...
set PS2X_WINDOW=1280x720

rem Widescreen: 16:9 = the game shows a wider view (also 21:9), 0 = the original 4:3 view.
rem Menus, text and videos are stretched to the wide picture, as with PCSX2's widescreen patch.
set PS2X_WIDESCREEN=16:9

rem Picture shape: empty = follow the widescreen setting (16:9, or 4:3 with black bars at the sides),
rem stretch = fill the whole screen.
set PS2X_ASPECT=

rem Scaling to the screen: bilinear = smooth, nearest = sharp pixels.
set PS2X_FILTER=bilinear

rem Frames per second: 60 = smooth, 50 = the original PAL game. run_rayman_hd_120fps.bat runs these settings at 120.
if not defined PS2X_FRAMERATE set PS2X_FRAMERATE=60

rem HUD (text, icons, menus) drawn sharp at screen resolution and kept 4:3 in widescreen: 1 = yes, 0 = the PS2 HUD.
set PS2X_HUD=1
rem Collect the HUD pictures you meet into hud_dump, for upscale_hud.bat (AI upscaling): 1 = yes, 0 = no.
set PS2X_HUD_DUMP=1

rem Show the FPS counter: 1 = yes, 0 = no.
set PS2X_SHOW_FPS=1

rem Auto-save: 1 = when the game asks "do you want to save the game?" (for example when you enter a level), it saves
rem into the save you are playing and goes on without asking. The memory card is backed up first, once per run, to
rem "Rayman revolution files\mc0_backups" (the newest 10 are kept). 0 = off (the game asks, as on the PS2).
set PS2X_AUTOSAVE=1

call "%~dp0run_rayman.bat"
