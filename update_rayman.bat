@echo off
rem The launcher's UPDATE button: the newest version of the port from GitHub, then a rebuild of what changed.
rem Exit code: 0 = done (or already up to date), 1 = getting the update failed, 2 = the build failed.
setlocal
cd /d "%~dp0"
echo === Getting the newest version from GitHub ===
git pull --ff-only || exit /b 1
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo [!] Visual Studio C++ tools not found. & exit /b 2 )
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 2
echo === Rebuilding the port (only what changed) ===
cmake --build out\game --target ps2EntryRunner > out\build_game.log 2>&1 || exit /b 2
echo === Done ===
exit /b 0
