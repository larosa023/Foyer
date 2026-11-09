// Foyer - locate the taskbar Start button(s)
//
// Windows 11's taskbar is XAML, so the Start button is not a real window.
// We ask UI Automation for the element with AutomationId "StartButton" on
// each taskbar (primary + secondary monitors) and cache its rectangle. The
// mouse hook then only does a cheap point-in-rect test.
#include "common.h"
#include <uiautomation.h>

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::vector<RECT> g_startRects;
static std::vector<RECT> g_taskbarRects;

static IUIAutomation* Uia() {
    // Created per scan thread (cheap) and released at the end of the scan.
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
    return uia;
}

static bool FindStartInTaskbar(IUIAutomation* uia, HWND taskbar, RECT& out) {
    // Windows 10 style: a real child window of class "Start"
    if (HWND start = FindWindowExW(taskbar, nullptr, L"Start", nullptr)) {
        if (IsWindowVisible(start) && GetWindowRect(start, &out)) return true;
    }
    if (!uia) return false;

    bool found = false;
    IUIAutomationElement* root = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(taskbar, &root)) && root) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"StartButton");
        IUIAutomationCondition* cond = nullptr;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &cond)) && cond) {
            IUIAutomationElement* el = nullptr;
            if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, cond, &el)) && el) {
                RECT r{};
                if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && r.right > r.left && r.bottom > r.top) {
                    out = r;
                    found = true;
                }
                el->Release();
            }
            cond->Release();
        }
        VariantClear(&v);
        root->Release();
    }
    return found;
}

static BOOL CALLBACK EnumTaskbars(HWND hwnd, LPARAM lp) {
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, 64)) return TRUE;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        reinterpret_cast<std::vector<HWND>*>(lp)->push_back(hwnd);
    }
    return TRUE;
}

void StartButtonRefresh() {
    std::vector<HWND> bars;
    EnumWindows(EnumTaskbars, (LPARAM)&bars);
    std::vector<RECT> starts, tbs;
    IUIAutomation* uia = Uia();
    for (HWND bar : bars) {
        RECT tr;
        if (GetWindowRect(bar, &tr)) tbs.push_back(tr);
        RECT sr;
        if (FindStartInTaskbar(uia, bar, sr)) starts.push_back(sr);
    }
    if (uia) uia->Release();
    AcquireSRWLockExclusive(&g_lock);
    g_startRects = std::move(starts);
    g_taskbarRects = std::move(tbs);
    size_t n = g_startRects.size();
    RECT first = n ? g_startRects[0] : RECT{};
    ReleaseSRWLockExclusive(&g_lock);
    Log(L"Start button scan: %u taskbar(s), %u start button(s); first=(%ld,%ld,%ld,%ld)",
        (unsigned)bars.size(), (unsigned)n, first.left, first.top, first.right, first.bottom);
}

static std::atomic<bool> g_scanning{false};

static DWORD WINAPI ScanThread(LPVOID p) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    StartButtonRefresh();
    CoUninitialize();
    g_scanning = false;
    if (p) PostMessageW((HWND)p, WM_FOYER_START_FOUND, 0, 0);
    return 0;
}

// UI Automation talks to Explorer and can be slow if Explorer is busy, so the
// scan runs on its own short-lived thread and never blocks the menu.
void StartButtonRefreshAsync(HWND notify) {
    bool expected = false;
    if (!g_scanning.compare_exchange_strong(expected, true)) return;
    HANDLE h = CreateThread(nullptr, 0, ScanThread, notify, 0, nullptr);
    if (h) CloseHandle(h); else g_scanning = false;
}

bool StartButtonHitTest(POINT pt) {
    bool hit = false;
    AcquireSRWLockShared(&g_lock);
    for (const RECT& r : g_startRects) if (PtInRect(&r, pt)) { hit = true; break; }
    ReleaseSRWLockShared(&g_lock);
    return hit;
}

bool PointOnAnyTaskbar(POINT pt) {
    bool hit = false;
    AcquireSRWLockShared(&g_lock);
    for (const RECT& r : g_taskbarRects) if (PtInRect(&r, pt)) { hit = true; break; }
    ReleaseSRWLockShared(&g_lock);
    return hit;
}
