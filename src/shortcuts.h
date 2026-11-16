// Foyer - quick shortcuts for the bottom bar
#pragma once
#include <cwchar>

struct QuickShortcut { const wchar_t* id; const wchar_t* name; wchar_t glyph; const wchar_t* target; const wchar_t* args; };

inline const QuickShortcut kQuickShortcuts[] = {
    {L"downloads", L"Downloads", 0xE896, L"shell:Downloads", nullptr},
    {L"documents", L"Documents", 0xE8A5, L"shell:Personal", nullptr},
    {L"pictures", L"Pictures", 0xEB9F, L"shell:My Pictures", nullptr},
    {L"music", L"Music", 0xEC4F, L"shell:My Music", nullptr},
    {L"videos", L"Videos", 0xE714, L"shell:My Video", nullptr},
    {L"userfolder", L"Your folder", 0xE77B, L"shell:Profile", nullptr},
    {L"explorer", L"File Explorer", 0xEC50, L"explorer.exe", nullptr},
    {L"recyclebin", L"Recycle Bin", 0xE74D, L"shell:RecycleBinFolder", nullptr},
    {L"settings", L"Windows Settings", 0xE713, L"ms-settings:", nullptr},
    {L"network", L"Network", 0xE774, L"ms-settings:network-status", nullptr},
    {L"personalization", L"Personalization", 0xE771, L"ms-settings:personalization", nullptr},
    {L"taskmgr", L"Task Manager", 0xE9D9, L"taskmgr.exe", nullptr},
    {L"devmgr", L"Device Manager", 0xE7F8, L"devmgmt.msc", nullptr},
    {L"controlpanel", L"Control Panel", 0xE80F, L"control.exe", nullptr},
    {L"terminal", L"Terminal", 0xE756, L"wt.exe", nullptr},
    {L"run", L"Run", 0xE7C3, L"shell:::{2559a1f3-21d7-11d4-bdaf-00c04f60b9f0}", nullptr},
};
inline constexpr int kQuickShortcutCount = (int)(sizeof(kQuickShortcuts) / sizeof(kQuickShortcuts[0]));
inline constexpr int kMaxQuickShortcuts = 6;

inline const QuickShortcut* FindQuickShortcut(const std::wstring& id) {
    for (auto& s : kQuickShortcuts) if (id == s.id) return &s;
    return nullptr;
}
