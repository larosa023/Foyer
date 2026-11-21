// Foyer - change notifications for the things Foyer shows, so it never re-scans for nothing
#pragma once
#include "common.h"

enum WatchFlags : unsigned {
    WatchApps = 1,      // Start menu shortcuts added/removed/renamed
    WatchRecent = 2,    // a file was opened (Windows' Recent list changed)
    WatchFiles = 4,     // something changed in the indexed folders
};

// One thread waits on change notifications (it costs nothing while idle) and posts
// WM_FOYER_CHANGED with a WatchFlags mask, after a short settle delay so a burst of
// changes (an installer, a folder copy) becomes a single notification.
void WatchStart(HWND notify, const std::vector<std::wstring>& fileRoots);
