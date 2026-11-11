@echo off
rem Rayman Revolution in high resolution. Change the settings below (Notepad), save, then run this file.

rem Picture quality:
rem   1  = the PS2's own picture (640 x 512)
rem   2  = smoother edges
rem   4  = double resolution (1280 x 1024) and smooth edges
rem   8, 16 = even smoother edges (needs a strong graphics card)
set PS2X_GS_SSAA=4

rem Full screen at the monitor's resolution (720p, 1080p, 1440p...): 1 = yes, 0 = a window.
rem F10 switches between full screen and a window while playing.
set PS2X_FULLSCREEN=1

rem The window's size when not full screen: 1280x720, 1920x1080, 2560x1440...
set PS2X_WINDOW=1280x720

rem Picture shape: 4:3 = as on a TV (black bars at the sides), stretch = fill the whole screen.
set PS2X_ASPECT=4:3

rem Scaling to the screen: bilinear = smooth, nearest = sharp pixels.
set PS2X_FILTER=bilinear

rem Frames per second: 60 = smooth, 50 = the original PAL game.
set PS2X_FRAMERATE=60

rem Show the FPS counter: 1 = yes, 0 = no.
set PS2X_SHOW_FPS=1

call "%~dp0run_rayman.bat"
