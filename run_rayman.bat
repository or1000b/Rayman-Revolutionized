@echo off
cd /d "%~dp0"
if exist out\run.log move /y out\run.log out\run_prev.log >nul
echo Starting Rayman Revolution... (log: out\run.log)
echo F11 = save two frames of graphics commands + pictures for Claude (out\gif_capture.bin, out\gif_capture_frame*.bmp)
echo Keys: arrows = move, A = jump, Space = shoot, Enter = Start, C = Circle, V = Triangle, Q/E = L1/R1, 1/3 = L2/R2
echo F10 = full screen on/off (high resolution settings: run_rayman_hd.bat)
echo Controllers: Xbox, PS3, PS4 and PS5 work (plug in any time). A/Cross = jump, X/Square = shoot.
"out\game\ps2xRuntime\ps2EntryRunner.exe" "%CD%\Rayman revolution files\SLES_500.44" > out\run.log 2>&1
echo.
echo The game exited (code %ERRORLEVEL%). Tell Claude what you saw - the log is in out\run.log
pause
