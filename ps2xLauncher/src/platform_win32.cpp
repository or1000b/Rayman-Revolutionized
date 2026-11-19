#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace launcher
{
    namespace
    {
        std::wstring wide(const std::string &s)
        {
            if (s.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
            return w;
        }

        std::string utf8(const std::wstring &w)
        {
            if (w.empty())
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(size_t(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        std::filesystem::path fsPath(const std::string &s) { return std::filesystem::path(wide(s)); }

        std::wstring upper(std::wstring s)
        {
            for (wchar_t &c : s)
                c = wchar_t(std::towupper(c));
            return s;
        }
    }

    std::string portRoot()
    {
        wchar_t buf[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
        std::filesystem::path dir = std::filesystem::path(buf).parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (std::filesystem::exists(dir / L"run_rayman.bat") && std::filesystem::exists(dir / L"rayman.toml"))
                return utf8(dir.wstring());
            if (dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        return utf8(std::filesystem::current_path().wstring());
    }

    std::string pickDiscImage(void *owner)
    {
        wchar_t file[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = static_cast<HWND>(owner);
        ofn.lpstrFilter = L"Disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = DWORD(std::size(file));
        ofn.lpstrTitle = L"Choose your Rayman Revolution (PAL) disc image";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetOpenFileNameW(&ofn) ? utf8(file) : std::string();
    }

    bool fileExists(const std::string &path)
    {
        std::error_code ec;
        return std::filesystem::exists(fsPath(path), ec);
    }

    void makeDirs(const std::string &path)
    {
        std::error_code ec;
        std::filesystem::create_directories(fsPath(path), ec);
    }

    std::string readTextFile(const std::string &path)
    {
        std::ifstream f(fsPath(path), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    bool writeTextFile(const std::string &path, const std::string &text)
    {
        std::ofstream f(fsPath(path), std::ios::binary | std::ios::trunc);
        f << text;
        return bool(f);
    }

    void copyFile(const std::string &from, const std::string &to)
    {
        std::error_code ec;
        std::filesystem::copy_file(fsPath(from), fsPath(to), std::filesystem::copy_options::overwrite_existing, ec);
    }

    std::string timeStamp()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return buf;
    }

    void keepNewestFiles(const std::string &dir, const std::string &prefix, size_t keep)
    {
        std::error_code ec;
        std::vector<std::filesystem::path> found;
        for (const auto &entry : std::filesystem::directory_iterator(fsPath(dir), ec))
            if (entry.is_regular_file() && utf8(entry.path().filename().wstring()).rfind(prefix, 0) == 0)
                found.push_back(entry.path());
        std::sort(found.begin(), found.end()); // names carry the time stamp
        while (found.size() > keep)
        {
            std::filesystem::remove(found.front(), ec);
            found.erase(found.begin());
        }
    }

    void openInExplorer(const std::string &path)
    {
        ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool startProcess(const std::string &commandLine, const std::string &workDir,
                      const std::vector<std::pair<std::string, std::string>> &env, const std::string &log, bool hidden,
                      bool newConsole, Process &process)
    {
        // The environment: the launcher's own, without PS2X_*, plus env (upper-case keys, sorted as Windows wants).
        std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars; // key -> (name, value)
        if (wchar_t *block = GetEnvironmentStringsW())
        {
            for (const wchar_t *p = block; *p; p += wcslen(p) + 1)
            {
                const std::wstring item(p);
                const size_t eq = item.find(L'=', 1);
                if (eq == std::wstring::npos)
                    continue;
                const std::wstring name = item.substr(0, eq);
                if (upper(name).rfind(L"PS2X_", 0) == 0)
                    continue;
                vars[upper(name)] = {name, item.substr(eq + 1)};
            }
            FreeEnvironmentStringsW(block);
        }
        for (const auto &[name, value] : env)
        {
            if (value.empty())
                vars.erase(upper(wide(name)));
            else
                vars[upper(wide(name))] = {wide(name), wide(value)};
        }
        std::wstring block;
        for (const auto &[key, nv] : vars)
            block += nv.first + L"=" + nv.second + L'\0';
        block += L'\0';

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE logFile = INVALID_HANDLE_VALUE;
        if (!log.empty())
        {
            logFile = CreateFileW(wide(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (logFile != INVALID_HANDLE_VALUE)
        {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = logFile;
            si.hStdError = logFile;
            si.hStdInput = nullptr;
        }
        PROCESS_INFORMATION pi{};
        std::wstring cmd = wide(commandLine);
        DWORD flags = CREATE_UNICODE_ENVIRONMENT;
        if (hidden)
            flags |= CREATE_NO_WINDOW;
        else if (newConsole)
            flags |= CREATE_NEW_CONSOLE;
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, logFile != INVALID_HANDLE_VALUE, flags,
                                       block.data(), wide(workDir).c_str(), &si, &pi);
        if (logFile != INVALID_HANDLE_VALUE)
            CloseHandle(logFile);
        if (!ok)
            return false;
        CloseHandle(pi.hThread);
        process.handle = pi.hProcess;
        return true;
    }

    bool processRunning(Process &process, int &exitCode)
    {
        if (!process.handle)
            return false;
        if (WaitForSingleObject(static_cast<HANDLE>(process.handle), 0) == WAIT_TIMEOUT)
            return true;
        DWORD code = 0;
        GetExitCodeProcess(static_cast<HANDLE>(process.handle), &code);
        exitCode = int(code);
        CloseHandle(static_cast<HANDLE>(process.handle));
        process.handle = nullptr;
        return false;
    }
}
