@echo off
setlocal
rem Rayman Revolution (SLES-500.44) - build with PS2Recomp
rem Run from the PS2Recomp-main folder (the one that holds CMakeLists.txt).
cd /d "%~dp0"

rem 1. Load the Visual Studio C++ tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo [!] Visual Studio C++ tools not found. & pause & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || ( echo [!] vcvars64 failed & pause & exit /b 1 )

rem 2. Build the recompiler (tools only)
echo === 1/4 Building the recompiler ===
cmake -S . -B out\tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF || goto fail
cmake --build out\tools --target ps2_recomp || goto fail

rem 3. Translate the game code to C++
echo === 2/4 Recompiling SLES_500.44 ===
if exist out\rayman_recomp rmdir /s /q out\rayman_recomp
out\tools\ps2xRecomp\ps2_recomp.exe rayman.toml > out\recomp.log 2>&1 || ( echo [!] see out\recomp.log & goto fail )

rem 4. Put the generated code into the runtime
echo === 3/4 Copying generated code into ps2xRuntime ===
del /q ps2xRuntime\src\runner\sub_*.cpp ps2xRuntime\src\runner\entry_*.cpp 2>nul
copy /y out\rayman_recomp\*.cpp ps2xRuntime\src\runner\ >nul || goto fail
copy /y out\rayman_recomp\*.h ps2xRuntime\include\ >nul || goto fail

rem 5. Build the game
echo === 4/4 Building the game (this takes a while) ===
cmake -S . -B out\game -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF || goto fail
cmake --build out\game --target ps2EntryRunner > out\build_game.log 2>&1 || ( echo [!] build failed, see out\build_game.log & goto fail )

for /r out\game %%f in (ps2EntryRunner.exe) do if exist "%%f" set "EXE=%%f"
echo.
echo === Done ===
echo Starting the game...
call "%~dp0run_rayman.bat"
exit /b 0

:fail
echo.
echo [!] Failed. Send Claude the last lines above (or the .log files in out\).
pause
exit /b 1
