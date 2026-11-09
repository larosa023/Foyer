// Foyer - installed apps (the shell "AppsFolder": desktop + Store apps)
#pragma once
#include "common.h"

struct AppEntry {
    std::wstring id;         // AppsFolder parsing name
    std::wstring name;       // display name
    std::wstring nameLower;
    std::wstring target;     // exe/file path for desktop apps (may be empty for Store apps)
};

struct AppListResult { std::vector<AppEntry> apps; };

struct IconResult {
    std::wstring id;
    int size = 0;                 // width == height, pixels
    std::vector<uint8_t> pixels;  // premultiplied BGRA, top-down
};

// Enumerates apps on a background thread. Posts WM_FOYER_APPS_READY with an
// AppListResult*, then one WM_FOYER_ICON_READY per app icon not in `haveIcons`.
// The receiver owns (and must delete) the posted objects.
void AppsRefreshAsync(HWND notify, std::vector<std::wstring> haveIcons, int iconPx);
bool AppsRefreshRunning();

void AppLaunch(const std::wstring& id);
void AppRunAsAdmin(HWND owner, const std::wstring& id, const std::wstring& target);
void AppOpenLocation(HWND owner, const std::wstring& id, const std::wstring& target);
void AppUninstall(const std::wstring& id);
void ShellOpen(const wchar_t* what, const wchar_t* args = nullptr);
