// update/self_update_win.cpp -- SelfUpdater::ApplyAndRestart on Windows: a
// detached batch helper. See update/self_update.h.
#include "update/self_update.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace nds::update {

namespace {

// The script is written as UTF-8 and switches the console to code page
// 65001 first, so a path outside the ANSI code page (a user name with
// accents, say) still reaches `move` and `start` intact.
std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                        static_cast<int>(wide.size()), nullptr,
                                        0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::string Utf8(const std::filesystem::path& path) { return Utf8(path.wstring()); }

}  // namespace

bool SelfUpdater::ApplyAndRestart(const std::filesystem::path& install_root,
                                  const std::filesystem::path& executable_path) {
    if (!HasPendingSelfUpdate(install_root, {})) return false;
    const auto staging = StagingRoot(install_root);

    std::error_code ec;
    std::vector<std::filesystem::path> staged_entries;
    for (const auto& entry : std::filesystem::directory_iterator(staging, ec))
        staged_entries.push_back(entry.path());
    if (staged_entries.empty()) return false;

    // This process's own executable lives under `install_root` and stays
    // locked by the loader for as long as it runs, including a relaunched
    // instance, which would still be executing from that same file when it
    // tried to replace it. A detached batch script outside `install_root`
    // has no such problem: it waits for this pid to exit before touching
    // anything.
    wchar_t temp_dir[MAX_PATH];
    const DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
    if (temp_len == 0 || temp_len >= MAX_PATH) {
        std::fprintf(stderr, "[update] GetTempPathW failed\n");
        return false;
    }
    const DWORD pid = GetCurrentProcessId();
    const std::filesystem::path script_path =
        std::filesystem::path(temp_dir) /
        (L"nds_selfupdate_" + std::to_wstring(pid) + L".bat");
    const std::filesystem::path log_path =
        std::filesystem::path(temp_dir) / L"nds_selfupdate.log";

    std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
    if (!script) {
        std::fprintf(stderr, "[update] could not create the update helper script\n");
        return false;
    }
    const std::string spid = std::to_string(pid);
    script << "@echo off\r\n";
    script << "chcp 65001 >NUL\r\n";
    script << "set \"NDSLOG=" << Utf8(log_path) << "\"\r\n";
    // The subroutine sits above the main flow so the self-delete can stay
    // the last line of the file (see its comment below).
    script << "goto ndsmain\r\n";
    script << "\r\n";
    // :ndsapply <staged> <dest>: clear whatever is at <dest> and move
    // <staged> onto it, retrying for a while. Even after the game's pid is
    // gone its files can stay locked for a moment (a delayed handle release,
    // or antivirus scanning re-opening the exe the instant it is unmapped),
    // which is exactly what hits the large executable while the small files
    // move fine.
    script << ":ndsapply\r\n";
    script << "set _ndstry=0\r\n";
    script << ":ndsapply_loop\r\n";
    script << "rmdir /s /q %2 >>\"%NDSLOG%\" 2>&1\r\n";
    script << "del /f /q %2 >>\"%NDSLOG%\" 2>&1\r\n";
    script << "move /y %1 %2 >>\"%NDSLOG%\" 2>&1\r\n";
    script << "if not exist %1 (\r\n";
    script << "  echo [ok] %~2 >>\"%NDSLOG%\"\r\n";
    script << "  exit /b 0\r\n";
    script << ")\r\n";
    script << "set /a _ndstry+=1\r\n";
    script << "echo [retry %_ndstry%] %~2 >>\"%NDSLOG%\"\r\n";
    script << "if %_ndstry% GEQ 20 (\r\n";
    script << "  echo [FAILED] %~2 >>\"%NDSLOG%\"\r\n";
    script << "  exit /b 1\r\n";
    script << ")\r\n";
    script << "timeout /t 1 /nobreak >NUL\r\n";
    script << "goto ndsapply_loop\r\n";
    script << "\r\n";
    script << ":ndsmain\r\n";
    script << "echo ==== self-update helper, pid " << spid << " ==== >>\"%NDSLOG%\"\r\n";
    script << "echo %DATE% %TIME% waiting for pid " << spid << " >>\"%NDSLOG%\"\r\n";
    script << ":wait\r\n";
    script << "tasklist /fi \"PID eq " << spid << "\" 2>NUL | find \"" << spid
           << "\" >NUL\r\n";
    script << "if not errorlevel 1 (\r\n";
    script << "  timeout /t 1 /nobreak >NUL\r\n";
    script << "  goto wait\r\n";
    script << ")\r\n";
    script << "echo %DATE% %TIME% pid gone, applying >>\"%NDSLOG%\"\r\n";
    for (const auto& staged_entry : staged_entries) {
        const auto dest = install_root / staged_entry.filename();
        script << "call :ndsapply \"" << Utf8(staged_entry) << "\" \"" << Utf8(dest)
               << "\"\r\n";
    }
    script << "rmdir /s /q \"" << Utf8(staging) << "\" >>\"%NDSLOG%\" 2>&1\r\n";
    script << "echo %DATE% %TIME% relaunching >>\"%NDSLOG%\"\r\n";
    script << "start \"\" /D \"" << Utf8(install_root) << "\" \""
           << Utf8(executable_path) << "\"\r\n";
    // Self-delete through one more detached `start` rather than a trailing
    // `del "%~f0"`: a self-delete chained right after the `start` above is
    // flaky under CREATE_NO_WINDOW (cmd.exe sometimes loses its read
    // position in the batch file once `start` returns, logs "Cannot find
    // the batch file" and never runs the delete). A temp file left behind
    // on the rare failure is harmless. Keep this the last line, so cmd.exe
    // never has to read past it.
    script << "start \"\" /B cmd /c del \"" << Utf8(script_path) << "\"\r\n";
    script.close();
    if (!script) {
        std::fprintf(stderr, "[update] could not write the update helper script\n");
        return false;
    }

    std::wstring cmdline = L"cmd.exe /c \"" + script_path.wstring() + L"\"";
    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW (a real but hidden console) rather than
    // DETACHED_PROCESS (no console at all): `timeout` and `start` are both
    // console-aware and misbehave without one.
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "[update] could not start the update helper (error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::fprintf(stderr, "[update] helper started: %s\n", script_path.string().c_str());
    return true;
}

}  // namespace nds::update
