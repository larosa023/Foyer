// Foyer - shared declarations
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <iterator>

#define FOYER_VERSION L"0.2.1"

// Marker placed in dwExtraInfo of every input event Foyer injects, so our own
// hooks recognise and ignore them.
constexpr ULONG_PTR FOYER_INPUT_MAGIC = 0xF0E7F0E7;

// Messages posted to the main (UI) window
constexpr UINT WM_FOYER_TOGGLE        = WM_APP + 1;  // wParam: 0 keyboard, 1 mouse; lParam: MAKELPARAM(x,y) for mouse
constexpr UINT WM_FOYER_ORIGINAL      = WM_APP + 2;  // open the real Windows Start menu
constexpr UINT WM_FOYER_TRAY          = WM_APP + 3;  // tray icon callback
constexpr UINT WM_FOYER_APPS_READY    = WM_APP + 4;  // lParam: AppListResult*
constexpr UINT WM_FOYER_ICON_READY    = WM_APP + 5;  // lParam: IconResult*
constexpr UINT WM_FOYER_REFRESH_START = WM_APP + 6;  // re-locate the Start button
constexpr UINT WM_FOYER_QUIT          = WM_APP + 7;  // exit Foyer (sent by "Foyer.exe --quit")
constexpr UINT WM_FOYER_START_FOUND   = WM_APP + 8;  // background Start-button scan finished
constexpr UINT WM_FOYER_UPDATE        = WM_APP + 9;  // updater status changed
constexpr UINT WM_FOYER_SETTINGS      = WM_APP + 10; // open settings; wParam = page index

// ---- menu.cpp ----
HWND MenuCreate(HINSTANCE inst);
void MenuToggle(bool fromMouse, POINT pt);
void MenuHide();
bool MenuVisible();
void MenuReloadConfig();
void MenuApplySettings();         // theme / layout / behaviour changed
void MenuSetPreview(bool on);     // live preview while editing appearance
void MenuRefreshApps();
HWND MenuWindow();

// ---- settings.cpp ----
enum SettingsPage { PageGeneral = 0, PageAppearance, PageLayout, PageApps, PageUpdates, PageAbout, PageCount };
void SettingsOpen(int page = -1);
void SettingsOnUpdateStatus();    // refresh the Updates page
bool SettingsIsOpen();

// ---- autoupdate.cpp ----
void AutoUpdateInit(HWND owner, const wchar_t* cmdLine);
void AutoUpdateOnStatus();
void AutoUpdateOnTimer();
void AutoUpdateApplySettings();
void AutoUpdateCheckNow();
void AutoUpdateDownloadNow();
void AutoUpdateInstallNow();
bool AutoUpdateAvailable();       // an update is available or ready
void AutoUpdateOnStarted();       // called once the app is fully running
constexpr UINT_PTR TIMER_UPDATE_CHECK = 100;
constexpr UINT_PTR TIMER_HEALTHY = 101;
void ReleaseInstanceMutex();      // main.cpp

// ---- tray.cpp ----
void TrayAdd(HWND owner);
void TrayRemove();
void TrayUpdateTip();
void TrayOnMessage(HWND owner, LPARAM lParam);
void TrayOnCommand(HWND owner, UINT id);
bool StartWithWindowsEnabled();
void SetStartWithWindows(bool on);
void TrayBalloon(const std::wstring& title, const std::wstring& text);
constexpr UINT TRAY_CMD_FIRST = 40000;

// ---- log.cpp ----
void LogInit();
void Log(const wchar_t* fmt, ...);
std::wstring DataDir();          // %APPDATA%\Foyer, or exe folder in portable mode
std::wstring ExeDir();
std::wstring ExePath();

// ---- hook.cpp ----
bool HookStart(HWND mainWnd);    // starts the hook thread
void HookStop();
extern std::atomic<bool> g_paused;
extern std::atomic<bool> g_interceptStartButton;
extern std::atomic<bool> g_interceptWinKey;
void OpenOriginalStartMenu();     // injects a Win tap our hooks ignore
void SendMaskKey();               // harmless injected key (gives us foreground rights)

// ---- startbutton.cpp ----
void StartButtonRefresh();        // synchronous scan (UI Automation)
void StartButtonRefreshAsync(HWND notify);  // scan on a worker thread
bool StartButtonHitTest(POINT pt);// called from hook thread; cheap
bool PointOnAnyTaskbar(POINT pt);

// ---- power.cpp ----
void PowerSleep();
void PowerHibernate();
void PowerShutdown();
void PowerRestart();
void PowerRestartToFirmware(HWND owner);
void AccountLock();
void AccountSignOut();
void AccountSwitchUser();
std::wstring CurrentUserDisplayName();

// ---- util ----
inline std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
