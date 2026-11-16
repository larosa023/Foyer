// Foyer - recently opened files and folders
#include "recent.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <set>
#include <thread>
#include <atomic>

static std::atomic<bool> s_busy{false};

static bool IsAppLike(const std::wstring& lowerPath) {
    static const wchar_t* exts[] = {L".exe", L".lnk", L".msi", L".msix", L".appx", L".bat", L".cmd", L".com", L".scr", L".appref-ms", L".url", L".ps1"};
    for (auto e : exts) {
        size_t n = wcslen(e);
        if (lowerPath.size() > n && lowerPath.compare(lowerPath.size() - n, n, e) == 0) return true;
    }
    return false;
}

void RecentRefreshAsync(HWND notify, size_t max, std::vector<std::wstring> hidden) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return;
    std::thread([notify, max, hidden] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto* res = new RecentResult();
        PWSTR rp = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &rp)) && rp) {
            std::wstring dir = rp;
            CoTaskMemFree(rp);
            struct L { std::wstring lnk; long long t; };
            std::vector<L> links;
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    long long t = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                    links.push_back({dir + L"\\" + fd.cFileName, t});
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            std::sort(links.begin(), links.end(), [](const L& a, const L& b) { return a.t > b.t; });
            std::set<std::wstring> seen, hide;
            for (auto& h : hidden) hide.insert(ToLower(h));
            IShellLinkW* link = nullptr;
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) && link &&
                SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf))) && pf) {
                size_t looked = 0;
                for (auto& l : links) {
                    if (res->items.size() >= max || ++looked > 250) break;
                    if (FAILED(pf->Load(l.lnk.c_str(), STGM_READ))) continue;
                    wchar_t target[MAX_PATH * 2] = {};
                    if (FAILED(link->GetPath(target, MAX_PATH * 2, nullptr, SLGP_RAWPATH)) || !target[0]) continue;
                    wchar_t expanded[MAX_PATH * 2];
                    std::wstring path = ExpandEnvironmentStringsW(target, expanded, MAX_PATH * 2) ? expanded : target;
                    std::wstring lower = ToLower(path);
                    if (IsAppLike(lower) || seen.count(lower) || hide.count(lower)) continue;
                    if (lower.compare(0, 2, L"\\\\") == 0) continue;           // network paths can stall
                    if (lower.find(L"\\appdata\\local\\temp\\") != std::wstring::npos) continue;
                    DWORD attr = GetFileAttributesW(path.c_str());
                    if (attr == INVALID_FILE_ATTRIBUTES) continue;                 // moved or deleted
                    seen.insert(lower);
                    RecentItem it;
                    it.path = path;
                    it.dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    std::wstring p = path;
                    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
                    size_t sl = p.find_last_of(L"\\/");
                    it.name = sl == std::wstring::npos ? p : p.substr(sl + 1);
                    it.folder = sl == std::wstring::npos ? L"" : p.substr(0, sl);
                    if (it.name.empty()) it.name = p;
                    it.when = l.t;
                    res->items.push_back(std::move(it));
                }
            }
            if (pf) pf->Release();
            if (link) link->Release();
        }
        CoUninitialize();
        s_busy = false;
        if (!PostMessageW(notify, WM_FOYER_RECENT_READY, 0, (LPARAM)res)) delete res;
    }).detach();
}

std::wstring RelativeTime(long long ft) {
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    long long now = ((long long)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    long long secs = (now - ft) / 10000000LL;
    if (secs < 90) return L"Just now";
    if (secs < 3600) return std::to_wstring(secs / 60) + L" min ago";
    if (secs < 86400) return std::to_wstring(secs / 3600) + L"h ago";
    if (secs < 2 * 86400) return L"Yesterday";
    if (secs < 7 * 86400) return std::to_wstring(secs / 86400) + L" days ago";
    FILETIME f{(DWORD)(ft & 0xFFFFFFFF), (DWORD)(ft >> 32)}, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[64];
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"MMM d", buf, 64, nullptr)) return buf;
    return L"";
}
