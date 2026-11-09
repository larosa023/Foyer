// Foyer - system tray icon and quick menu
#include "common.h"
#include "config.h"
#include "apps.h"
#include <shellapi.h>

static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;

enum TrayCmd : UINT {
    T_OPEN = TRAY_CMD_FIRST, T_ORIGINAL, T_PAUSE, T_STARTUP, T_BUTTON, T_CONFIG, T_LOGS, T_RESCAN, T_UNHIDE, T_QUIT,
    T_SETTINGS, T_UPDATES,
};

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool StartWithWindowsEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Foyer", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetStartWithWindows(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(key, L"Foyer", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"Foyer");
    }
    RegCloseKey(key);
    Log(L"Start with Windows: %ls", on ? L"on" : L"off");
}

void TrayUpdateTip() {
    if (!g_trayAdded) return;
    wcscpy_s(g_nid.szTip, g_paused ? L"Foyer (paused - Windows Start menu active)" : L"Foyer");
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void TrayAdd(HWND owner) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = owner;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_FOYER_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Foyer");
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    TrayUpdateTip();
}

void TrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    n.hBalloonIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayRemove() {
    if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

static void ShowTrayMenu(HWND owner) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, T_OPEN, L"Open Foyer");
    AppendMenuW(m, MF_STRING, T_ORIGINAL, L"Open Windows Start menu\tCtrl+Win");
    AppendMenuW(m, MF_STRING, T_SETTINGS, L"Settings\x2026");
    AppendMenuW(m, MF_STRING, T_UPDATES, AutoUpdateAvailable() ? L"Update available\x2026" : L"Check for updates");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), T_PAUSE, L"Pause Foyer");
    AppendMenuW(m, MF_STRING | (g_interceptStartButton ? MF_CHECKED : 0), T_BUTTON, L"Open Foyer from the Start button");
    AppendMenuW(m, MF_STRING | (StartWithWindowsEnabled() ? MF_CHECKED : 0), T_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, T_CONFIG, L"Edit settings file (advanced)");
    AppendMenuW(m, MF_STRING, T_LOGS, L"Open data folder (logs)");
    AppendMenuW(m, MF_STRING, T_RESCAN, L"Re-detect Start button");
    AppendMenuW(m, MF_STRING | (g_config.hidden.empty() ? MF_GRAYED : 0), T_UNHIDE, L"Unhide all apps");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Foyer " FOYER_VERSION);
    AppendMenuW(m, MF_STRING, T_QUIT, L"Quit Foyer");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);   // required so the menu closes when clicking away
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
}

void TrayOnMessage(HWND owner, LPARAM lParam) {
    switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT: {
            POINT pt; GetCursorPos(&pt);
            if (!MenuVisible()) MenuToggle(true, pt);
            break;
        }
        case NIN_BALLOONUSERCLICK:
            SettingsOpen(PageUpdates);
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            MenuHide();
            ShowTrayMenu(owner);
            break;
    }
}

void TrayOnCommand(HWND owner, UINT id) {
    switch (id) {
        case T_OPEN: { POINT pt; GetCursorPos(&pt); MenuToggle(true, pt); break; }
        case T_ORIGINAL: PostMessageW(owner, WM_FOYER_ORIGINAL, 0, 0); break;
        case T_PAUSE:
            g_paused = !g_paused;
            Log(L"Foyer %ls", g_paused ? L"paused" : L"resumed");
            TrayUpdateTip();
            break;
        case T_BUTTON:
            g_config.interceptStartButton = !g_config.interceptStartButton;
            g_interceptStartButton = g_config.interceptStartButton;
            ConfigSave();
            SettingsOnUpdateStatus();
            break;
        case T_STARTUP: SetStartWithWindows(!StartWithWindowsEnabled()); break;
        case T_CONFIG: ShellOpen(L"notepad.exe", (L"\"" + DataDir() + L"\\config.json\"").c_str()); break;
        case T_LOGS: ShellOpen(DataDir().c_str()); break;
        case T_RESCAN: StartButtonRefreshAsync(owner); break;
        case T_UNHIDE: g_config.hidden.clear(); ConfigSave(); MenuReloadConfig(); break;
        case T_SETTINGS: SettingsOpen(-1); break;
        case T_UPDATES: SettingsOpen(PageUpdates); if (!AutoUpdateAvailable()) AutoUpdateCheckNow(); break;
        case T_QUIT: DestroyWindow(owner); break;
    }
}
