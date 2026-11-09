// Foyer - settings and pins
#pragma once
#include "common.h"

struct Pin {
    std::wstring id;     // AppsFolder parsing name (e.g. "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App")
    std::wstring name;   // display name (cached so the tile shows before apps load)
};

struct Config {
    std::vector<Pin> pins;
    std::vector<std::wstring> hidden;      // app ids hidden from All apps
    bool interceptStartButton = true;
    bool interceptWinKey = true;

    bool IsHidden(const std::wstring& id) const;
    bool IsPinned(const std::wstring& id) const;
};

extern Config g_config;
void ConfigLoad();
void ConfigSave();
