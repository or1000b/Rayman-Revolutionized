# Rayman Revolution - PC port launcher (ps2xLauncher/src/launcher.cpp): install from the player's own PAL disc image,
# graphics settings, play, update from GitHub. Windows only; uses the raylib that ps2xRuntime fetches.
add_executable(RaymanLauncher WIN32
    src/launcher.cpp
    src/platform_win32.cpp
    src/iso9660.cpp
)
target_compile_features(RaymanLauncher PRIVATE cxx_std_20)
target_link_libraries(RaymanLauncher PRIVATE raylib comdlg32 shell32)
if(MSVC)
    # A window program with a plain main(): no console window behind the launcher.
    target_link_options(RaymanLauncher PRIVATE /ENTRY:mainCRTStartup)
    target_compile_options(RaymanLauncher PRIVATE /utf-8)
endif()
