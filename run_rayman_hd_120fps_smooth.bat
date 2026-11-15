@echo off
rem EXPERIMENT (docs\interpolation.md): 120 pictures per second, but the game itself runs as on the console
rem (25 steps per second, the original physics and timing). The pictures between the steps are interpolated.
rem Known: the dark band behind Murfy's text flickers. Compare with run_rayman_hd_120fps.bat.
set PS2X_FRAMERATE=120
set PS2X_RAYMAN_LOGIC_HZ=25
set PS2X_RAYMAN_INTERP=1
call "%~dp0run_rayman_hd.bat"
