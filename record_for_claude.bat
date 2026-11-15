@echo off
rem Records your play for Claude: the game starts like an automatic test (the scenario's memory card, deterministic),
rem but you play. Load the save, go to the place of the bug, show it, then close the game window.
rem The recording (tests\fixtures\<scenario>.input) lets Claude replay exactly what you did, as often as needed.
rem Usage: record_for_claude.bat [scenario]   (default: shop_exit)   F10 = full screen.
cd /d "%~dp0"
set SCENARIO=%1
if "%SCENARIO%"=="" set SCENARIO=shop_exit
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_tests.ps1" -NoBuild -Record -Only %SCENARIO%
pause
