// Foyer - entry point
//
//   Foyer.exe            start Foyer (or open the menu if already running)
//   Foyer.exe --tray     start quietly in the tray (used by "Start with Windows")
//   Foyer.exe --quit     close a running Foyer
//   Foyer.exe --original open the real Windows Start menu
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

static bool HasArg(const wchar_t* cmdLine, const wchar_t* arg) {
    return cmdLine && wcsstr(cmdLine, arg) != nullptr;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    SetUnhandledExceptionFilter(CrashHandler);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Foyer.larosa023.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"FoyerMenu", nullptr);
        if (existing) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            if (HasArg(cmdLine, L"--quit")) PostMessageW(existing, WM_FOYER_QUIT, 0, 0);
            else if (HasArg(cmdLine, L"--original")) PostMessageW(existing, WM_FOYER_ORIGINAL, 0, 0);
            else if (!HasArg(cmdLine, L"--tray")) {
                POINT pt; GetCursorPos(&pt);
                PostMessageW(existing, WM_FOYER_TOGGLE, 1, MAKELPARAM((short)pt.x, (short)pt.y));
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (HasArg(cmdLine, L"--quit")) return 0;
    if (HasArg(cmdLine, L"--original")) { OpenOriginalStartMenu(); return 0; }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LogInit();
    Log(L"==== Foyer %ls starting (data: %ls)", FOYER_VERSION, DataDir().c_str());
    ConfigLoad();
    g_interceptStartButton = g_config.interceptStartButton;
    g_interceptWinKey = g_config.interceptWinKey;
    ApplyDarkMenus();

    HWND hwnd = MenuCreate(inst);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Foyer couldn't start (see foyer.log). Your Windows Start menu is unchanged.", L"Foyer", MB_ICONERROR);
        return 1;
    }
    TrayAdd(hwnd);
    StartButtonRefreshAsync(hwnd);
    if (!HookStart(hwnd)) Log(L"Failed to start hook thread");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    HookStop();
    Log(L"Foyer exited normally");
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
