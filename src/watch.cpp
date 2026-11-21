// Foyer - change notifications
#include "watch.h"
#include <shlobj.h>
#include <thread>

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

void WatchStart(HWND notify, const std::vector<std::wstring>& fileRoots) {
    struct W { std::wstring path; BOOL subtree; DWORD filter; unsigned flag; };
    std::vector<W> watches;
    const DWORD names = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    for (auto id : {&FOLDERID_Programs, &FOLDERID_CommonPrograms})
        if (auto p = KnownFolder(*id); !p.empty()) watches.push_back({p, TRUE, names | FILE_NOTIFY_CHANGE_LAST_WRITE, WatchApps});
    if (auto p = KnownFolder(FOLDERID_Recent); !p.empty()) watches.push_back({p, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, WatchRecent});
    for (auto& r : fileRoots) watches.push_back({r, TRUE, names, WatchFiles});

    std::thread([notify, watches] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<HANDLE> handles;
        std::vector<unsigned> flags;
        for (auto& w : watches) {
            HANDLE h = FindFirstChangeNotificationW(w.path.c_str(), w.subtree, w.filter);
            if (h == INVALID_HANDLE_VALUE) continue;
            handles.push_back(h);
            flags.push_back(w.flag);
            if (handles.size() == MAXIMUM_WAIT_OBJECTS) break;
        }
        // Store apps don't use Start menu shortcuts; their installs show up in this registry key.
        HKEY pkgKey = nullptr;
        HANDLE pkgEvent = nullptr;
        size_t pkgIndex = (size_t)-1;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
                          0, KEY_NOTIFY, &pkgKey) == ERROR_SUCCESS) {
            pkgEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (pkgEvent && RegNotifyChangeKeyValue(pkgKey, FALSE, REG_NOTIFY_CHANGE_NAME, pkgEvent, TRUE) == ERROR_SUCCESS) {
                pkgIndex = handles.size();
                handles.push_back(pkgEvent);
                flags.push_back(WatchApps);
            }
        }
        Log(L"Watching %u location(s) for changes", (unsigned)handles.size());
        if (handles.empty()) return;
        unsigned pending = 0;
        for (;;) {
            // wait forever while nothing is pending; otherwise let changes settle for 1.5 s
            DWORD r = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, pending ? 1500 : INFINITE);
            if (r == WAIT_TIMEOUT) {
                PostMessageW(notify, WM_FOYER_CHANGED, pending, 0);
                pending = 0;
                continue;
            }
            if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + handles.size()) {
                size_t i = r - WAIT_OBJECT_0;
                pending |= flags[i];
                if (i == pkgIndex) {   // registry notifications are one-shot: re-arm
                    RegNotifyChangeKeyValue(pkgKey, FALSE, REG_NOTIFY_CHANGE_NAME, pkgEvent, TRUE);
                    continue;
                }
                if (!FindNextChangeNotification(handles[i])) {   // the folder went away: stop watching it
                    FindCloseChangeNotification(handles[i]);
                    handles.erase(handles.begin() + i);
                    flags.erase(flags.begin() + i);
                    if (pkgIndex != (size_t)-1 && i < pkgIndex) --pkgIndex;
                    if (handles.empty()) return;
                }
                continue;
            }
            return;   // WAIT_FAILED
        }
    }).detach();
}
