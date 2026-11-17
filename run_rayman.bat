@echo off
cd /d "%~dp0"
rem Every run keeps its own log in out\logs (the newest 30), and out\run.log is a copy of the last one.
if not exist out\logs mkdir out\logs
set PS2X_RUN_STAMP=
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HH-mm-ss"') do set PS2X_RUN_STAMP=%%t
if "%PS2X_RUN_STAMP%"=="" set PS2X_RUN_STAMP=latest
set RUNLOG=out\logs\run_%PS2X_RUN_STAMP%.log
if exist out\run.log move /y out\run.log out\run_prev.log >nul
rem Record the VU1 / VU0 microcode the translated (native) code does not cover yet: out\vu1prof, out\vu0prof
rem (docs\vu1_aot.md). It only hooks the interpreter, which runs just that code, so covered places cost nothing.
rem Claude regenerates the native code from the recordings (tools\vu1aot\vu1gen.py). Set to 0 to turn it off.
if not defined PS2X_VU1_PROFILE set PS2X_VU1_PROFILE=1
if not defined PS2X_VU0_PROFILE set PS2X_VU0_PROFILE=1
rem Auto-save into the game's own "AUTO" slot (your saves are never touched; the card is backed up first to
rem "Rayman revolution files\mc0_backups"). run_rayman_hd.bat has the setting; 0 = off.
if not defined PS2X_AUTOSAVE set PS2X_AUTOSAVE=1
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
