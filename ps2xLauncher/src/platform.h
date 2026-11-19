// Windows services for the launcher, kept out of launcher.cpp (windows.h and raylib.h clash on names).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace launcher
{
    // The port's folder (the one with run_rayman.bat), found upwards from the launcher's own location.
    std::string portRoot();

    // The Windows "open" dialog for a disc image; "" when cancelled. owner: the launcher's window (HWND) or null.
    std::string pickDiscImage(void *owner);

    bool fileExists(const std::string &path);
    void makeDirs(const std::string &path);
    std::string readTextFile(const std::string &path);
    bool writeTextFile(const std::string &path, const std::string &text);
    void copyFile(const std::string &from, const std::string &to);
    std::string timeStamp(); // 2026-11-19_16-40-05
    void keepNewestFiles(const std::string &dir, const std::string &prefix, size_t keep);
    void openInExplorer(const std::string &path);

    // A started program. log: its output (stdout and stderr) goes to that file ("" = none). hidden: no console window.
    // env: variables set for it (value "" = removed); every PS2X_* variable of the launcher's own environment is
    // removed first, so only these settings reach the game.
    struct Process
    {
        void *handle = nullptr;
    };
    bool startProcess(const std::string &commandLine, const std::string &workDir,
                      const std::vector<std::pair<std::string, std::string>> &env, const std::string &log, bool hidden,
                      bool newConsole, Process &process);
    // True while it runs; when it has ended, exitCode is set and the handle closed.
    bool processRunning(Process &process, int &exitCode);
}
