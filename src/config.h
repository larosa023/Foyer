// Foyer - settings and pins
#pragma once
#include "common.h"

struct Pin {
    std::wstring id;     // AppsFolder parsing name (e.g. "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App")
    std::wstring name;   // display name (cached so the tile shows before apps load)
};

struct Config {
    // Pins and apps
    std::vector<Pin> pins;
    std::vector<std::wstring> hidden;      // app ids hidden from All apps

    // General
    bool interceptStartButton = true;
    bool interceptWinKey = true;
    bool showGreeting = true;

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
bool ConfigExportTo(const std::wstring& path);
bool ConfigImportFrom(const std::wstring& path, std::wstring* error);
