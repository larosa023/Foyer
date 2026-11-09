// Foyer - the menu window (Direct2D + DirectWrite, custom drawn)
//
// The window is created hidden at startup and stays alive with its render
// target, fonts and icons ready, so opening it is just "position, show,
// paint". Animations run on a short timer only while something is moving;
// when the menu is still, Foyer uses no CPU at all.
#include "common.h"
#include "config.h"
#include "apps.h"
#include "gfx.h"
#include "updater.h"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <powrprof.h>
#include <map>
#include <algorithm>
#include <cmath>

using namespace gfx;

// ============================================================ metrics
struct Metrics {
    float W = 760, H = 660;           // panel size (DIPs)
    bool classic = false, full = false;
    int cols = 4;
    float tileW = 88, tileH = 88, tileIcon = 32;
    float rowH = 38, rowIcon = 24, letterH = 30, resultH = 48, resultIcon = 30;
    float pad = 20, greetH = 0, searchH = 42, gap = 14, headerH = 34, bottomH = 62, colGap = 24;
};

enum class Kind { Pin, AppRow, Letter, Result, Section, User, UpdatePill, BtnFoyer, BtnDownloads, BtnExplorer, BtnSettings, BtnPower };
enum Region { RNone = 0, RPins, RList, RResults };

struct Item {
    Kind kind;
    int index;            // pin index / row index / result index / section id
    D2D1_RECT_F rc;       // window DIPs, scroll applied
    Region region;
};

struct ListRow { bool header; wchar_t letter; int app; };

struct IconData {
    int size = 0;
    std::vector<uint8_t> pixels;
    ID2D1Bitmap* bmp = nullptr;
};

// ============================================================ state
static HWND g_hwnd;
static Theme g_theme;
static Metrics g_m;
static UINT g_dpi = 96;
static D2D1_SIZE_F g_win = {760, 660};     // window size (DIPs)
static D2D1_RECT_F g_panel;                // panel rect inside the window (DIPs)

static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static ID2D1RadialGradientBrush* g_glow;
static ID2D1RadialGradientBrush* g_glow2;
static Canvas g_cv;
static IDWriteTextFormat *g_fGreet, *g_fDate, *g_fSearch, *g_fHeader, *g_fRow, *g_fTile, *g_fSmall, *g_fIcon, *g_fIconSm,
    *g_fLetter, *g_fAvatar, *g_fJump, *g_fPill, *g_fTip;

static std::vector<AppEntry> g_apps;
static std::map<std::wstring, int> g_appById;
static std::map<std::wstring, IconData> g_icons;
static std::vector<ListRow> g_rows;
static std::vector<int> g_results;
static DWORD g_lastRefresh = 0;
static bool g_appsLoaded = false;
static int g_iconPx = 48;

static std::wstring g_query;
static bool g_selectAll = false;
static bool g_caretOn = true;
static bool g_jumpMode = false;
static bool g_preview = false;
static std::vector<Item> g_items;
static int g_hot = -1, g_sel = -1;
static bool g_tracking = false;
static float g_scroll[4] = {0, 0, 0, 0}, g_scrollTarget[4] = {0, 0, 0, 0};
static float g_contentH[4] = {0, 0, 0, 0};
static D2D1_RECT_F g_regionRc[4];
static std::wstring g_userName, g_firstName;
static bool g_hibernateAvailable = false;

// animation
static LARGE_INTEGER g_qpf;
static double g_openStart = 0;            // seconds
static double g_lastTick = 0;
static float g_openDur = 0.26f;
static bool g_animTimer = false;
static std::map<uint64_t, float> g_hoverA;
static uint64_t g_pressKey = ~0ull;
static float g_pressA = 0;

constexpr UINT_PTR TIMER_CARET = 1;
constexpr UINT_PTR TIMER_ANIM = 2;

// ============================================================ helpers
static double Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / g_qpf.QuadPart; }
static float Dip(int px) { return px * 96.0f / g_dpi; }
static int Px(float dip) { return (int)std::lround(dip * g_dpi / 96.0f); }
static bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static bool Searching() { return !g_query.empty(); }
static bool Animated() { return g_config.animation != L"none"; }
static uint64_t Key(Kind k, int idx) { return ((uint64_t)k << 32) | (uint32_t)idx; }
static float EaseOut(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }
static D2D1_RECT_F Inset(const D2D1_RECT_F& r, float d) { return D2D1::RectF(r.left + d, r.top + d, r.right - d, r.bottom - d); }

HWND MenuWindow() { return g_hwnd; }

static const AppEntry* AppById(const std::wstring& id) {
    auto it = g_appById.find(ToLower(id));
    return it == g_appById.end() ? nullptr : &g_apps[it->second];
}

static std::wstring PinName(int i) {
    const Pin& p = g_config.pins[i];
    if (const AppEntry* a = AppById(p.id)) return a->name;
    return p.name.empty() ? p.id : p.name;
}

static float OpenProgress() {
    if (!Animated() || g_openDur <= 0) return 1.0f;
    return (float)std::clamp((Now() - g_openStart) / g_openDur, 0.0, 1.0);
}

// ============================================================ metrics from settings
static void ComputeMetrics(const RECT& work) {
    Metrics m;
    bool compact = g_config.density == L"compact";
    m.rowH = compact ? 32.0f : 38.0f;
    m.letterH = compact ? 26.0f : 30.0f;
    m.resultH = compact ? 42.0f : 50.0f;
    m.tileW = compact ? 84.0f : 90.0f;
    m.tileIcon = g_config.iconSize == L"small" ? 26.0f : g_config.iconSize == L"large" ? 40.0f : 32.0f;
    m.rowIcon = g_config.iconSize == L"small" ? 20.0f : g_config.iconSize == L"large" ? 28.0f : 24.0f;
    m.resultIcon = m.rowIcon + 6;
    m.tileH = (compact ? 74.0f : 84.0f) + (m.tileIcon - 32);
    m.greetH = g_config.showGreeting ? 44.0f : 0.0f;
    m.classic = g_config.layout == L"classic";
    m.full = g_config.layout == L"fullscreen";
    float workW = Dip(work.right - work.left), workH = Dip(work.bottom - work.top);
    if (m.classic) {
        m.W = 420; m.H = 700;
    } else if (m.full) {
        m.W = std::min(1240.0f, workW - 120);
        m.H = std::min(880.0f, workH - 100);
        m.pad = 32;
        m.tileW += 8; m.tileH += 6;
        float leftW = (m.W - 2 * m.pad - m.colGap) * 0.6f;
        m.cols = std::max(4, (int)(leftW / m.tileW));
    } else {
        m.W = 4 * m.tileW + 2 * m.pad + m.colGap + 340;
        m.H = 640 + m.greetH;
    }
    m.H = std::min(m.H, workH - 24);
    m.W = std::min(m.W, workW - 24);
    g_m = m;
}

// ============================================================ fonts & device resources
static void ReleaseFormats() {
    IDWriteTextFormat** all[] = {&g_fGreet, &g_fDate, &g_fSearch, &g_fHeader, &g_fRow, &g_fTile, &g_fSmall, &g_fIcon,
                                 &g_fIconSm, &g_fLetter, &g_fAvatar, &g_fJump, &g_fPill, &g_fTip};
    for (auto f : all) SafeRelease(*f);
}

static void CreateFormats() {
    ReleaseFormats();
    const wchar_t* ui = UiFont();
    g_fGreet  = Format(DisplayFont(), 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fDate   = Format(ui, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    g_fSearch = Format(ui, 15.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fHeader = Format(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fRow    = Format(ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fTile   = Format(ui, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fSmall  = Format(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fIcon   = Format(IconFont(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconSm = Format(IconFont(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fLetter = Format(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fAvatar = Format(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fJump   = Format(ui, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fPill   = Format(ui, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fTip    = Format(ui, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
}

static void DiscardDeviceResources() {
    for (auto& kv : g_icons) SafeRelease(kv.second.bmp);
    SafeRelease(g_glow);
    SafeRelease(g_glow2);
    SafeRelease(g_brush);
    SafeRelease(g_rt);
}

static ID2D1RadialGradientBrush* MakeGlow(D2D1_COLOR_F c, float a) {
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP s[3] = {{0.0f, Alpha(c, a)}, {0.55f, Alpha(c, a * 0.35f)}, {1.0f, Alpha(c, 0.0f)}};
    g_rt->CreateGradientStopCollection(s, 3, &stops);
    ID2D1RadialGradientBrush* b = nullptr;
    if (stops) {
        g_rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), 300, 220), stops, &b);
        stops->Release();
    }
    return b;
}

static bool EnsureTarget() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    props.dpiX = props.dpiY = (float)g_dpi;
    HRESULT hr = d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY), &g_rt);
    if (FAILED(hr)) { Log(L"CreateHwndRenderTarget failed 0x%08lx", hr); return false; }
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(g_theme.text, &g_brush);
    g_glow = MakeGlow(g_theme.accent, g_theme.dark ? 0.26f : 0.20f);
    g_glow2 = MakeGlow(Mix(g_theme.accent, D2D1::ColorF(0.85f, 0.35f, 0.9f, 1.0f), 0.45f), g_theme.dark ? 0.12f : 0.09f);
    g_cv.rt = g_rt;
    g_cv.br = g_brush;
    return g_brush != nullptr;
}

static ID2D1Bitmap* IconBitmap(const std::wstring& id) {
    auto it = g_icons.find(id);
    if (it == g_icons.end()) return nullptr;
    IconData& d = it->second;
    if (!d.bmp && g_rt && d.size > 0) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        g_rt->CreateBitmap(D2D1::SizeU(d.size, d.size), d.pixels.data(), d.size * 4, bp, &d.bmp);
    }
    return d.bmp;
}

// ============================================================ model
static void RebuildRows() {
    g_rows.clear();
    std::vector<std::pair<wchar_t, std::vector<int>>> groups;
    for (int i = 0; i < (int)g_apps.size(); ++i) {
        const AppEntry& a = g_apps[i];
        if (g_config.IsHidden(a.id)) continue;
        wchar_t c = a.name.empty() ? L'#' : a.name[0];
        wchar_t up = (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)c);
        wchar_t letter = IsCharAlphaW(up) ? up : L'#';
        auto it = std::find_if(groups.begin(), groups.end(), [&](auto& g) { return g.first == letter; });
        if (it == groups.end()) { groups.push_back({letter, {}}); it = groups.end() - 1; }
        it->second.push_back(i);
    }
    std::stable_partition(groups.begin(), groups.end(), [](auto& g) { return g.first == L'#'; });
    for (auto& g : groups) {
        g_rows.push_back({true, g.first, -1});
        for (int i : g.second) g_rows.push_back({false, g.first, i});
    }
}

static int Score(const AppEntry& a, const std::wstring& q) {
    const std::wstring& n = a.nameLower;
    if (n.compare(0, q.size(), q) == 0) return 1000 - (int)std::min<size_t>(n.size(), 200);
    for (size_t i = 1; i < n.size(); ++i) {
        if ((n[i - 1] == L' ' || n[i - 1] == L'-' || n[i - 1] == L'.' || n[i - 1] == L'(') && n.compare(i, q.size(), q) == 0)
            return 800 - (int)std::min<size_t>(n.size(), 200);
    }
    std::wstring initials;
    bool start = true;
    for (wchar_t c : n) { if (start && c != L' ') initials += c; start = (c == L' ' || c == L'-'); }
    if (q.size() >= 2 && initials.compare(0, q.size(), q) == 0) return 600;
    if (n.find(q) != std::wstring::npos) return 400 - (int)std::min<size_t>(n.size(), 200);
    return -1;
}

static void RebuildResults() {
    g_results.clear();
    std::wstring q = ToLower(g_query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    while (!q.empty() && q.front() == L' ') q.erase(q.begin());
    if (q.empty()) return;
    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < (int)g_apps.size(); ++i) {
        if (g_config.IsHidden(g_apps[i].id)) continue;
        int s = Score(g_apps[i], q);
        if (s >= 0) scored.push_back({s, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < scored.size() && i < 60; ++i) g_results.push_back(scored[i].second);
}

// ============================================================ layout
static float ContentTop() { return g_panel.top + g_m.pad + g_m.greetH + g_m.searchH + g_m.gap; }
static float ContentBottom() { return g_panel.bottom - g_m.bottomH - 6; }

static void Layout() {
    g_items.clear();
    for (auto& r : g_regionRc) r = D2D1::RectF(0, 0, 0, 0);
    const Metrics& m = g_m;
    const float ox = g_panel.left, top = ContentTop(), bottom = ContentBottom();
    const float right = g_panel.right - m.pad;

    if (Searching()) {
        D2D1_RECT_F reg = D2D1::RectF(ox + m.pad, top + m.headerH, right, bottom);
        g_regionRc[RResults] = reg;
        float y = reg.top - g_scroll[RResults];
        for (int k = 0; k < (int)g_results.size(); ++k) {
            g_items.push_back({Kind::Result, k, D2D1::RectF(reg.left, y, reg.right, y + m.resultH), RResults});
            y += m.resultH;
        }
        g_contentH[RResults] = g_results.size() * m.resultH;
    } else if (m.classic) {
        D2D1_RECT_F reg = D2D1::RectF(ox + m.pad - 6, top, right + 6, bottom);
        g_regionRc[RList] = reg;
        float y = reg.top - g_scroll[RList];
        if (!g_config.pins.empty()) {
            g_items.push_back({Kind::Section, 0, D2D1::RectF(reg.left, y, reg.right, y + m.headerH), RList});
            y += m.headerH;
            for (int i = 0; i < (int)g_config.pins.size(); ++i) {
                g_items.push_back({Kind::Pin, i, D2D1::RectF(reg.left, y, reg.right, y + m.rowH), RList});
                y += m.rowH;
            }
            y += 8;
        }
        g_items.push_back({Kind::Section, 1, D2D1::RectF(reg.left, y, reg.right, y + m.headerH), RList});
        y += m.headerH;
        for (int r = 0; r < (int)g_rows.size(); ++r) {
            float h = g_rows[r].header ? m.letterH : m.rowH;
            g_items.push_back({g_rows[r].header ? Kind::Letter : Kind::AppRow, r, D2D1::RectF(reg.left, y, reg.right, y + h), RList});
            y += h;
        }
        g_contentH[RList] = y + g_scroll[RList] - reg.top;
    } else {
        float x0 = ox + m.pad, x1 = x0 + m.cols * m.tileW;
        D2D1_RECT_F pins = D2D1::RectF(x0, top + m.headerH, x1, bottom);
        g_regionRc[RPins] = pins;
        for (int i = 0; i < (int)g_config.pins.size(); ++i) {
            float x = x0 + (i % m.cols) * m.tileW;
            float y = pins.top + (i / m.cols) * m.tileH - g_scroll[RPins];
            g_items.push_back({Kind::Pin, i, D2D1::RectF(x, y, x + m.tileW, y + m.tileH), RPins});
        }
        g_contentH[RPins] = ((g_config.pins.size() + m.cols - 1) / m.cols) * m.tileH;

        float lx0 = x1 + m.colGap, lx1 = right;
        D2D1_RECT_F list = D2D1::RectF(lx0, top + m.headerH, lx1, bottom);
        g_regionRc[RList] = list;
        float y = list.top - g_scroll[RList];
        for (int r = 0; r < (int)g_rows.size(); ++r) {
            float h = g_rows[r].header ? m.letterH : m.rowH;
            g_items.push_back({g_rows[r].header ? Kind::Letter : Kind::AppRow, r, D2D1::RectF(lx0, y, lx1, y + h), RList});
            y += h;
        }
        g_contentH[RList] = y + g_scroll[RList] - list.top;
    }

    // Bottom bar
    float by = g_panel.bottom - m.bottomH;
    float ux = ox + m.pad - 8;
    float userW = std::min(230.0f, 50 + MeasureWidth(g_userName, g_fRow) + 16);
    g_items.push_back({Kind::User, 0, D2D1::RectF(ux, by + 11, ux + userW, g_panel.bottom - 11), RNone});
    if (AutoUpdateAvailable()) {
        float px = ux + userW + 8;
        g_items.push_back({Kind::UpdatePill, 0, D2D1::RectF(px, by + 17, px + 132, g_panel.bottom - 17), RNone});
    }
    float bx = g_panel.right - m.pad + 4;
    const Kind btns[] = {Kind::BtnPower, Kind::BtnSettings, Kind::BtnExplorer, Kind::BtnDownloads, Kind::BtnFoyer};
    for (Kind k : btns) {
        bx -= 40;
        g_items.push_back({k, 0, D2D1::RectF(bx, by + 11, bx + 40, g_panel.bottom - 11), RNone});
        bx -= 2;
    }
}

static bool IsSelectable(Kind k) { return k == Kind::Pin || k == Kind::AppRow || k == Kind::Result; }

static float MaxScroll(int r) {
    float viewH = g_regionRc[r].bottom - g_regionRc[r].top;
    return std::max(0.0f, g_contentH[r] - viewH);
}

static int FindItem(Kind k, int index) {
    for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].kind == k && g_items[i].index == index) return i;
    return -1;
}

static void StartAnim();

static void EnsureVisible(int itemIdx) {
    if (itemIdx < 0 || itemIdx >= (int)g_items.size()) return;
    const Item& it = g_items[itemIdx];
    if (it.region == RNone) return;
    const D2D1_RECT_F& reg = g_regionRc[it.region];
    // Work against the target scroll so keyboard navigation can glide.
    float delta = g_scrollTarget[it.region] - g_scroll[it.region];
    float top = it.rc.top - delta, bottom = it.rc.bottom - delta;
    float d = 0;
    if (top < reg.top) d = top - reg.top;
    else if (bottom > reg.bottom) d = bottom - reg.bottom;
    if (d != 0) {
        g_scrollTarget[it.region] = std::clamp(g_scrollTarget[it.region] + d, 0.0f, MaxScroll(it.region));
        if (!Animated()) g_scroll[it.region] = g_scrollTarget[it.region];
        Kind k = it.kind; int idx = it.index;
        Layout();
        g_sel = FindItem(k, idx);
        StartAnim();
    }
}

static int HitTest(float x, float y) {
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (it.region != RNone && !InRect(g_regionRc[it.region], x, y)) continue;
        if (InRect(it.rc, x, y)) return i;
    }
    return -1;
}

// ============================================================ animation
static float HoverOf(Kind k, int idx) {
    auto it = g_hoverA.find(Key(k, idx));
    return it == g_hoverA.end() ? 0.0f : it->second;
}

static bool AnimTick() {
    double now = Now();
    float dt = (float)std::min(0.05, now - g_lastTick);
    g_lastTick = now;
    bool moving = OpenProgress() < 1.0f;

    uint64_t hotKey = (g_hot >= 0 && g_hot < (int)g_items.size()) ? Key(g_items[g_hot].kind, g_items[g_hot].index) : ~0ull;
    if (hotKey != ~0ull && !g_hoverA.count(hotKey)) g_hoverA[hotKey] = 0.0f;
    float rate = dt / 0.12f;
    for (auto it = g_hoverA.begin(); it != g_hoverA.end();) {
        float target = it->first == hotKey ? 1.0f : 0.0f;
        float& v = it->second;
        if (!Animated()) v = target;
        else if (v < target) v = std::min(target, v + rate);
        else if (v > target) v = std::max(target, v - rate * 0.8f);
        if (v != target) moving = true;
        if (v <= 0.0f && target == 0.0f) it = g_hoverA.erase(it); else ++it;
    }

    float pTarget = g_pressKey != ~0ull ? 1.0f : 0.0f;
    if (!Animated()) g_pressA = pTarget;
    else if (g_pressA != pTarget) {
        g_pressA = pTarget > g_pressA ? std::min(1.0f, g_pressA + dt / 0.06f) : std::max(0.0f, g_pressA - dt / 0.12f);
        moving = true;
    }

    bool scrolled = false;
    for (int r = 1; r < 4; ++r) {
        float d = g_scrollTarget[r] - g_scroll[r];
        if (std::fabs(d) < 0.5f) { if (d != 0) { g_scroll[r] = g_scrollTarget[r]; scrolled = true; } continue; }
        g_scroll[r] += d * std::min(1.0f, dt * 16.0f);
        scrolled = moving = true;
    }
    if (scrolled) {
        int selKind = g_sel >= 0 ? (int)g_items[g_sel].kind : -1, selIdx = g_sel >= 0 ? g_items[g_sel].index : -1;
        Layout();
        if (selKind >= 0) g_sel = FindItem((Kind)selKind, selIdx);
        POINT pt; GetCursorPos(&pt); ScreenToClient(g_hwnd, &pt);
        g_hot = HitTest(Dip(pt.x), Dip(pt.y));
    }
    return moving;
}

static void StartAnim() {
    if (!g_hwnd) return;
    if (!g_animTimer) {
        g_lastTick = Now();
        SetTimer(g_hwnd, TIMER_ANIM, 8, nullptr);
        g_animTimer = true;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ============================================================ drawing
static void DrawIcon(const std::wstring& id, const std::wstring& name, float cx, float cy, float size, float opacity = 1.0f) {
    D2D1_RECT_F r = D2D1::RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2);
    if (ID2D1Bitmap* bmp = IconBitmap(id)) {
        g_rt->DrawBitmap(bmp, r, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    g_cv.FillRound(r, size * 0.24f, Alpha(Mix(g_theme.surfaceStrong, g_theme.accent, 0.18f), opacity));
    std::wstring initial = name.empty() ? L"?" : name.substr(0, 1);
    g_cv.Text(initial, g_fAvatar, r, Alpha(g_theme.text, opacity));
}

static void DrawItemBg(const D2D1_RECT_F& rc, Kind k, int idx, bool selected, float radius) {
    float h = HoverOf(k, idx);
    if (selected) {
        g_cv.FillRound(rc, radius, Alpha(g_theme.accent, g_theme.dark ? 0.16f : 0.13f));
        g_cv.StrokeRound(rc, radius, Alpha(g_theme.accent, 0.85f), 1.25f);
    } else if (h > 0.0f) {
        g_cv.FillRound(rc, radius, Alpha(g_theme.hover, h));
    }
}

static void DrawScrollbar(int r) {
    const D2D1_RECT_F& reg = g_regionRc[r];
    float viewH = reg.bottom - reg.top;
    if (g_contentH[r] <= viewH + 1) return;
    float thumbH = std::max(28.0f, viewH * viewH / g_contentH[r]);
    float t = g_scroll[r] / (g_contentH[r] - viewH);
    float y = reg.top + std::clamp(t, 0.0f, 1.0f) * (viewH - thumbH);
    g_cv.FillRound(D2D1::RectF(reg.right - 3, y, reg.right, y + thumbH), 1.5f, Alpha(g_theme.subtext, 0.45f));
}

static std::wstring GreetingText() {
    SYSTEMTIME st; GetLocalTime(&st);
    const wchar_t* part = st.wHour < 5 ? L"Good night" : st.wHour < 12 ? L"Good morning" : st.wHour < 17 ? L"Good afternoon" : L"Good evening";
    return g_firstName.empty() ? std::wstring(part) : std::wstring(part) + L", " + g_firstName;
}

static std::wstring DateText() {
    wchar_t buf[128];
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"dddd, MMMM d", buf, 128, nullptr)) return buf;
    return L"";
}

static void DrawSearch() {
    const Metrics& m = g_m;
    float top = g_panel.top + m.pad + m.greetH;
    D2D1_RECT_F box = D2D1::RectF(g_panel.left + m.pad, top, g_panel.right - m.pad, top + m.searchH);
    float rad = std::min(m.searchH / 2, std::max(6.0f, g_theme.radius * 2.5f));
    g_cv.FillRound(Inset(box, -3), rad + 3, Alpha(g_theme.accent, 0.12f));      // soft focus halo
    g_cv.FillRound(box, rad, g_theme.surface);
    g_cv.StrokeRound(box, rad, Alpha(g_theme.accent, 0.9f), 1.5f);
    g_cv.Glyph(0xE721, g_fIcon, D2D1::RectF(box.left + 10, box.top, box.left + 42, box.bottom), g_theme.accent);

    D2D1_RECT_F tr = D2D1::RectF(box.left + 46, box.top, box.right - 18, box.bottom);
    if (g_query.empty()) {
        g_cv.Text(L"Search apps", g_fSearch, D2D1::RectF(tr.left + 4, tr.top, tr.right, tr.bottom), g_theme.subtext);
        if (g_caretOn) g_cv.Fill(D2D1::RectF(tr.left, box.top + 12, tr.left + 1.5f, box.bottom - 12), g_theme.accent);
        return;
    }
    IDWriteTextLayout* lay = nullptr;
    dw->CreateTextLayout(g_query.c_str(), (UINT32)g_query.size(), g_fSearch, 4000, tr.bottom - tr.top, &lay);
    float width = 0;
    if (lay) { DWRITE_TEXT_METRICS mt; lay->GetMetrics(&mt); width = mt.widthIncludingTrailingWhitespace; }
    float maxW = tr.right - tr.left;
    float offset = std::max(0.0f, width - maxW + 4);
    if (g_selectAll) g_cv.FillRound(D2D1::RectF(tr.left - 2, box.top + 10, tr.left + std::min(width, maxW) + 2, box.bottom - 10), 3, Alpha(g_theme.accent, 0.35f));
    if (lay) {
        g_rt->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_ALIASED);
        g_brush->SetColor(g_theme.text);
        g_rt->DrawTextLayout(D2D1::Point2F(tr.left - offset, tr.top), lay, g_brush);
        g_rt->PopAxisAlignedClip();
        lay->Release();
    }
    if (g_caretOn && !g_selectAll) {
        float cx = tr.left + width - offset + 1;
        g_cv.Fill(D2D1::RectF(cx, box.top + 12, cx + 1.5f, box.bottom - 12), g_theme.accent);
    }
}

static void DrawJumpOverlay() {
    const D2D1_RECT_F& reg = g_regionRc[RList];
    D2D1_RECT_F all = D2D1::RectF(reg.left - 4, reg.top - g_m.headerH, reg.right + 4, reg.bottom);
    g_cv.FillRound(all, g_theme.radius + 2, Mix(g_theme.bg, g_theme.surface, 0.5f));
    const wchar_t* letters = L"#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    float cell = std::min(56.0f, (reg.right - reg.left) / 6.0f);
    float gx = reg.left + ((reg.right - reg.left) - cell * 6) / 2;
    for (int i = 0; letters[i]; ++i) {
        wchar_t c = letters[i];
        bool present = std::any_of(g_rows.begin(), g_rows.end(), [&](const ListRow& r) { return r.header && r.letter == c; });
        float x = gx + (i % 6) * cell, y = all.top + 10 + (i / 6) * cell;
        D2D1_RECT_F r = D2D1::RectF(x + 3, y + 3, x + cell - 3, y + cell - 3);
        if (present) g_cv.FillRound(r, g_theme.radius, Alpha(g_theme.accent, 0.14f));
        wchar_t s[2] = {c, 0};
        g_cv.Text(s, g_fJump, r, present ? g_theme.text : Alpha(g_theme.subtext, 0.35f));
    }
}

static int JumpHit(float x, float y) {
    const D2D1_RECT_F& reg = g_regionRc[RList];
    float top = reg.top - g_m.headerH + 10;
    float cell = std::min(56.0f, (reg.right - reg.left) / 6.0f);
    float gx = reg.left + ((reg.right - reg.left) - cell * 6) / 2;
    if (x < gx || x >= gx + cell * 6 || y < top) return -1;
    int i = (int)((y - top) / cell) * 6 + (int)((x - gx) / cell);
    const wchar_t* letters = L"#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    return (i >= 0 && i < 27) ? letters[i] : -1;
}

static void DrawTooltip(const Item& it, const wchar_t* text, float alpha) {
    if (alpha <= 0.05f) return;
    float w = MeasureWidth(text, g_fTip) + 18;
    float cx = (it.rc.left + it.rc.right) / 2;
    float x = std::clamp(cx - w / 2, g_panel.left + 6, g_panel.right - w - 6);
    D2D1_RECT_F r = D2D1::RectF(x, it.rc.top - 32 + (1 - alpha) * 4, x + w, it.rc.top - 8 + (1 - alpha) * 4);
    g_cv.FillRound(r, 6, Alpha(g_theme.surfaceStrong, 0.97f * alpha));
    g_cv.StrokeRound(r, 6, Alpha(g_theme.divider, alpha));
    g_cv.Text(text, g_fTip, r, Alpha(g_theme.text, alpha));
}

static void DrawPinTile(int i, const Item& it, float appear) {
    const Pin& p = g_config.pins[it.index];
    std::wstring name = PinName(it.index);
    float h = HoverOf(Kind::Pin, it.index);
    bool pressed = g_pressKey == Key(Kind::Pin, it.index);
    float scale = pressed ? 1.0f - 0.06f * g_pressA : 1.0f;
    float dy = (1 - appear) * 12;
    D2D1_RECT_F tile = D2D1::RectF(it.rc.left + 3, it.rc.top + 3 + dy, it.rc.right - 3, it.rc.bottom - 3 + dy);
    float cx = (tile.left + tile.right) / 2, cy = (tile.top + tile.bottom) / 2;
    D2D1_MATRIX_3X2_F saved;
    g_rt->GetTransform(&saved);
    if (scale != 1.0f) g_rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)) * saved);
    DrawItemBg(tile, Kind::Pin, it.index, i == g_sel, g_theme.radius);
    float lift = -2.5f * h;
    DrawIcon(p.id, name, cx, tile.top + 8 + g_m.tileIcon / 2 + 2 + lift, g_m.tileIcon, appear);
    float ty = tile.top + 14 + g_m.tileIcon;
    g_cv.Text(name, g_fTile, D2D1::RectF(tile.left + 4, ty, tile.right - 4, ty + 22), Alpha(g_theme.text, appear));
    g_rt->SetTransform(saved);
}

static void DrawRow(int i, const Item& it, const std::wstring& id, const std::wstring& name, const wchar_t* sub) {
    D2D1_RECT_F rc = Inset(it.rc, 1);
    bool pressed = g_pressKey == Key(it.kind, it.index);
    D2D1_MATRIX_3X2_F saved;
    g_rt->GetTransform(&saved);
    if (pressed) {
        float s = 1.0f - 0.02f * g_pressA;
        g_rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2)) * saved);
    }
    DrawItemBg(rc, it.kind, it.index, i == g_sel, g_theme.radius);
    float h = HoverOf(it.kind, it.index);
    float icon = sub ? g_m.resultIcon : g_m.rowIcon;
    float cy = (rc.top + rc.bottom) / 2;
    DrawIcon(id, name, rc.left + 10 + icon / 2 + 2 * h, cy, icon);
    float tx = rc.left + 22 + icon + 2 * h;
    if (sub) {
        g_cv.Text(name, g_fRow, D2D1::RectF(tx, rc.top + 5, rc.right - 12, cy + 2), g_theme.text);
        g_cv.Text(sub, g_fSmall, D2D1::RectF(tx, cy, rc.right - 12, rc.bottom - 5), g_theme.subtext);
    } else {
        g_cv.Text(name, g_fRow, D2D1::RectF(tx, rc.top, rc.right - 12, rc.bottom), g_theme.text);
    }
    if (i == g_sel) g_cv.FillRound(D2D1::RectF(rc.left + 2, cy - 8, rc.left + 5, cy + 8), 1.5f, g_theme.accent);
    g_rt->SetTransform(saved);
}

static void DrawBackground() {
    const Theme& t = g_theme;
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    float bgA = t.backdrop ? t.opacity : 1.0f;
    if (g_m.full) {
        // Launcher overlay: tinted full-screen layer + floating panel.
        g_cv.Fill(D2D1::RectF(0, 0, g_win.width, g_win.height), t.backdrop ? Alpha(t.scrim, 0.85f) : Alpha(t.bg, 0.97f));
        g_cv.FillRound(g_panel, t.radius * 2, Alpha(t.bg, std::min(1.0f, bgA + 0.1f)));
        g_cv.StrokeRound(g_panel, t.radius * 2, t.divider);
    } else {
        g_cv.Fill(D2D1::RectF(0, 0, g_win.width, g_win.height), Alpha(t.bg, bgA));
    }
    if (t.glow && g_glow && g_glow2) {
        g_glow->SetCenter(D2D1::Point2F(g_panel.left + 90, g_panel.top + 30));
        g_glow->SetRadiusX(std::max(320.0f, (g_panel.right - g_panel.left) * 0.55f));
        g_glow->SetRadiusY(260);
        g_rt->FillRectangle(g_panel, g_glow);
        g_glow2->SetCenter(D2D1::Point2F(g_panel.right - 60, g_panel.bottom - 40));
        g_glow2->SetRadiusX(360);
        g_glow2->SetRadiusY(240);
        g_rt->FillRectangle(g_panel, g_glow2);
    }
    // Subtle top highlight for a glassy edge
    g_cv.Fill(D2D1::RectF(g_panel.left + 12, g_panel.top, g_panel.right - 12, g_panel.top + 1), Alpha(D2D1::ColorF(1, 1, 1, 1), t.dark ? 0.07f : 0.5f));
}

static void Paint() {
    if (!EnsureTarget()) return;
    const Metrics& m = g_m;
    const Theme& t = g_theme;
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    DrawBackground();

    float p = EaseOut(OpenProgress());
    float raw = OpenProgress();
    ID2D1Layer* layer = nullptr;
    if (p < 1.0f) {
        g_rt->CreateLayer(nullptr, &layer);
        if (layer) g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                         D2D1::IdentityMatrix(), p), layer);
        float s = m.full ? 0.97f + 0.03f * p : 1.0f;
        D2D1_POINT_2F c = D2D1::Point2F((g_panel.left + g_panel.right) / 2, (g_panel.top + g_panel.bottom) / 2);
        g_rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * D2D1::Matrix3x2F::Translation(0, (1 - p) * 18));
    }

    // Greeting
    if (m.greetH > 0) {
        float gy = g_panel.top + m.pad - 4;
        D2D1_RECT_F gr = D2D1::RectF(g_panel.left + m.pad + 2, gy, g_panel.right - m.pad - 2, gy + m.greetH - 8);
        g_cv.Text(GreetingText(), g_fGreet, gr, t.text);
        if (!m.classic) g_cv.Text(DateText(), g_fDate, gr, t.subtext);
    }
    DrawSearch();

    const float top = ContentTop();
    if (Searching()) {
        std::wstring hdr = g_results.empty() ? (g_appsLoaded ? L"No matches" : L"Loading apps\x2026")
                                             : (L"Apps  \x00B7  " + std::to_wstring(g_results.size()));
        g_cv.Text(hdr, g_fHeader, D2D1::RectF(g_panel.left + m.pad + 4, top, g_panel.right - m.pad, top + m.headerH), t.text);
        g_rt->PushAxisAlignedClip(g_regionRc[RResults], D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const Item& it = g_items[i];
            if (it.kind != Kind::Result || it.rc.bottom < g_regionRc[RResults].top || it.rc.top > g_regionRc[RResults].bottom) continue;
            const AppEntry& a = g_apps[g_results[it.index]];
            DrawRow(i, it, a.id, a.name, a.id.find(L'!') != std::wstring::npos ? L"Store app" : L"App");
        }
        g_rt->PopAxisAlignedClip();
        DrawScrollbar(RResults);
    } else {
        if (!m.classic) {
            float x1 = g_panel.left + m.pad + m.cols * m.tileW;
            g_cv.Text(L"Pinned", g_fHeader, D2D1::RectF(g_panel.left + m.pad + 6, top, x1, top + m.headerH), t.text);
            g_cv.Text(L"All apps", g_fHeader, D2D1::RectF(x1 + m.colGap + 6, top, g_panel.right - m.pad, top + m.headerH), t.text);
            g_rt->PushAxisAlignedClip(g_regionRc[RPins], D2D1_ANTIALIAS_MODE_ALIASED);
            if (g_config.pins.empty())
                g_cv.Text(L"Right-click any app to pin it here.", g_fSmall,
                          D2D1::RectF(g_regionRc[RPins].left + 6, g_regionRc[RPins].top, g_regionRc[RPins].right, g_regionRc[RPins].top + 30), t.subtext);
            for (int i = 0; i < (int)g_items.size(); ++i) {
                const Item& it = g_items[i];
                if (it.kind != Kind::Pin) continue;
                // Staggered entrance: each tile arrives a little after the previous one.
                float appear = Animated() ? EaseOut((raw * g_openDur - it.index * 0.018f) / std::max(0.08f, g_openDur * 0.7f)) : 1.0f;
                DrawPinTile(i, it, appear);
            }
            g_rt->PopAxisAlignedClip();
            DrawScrollbar(RPins);
        }

        g_rt->PushAxisAlignedClip(g_regionRc[RList], D2D1_ANTIALIAS_MODE_ALIASED);
        if (!g_appsLoaded && g_rows.empty() && !m.classic)
            g_cv.Text(L"Loading apps\x2026", g_fSmall, D2D1::RectF(g_regionRc[RList].left + 12, g_regionRc[RList].top, g_regionRc[RList].right, g_regionRc[RList].top + 30), t.subtext);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const Item& it = g_items[i];
            if (it.region != RList || it.rc.bottom < g_regionRc[RList].top || it.rc.top > g_regionRc[RList].bottom) continue;
            if (it.kind == Kind::Section) {
                g_cv.Text(it.index == 0 ? L"Pinned" : L"All apps", g_fHeader, D2D1::RectF(it.rc.left + 10, it.rc.top, it.rc.right, it.rc.bottom), t.text);
                continue;
            }
            if (it.kind == Kind::Pin) {
                const Pin& pin = g_config.pins[it.index];
                DrawRow(i, it, pin.id, PinName(it.index), nullptr);
                continue;
            }
            const ListRow& row = g_rows[it.index];
            if (row.header) {
                float h = HoverOf(Kind::Letter, it.index);
                if (h > 0) g_cv.FillRound(Inset(it.rc, 1), t.radius, Alpha(t.hover, h));
                D2D1_RECT_F badge = D2D1::RectF(it.rc.left + 8, it.rc.top + 4, it.rc.left + 30, it.rc.bottom - 4);
                g_cv.FillRound(badge, 6, Alpha(t.accent, 0.14f + 0.1f * h));
                wchar_t s[2] = {row.letter, 0};
                IDWriteTextFormat* f = g_fAvatar;
                g_cv.Text(s, f, badge, t.accent);
                g_cv.Fill(D2D1::RectF(it.rc.left + 38, (it.rc.top + it.rc.bottom) / 2, it.rc.right - 10, (it.rc.top + it.rc.bottom) / 2 + 1), Alpha(t.divider, 0.8f));
                continue;
            }
            const AppEntry& a = g_apps[row.app];
            DrawRow(i, it, a.id, a.name, nullptr);
        }
        g_rt->PopAxisAlignedClip();
        DrawScrollbar(RList);
        if (g_jumpMode) DrawJumpOverlay();
    }

    // Bottom bar
    float by = g_panel.bottom - m.bottomH;
    D2D1_RECT_F bar = D2D1::RectF(g_panel.left, by, g_panel.right, g_panel.bottom);
    if (m.full) {
        g_rt->PushAxisAlignedClip(bar, D2D1_ANTIALIAS_MODE_ALIASED);
        g_cv.FillRound(D2D1::RectF(bar.left, bar.top - 40, bar.right, bar.bottom), t.radius * 2, Alpha(t.bottomBg, t.backdrop ? 0.55f : 1.0f));
        g_rt->PopAxisAlignedClip();
    } else {
        g_cv.Fill(bar, Alpha(t.bottomBg, t.backdrop ? std::min(1.0f, t.opacity * 0.6f) : 1.0f));
    }
    g_cv.Fill(D2D1::RectF(bar.left, by, bar.right, by + 1), t.divider);
    const Item* tipItem = nullptr;
    const wchar_t* tipText = nullptr;
    float tipA = 0;
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (it.region != RNone) continue;
        float h = HoverOf(it.kind, it.index);
        float cy = (it.rc.top + it.rc.bottom) / 2;
        if (it.kind == Kind::UpdatePill) {
            g_cv.FillRound(it.rc, (it.rc.bottom - it.rc.top) / 2, Alpha(t.accent, 0.85f + 0.15f * h));
            g_cv.Glyph(0xE895, g_fIconSm, D2D1::RectF(it.rc.left + 6, it.rc.top, it.rc.left + 28, it.rc.bottom), t.onAccent);
            g_cv.Text(updater::Get().state == updater::State::Ready ? L"Restart to update" : L"Update available", g_fPill,
                      D2D1::RectF(it.rc.left + 28, it.rc.top, it.rc.right - 6, it.rc.bottom), t.onAccent);
            continue;
        }
        DrawItemBg(it.rc, it.kind, it.index, i == g_sel, t.radius);
        D2D1_RECT_F icon = it.rc;
        icon.top -= 1.5f * h; icon.bottom -= 1.5f * h;
        switch (it.kind) {
            case Kind::User: {
                float cx = it.rc.left + 26;
                g_cv.Ring(cx, cy, 17, Alpha(t.accent, 0.35f + 0.4f * h), 1.5f);
                g_cv.Circle(cx, cy, 14.5f, t.accent);
                std::wstring initial = g_userName.empty() ? L"?" : g_userName.substr(0, 1);
                g_cv.Text(initial, g_fAvatar, D2D1::RectF(cx - 15, cy - 15, cx + 15, cy + 15), t.onAccent);
                g_cv.Text(g_userName, g_fRow, D2D1::RectF(it.rc.left + 50, it.rc.top, it.rc.right - 8, it.rc.bottom), t.text);
                break;
            }
            case Kind::BtnPower:     g_cv.Glyph(0xE7E8, g_fIcon, icon, t.text); tipText = h > tipA ? L"Power" : tipText; break;
            case Kind::BtnSettings:  g_cv.Glyph(0xE713, g_fIcon, icon, t.text); tipText = h > tipA ? L"Windows Settings" : tipText; break;
            case Kind::BtnExplorer:  g_cv.Glyph(0xEC50, g_fIcon, icon, t.text); tipText = h > tipA ? L"File Explorer" : tipText; break;
            case Kind::BtnDownloads: g_cv.Glyph(0xE896, g_fIcon, icon, t.text); tipText = h > tipA ? L"Downloads" : tipText; break;
            case Kind::BtnFoyer:     g_cv.Glyph(0xE771, g_fIcon, icon, t.accent); tipText = h > tipA ? L"Foyer settings" : tipText; break;
            default: break;
        }
        if (it.kind != Kind::User && h > tipA) { tipA = h; tipItem = &it; }
    }
    if (tipItem && tipText) DrawTooltip(*tipItem, tipText, tipA);

    if (layer) { g_rt->PopLayer(); layer->Release(); }
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { DiscardDeviceResources(); InvalidateRect(g_hwnd, nullptr, FALSE); }
}

static void Redraw() {
    Layout();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ============================================================ positioning
static void PlaceWindow(POINT anchor) {
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    if (dx != g_dpi) { g_dpi = dx; DiscardDeviceResources(); }

    const RECT& mr = mi.rcMonitor;
    const RECT& w = mi.rcWork;
    ComputeMetrics(w);
    int margin = Px(12);
    int width = Px(g_m.W), height = Px(g_m.H);
    int x, y;

    if (g_m.full) {
        x = w.left; y = w.top;
        width = w.right - w.left; height = w.bottom - w.top;
    } else if (g_config.position == L"center") {
        x = w.left + ((w.right - w.left) - width) / 2;
        y = w.top + ((w.bottom - w.top) - height) / 2;
    } else {
        enum { Bottom, Top, Left, Right } edge = Bottom;
        if (w.top > mr.top) edge = Top;
        else if (w.left > mr.left) edge = Left;
        else if (w.right < mr.right) edge = Right;
        DWORD al = 1, size = sizeof(al);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAl",
                         RRF_RT_REG_DWORD, nullptr, &al, &size) != ERROR_SUCCESS) al = 1;
        switch (edge) {
            case Top:   y = w.top + margin; x = (al == 0) ? w.left + margin : w.left + ((w.right - w.left) - width) / 2; break;
            case Left:  x = w.left + margin; y = w.top + margin; break;
            case Right: x = w.right - width - margin; y = w.top + margin; break;
            default:    y = w.bottom - height - margin; x = (al == 0) ? w.left + margin : w.left + ((w.right - w.left) - width) / 2; break;
        }
    }
    if (g_preview && !g_m.full) {
        // Live preview: sit beside the settings window instead of under it.
        if (HWND sw = FindWindowW(L"FoyerSettings", nullptr)) {
            RECT sr; GetWindowRect(sw, &sr);
            int gap = Px(16);
            if (sr.right + gap + width <= w.right) x = sr.right + gap;
            else if (sr.left - gap - width >= w.left) x = sr.left - gap - width;
            else if ((sr.right - sr.left) + gap + width <= (w.right - w.left)) {
                // Slide the settings window over to make room for both.
                int total = (sr.right - sr.left) + gap + width;
                int left = w.left + ((w.right - w.left) - total) / 2;
                SetWindowPos(sw, nullptr, left, sr.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                x = left + (sr.right - sr.left) + gap;
            }
        }
    }
    g_win = D2D1::SizeF(Dip(width), Dip(height));
    if (g_m.full) {
        float pw = g_m.W, ph = g_m.H;
        float px = (g_win.width - pw) / 2, py = (g_win.height - ph) / 2;
        g_panel = D2D1::RectF(px, py, px + pw, py + ph);
    } else {
        g_panel = D2D1::RectF(0, 0, g_win.width, g_win.height);
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    if (g_rt) {
        g_rt->SetDpi((float)g_dpi, (float)g_dpi);
        g_rt->Resize(D2D1::SizeU(width, height));
    }
}

static void ForceForeground(HWND hwnd) {
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return;
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, FALSE);
}

static void MaybeRefreshApps(bool force) {
    DWORD now = GetTickCount();
    if (!force && g_appsLoaded && now - g_lastRefresh < 30000) return;
    g_lastRefresh = now;
    std::vector<std::wstring> have;
    for (auto& kv : g_icons) have.push_back(kv.first);
    AppsRefreshAsync(g_hwnd, std::move(have), g_iconPx);
}

void MenuRefreshApps() { MaybeRefreshApps(true); }

bool MenuVisible() { return g_hwnd && IsWindowVisible(g_hwnd); }

static void ResetState() {
    g_query.clear();
    g_selectAll = false;
    g_jumpMode = false;
    g_hot = g_sel = -1;
    for (int r = 0; r < 4; ++r) g_scroll[r] = g_scrollTarget[r] = 0;
    g_results.clear();
    g_hoverA.clear();
    g_pressKey = ~0ull;
    g_pressA = 0;
}

void MenuHide() {
    if (!MenuVisible()) return;
    KillTimer(g_hwnd, TIMER_CARET);
    KillTimer(g_hwnd, TIMER_ANIM);
    g_animTimer = false;
    g_preview = false;
    ShowWindow(g_hwnd, SW_HIDE);
}

static void MenuShow(bool fromMouse, POINT pt, bool preview) {
    double t0 = Now();
    if (!fromMouse) GetCursorPos(&pt);
    else if (!preview) SendMaskKey();
    ResetState();
    g_preview = preview;
    g_openDur = g_config.animation == L"fast" ? 0.15f : g_config.animation == L"smooth" ? 0.28f : 0.0f;
    PlaceWindow(pt);
    Layout();
    g_caretOn = true;
    g_openStart = Now();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    Paint();
    ValidateRect(g_hwnd, nullptr);
    if (!preview) {
        ForceForeground(g_hwnd);
        if (GetForegroundWindow() != g_hwnd) Log(L"Warning: Windows did not give Foyer focus");
    }
    SetTimer(g_hwnd, TIMER_CARET, GetCaretBlinkTime() ? GetCaretBlinkTime() : 530, nullptr);
    if (Animated()) StartAnim();
    Log(L"Menu shown via %ls in %.1f ms", preview ? L"preview" : fromMouse ? L"Start button" : L"Win key", (Now() - t0) * 1000);
    MaybeRefreshApps(false);
}

void MenuToggle(bool fromMouse, POINT pt) {
    if (MenuVisible() && !g_preview) MenuHide();
    else { if (g_preview) MenuHide(); MenuShow(fromMouse, pt, false); }
}

void MenuSetPreview(bool on) {
    if (on) {
        POINT pt; GetCursorPos(&pt);
        if (HWND s = FindWindowW(L"FoyerSettings", nullptr)) {
            RECT r; GetWindowRect(s, &r);
            pt = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        }
        MenuShow(true, pt, true);
    } else if (g_preview) {
        MenuHide();
    }
}

void MenuReloadConfig() {
    RebuildRows();
    if (MenuVisible()) Redraw();
}

void MenuApplySettings() {
    g_theme = BuildTheme(g_config);
    ApplyWindowTheme(g_hwnd, g_theme, true);
    ApplyMenuTheme(g_theme.dark);
    DiscardDeviceResources();
    RebuildRows();
    if (MenuVisible()) {
        POINT pt; GetCursorPos(&pt);
        if (g_preview) {
            if (HWND s = FindWindowW(L"FoyerSettings", nullptr)) {
                RECT r; GetWindowRect(s, &r);
                pt = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            }
        } else {
            RECT r; GetWindowRect(g_hwnd, &r);
            pt = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        }
        PlaceWindow(pt);
        Redraw();
    }
}

// ============================================================ actions
enum Cmd : UINT {
    CMD_OPEN = 1, CMD_ADMIN, CMD_LOCATION, CMD_PIN, CMD_UNPIN, CMD_LEFT, CMD_RIGHT, CMD_HIDE, CMD_UNINSTALL,
    CMD_LOCK = 50, CMD_SIGNOUT, CMD_SWITCH, CMD_ACCOUNT,
    CMD_SLEEP = 60, CMD_HIBERNATE, CMD_SHUTDOWN, CMD_RESTART, CMD_UEFI,
    CMD_FOYER_SETTINGS = 70, CMD_REFRESH,
};

static std::wstring ItemAppId(const Item& it, std::wstring* name = nullptr, std::wstring* target = nullptr) {
    const AppEntry* a = nullptr;
    std::wstring id;
    if (it.kind == Kind::Pin) {
        id = g_config.pins[it.index].id;
        a = AppById(id);
        if (name) *name = PinName(it.index);
    } else if (it.kind == Kind::AppRow) {
        a = &g_apps[g_rows[it.index].app]; id = a->id;
    } else if (it.kind == Kind::Result) {
        a = &g_apps[g_results[it.index]]; id = a->id;
    }
    if (a) { if (name) *name = a->name; if (target) *target = a->target; }
    return id;
}

static void Launch(const Item& it, bool admin) {
    std::wstring target;
    std::wstring id = ItemAppId(it, nullptr, &target);
    if (id.empty()) return;
    if (admin) AppRunAsAdmin(g_hwnd, id, target);
    else AppLaunch(id);
    MenuHide();
}

static POINT ScreenPointFor(const Item& it, bool useMouse) {
    POINT pt;
    if (useMouse) { GetCursorPos(&pt); return pt; }
    pt.x = Px(it.rc.left + 16); pt.y = Px(it.rc.bottom);
    ClientToScreen(g_hwnd, &pt);
    return pt;
}

static void OpenFoyerSettings(int page) {
    MenuHide();
    SettingsOpen(page);
}

static void ShowAppMenu(int itemIdx, bool useMouse) {
    const Item it = g_items[itemIdx];
    std::wstring name, target;
    std::wstring id = ItemAppId(it, &name, &target);
    if (id.empty()) return;
    bool pinned = g_config.IsPinned(id);
    bool isPin = it.kind == Kind::Pin;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, CMD_ADMIN, L"Run as administrator\tCtrl+Shift+Enter");
    AppendMenuW(m, MF_STRING, CMD_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (pinned) AppendMenuW(m, MF_STRING, CMD_UNPIN, L"Unpin from Foyer");
    else AppendMenuW(m, MF_STRING, CMD_PIN, L"Pin to Foyer");
    if (isPin) {
        AppendMenuW(m, MF_STRING | (it.index == 0 ? MF_GRAYED : 0), CMD_LEFT, g_m.classic ? L"Move up" : L"Move left");
        AppendMenuW(m, MF_STRING | (it.index + 1 >= (int)g_config.pins.size() ? MF_GRAYED : 0), CMD_RIGHT, g_m.classic ? L"Move down" : L"Move right");
    } else {
        AppendMenuW(m, MF_STRING, CMD_HIDE, L"Hide from Foyer");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_UNINSTALL, L"Uninstall\x2026");

    POINT pt = ScreenPointFor(it, useMouse);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);

    switch (cmd) {
        case CMD_OPEN: Launch(it, false); return;
        case CMD_ADMIN: Launch(it, true); return;
        case CMD_LOCATION: AppOpenLocation(g_hwnd, id, target); MenuHide(); return;
        case CMD_PIN: g_config.pins.push_back({id, name}); break;
        case CMD_UNPIN:
            g_config.pins.erase(std::remove_if(g_config.pins.begin(), g_config.pins.end(),
                                               [&](const Pin& p) { return _wcsicmp(p.id.c_str(), id.c_str()) == 0; }), g_config.pins.end());
            break;
        case CMD_LEFT: std::swap(g_config.pins[it.index], g_config.pins[it.index - 1]); break;
        case CMD_RIGHT: std::swap(g_config.pins[it.index], g_config.pins[it.index + 1]); break;
        case CMD_HIDE: g_config.hidden.push_back(id); RebuildRows(); RebuildResults(); break;
        case CMD_UNINSTALL: AppUninstall(id); MenuHide(); return;
        default: return;
    }
    ConfigSave();
    g_sel = -1;
    Redraw();
}

static void ShowBackgroundMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_FOYER_SETTINGS, L"Foyer settings\tCtrl+,");
    AppendMenuW(m, MF_STRING, CMD_REFRESH, L"Refresh app list");
    POINT pt; GetCursorPos(&pt);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == CMD_FOYER_SETTINGS) OpenFoyerSettings(-1);
    else if (cmd == CMD_REFRESH) MaybeRefreshApps(true);
}

static void ShowAccountMenu(const Item& it) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_LOCK, L"Lock");
    AppendMenuW(m, MF_STRING, CMD_SIGNOUT, L"Sign out");
    AppendMenuW(m, MF_STRING, CMD_SWITCH, L"Switch user");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_ACCOUNT, L"Account settings");
    POINT pt{Px(it.rc.left), Px(it.rc.top)};
    ClientToScreen(g_hwnd, &pt);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_BOTTOMALIGN | TPM_NONOTIFY, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    MenuHide();
    switch (cmd) {
        case CMD_LOCK: AccountLock(); break;
        case CMD_SIGNOUT: AccountSignOut(); break;
        case CMD_SWITCH: AccountSwitchUser(); break;
        case CMD_ACCOUNT: ShellOpen(L"ms-settings:yourinfo"); break;
    }
}

static void ShowPowerMenu(const Item& it) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_SLEEP, L"Sleep");
    if (g_hibernateAvailable) AppendMenuW(m, MF_STRING, CMD_HIBERNATE, L"Hibernate");
    AppendMenuW(m, MF_STRING, CMD_SHUTDOWN, L"Shut down");
    AppendMenuW(m, MF_STRING, CMD_RESTART, L"Restart");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_UEFI, L"Restart to BIOS/UEFI\x2026");
    POINT pt{Px(it.rc.right), Px(it.rc.top)};
    ClientToScreen(g_hwnd, &pt);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_NONOTIFY, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    MenuHide();
    switch (cmd) {
        case CMD_SLEEP: PowerSleep(); break;
        case CMD_HIBERNATE: PowerHibernate(); break;
        case CMD_SHUTDOWN: PowerShutdown(); break;
        case CMD_RESTART: PowerRestart(); break;
        case CMD_UEFI: PowerRestartToFirmware(nullptr); break;
    }
}

static void Activate(int idx, bool admin) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    const Item it = g_items[idx];
    switch (it.kind) {
        case Kind::Pin: case Kind::AppRow: case Kind::Result: Launch(it, admin); break;
        case Kind::Letter: g_jumpMode = true; Redraw(); break;
        case Kind::User: ShowAccountMenu(it); break;
        case Kind::UpdatePill: OpenFoyerSettings(PageUpdates); break;
        case Kind::BtnFoyer: OpenFoyerSettings(-1); break;
        case Kind::BtnPower: ShowPowerMenu(it); break;
        case Kind::BtnSettings: ShellOpen(L"ms-settings:"); MenuHide(); break;
        case Kind::BtnExplorer: ShellOpen(L"explorer.exe"); MenuHide(); break;
        case Kind::BtnDownloads: ShellOpen(L"shell:Downloads"); MenuHide(); break;
        default: break;
    }
}

static void JumpTo(wchar_t letter) {
    g_jumpMode = false;
    for (const Item& it : g_items) {
        if (it.kind == Kind::Letter && g_rows[it.index].letter == letter) {
            float target = g_scroll[RList] + (it.rc.top - g_regionRc[RList].top);
            g_scrollTarget[RList] = std::clamp(target, 0.0f, MaxScroll(RList));
            if (!Animated()) g_scroll[RList] = g_scrollTarget[RList];
            break;
        }
    }
    Redraw();
    StartAnim();
}

// ============================================================ keyboard
static void OnQueryChanged() {
    g_jumpMode = false;
    g_scroll[RResults] = g_scrollTarget[RResults] = 0;
    RebuildResults();
    Layout();
    g_sel = Searching() && !g_results.empty() ? FindItem(Kind::Result, 0) : -1;
    g_caretOn = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void MoveLinear(int dir, Region region) {
    std::vector<int> order;
    for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].region == region && IsSelectable(g_items[i].kind)) order.push_back(i);
    if (order.empty()) return;
    auto pos = std::find(order.begin(), order.end(), g_sel);
    int p = pos == order.end() ? -1 : (int)(pos - order.begin());
    p = p < 0 ? (dir > 0 ? 0 : (int)order.size() - 1) : std::clamp(p + dir, 0, (int)order.size() - 1);
    g_sel = order[p];
}

static void MoveSelection(int dir, bool horizontal) {
    if (Searching()) {
        if (!horizontal) MoveLinear(dir, RResults);
    } else if (g_m.classic) {
        if (!horizontal) MoveLinear(dir, RList);
    } else {
        int pinCount = (int)g_config.pins.size();
        int cols = g_m.cols;
        auto firstAppRow = [&]() { for (int r = 0; r < (int)g_rows.size(); ++r) if (!g_rows[r].header) return r; return -1; };
        if (g_sel < 0 || (g_items[g_sel].kind != Kind::Pin && g_items[g_sel].kind != Kind::AppRow)) {
            g_sel = pinCount ? FindItem(Kind::Pin, 0) : FindItem(Kind::AppRow, firstAppRow());
        } else if (g_items[g_sel].kind == Kind::Pin) {
            int i = g_items[g_sel].index;
            if (horizontal) {
                int col = i % cols;
                if (dir > 0 && (col == cols - 1 || i + 1 >= pinCount)) {
                    int r = firstAppRow();
                    if (r >= 0) g_sel = FindItem(Kind::AppRow, r);
                } else {
                    g_sel = FindItem(Kind::Pin, std::clamp(i + dir, 0, pinCount - 1));
                }
            } else {
                int n = i + dir * cols;
                if (n >= 0 && n < pinCount) g_sel = FindItem(Kind::Pin, n);
                else if (dir > 0 && (i / cols) < (pinCount - 1) / cols) g_sel = FindItem(Kind::Pin, pinCount - 1);
            }
        } else {
            int r = g_items[g_sel].index;
            if (horizontal) {
                if (dir < 0 && pinCount) g_sel = FindItem(Kind::Pin, 0);
            } else {
                int n = r;
                do { n += dir; } while (n >= 0 && n < (int)g_rows.size() && g_rows[n].header);
                if (n >= 0 && n < (int)g_rows.size()) g_sel = FindItem(Kind::AppRow, n);
            }
        }
    }
    EnsureVisible(g_sel);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void PageList(int dir) {
    int region = Searching() ? RResults : RList;
    g_scrollTarget[region] = std::clamp(g_scrollTarget[region] + dir * (g_regionRc[region].bottom - g_regionRc[region].top) * 0.9f, 0.0f, MaxScroll(region));
    if (!Animated()) g_scroll[region] = g_scrollTarget[region];
    g_sel = -1;
    Redraw();
    StartAnim();
}

static void ToggleColumn() {
    if (Searching() || g_m.classic) { MoveSelection(+1, false); return; }
    bool inPins = g_sel >= 0 && g_items[g_sel].kind == Kind::Pin;
    if (inPins || g_config.pins.empty()) {
        for (int r = 0; r < (int)g_rows.size(); ++r) if (!g_rows[r].header) { g_sel = FindItem(Kind::AppRow, r); break; }
    } else {
        g_sel = FindItem(Kind::Pin, 0);
    }
    EnsureVisible(g_sel);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static bool OnKeyDown(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
        case VK_ESCAPE:
            if (g_jumpMode) { g_jumpMode = false; Redraw(); }
            else if (!g_query.empty()) { g_query.clear(); g_selectAll = false; OnQueryChanged(); }
            else MenuHide();
            return true;
        case VK_RETURN: {
            int idx = g_sel;
            if (idx < 0 && Searching() && !g_results.empty()) idx = FindItem(Kind::Result, 0);
            if (idx >= 0) Activate(idx, ctrl && shift);
            return true;
        }
        case VK_UP: MoveSelection(-1, false); return true;
        case VK_DOWN: MoveSelection(+1, false); return true;
        case VK_LEFT: MoveSelection(-1, true); return true;
        case VK_RIGHT: MoveSelection(+1, true); return true;
        case VK_TAB: ToggleColumn(); return true;
        case VK_PRIOR: PageList(-1); return true;
        case VK_NEXT: PageList(+1); return true;
        case VK_APPS:
            if (g_sel >= 0) ShowAppMenu(g_sel, false);
            return true;
        case VK_BACK:
            if (g_selectAll) g_query.clear();
            else if (ctrl) {
                while (!g_query.empty() && g_query.back() == L' ') g_query.pop_back();
                while (!g_query.empty() && g_query.back() != L' ') g_query.pop_back();
            } else if (!g_query.empty()) g_query.pop_back();
            g_selectAll = false;
            OnQueryChanged();
            return true;
        case 'A':
            if (ctrl) { g_selectAll = !g_query.empty(); InvalidateRect(g_hwnd, nullptr, FALSE); return true; }
            break;
        case 'V':
            if (ctrl && OpenClipboard(g_hwnd)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = (const wchar_t*)GlobalLock(h)) {
                        std::wstring s = p;
                        GlobalUnlock(h);
                        size_t nl = s.find_first_of(L"\r\n");
                        if (nl != std::wstring::npos) s.resize(nl);
                        if (g_selectAll) g_query.clear();
                        g_selectAll = false;
                        g_query += s.substr(0, 200);
                    }
                }
                CloseClipboard();
                OnQueryChanged();
                return true;
            }
            break;
        case VK_OEM_COMMA:
            if (ctrl) { OpenFoyerSettings(-1); return true; }
            break;
    }
    return false;
}

static void OnChar(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return;
    if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) return;
    if (g_selectAll) { g_query.clear(); g_selectAll = false; }
    if (g_query.size() < 200) g_query += ch;
    OnQueryChanged();
}

// ============================================================ mouse
static void OnMouseMove(float x, float y) {
    if (!g_tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_hwnd, 0};
        TrackMouseEvent(&tme);
        g_tracking = true;
    }
    int h = HitTest(x, y);
    if (g_jumpMode && h >= 0 && g_items[h].region != RNone) h = -1;
    if (h >= 0 && (g_items[h].kind == Kind::Section)) h = -1;
    if (h != g_hot) {
        g_hot = h;
        if (Animated()) StartAnim();
        else { AnimTick(); InvalidateRect(g_hwnd, nullptr, FALSE); }
    }
}

static void OnWheel(float x, float y, int delta) {
    int region = -1;
    for (int r = 1; r < 4; ++r) if (InRect(g_regionRc[r], x, y)) region = r;
    if (region < 0) region = Searching() ? RResults : RList;
    g_scrollTarget[region] = std::clamp(g_scrollTarget[region] - delta / 120.0f * g_m.rowH * 3, 0.0f, MaxScroll(region));
    if (!Animated()) {
        g_scroll[region] = g_scrollTarget[region];
        Layout();
        g_hot = HitTest(x, y);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    } else {
        StartAnim();
    }
}

// ============================================================ window proc
static LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            return 0;
        case WM_DPICHANGED:
            g_dpi = HIWORD(wParam);
            if (g_rt) g_rt->SetDpi((float)g_dpi, (float)g_dpi);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !g_preview) MenuHide();
            return 0;
        case WM_CLOSE: MenuHide(); return 0;
        case WM_TIMER:
            if (wParam == TIMER_CARET) {
                if (!g_preview) {
                    HWND fg = GetForegroundWindow();
                    DWORD pid = 0;
                    if (fg) GetWindowThreadProcessId(fg, &pid);
                    if (fg && pid != GetCurrentProcessId()) { MenuHide(); return 0; }
                }
                g_caretOn = !g_caretOn;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wParam == TIMER_ANIM) {
                if (!AnimTick()) { KillTimer(hwnd, TIMER_ANIM); g_animTimer = false; }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wParam == TIMER_UPDATE_CHECK) {
                AutoUpdateOnTimer();
            } else if (wParam == TIMER_HEALTHY) {
                KillTimer(hwnd, TIMER_HEALTHY);
                AutoUpdateOnStarted();
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (OnKeyDown(wParam)) return 0;
            break;
        case WM_CHAR: OnChar((wchar_t)wParam); return 0;
        case WM_MOUSEMOVE: OnMouseMove(Dip((short)LOWORD(lParam)), Dip((short)HIWORD(lParam))); return 0;
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_hot != -1) { g_hot = -1; StartAnim(); }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{(short)LOWORD(lParam), (short)HIWORD(lParam)};
            ScreenToClient(hwnd, &pt);
            OnWheel(Dip(pt.x), Dip(pt.y), GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int h = HitTest(Dip((short)LOWORD(lParam)), Dip((short)HIWORD(lParam)));
            if (h >= 0 && !g_jumpMode) { g_pressKey = Key(g_items[h].kind, g_items[h].index); StartAnim(); }
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            uint64_t pressed = g_pressKey;
            g_pressKey = ~0ull;
            if (g_m.full && !InRect(g_panel, x, y)) { MenuHide(); return 0; }   // click on the scrim
            if (g_jumpMode) {
                int h = HitTest(x, y);
                if (h >= 0 && g_items[h].region == RNone) { g_jumpMode = false; Activate(h, false); return 0; }
                int letter = JumpHit(x, y);
                if (letter > 0) JumpTo((wchar_t)letter);
                else { g_jumpMode = false; Redraw(); }
                return 0;
            }
            int h = HitTest(x, y);
            if (h >= 0 && Key(g_items[h].kind, g_items[h].index) == pressed)
                Activate(h, (GetKeyState(VK_CONTROL) < 0) && (GetKeyState(VK_SHIFT) < 0));
            else StartAnim();
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            int h = g_jumpMode ? -1 : HitTest(x, y);
            if (h >= 0 && IsSelectable(g_items[h].kind)) ShowAppMenu(h, true);
            else if (InRect(g_panel, x, y)) ShowBackgroundMenu();
            return 0;
        }
        case WM_FOYER_TOGGLE: {
            POINT pt{(short)LOWORD(lParam), (short)HIWORD(lParam)};
            MenuToggle(wParam == 1, pt);
            return 0;
        }
        case WM_FOYER_ORIGINAL: {
            MenuHide();
            HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { OpenOriginalStartMenu(); return 0; }, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            return 0;
        }
        case WM_FOYER_SETTINGS: SettingsOpen((int)wParam); return 0;
        case WM_FOYER_UPDATE: AutoUpdateOnStatus(); return 0;
        case WM_FOYER_REFRESH_START: StartButtonRefreshAsync(nullptr); return 0;
        case WM_FOYER_START_FOUND: return 0;
        case WM_FOYER_APPS_READY: {
            auto* res = reinterpret_cast<AppListResult*>(lParam);
            g_apps = std::move(res->apps);
            delete res;
            g_appById.clear();
            for (int i = 0; i < (int)g_apps.size(); ++i) g_appById[ToLower(g_apps[i].id)] = i;
            bool changed = false;
            for (auto& p : g_config.pins) if (const AppEntry* a = AppById(p.id)) if (a->name != p.name) { p.name = a->name; changed = true; }
            if (changed) ConfigSave();
            g_appsLoaded = true;
            RebuildRows();
            RebuildResults();
            g_sel = -1;
            if (MenuVisible()) Redraw();
            return 0;
        }
        case WM_FOYER_ICON_READY: {
            auto* ir = reinterpret_cast<IconResult*>(lParam);
            IconData& d = g_icons[ir->id];
            SafeRelease(d.bmp);
            d.size = ir->size;
            d.pixels = std::move(ir->pixels);
            delete ir;
            if (MenuVisible()) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_FOYER_TRAY: TrayOnMessage(hwnd, lParam); return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) >= TRAY_CMD_FIRST) { TrayOnCommand(hwnd, LOWORD(wParam)); return 0; }
            break;
        case WM_FOYER_QUIT: DestroyWindow(hwnd); return 0;
        case WM_DISPLAYCHANGE:
            StartButtonRefreshAsync(nullptr);
            break;
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp((const wchar_t*)lParam, L"ImmersiveColorSet") == 0) MenuApplySettings();  // light/dark switch
            StartButtonRefreshAsync(nullptr);
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            MenuApplySettings();
            return 0;
        case WM_DESTROY:
            TrayRemove();
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == taskbarCreated && taskbarCreated) {
                TrayAdd(hwnd);
                StartButtonRefreshAsync(nullptr);
                return 0;
            }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND MenuCreate(HINSTANCE inst) {
    QueryPerformanceFrequency(&g_qpf);
    if (!gfx::Init()) { Log(L"Failed to create Direct2D/DirectWrite factories"); return nullptr; }
    CreateFormats();
    g_userName = CurrentUserDisplayName();
    size_t sp = g_userName.find(L' ');
    g_firstName = sp == std::wstring::npos ? g_userName : g_userName.substr(0, sp);
    SYSTEM_POWER_CAPABILITIES caps{};
    g_hibernateAvailable = GetPwrCapabilities(&caps) && caps.SystemS4 && caps.HiberFilePresent;
    g_iconPx = std::clamp(MulDiv(40, GetDpiForSystem(), 96), 32, 96);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MenuProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"FoyerMenu";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"FoyerMenu", L"Foyer", WS_POPUP,
                             0, 0, 760, 660, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { Log(L"CreateWindowEx failed (%lu)", GetLastError()); return nullptr; }
    g_dpi = GetDpiForWindow(g_hwnd);

    g_theme = BuildTheme(g_config);
    ApplyWindowTheme(g_hwnd, g_theme, true);
    ApplyMenuTheme(g_theme.dark);
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    ComputeMetrics(work);
    EnsureTarget();   // pre-create so the first open is instant
    MaybeRefreshApps(true);
    return g_hwnd;
}
