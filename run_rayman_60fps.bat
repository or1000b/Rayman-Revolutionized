@echo off
rem Same as run_rayman.bat, at 60 frames per second. The game's speed stays real time (docs\framerate.md).
set PS2X_FRAMERATE=60
call "%~dp0run_rayman.bat"
