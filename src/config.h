// Foyer - settings and pins
#pragma once
#include "common.h"

enum class PinType { App, File, Folder, Url, Group };

struct Pin {
    std::wstring id;     // App: AppsFolder parsing name or exe path; File/Folder: path; Url: address; Group: unused
    std::wstring name;   // cached display name (so the tile shows before apps load)
    PinType type = PinType::App;
    std::wstring label;  // custom name set by the user (empty = automatic)
    std::wstring icon;   // custom icon file (.ico/.exe/.dll/.png), empty = automatic
    std::vector<Pin> items;   // Group only
};

std::wstring PinDisplayName(const Pin& p);   // label, else cached name, else file name / host

struct Config {
    // Pins and apps
    std::vector<Pin> pins;
    std::vector<std::wstring> hidden;      // app ids hidden from All apps

    // General
    bool interceptStartButton = true;
    bool interceptWinKey = true;
    bool showGreeting = true;
    bool askedInstall = false;
    bool elevatedMode = false;             // started as admin via a scheduled task (works over admin windows)

    // Recent
    bool recentEnabled = true;
    std::vector<std::wstring> hiddenRecent;

    // Pinned
    bool myAppsGroup = true;               // "My Apps" folder with every larosa023 app

    // Bottom bar
    std::vector<std::wstring> shortcuts{L"downloads", L"explorer", L"settings"};

    // Search
    bool searchFiles = true;
    bool searchSettings = true;

    // Appearance
    std::wstring theme = L"dark";          // dark, light, midnight, glass, ember, ocean, system
    bool useWindowsAccent = true;
    uint32_t accentColor = 0x4CC2FF;       // 0xRRGGBB when not using the Windows accent
    std::wstring backdrop = L"acrylic";    // solid, mica, acrylic
    float opacity = 0.78f;                 // tint strength over the backdrop (0.4 - 1.0)
    float cornerRadius = 8.0f;             // 0 - 16
    std::wstring iconSize = L"medium";     // small, medium, large
    std::wstring density = L"comfortable"; // compact, comfortable
    std::wstring animation = L"smooth";    // none, fast, smooth
    bool accentGlow = true;
    std::wstring font;                     // empty = Segoe UI Variable / Segoe UI
    float fontScale = 1.0f;                // 0.85 - 1.3

    // Layout
    std::wstring layout = L"modern";       // modern, classic, fullscreen
    std::wstring position = L"taskbar";    // taskbar, center

    // Updates
    bool autoCheck = true;
    bool autoInstall = false;
    std::wstring channel = L"stable";      // stable, beta
    int checkHours = 6;
    double lastCheck = 0;                  // unix seconds
    std::wstring notifiedVersion;          // last version we showed a notification for

    bool IsHidden(const std::wstring& id) const;
    bool IsPinned(const std::wstring& id) const;
};

extern Config g_config;
void ConfigLoad();
void ConfigSave();
void ConfigDefaultPins();
std::wstring ConfigPath();
std::wstring ThemesDir();                 // %APPDATA%\Foyer\themes (user theme files)
bool ConfigExportTo(const std::wstring& path);
bool ConfigImportFrom(const std::wstring& path, std::wstring* error);
