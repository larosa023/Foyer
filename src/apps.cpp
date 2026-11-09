// Foyer - app enumeration, icons and launching
#include "apps.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <algorithm>
#include <set>

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

static bool ExtractIcon(IShellItem* item, int px, IconResult& out) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&f))) || !f) return false;
    HBITMAP hbm = nullptr;
    SIZE sz{px, px};
    HRESULT hr = f->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT, &hbm);
    f->Release();
    if (FAILED(hr) || !hbm) return false;

    BITMAP bm{};
    GetObjectW(hbm, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) { DeleteObject(hbm); return false; }

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

    // Center into a square canvas so every icon is the same size.
    int s = std::max(w, h);
    out.size = s;
    out.pixels.assign((size_t)s * s * 4, 0);
    int ox = (s - w) / 2, oy = (s - h) / 2;
    for (int y = 0; y < h; ++y)
        memcpy(&out.pixels[((size_t)(y + oy) * s + ox) * 4], &src[(size_t)y * w * 4], (size_t)w * 4);
    return true;
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
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = what;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) Log(L"ShellOpen failed for %ls (error %lu)", what, GetLastError());
}

void AppLaunch(const std::wstring& id) {
    std::wstring target = L"shell:AppsFolder\\" + id;
    Log(L"Launch %ls", id.c_str());
    ShellOpen(target.c_str());
}

static bool InvokeVerb(HWND owner, const std::wstring& id, const char* verb) {
    std::wstring path = L"shell:AppsFolder\\" + id;
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
