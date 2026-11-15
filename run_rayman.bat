@echo off
cd /d "%~dp0"
rem Every run keeps its own log in out\logs (the newest 30), and out\run.log is a copy of the last one.
if not exist out\logs mkdir out\logs
set PS2X_RUN_STAMP=
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HH-mm-ss"') do set PS2X_RUN_STAMP=%%t
if "%PS2X_RUN_STAMP%"=="" set PS2X_RUN_STAMP=latest
set RUNLOG=out\logs\run_%PS2X_RUN_STAMP%.log
if exist out\run.log move /y out\run.log out\run_prev.log >nul
echo Starting Rayman Revolution... (log: %RUNLOG%)
echo F11 = save two frames of graphics commands + pictures for Claude (out\gif_capture.bin, out\gif_capture_frame*.bmp)
echo Keys: arrows = move, A = jump, Space = shoot, Enter = Start, C = Circle, V = Triangle, Q/E = L1/R1, 1/3 = L2/R2
echo F10 = full screen on/off (high resolution settings: run_rayman_hd.bat)
echo Controllers: Xbox, PS3, PS4 and PS5 work (plug in any time). A/Cross = jump, X/Square = shoot.
"out\game\ps2xRuntime\ps2EntryRunner.exe" "%CD%\Rayman revolution files\SLES_500.44" > "%RUNLOG%" 2>&1
set PS2X_EXIT=%ERRORLEVEL%
copy /y "%RUNLOG%" out\run.log >nul
for /f "skip=30 delims=" %%f in ('dir /b /o-d out\logs\run_*.log 2^>nul') do del "out\logs\%%f"
echo.
echo The game exited (code %PS2X_EXIT%). Tell Claude what you saw - the log is in %RUNLOG%
pause
