@echo off
rem Records your play for Claude: the game starts like an automatic test (the scenario's memory card, deterministic,
rem run_rayman_hd_120fps.bat settings), but you play. Load the save, go to the place of the bug, show it, then close
rem the game window. The recording (tests\fixtures\<scenario>.input) lets Claude replay exactly what you did.
rem A recording made before is kept (tests\fixtures\<scenario>_<date>.input).
rem Usage: double-click and choose, or record_for_claude.bat <scenario>.   F10 = full screen on/off.
cd /d "%~dp0"
set SCENARIO=%1
if not "%SCENARIO%"=="" goto record
echo What do you want to record?
echo   1 = the shop under the well of whirlpools (shop_exit)
echo   2 = running through crowded places at 120 FPS (hub_run_120)
choice /c 12 /n /m "Press 1 or 2: "
if errorlevel 2 (set SCENARIO=hub_run_120) else (set SCENARIO=shop_exit)
:record
echo Recording: %SCENARIO%   (do not save the game while recording)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_tests.ps1" -NoBuild -Record -Only %SCENARIO%
pause
