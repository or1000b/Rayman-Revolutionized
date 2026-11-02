@echo off
rem Build, run the game unattended and check it (see run_tests.ps1 / tests\scenarios.json).
rem No pause at the end on purpose: Claude runs this too. Exit code 0 = no FAIL. Report: out\test_report.txt
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_tests.ps1" %*
exit /b %ERRORLEVEL%
