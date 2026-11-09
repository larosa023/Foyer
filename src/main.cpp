// Foyer - entry point
//
//   Foyer.exe            start Foyer (or open the menu if already running)
//   Foyer.exe --tray     start quietly in the tray (used by "Start with Windows")
//   Foyer.exe --quit     close a running Foyer
//   Foyer.exe --original open the real Windows Start menu
//   Foyer.exe --settings open Foyer's settings window
//   (--updated <pid> / --rolled-back <version> are used by the built-in updater)
#include "common.h"
#include "config.h"
#include <shellapi.h>
#include <objbase.h>

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    // The hooks die with the process, so the Windows Start menu works again
    // immediately. Leave a note in the log for diagnosis.
    Log(L"CRASH: exception 0x%08lx at %p - Foyer is exiting; the Windows Start menu is active.",
        ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    TrayRemove();
    return EXCEPTION_EXECUTE_HANDLER;
}

static HANDLE g_mutex = nullptr;

void ReleaseInstanceMutex() {
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}

static bool HasArg(const wchar_t* cmdLine, const wchar_t* arg) {
    return cmdLine && wcsstr(cmdLine, arg) != nullptr;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    SetUnhandledExceptionFilter(CrashHandler);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\Foyer.larosa023.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"FoyerMenu", nullptr);
        if (existing) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            if (HasArg(cmdLine, L"--quit")) PostMessageW(existing, WM_FOYER_QUIT, 0, 0);
            else if (HasArg(cmdLine, L"--original")) PostMessageW(existing, WM_FOYER_ORIGINAL, 0, 0);
            else if (HasArg(cmdLine, L"--settings")) PostMessageW(existing, WM_FOYER_SETTINGS, (WPARAM)-1, 0);
            else if (!HasArg(cmdLine, L"--tray")) {
                POINT pt; GetCursorPos(&pt);
                PostMessageW(existing, WM_FOYER_TOGGLE, 1, MAKELPARAM((short)pt.x, (short)pt.y));
            }
        }
        if (g_mutex) CloseHandle(g_mutex);
        return 0;
    }
    if (HasArg(cmdLine, L"--quit")) return 0;
    if (HasArg(cmdLine, L"--original")) { OpenOriginalStartMenu(); return 0; }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LogInit();
    Log(L"==== Foyer %ls starting (data: %ls)", FOYER_VERSION, DataDir().c_str());
    bool firstRun = GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES;
    ConfigLoad();
    g_interceptStartButton = g_config.interceptStartButton;
    g_interceptWinKey = g_config.interceptWinKey;

    HWND hwnd = MenuCreate(inst);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Foyer couldn't start (see foyer.log). Your Windows Start menu is unchanged.", L"Foyer", MB_ICONERROR);
        return 1;
    }
    TrayAdd(hwnd);
    AutoUpdateInit(hwnd, cmdLine);
    StartButtonRefreshAsync(hwnd);
    if (!HookStart(hwnd)) Log(L"Failed to start hook thread");
    if (HasArg(cmdLine, L"--settings")) SettingsOpen(-1);
    if (firstRun) TrayBalloon(L"Foyer is ready", L"Press the Windows key to open Foyer. Ctrl+Win still opens the original Start menu.");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    HookStop();
    Log(L"Foyer exited normally");
    CoUninitialize();
    ReleaseInstanceMutex();
    return 0;
}
