// Foyer - installed apps (the shell "AppsFolder": desktop + Store apps)
#pragma once
#include "common.h"

struct AppEntry {
    std::wstring id;         // AppsFolder parsing name
    std::wstring name;       // display name
    std::wstring nameLower;
    std::wstring target;     // exe/file path for desktop apps (may be empty for Store apps)
    bool suite = false;      // a larosa023 app (Hangar knows about it)
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
bool AppCacheLoad(std::vector<AppEntry>& out);
void AppCacheSave(const std::vector<AppEntry>& apps);

void AppsRefreshAsync(HWND notify, std::vector<std::wstring> haveIcons, int iconPx);
bool AppsRefreshRunning();

void AppLaunch(const std::wstring& id);
void AppRunAsAdmin(HWND owner, const std::wstring& id, const std::wstring& target);
void AppOpenLocation(HWND owner, const std::wstring& id, const std::wstring& target);
void AppUninstall(const std::wstring& id);
void ShellOpen(const wchar_t* what, const wchar_t* args = nullptr);
void OpenPath(const std::wstring& path);              // files, folders, URLs (records usage)
void OpenFileLocation(const std::wstring& path);      // Explorer with the item selected

// Icons for anything that isn't an app: files, folders, custom pin icons. Extracted on a
// background thread; each posts WM_FOYER_ICON_READY with an IconResult* whose id is `key`.
// `thumbnail` uses the picture itself for image files (custom icons).
void IconRequest(HWND notify, const std::wstring& key, const std::wstring& parsingName, int px, bool thumbnail = false);
std::wstring FileIconKey(const std::wstring& path, bool dir);   // shared per extension where possible

// Icon cache on disk so icons are there instantly after boot.
struct CachedIcon { std::wstring id; int size; std::vector<uint8_t> pixels; };
bool IconCacheLoad(int px, std::vector<CachedIcon>& out, bool* stale);
void IconCacheSave(int px, const std::vector<CachedIcon>& icons);
