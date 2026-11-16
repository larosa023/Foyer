// Foyer - a small text prompt built from standard controls (keeps IME, clipboard and
// screen readers working without any custom text handling)
#include "inputbox.h"
#include <dwmapi.h>
#include <uxtheme.h>

struct InputState { std::wstring title, prompt, value; bool ok = false; bool done = false; bool dark = true; HWND edit = nullptr; HFONT font = nullptr; HBRUSH bg = nullptr; };

static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (InputState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_CREATE: {
            st = (InputState*)((CREATESTRUCTW*)l)->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
            UINT dpi = GetDpiForWindow(h);
            auto px = [&](int v) { return MulDiv(v, dpi, 96); };
            NONCLIENTMETRICSW ncm{sizeof(ncm)};
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
            ncm.lfMessageFont.lfHeight = -px(14);
            st->font = CreateFontIndirectW(&ncm.lfMessageFont);
            st->bg = CreateSolidBrush(st->dark ? RGB(0x20, 0x20, 0x24) : RGB(0xF6, 0xF6, 0xF8));
            HWND label = CreateWindowExW(0, L"STATIC", st->prompt.c_str(), WS_CHILD | WS_VISIBLE, px(20), px(16), px(380), px(40), h, nullptr, nullptr, nullptr);
            st->edit = CreateWindowExW(0, L"EDIT", st->value.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                       px(20), px(58), px(380), px(30), h, (HMENU)100, nullptr, nullptr);
            HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, px(214), px(104), px(90), px(32), h, (HMENU)IDOK, nullptr, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(310), px(104), px(90), px(32), h, (HMENU)IDCANCEL, nullptr, nullptr);
            for (HWND c : {label, st->edit, ok, cancel}) {
                SendMessageW(c, WM_SETFONT, (WPARAM)st->font, TRUE);
                if (st->dark) SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            }
            if (st->dark) SetWindowTheme(st->edit, L"DarkMode_CFD", nullptr);
            SetFocus(st->edit);
            SendMessageW(st->edit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            if (st && st->dark) {
                SetTextColor((HDC)w, RGB(0xF3, 0xF3, 0xF5));
                SetBkColor((HDC)w, m == WM_CTLCOLOREDIT ? RGB(0x2C, 0x2C, 0x31) : RGB(0x20, 0x20, 0x24));
                static HBRUSH editBg = CreateSolidBrush(RGB(0x2C, 0x2C, 0x31));
                return (LRESULT)(m == WM_CTLCOLOREDIT ? editBg : st->bg);
            }
            break;
        case WM_ERASEBKGND: {
            RECT r; GetClientRect(h, &r);
            FillRect((HDC)w, &r, st->bg);
            return 1;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
                if (LOWORD(w) == IDOK) {
                    int n = GetWindowTextLengthW(st->edit);
                    std::wstring v(n, L'\0');
                    GetWindowTextW(st->edit, v.data(), n + 1);
                    st->value = v;
                    st->ok = true;
                }
                DestroyWindow(h);
            }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            st->done = true;
            if (st->font) DeleteObject(st->font);
            if (st->bg) DeleteObject(st->bg);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value, bool dark) {
    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = InputProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"FoyerInput";
        RegisterClassExW(&wc);
        reg = true;
    }
    InputState st;
    st.title = title; st.prompt = prompt; st.value = value; st.dark = dark;
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    { UINT dy; using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
      if (HMODULE sh = LoadLibraryW(L"shcore.dll")) { if (auto f = (Fn)GetProcAddress(sh, "GetDpiForMonitor")) f(mon, 0, &dpi, &dy); } }
    RECT r{0, 0, MulDiv(420, dpi, 96), MulDiv(152, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 3;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"FoyerInput", title.c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                x, y, w, h, owner, nullptr, inst, &st);
    if (!hwnd) return false;
    BOOL d = dark;
    DwmSetWindowAttribute(hwnd, 20, &d, sizeof(d));
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (st.ok) value = st.value;
    return st.ok;
}
