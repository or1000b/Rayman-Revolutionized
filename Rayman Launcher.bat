@echo off
rem Starts the Rayman Revolution launcher (built with the port: out\game\ps2xLauncher\RaymanLauncher.exe).
cd /d "%~dp0"
if exist "out\game\ps2xLauncher\RaymanLauncher.exe" (
    start "" "out\game\ps2xLauncher\RaymanLauncher.exe"
    exit /b 0
)
echo The launcher is not built yet. Building it...
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo [!] Visual Studio C++ tools not found. & pause & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake --build out\game --target RaymanLauncher || ( echo [!] Build failed. & pause & exit /b 1 )
start "" "out\game\ps2xLauncher\RaymanLauncher.exe"
