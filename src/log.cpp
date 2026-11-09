// Foyer - local logging and app paths
#include "common.h"
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

static std::mutex g_logLock;
static std::wstring g_logPath;

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(0, s);
}

std::wstring DataDir() {
    static std::wstring dir;
    if (!dir.empty()) return dir;
    // Portable mode: a file named "portable" (or portable.txt) next to Foyer.exe
    std::wstring exeDir = ExeDir();
    if (GetFileAttributesW((exeDir + L"\\portable").c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((exeDir + L"\\portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
        dir = exeDir + L"\\data";
    } else {
        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
            dir = std::wstring(appData) + L"\\Foyer";
            CoTaskMemFree(appData);
        } else {
            dir = exeDir + L"\\data";
        }
    }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

void LogInit() {
    g_logPath = DataDir() + L"\\foyer.log";
    // Keep the log small: roll over at ~1 MB.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 1024 * 1024) {
        std::wstring old = g_logPath + L".old";
        DeleteFileW(old.c_str());
        MoveFileW(g_logPath.c_str(), old.c_str());
    }
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, std::size(msg) - 1, fmt, ap);
    va_end(ap);
    msg[std::size(msg) - 1] = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    int n = swprintf(line, std::size(line), L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %ls\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    if (n <= 0) return;

    // UTF-8 on disk
    char utf8[3600];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), nullptr, nullptr);
    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_logPath.empty()) return;
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, utf8, bytes, &written, nullptr);
    CloseHandle(h);
}
