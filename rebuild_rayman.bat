@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || ( echo [!] vcvars64 failed & pause & exit /b 1 )
echo === Rebuilding the game (only what changed) ===
cmake --build out\game --target ps2EntryRunner > out\build_game.log 2>&1 || ( echo [!] build failed, see out\build_game.log & pause & exit /b 1 )
echo === Done. Starting the game... ===
call "%~dp0run_rayman.bat"
