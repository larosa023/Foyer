// Foyer - recently opened files and folders (from Windows' own Recent list)
#pragma once
#include "common.h"

struct RecentItem {
    std::wstring path, name, folder;
    bool dir = false;
    long long when = 0;     // FILETIME as an integer (when it was last opened)
};

struct RecentResult { std::vector<RecentItem> items; };

// Reads Recent on a background thread and posts WM_FOYER_RECENT_READY with a RecentResult*.
// Apps, installers and anything in `hidden` are left out.
void RecentRefreshAsync(HWND notify, size_t max, std::vector<std::wstring> hidden);
std::wstring RelativeTime(long long filetime);   // "Just now", "2h ago", "Yesterday", "Mar 4"
