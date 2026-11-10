@echo off
rem Plays like run_rayman.bat and also records the VU1 microprograms the game runs (out\vu1prof\), so the ones not
rem translated yet can be translated (tools\vu1aot\vu1gen.py, docs\vu1_aot.md). Translated programs stay native; a
rem new one runs on the interpreter (slower) and is recorded. The recording is saved every ~10 seconds and at exit.
cd /d "%~dp0"
if exist out\run_profile.log move /y out\run_profile.log out\run_profile_prev.log >nul
set PS2X_VU1_PROFILE=1
echo Starting Rayman Revolution with VU1 recording... (log: out\run_profile.log, recording: out\vu1prof)
echo Visit as many different places as you like. Wait ~10 seconds in a new place before quitting.
"out\game\ps2xRuntime\ps2EntryRunner.exe" "%CD%\Rayman revolution files\SLES_500.44" > out\run_profile.log 2>&1
echo.
echo The game exited (code %ERRORLEVEL%). Tell Claude you played - the recording is in out\vu1prof
pause
