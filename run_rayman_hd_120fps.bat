@echo off
rem Same as run_rayman_hd.bat (all its settings), at 120 frames per second. The game's speed stays real time.
rem Needs a strong PC: when it cannot keep 120, the game skips time instead of going slow (docs\framerate.md).
set PS2X_FRAMERATE=120
call "%~dp0run_rayman_hd.bat"
