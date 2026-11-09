// Foyer - Windows key and Start button interception
//
// How it works (and why it is safe):
//
// * Foyer installs two low-level hooks (WH_KEYBOARD_LL, WH_MOUSE_LL) on a
//   dedicated thread that does nothing else. Nothing is injected into
//   Explorer, nothing is patched, no system file or registry setting is
//   changed. If the Foyer process exits or crashes, Windows removes the hooks
//   automatically and the real Start menu works on the very next key press.
//
// * Windows opens Start when the Win key is RELEASED and no other key was
//   pressed while it was held. Foyer lets every Win key-down through
//   untouched, so Win+E, Win+D, Win+R, Win+Shift+S etc. behave exactly as
//   normal. Only when the Win key is released "alone" does Foyer block that
//   release and replay it with a harmless unassigned key (vkE8) in front of
//   it. Windows sees "Win, vkE8, Win-up" -- a combo -- and does not open
//   Start. Foyer then shows its own menu.
//
// * Fallbacks that always reach the real Start menu:
//     - Ctrl+Win (press Ctrl, tap Win)     -> original Start menu
//     - Ctrl+Esc                            -> never touched by Foyer
//     - Shift+click the Start button        -> passed straight through
//     - Tray icon > "Open Windows Start"
//     - Tray icon > "Pause Foyer"           -> hooks pass everything through
//
// * Hang protection: the hook thread never waits on the UI. Before swallowing
//   a key it asks Windows whether the Foyer window is hung (IsHungAppWindow);
//   if it is, the key passes straight through and Windows opens its Start
//   menu. Windows also enforces a time limit on low-level hooks and drops a
//   hook that keeps exceeding it -- which again just means the real Start
//   menu comes back.
//
// * Admin windows: if an elevated app has focus, Windows would block our
//   replayed key-up, so Foyer steps aside and the normal Start menu opens.
#include "common.h"

std::atomic<bool> g_paused{false};
std::atomic<bool> g_interceptStartButton{true};
std::atomic<bool> g_interceptWinKey{true};

static HWND   g_main = nullptr;
static HANDLE g_thread = nullptr;
static DWORD  g_threadId = 0;
static HHOOK  g_kbHook = nullptr;
static HHOOK  g_msHook = nullptr;

// Keyboard state (touched only on the hook thread)
static bool g_winDown = false;      // a Win key is currently held
static DWORD g_winVk = 0;           // which one (VK_LWIN / VK_RWIN)
static bool g_comboUsed = false;    // another key/mouse button pressed while Win held
static bool g_ctrlUsed = false;     // Ctrl held at some point while Win held
static DWORD g_lastWinTime = 0;     // time of the last Win key-down event

// Mouse state
static bool g_swallowNextUp = false;
static DWORD g_lastRefreshTick = 0;

constexpr WORD VK_MASK = 0xE8;      // unassigned virtual key (same trick AutoHotkey uses)

static bool UiIsResponsive() {
    return g_main && IsWindow(g_main) && !IsHungAppWindow(g_main);
}

// Windows silently drops input that a normal process injects into an
// elevated (admin) window (UIPI). If we swallowed the Win key-up in that case
// and our replay were dropped, Windows would think Win is still held. So when
// an elevated window has focus, Foyer stands aside and lets Windows handle the
// key. Cached per foreground window to keep the hook fast.
static bool SelfElevated() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        HANDLE tok;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION e{}; DWORD sz;
            if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &sz)) cached = e.TokenIsElevated ? 1 : 0;
            CloseHandle(tok);
        }
    }
    return cached == 1;
}

static bool ForegroundIsElevated() {
    if (SelfElevated()) return false;
    static HWND lastWnd = nullptr;
    static DWORD lastPid = 0;
    static bool lastResult = false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (fg == lastWnd && pid == lastPid) return lastResult;
    bool elevated = false;
    if (pid && pid != GetCurrentProcessId()) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc) elevated = true;   // can't even query it: treat as protected
        else {
            HANDLE tok;
            if (OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
                TOKEN_ELEVATION e{}; DWORD sz;
                if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &sz)) elevated = e.TokenIsElevated != 0;
                CloseHandle(tok);
            } else {
                elevated = true;
            }
            CloseHandle(proc);
        }
    }
    lastWnd = fg; lastPid = pid; lastResult = elevated;
    return elevated;
}

static void FillKey(INPUT& in, WORD vk, bool up) {
    in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_LWIN || vk == VK_RWIN) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    in.ki.dwExtraInfo = FOYER_INPUT_MAGIC;
}

void SendMaskKey() {
    INPUT in[2];
    FillKey(in[0], VK_MASK, false);
    FillKey(in[1], VK_MASK, true);
    SendInput(2, in, sizeof(INPUT));
}

void OpenOriginalStartMenu() {
    // If the user is still physically holding Ctrl (from Ctrl+Win), wait for it
    // to be released, otherwise Windows treats our tap as Ctrl+Win.
    for (int i = 0; i < 60 && (GetAsyncKeyState(VK_CONTROL) & 0x8000); ++i) Sleep(15);
    INPUT in[2];
    FillKey(in[0], VK_LWIN, false);
    FillKey(in[1], VK_LWIN, true);
    SendInput(2, in, sizeof(INPUT));
    Log(L"Opened original Windows Start menu");
}

static bool IsCtrl(DWORD vk) { return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);
    auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

    // Ignore everything Foyer itself injected.
    if (k->dwExtraInfo == FOYER_INPUT_MAGIC) return CallNextHookEx(nullptr, code, wParam, lParam);

    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    const DWORD vk  = k->vkCode;
    const bool isWin = (vk == VK_LWIN || vk == VK_RWIN);

    if (isWin && down) {
        // Auto-repeat arrives every ~30 ms. A long gap means the key-up was
        // never seen (e.g. Win+L switched to the lock screen), so start fresh.
        bool stale = g_winDown && (k->time - g_lastWinTime > 700 || vk != g_winVk);
        g_lastWinTime = k->time;
        if (!g_winDown || stale) {  // first press (ignore auto-repeat)
            g_winDown = true;
            g_winVk = vk;
            g_comboUsed = false;
            g_ctrlUsed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);   // always let Win-down through
    }

    if (!isWin && down && g_winDown) {
        if (IsCtrl(vk)) g_ctrlUsed = true;
        else g_comboUsed = true;    // Win+<key> shortcut - leave it to Windows
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (isWin && up && g_winDown && vk == g_winVk) {
        g_winDown = false;
        if (g_comboUsed) return CallNextHookEx(nullptr, code, wParam, lParam);

        if (g_ctrlUsed) {
            // Ctrl+Win fallback: let the release through, then open the real Start.
            if (g_main) PostMessageW(g_main, WM_FOYER_ORIGINAL, 0, 0);
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        if (g_paused.load() || !g_interceptWinKey.load() || !UiIsResponsive() || ForegroundIsElevated()) {
            return CallNextHookEx(nullptr, code, wParam, lParam); // Windows opens its Start menu
        }

        // Swallow the real release and replay it behind a mask key.
        INPUT in[3];
        FillKey(in[0], VK_MASK, false);
        FillKey(in[1], VK_MASK, true);
        FillKey(in[2], (WORD)vk, true);
        if (SendInput(3, in, sizeof(INPUT)) != 3) {
            // Injection failed: let the real key-up through so Win never sticks.
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        PostMessageW(g_main, WM_FOYER_TOGGLE, 0, 0);
        return 1;
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION || wParam == WM_MOUSEMOVE) return CallNextHookEx(nullptr, code, wParam, lParam);
    auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (m->dwExtraInfo == FOYER_INPUT_MAGIC) return CallNextHookEx(nullptr, code, wParam, lParam);

    // Any mouse button while Win is held counts as a combo (Win+click etc).
    if (g_winDown && wParam != WM_MOUSEWHEEL && wParam != WM_MOUSEHWHEEL) g_comboUsed = true;

    if (wParam == WM_LBUTTONDOWN) {
        if (!g_interceptStartButton.load() || g_paused.load()) return CallNextHookEx(nullptr, code, wParam, lParam);
        if (StartButtonHitTest(m->pt)) {
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return CallNextHookEx(nullptr, code, wParam, lParam); // Shift+click = original
            if (!UiIsResponsive() || ForegroundIsElevated()) return CallNextHookEx(nullptr, code, wParam, lParam);
            g_swallowNextUp = true;
            PostMessageW(g_main, WM_FOYER_TOGGLE, 1, MAKELPARAM((short)m->pt.x, (short)m->pt.y));
            return 1;
        }
        // Click somewhere on a taskbar: the Start button may have moved
        // (alignment change, new monitor...). Ask the UI to re-locate it, rate-limited.
        if (PointOnAnyTaskbar(m->pt)) {
            DWORD now = GetTickCount();
            if (now - g_lastRefreshTick > 3000) {
                g_lastRefreshTick = now;
                PostMessageW(g_main, WM_FOYER_REFRESH_START, 0, 0);
            }
        }
    } else if (wParam == WM_LBUTTONUP && g_swallowNextUp) {
        g_swallowNextUp = false;
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static DWORD WINAPI HookThread(LPVOID) {
    // Run the hook thread at high priority so key handling never lags.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInst, 0);
    g_msHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    Log(L"Hooks installed: keyboard=%ls mouse=%ls", g_kbHook ? L"ok" : L"FAILED", g_msHook ? L"ok" : L"FAILED");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_msHook) UnhookWindowsHookEx(g_msHook);
    g_kbHook = g_msHook = nullptr;
    return 0;
}

bool HookStart(HWND mainWnd) {
    g_main = mainWnd;
    g_thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_threadId);
    return g_thread != nullptr;
}

void HookStop() {
    if (!g_thread) return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    Log(L"Hooks removed");
}
