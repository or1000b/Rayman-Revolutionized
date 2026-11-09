@echo off
rem Build and run the unit tests (ps2xTest) in out\test. Reuses the libraries already downloaded by the
rem game (out\game) and tools (out\tools) builds, so no network access is needed.
rem Exit code = number of failing tests. Full output: out\unit_tests.log
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
set D=%CD:\=/%/out
if not exist out\test\build.ninja (
  cmake -S . -B out\test -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=ON -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_ANALYZER=ON -DPS2X_BUILD_RUNTIME=ON ^
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON ^
    -DFETCHCONTENT_SOURCE_DIR_IMGUI=%D%/game/_deps/imgui-src ^
    -DFETCHCONTENT_SOURCE_DIR_PS2X_GRANITE=%D%/game/_deps/ps2x_granite-src ^
    -DFETCHCONTENT_SOURCE_DIR_PS2X_PARALLEL_GS=%D%/game/_deps/ps2x_parallel_gs-src ^
    -DFETCHCONTENT_SOURCE_DIR_RAYLIB=%D%/game/_deps/raylib-src ^
    -DFETCHCONTENT_SOURCE_DIR_RLIMGUI=%D%/game/_deps/rlimgui-src ^
    -DFETCHCONTENT_SOURCE_DIR_ELFIO=%D%/tools/_deps/elfio-src ^
    -DFETCHCONTENT_SOURCE_DIR_FMT=%D%/tools/_deps/fmt-src ^
    -DFETCHCONTENT_SOURCE_DIR_LIBDWARF=%D%/tools/_deps/libdwarf-src ^
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=%D%/tools/_deps/nlohmann_json-src ^
    -DFETCHCONTENT_SOURCE_DIR_RABBITIZER=%D%/tools/_deps/rabbitizer-src ^
    -DFETCHCONTENT_SOURCE_DIR_TOML11=%D%/tools/_deps/toml11-src > out\configure_test.log 2>&1 || ( echo configure failed, see out\configure_test.log & exit /b 1 )
)
cmake --build out\test --target ps2x_tests > out\build_test.log 2>&1 || ( echo build failed, see out\build_test.log & exit /b 1 )
pushd "%~dp0out\test\ps2xTest"
"%~dp0out\test\ps2xTest\ps2x_tests.exe" > "%~dp0out\unit_tests.log" 2>&1
set RESULT=%ERRORLEVEL%
popd
echo %RESULT% failing test(s). Output: out\unit_tests.log
exit /b %RESULT%
