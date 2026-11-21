// Foyer - app enumeration, icons and launching
#include "apps.h"
#include "system.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <algorithm>
#include <set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "json.h"

static std::atomic<bool> g_refreshing{false};

bool AppsRefreshRunning() { return g_refreshing.load(); }

struct RefreshArgs {
    HWND notify;
    std::set<std::wstring> haveIcons;
    int iconPx;
};

static bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

static bool ExtractIcon(IShellItem* item, int px, IconResult& out, bool thumbnail = false) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&f))) || !f) return false;
    HBITMAP hbm = nullptr;
    SIZE sz{px, px};
    HRESULT hr = f->GetImage(sz, thumbnail ? (SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK) : (SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT), &hbm);
    f->Release();
    if (FAILED(hr) || !hbm) return false;

    BITMAP bm{};
    GetObjectW(hbm, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { DeleteObject(hbm); return false; }

    std::vector<uint8_t> src((size_t)w * h * 4);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hbm, 0, h, src.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(hbm);
    if (lines != h) return false;

    // Fix up alpha: no alpha at all -> opaque; straight alpha -> premultiply.
    bool anyAlpha = false, straight = false;
    for (size_t i = 0; i < src.size(); i += 4) {
        uint8_t a = src[i + 3];
        if (a) anyAlpha = true;
        if (src[i] > a || src[i + 1] > a || src[i + 2] > a) straight = true;
    }
    for (size_t i = 0; i < src.size(); i += 4) {
        if (!anyAlpha) { src[i + 3] = 255; continue; }
        if (straight) {
            uint8_t a = src[i + 3];
            src[i]     = (uint8_t)((src[i] * a + 127) / 255);
            src[i + 1] = (uint8_t)((src[i + 1] * a + 127) / 255);
            src[i + 2] = (uint8_t)((src[i + 2] * a + 127) / 255);
        }
    }

    // Thumbnails can come back larger than asked: shrink with a box filter.
    if (thumbnail && (w > px || h > px)) {
        float scale = (float)px / (float)std::max(w, h);
        int nw = std::max(1, (int)(w * scale)), nh = std::max(1, (int)(h * scale));
        std::vector<uint8_t> dst((size_t)nw * nh * 4, 0);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                int x0 = x * w / nw, x1 = std::max(x0 + 1, (x + 1) * w / nw), y0 = y * h / nh, y1 = std::max(y0 + 1, (y + 1) * h / nh);
                unsigned acc[4] = {0, 0, 0, 0}, n = 0;
                for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* sp = &src[((size_t)yy * w + xx) * 4];
                    for (int c = 0; c < 4; ++c) acc[c] += sp[c];
                    ++n;
                }
                for (int c = 0; c < 4; ++c) dst[((size_t)y * nw + x) * 4 + c] = (uint8_t)(acc[c] / n);
            }
        src.swap(dst); w = nw; h = nh;
    }

    // Center into a square canvas so every icon is the same size.
    int s = std::max(w, h);
    out.size = s;
    out.pixels.assign((size_t)s * s * 4, 0);
    int ox = (s - w) / 2, oy = (s - h) / 2;
    for (int y = 0; y < h; ++y)
        memcpy(&out.pixels[((size_t)(y + oy) * s + ox) * 4], &src[(size_t)y * w * 4], (size_t)w * 4);
    return true;
}


// ---------------------------------------------------------------- apps without a Start menu shortcut
// Windows (and the shell's AppsFolder) only knows about apps that have a Start menu shortcut
// or are Store apps. Portable apps run from Downloads, Documents or a tools folder never
// show up there. Hangar tracks where every larosa023 app lives, so Foyer adds those too.
static std::wstring ExeDisplayName(const std::wstring& exe) {
    std::wstring name;
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (size) {
        std::vector<BYTE> buf(size);
        if (GetFileVersionInfoW(exe.c_str(), 0, size, buf.data())) {
            struct LangCp { WORD lang, cp; } *tr = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LangCp)) {
                for (const wchar_t* field : {L"FileDescription", L"ProductName"}) {
                    wchar_t key[128];
                    swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\%ls", tr[0].lang, tr[0].cp, field);
                    wchar_t* val = nullptr;
                    UINT vl = 0;
                    if (VerQueryValueW(buf.data(), key, (void**)&val, &vl) && vl > 1 && val[0]) { name = val; break; }
                }
            }
        }
    }
    if (name.empty()) {
        size_t slash = exe.find_last_of(L"\\/");
        name = exe.substr(slash == std::wstring::npos ? 0 : slash + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
        if (!name.empty()) name[0] = (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)name[0]);
    }
    return name;
}

static std::vector<std::wstring> HangarAppExes() {
    std::vector<std::wstring> out;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return out;
    std::wstring path = std::wstring(local) + L"\\Hangar\\locations.json";
    CoTaskMemFree(local);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(h, nullptr), read = 0;
    std::string bytes(size < 1024 * 1024 ? size : 0, '\0');
    bool ok = !bytes.empty() && ReadFile(h, &bytes[0], (DWORD)bytes.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return out;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)read, nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)read, &text[0], n);
    json::Value root;
    json::Parser parser(text);
    if (!parser.Parse(root) || root.type != json::Value::Object) return out;
    for (auto& kv : root.obj) {
        if (kv.second.type != json::Value::String) continue;
        std::wstring exe = kv.second.s;
        // Hangar sometimes stores "dir\\\\App.exe"; collapse doubled separators
        for (size_t i; (i = exe.find(L"\\\\", 2)) != std::wstring::npos;) exe.erase(i, 1);
        DWORD attr = GetFileAttributesW(exe.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(exe);
    }
    return out;
}

// ---------------------------------------------------------------- app list cache
// The last app list is kept on disk so the full list is there the instant Foyer starts,
// even before Windows has finished enumerating apps after boot.
static std::wstring AppCachePath() { return DataDir() + L"\\apps.cache"; }

void AppCacheSave(const std::vector<AppEntry>& apps) {
    json::Value arr = json::Value::MakeArray();
    for (auto& a : apps) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"id", json::Value::Str(a.id));
        o.Set(L"name", json::Value::Str(a.name));
        if (!a.target.empty()) o.Set(L"target", json::Value::Str(a.target));
        if (a.suite) o.Set(L"suite", json::Value::Boolean(true));
        arr.arr.push_back(std::move(o));
    }
    std::wstring text;
    json::Write(arr, text);
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string buf(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], n, nullptr, nullptr);
    std::wstring tmp = AppCachePath() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr) && w == buf.size();
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), AppCachePath().c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

bool AppCacheLoad(std::vector<AppEntry>& out) {
    HANDLE h = CreateFileW(AppCachePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr), read = 0;
    std::string buf(size < 8 * 1024 * 1024 ? size : 0, '\0');
    bool ok = !buf.empty() && ReadFile(h, &buf[0], size, &read, nullptr) && read == size;
    CloseHandle(h);
    if (!ok) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, &text[0], n);
    json::Value root;
    json::Parser parser(text);
    if (!parser.Parse(root) || root.type != json::Value::Array) return false;
    for (auto& v : root.arr) {
        AppEntry e;
        e.id = v.GetStr(L"id");
        e.name = v.GetStr(L"name");
        e.target = v.GetStr(L"target");
        e.suite = v.GetBool(L"suite", false);
        if (e.id.empty() || e.name.empty()) continue;
        e.nameLower = ToLower(e.name);
        out.push_back(std::move(e));
    }
    return !out.empty();
}

static DWORD WINAPI RefreshThread(LPVOID p) {
    RefreshArgs* args = static_cast<RefreshArgs*>(p);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DWORD t0 = GetTickCount();

    auto* result = new AppListResult();
    std::vector<IShellItem*> items;

    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder))) && folder) {
        IEnumShellItems* en = nullptr;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en))) && en) {
            IShellItem* it = nullptr;
            while (en->Next(1, &it, nullptr) == S_OK && it) {
                AppEntry e;
                PWSTR s = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s) { e.name = s; CoTaskMemFree(s); }
                s = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &s)) && s) { e.id = s; CoTaskMemFree(s); }
                IShellItem2* it2 = nullptr;
                if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2))) && it2) {
                    s = nullptr;
                    if (SUCCEEDED(it2->GetString(PKEY_Link_TargetParsingPath, &s)) && s) { e.target = s; CoTaskMemFree(s); }
                    it2->Release();
                }
                // Debloat: skip uninstaller shortcuts and empty entries.
                if (e.id.empty() || e.name.empty() || StartsWithI(e.name, L"Uninstall ")) { it->Release(); continue; }
                if (e.target.empty() && e.id.size() > 3 && e.id[1] == L':') e.target = e.id;
                e.nameLower = ToLower(e.name);
                result->apps.push_back(std::move(e));
                items.push_back(it);
            }
            en->Release();
        }
        folder->Release();
    }

    // Add suite apps that live outside the Start menu (see HangarAppExes).
    {
        std::vector<std::wstring> hangar = HangarAppExes();
        std::set<std::wstring> suiteTargets;
        for (auto& h : hangar) suiteTargets.insert(ToLower(h));
        for (auto& a : result->apps) if (!a.target.empty() && suiteTargets.count(ToLower(a.target))) a.suite = true;
        std::set<std::wstring> knownTargets, knownNames;
        for (auto& a : result->apps) {
            if (!a.target.empty()) knownTargets.insert(ToLower(a.target));
            knownNames.insert(a.nameLower);
        }
        int added = 0;
        for (const std::wstring& exe : hangar) {
            if (knownTargets.count(ToLower(exe))) continue;
            AppEntry e;
            e.id = exe;
            e.target = exe;
            e.suite = true;
            e.name = ExeDisplayName(exe);
            e.nameLower = ToLower(e.name);
            if (e.name.empty() || knownNames.count(e.nameLower)) continue;
            IShellItem* it = nullptr;
            if (FAILED(SHCreateItemFromParsingName(exe.c_str(), nullptr, IID_PPV_ARGS(&it))) || !it) continue;
            knownTargets.insert(ToLower(exe));
            knownNames.insert(e.nameLower);
            result->apps.push_back(std::move(e));
            items.push_back(it);
            ++added;
        }
        if (added) Log(L"Added %d app(s) that have no Start menu shortcut", added);
    }

    // Sort alphabetically, keeping items aligned with apps.
    std::vector<size_t> order(result->apps.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                               result->apps[a].name.c_str(), -1, result->apps[b].name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    std::vector<AppEntry> sorted;
    std::vector<IShellItem*> sortedItems;
    sorted.reserve(order.size());
    for (size_t i : order) { sorted.push_back(std::move(result->apps[i])); sortedItems.push_back(items[i]); }
    result->apps = std::move(sorted);
    items = std::move(sortedItems);

    std::vector<std::wstring> ids;
    for (auto& a : result->apps) ids.push_back(a.id);
    size_t count = result->apps.size();
    AppCacheSave(result->apps);
    if (!PostMessageW(args->notify, WM_FOYER_APPS_READY, 0, (LPARAM)result)) delete result;
    Log(L"Enumerated %u apps in %lu ms", (unsigned)count, GetTickCount() - t0);

    int loaded = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!args->haveIcons.count(ids[i])) {
            auto* ir = new IconResult();
            ir->id = ids[i];
            if (ExtractIcon(items[i], args->iconPx, *ir) && PostMessageW(args->notify, WM_FOYER_ICON_READY, 0, (LPARAM)ir)) ++loaded;
            else delete ir;
        }
        items[i]->Release();
    }
    Log(L"Loaded %d icons; refresh took %lu ms", loaded, GetTickCount() - t0);

    delete args;
    CoUninitialize();
    g_refreshing = false;
    return 0;
}

void AppsRefreshAsync(HWND notify, std::vector<std::wstring> haveIcons, int iconPx) {
    bool expected = false;
    if (!g_refreshing.compare_exchange_strong(expected, true)) return;
    auto* args = new RefreshArgs{notify, std::set<std::wstring>(haveIcons.begin(), haveIcons.end()), iconPx};
    HANDLE h = CreateThread(nullptr, 0, RefreshThread, args, 0, nullptr);
    if (h) CloseHandle(h);
    else { delete args; g_refreshing = false; }
}

void ShellOpen(const wchar_t* what, const wchar_t* args) {
    if (!ShellExecAsUser(what, args ? args : L"", L"")) Log(L"ShellOpen failed for %ls (error %lu)", what, GetLastError());
}

static bool IsPathId(const std::wstring& id) { return id.size() > 3 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/'); }

void AppLaunch(const std::wstring& id) {
    if (IsPathId(id)) {
        // An exe outside the Start menu: start it in its own folder.
        std::wstring dir = id.substr(0, id.find_last_of(L"\\/"));
        Log(L"Launch %ls", id.c_str());
        if (!ShellExecAsUser(id, L"", dir)) Log(L"Launch failed for %ls (error %lu)", id.c_str(), GetLastError());
        return;
    }
    std::wstring target = L"shell:AppsFolder\\" + id;
    Log(L"Launch %ls", id.c_str());
    ShellOpen(target.c_str());
}

static bool InvokeVerb(HWND owner, const std::wstring& id, const char* verb) {
    std::wstring path = IsPathId(id) ? id : L"shell:AppsFolder\\" + id;
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item) return false;
    bool ok = false;
    IContextMenu* cm = nullptr;
    if (SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm))) && cm) {
        HMENU menu = CreatePopupMenu();
        if (SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL))) {
            CMINVOKECOMMANDINFOEX ici{sizeof(ici)};
            ici.fMask = CMIC_MASK_UNICODE;
            ici.hwnd = owner;
            ici.lpVerb = verb;
            std::wstring wverb(verb, verb + strlen(verb));
            ici.lpVerbW = wverb.c_str();
            ici.nShow = SW_SHOWNORMAL;
            ok = SUCCEEDED(cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ici));
        }
        DestroyMenu(menu);
        cm->Release();
    }
    item->Release();
    return ok;
}

void AppRunAsAdmin(HWND owner, const std::wstring& id, const std::wstring& target) {
    Log(L"Run as admin %ls", id.c_str());
    if (InvokeVerb(owner, id, "runas")) return;
    if (!target.empty()) {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.lpVerb = L"runas";
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        sei.hwnd = owner;
        if (ShellExecuteExW(&sei)) return;
    }
    MessageBoxW(owner, L"This app can't be started as administrator.", L"Foyer", MB_ICONINFORMATION);
}

void AppOpenLocation(HWND owner, const std::wstring& id, const std::wstring& target) {
    if (!target.empty() && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring args = L"/select,\"" + target + L"\"";
        ShellOpen(L"explorer.exe", args.c_str());
        return;
    }
    if (InvokeVerb(owner, id, "openfilelocation")) return;
    MessageBoxW(owner, L"This is a Microsoft Store app, so it doesn't have a regular file location.", L"Foyer", MB_ICONINFORMATION);
}

void AppUninstall(const std::wstring& id) {
    // Store apps contain "!" in their id (PackageFamilyName!AppId).
    if (id.find(L'!') != std::wstring::npos) ShellOpen(L"ms-settings:appsfeatures");
    else ShellOpen(L"control.exe", L"appwiz.cpl");
}

// ---------------------------------------------------------------- files, folders, URLs
void OpenPath(const std::wstring& path) {
    Log(L"Open %ls", path.c_str());
    if (!ShellExecAsUser(path, L"", L"")) Log(L"Open failed for %ls (error %lu)", path.c_str(), GetLastError());
}

void OpenFileLocation(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
        return;
    }
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellOpen(L"explorer.exe", args.c_str());
}

std::wstring FileIconKey(const std::wstring& path, bool dir) {
    if (dir) return L"path:" + ToLower(path);
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"ext:";
    std::wstring ext = ToLower(path.substr(dot));
    // these carry their own icons
    static const wchar_t* own[] = {L".exe", L".lnk", L".ico", L".url", L".appref-ms", L".msc", L".cpl", L".scr"};
    for (auto o : own) if (ext == o) return L"path:" + ToLower(path);
    return L"ext:" + ext;
}

static std::mutex s_iconLock;
static std::condition_variable s_iconCv;
struct IconJob { HWND notify; std::wstring key, parsing; int px; bool thumb; };
static std::vector<IconJob> s_iconJobs;
static std::set<std::wstring> s_iconQueued;
static bool s_iconWorker = false;

static void IconWorker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        IconJob job;
        {
            std::unique_lock<std::mutex> lk(s_iconLock);
            s_iconCv.wait(lk, [] { return !s_iconJobs.empty(); });
            job = s_iconJobs.back();          // newest first: what's on screen now
            s_iconJobs.pop_back();
        }
        IShellItem* item = nullptr;
        auto* ir = new IconResult();
        ir->id = job.key;
        bool ok = false;
        if (SUCCEEDED(SHCreateItemFromParsingName(job.parsing.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            ok = ExtractIcon(item, job.px, *ir, job.thumb);
            item->Release();
        }
        if (!ok || !PostMessageW(job.notify, WM_FOYER_ICON_READY, 0, (LPARAM)ir)) delete ir;
    }
}

void IconRequest(HWND notify, const std::wstring& key, const std::wstring& parsingName, int px, bool thumbnail) {
    std::lock_guard<std::mutex> lk(s_iconLock);
    if (!s_iconQueued.insert(key).second) return;   // asked before (loaded, failed or pending)
    s_iconJobs.push_back({notify, key, parsingName, px, thumbnail});
    if (!s_iconWorker) { s_iconWorker = true; std::thread(IconWorker).detach(); }
    s_iconCv.notify_one();
}

// ---------------------------------------------------------------- icon cache
static std::wstring IconCachePath() { return DataDir() + L"\\icons.cache"; }

bool IconCacheLoad(int px, std::vector<CachedIcon>& out, bool* stale) {
    HANDLE h = CreateFileW(IconCachePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft{}, now{};
    GetFileTime(h, nullptr, nullptr, &ft);
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = ft.dwLowDateTime; a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    if (stale) *stale = b.QuadPart > a.QuadPart && (b.QuadPart - a.QuadPart) > 7ULL * 86400ULL * 10000000ULL;
    DWORD size = GetFileSize(h, nullptr), read = 0;
    std::vector<uint8_t> buf(size < 64 * 1024 * 1024 ? size : 0);
    bool ok = !buf.empty() && ReadFile(h, buf.data(), size, &read, nullptr) && read == size;
    CloseHandle(h);
    if (!ok || size < 12 || memcmp(buf.data(), "FYI1", 4) != 0) return false;
    uint32_t cachedPx, count;
    memcpy(&cachedPx, &buf[4], 4);
    memcpy(&count, &buf[8], 4);
    if ((int)cachedPx != px) return false;
    size_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 2 > size) return false;
        uint16_t klen; memcpy(&klen, &buf[p], 2); p += 2;
        if (p + klen * 2 + 2 > size) return false;
        CachedIcon ci;
        ci.id.assign((const wchar_t*)&buf[p], klen); p += klen * 2;
        uint16_t isz; memcpy(&isz, &buf[p], 2); p += 2;
        size_t bytes = (size_t)isz * isz * 4;
        if (isz == 0 || isz > 1024 || p + bytes > size) return false;
        ci.size = isz;
        ci.pixels.assign(buf.begin() + p, buf.begin() + p + bytes);
        p += bytes;
        out.push_back(std::move(ci));
    }
    return true;
}

void IconCacheSave(int px, const std::vector<CachedIcon>& icons) {
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), {'F', 'Y', 'I', '1'});
    uint32_t p32 = (uint32_t)px, count = (uint32_t)icons.size();
    buf.insert(buf.end(), (uint8_t*)&p32, (uint8_t*)&p32 + 4);
    buf.insert(buf.end(), (uint8_t*)&count, (uint8_t*)&count + 4);
    for (auto& ci : icons) {
        uint16_t klen = (uint16_t)std::min<size_t>(ci.id.size(), 65535), isz = (uint16_t)ci.size;
        buf.insert(buf.end(), (uint8_t*)&klen, (uint8_t*)&klen + 2);
        buf.insert(buf.end(), (const uint8_t*)ci.id.data(), (const uint8_t*)ci.id.data() + klen * 2);
        buf.insert(buf.end(), (uint8_t*)&isz, (uint8_t*)&isz + 2);
        buf.insert(buf.end(), ci.pixels.begin(), ci.pixels.end());
    }
    std::wstring tmp = IconCachePath() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr) && w == buf.size();
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), IconCachePath().c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}
