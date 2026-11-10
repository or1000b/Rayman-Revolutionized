@echo off
rem Same as run_rayman.bat, at 50 frames per second (twice the original 25; the game's time unit is unchanged, so this
rem is the most faithful higher rate). The game's speed stays real time (docs\framerate.md).
set PS2X_FRAMERATE=50
call "%~dp0run_rayman.bat"
