// Foyer - the menu window (Direct2D + DirectWrite, custom drawn)
//
// The window is created hidden at startup and stays alive, with its render
// target, fonts and icon bitmaps ready. Opening the menu is just "position,
// show, paint" - no allocation, no disk access, no enumeration - which is how
// it stays well under the 100 ms target even right after boot.
#include "common.h"
#include "config.h"
#include "apps.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <powrprof.h>
#include <map>
#include <algorithm>
#include <cmath>

// ============================================================ theme
struct Theme {
    D2D1_COLOR_F bg, bottomBg, surface, hover, divider, text, subtext, accent, onAccent;
    float radius = 8.0f;
};

static D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

static D2D1_COLOR_F WindowsAccent() {
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &abgr, &size) != ERROR_SUCCESS)
        return Rgb(0x4CC2FF);
    float r = (abgr & 0xFF) / 255.f, g = ((abgr >> 8) & 0xFF) / 255.f, b = ((abgr >> 16) & 0xFF) / 255.f;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum < 0.45f) {   // lighten dark accents so they read on a dark background
        float t = (0.45f - lum) / 0.45f * 0.6f;
        r += (1 - r) * t; g += (1 - g) * t; b += (1 - b) * t;
    }
    return D2D1::ColorF(r, g, b, 1.0f);
}

static Theme DarkTheme() {
    Theme t;
    t.bg       = Rgb(0x202024);
    t.bottomBg = Rgb(0x1A1A1E);
    t.surface  = Rgb(0x2C2C31);
    t.hover    = Rgb(0xFFFFFF, 0.07f);
    t.divider  = Rgb(0xFFFFFF, 0.08f);
    t.text     = Rgb(0xF3F3F5);
    t.subtext  = Rgb(0xA4A4AE);
    t.accent   = WindowsAccent();
    t.onAccent = Rgb(0x101014);
    t.radius   = 8.0f;
    return t;
}

// ============================================================ layout constants (DIPs)
namespace L {
constexpr float W = 760, H = 640;
constexpr float Pad = 20, SearchH = 42, Gap = 14, HeaderH = 32;
constexpr int   Cols = 4;
constexpr float TileW = 88, TileH = 86;
constexpr float RowH = 38, LetterH = 30, ResultH = 48;
constexpr float BottomH = 62, ColGap = 24;
constexpr float Margin = 12;   // gap between menu and taskbar / screen edge
}

// ============================================================ state
enum class Kind { Pin, AppRow, Letter, Result, User, BtnDownloads, BtnExplorer, BtnSettings, BtnPower };
enum Region { RNone = 0, RPins, RList, RResults };

struct Item {
    Kind kind;
    int index;           // pin index / app index / letter char / result index
    D2D1_RECT_F rc;      // in window DIPs (scroll applied)
    Region region;
};

struct ListRow { bool header; wchar_t letter; int app; };

struct IconData {
    int size = 0;
    std::vector<uint8_t> pixels;
    ID2D1Bitmap* bmp = nullptr;
};

static HWND g_hwnd;
static Theme g_theme;
static UINT g_dpi = 96;
static float g_w = L::W, g_h = L::H;   // current window size in DIPs

static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static IDWriteTextFormat *g_fSearch, *g_fHeader, *g_fRow, *g_fTile, *g_fSmall, *g_fIcon, *g_fLetter, *g_fAvatar, *g_fJump;
static std::wstring g_iconFont = L"Segoe Fluent Icons";

static std::vector<AppEntry> g_apps;
static std::map<std::wstring, int> g_appById;     // lower-case id -> index
static std::map<std::wstring, IconData> g_icons;  // id -> icon
static std::vector<ListRow> g_rows;
static std::vector<int> g_results;
static DWORD g_lastRefresh = 0;
static bool g_appsLoaded = false;
static int g_iconPx = 48;

static std::wstring g_query;
static bool g_selectAll = false;
static bool g_caretOn = true;
static bool g_jumpMode = false;
static std::vector<Item> g_items;
static int g_hot = -1;           // item under the mouse
static int g_sel = -1;           // keyboard selection
static bool g_tracking = false;
static float g_scroll[4] = {0, 0, 0, 0};
static float g_contentH[4] = {0, 0, 0, 0};
static D2D1_RECT_F g_regionRc[4];
static std::wstring g_userName;
static bool g_hibernateAvailable = false;

constexpr UINT_PTR TIMER_CARET = 1;

// ============================================================ helpers
static float Dip(int px) { return px * 96.0f / g_dpi; }
static int Px(float dip) { return (int)std::lround(dip * g_dpi / 96.0f); }
static bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static bool Searching() { return !g_query.empty(); }

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static const AppEntry* AppById(const std::wstring& id) {
    auto it = g_appById.find(ToLower(id));
    return it == g_appById.end() ? nullptr : &g_apps[it->second];
}

static std::wstring PinName(int i) {
    const Pin& p = g_config.pins[i];
    if (const AppEntry* a = AppById(p.id)) return a->name;
    return p.name.empty() ? p.id : p.name;
}

// ============================================================ fonts
static void ReleaseFormats() {
    SafeRelease(g_fSearch); SafeRelease(g_fHeader); SafeRelease(g_fRow); SafeRelease(g_fTile);
    SafeRelease(g_fSmall); SafeRelease(g_fIcon); SafeRelease(g_fLetter); SafeRelease(g_fAvatar); SafeRelease(g_fJump);
}

static IDWriteTextFormat* MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                     DWRITE_TEXT_ALIGNMENT align, bool ellipsis) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        IDWriteInlineObject* sign = nullptr;
        g_dw->CreateEllipsisTrimmingSign(f, &sign);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, sign);
        SafeRelease(sign);
    }
    return f;
}

static void CreateFormats() {
    ReleaseFormats();
    // Prefer the Windows 11 icon font, fall back to the Windows 10 one.
    IDWriteFontCollection* sys = nullptr;
    if (SUCCEEDED(g_dw->GetSystemFontCollection(&sys, FALSE)) && sys) {
        UINT32 idx; BOOL exists = FALSE;
        sys->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists);
        g_iconFont = exists ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        sys->Release();
    }
    const wchar_t* ui = L"Segoe UI";
    g_fSearch = MakeFormat(ui, 15.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fHeader = MakeFormat(ui, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fRow    = MakeFormat(ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fTile   = MakeFormat(ui, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fSmall  = MakeFormat(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fIcon   = MakeFormat(g_iconFont.c_str(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fLetter = MakeFormat(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fAvatar = MakeFormat(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fJump   = MakeFormat(ui, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
}

// ============================================================ render target
static void DiscardDeviceResources() {
    for (auto& kv : g_icons) SafeRelease(kv.second.bmp);
    SafeRelease(g_brush);
    SafeRelease(g_rt);
}

static bool EnsureTarget() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left), std::max<LONG>(1, rc.bottom - rc.top));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = (float)g_dpi;
    HRESULT hr = g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY), &g_rt);
    if (FAILED(hr)) { Log(L"CreateHwndRenderTarget failed 0x%08lx", hr); return false; }
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(g_theme.text, &g_brush);
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
    // word start
    for (size_t i = 1; i < n.size(); ++i) {
        if ((n[i - 1] == L' ' || n[i - 1] == L'-' || n[i - 1] == L'.' || n[i - 1] == L'(') && n.compare(i, q.size(), q) == 0)
            return 800 - (int)std::min<size_t>(n.size(), 200);
    }
    // initials ("vsc" -> Visual Studio Code)
    std::wstring initials;
    bool start = true;
    for (wchar_t c : n) { if (start && c != L' ') initials += c; start = (c == L' ' || c == L'-'); }
    if (q.size() >= 2 && initials.compare(0, q.size(), q) == 0) return 600;
    if (n.find(q) != std::wstring::npos) return 400 - (int)std::min<size_t>(n.size(), 200);
    return -1;
}

static void RebuildResults() {
    g_results.clear();
    if (g_query.empty()) return;
    std::wstring q = ToLower(g_query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
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
static void Layout() {
    g_items.clear();
    const float contentTop = L::Pad + L::SearchH + L::Gap;
    const float contentBottom = g_h - L::BottomH - 8;
    for (auto& r : g_regionRc) r = D2D1::RectF(0, 0, 0, 0);

    if (Searching()) {
        D2D1_RECT_F reg = D2D1::RectF(L::Pad, contentTop + L::HeaderH, g_w - L::Pad, contentBottom);
        g_regionRc[RResults] = reg;
        float y = reg.top - g_scroll[RResults];
        for (int k = 0; k < (int)g_results.size(); ++k) {
            g_items.push_back({Kind::Result, k, D2D1::RectF(reg.left, y, reg.right, y + L::ResultH), RResults});
            y += L::ResultH;
        }
        g_contentH[RResults] = g_results.size() * L::ResultH;
    } else {
        // Pinned grid (left column)
        float x0 = L::Pad, x1 = L::Pad + L::Cols * L::TileW;
        D2D1_RECT_F pins = D2D1::RectF(x0, contentTop + L::HeaderH, x1, contentBottom);
        g_regionRc[RPins] = pins;
        for (int i = 0; i < (int)g_config.pins.size(); ++i) {
            float x = x0 + (i % L::Cols) * L::TileW;
            float y = pins.top + (i / L::Cols) * L::TileH - g_scroll[RPins];
            g_items.push_back({Kind::Pin, i, D2D1::RectF(x, y, x + L::TileW, y + L::TileH), RPins});
        }
        g_contentH[RPins] = ((g_config.pins.size() + L::Cols - 1) / L::Cols) * L::TileH;

        // All apps (right column)
        float lx0 = x1 + L::ColGap, lx1 = g_w - L::Pad;
        D2D1_RECT_F list = D2D1::RectF(lx0, contentTop + L::HeaderH, lx1, contentBottom);
        g_regionRc[RList] = list;
        float y = list.top - g_scroll[RList];
        for (int r = 0; r < (int)g_rows.size(); ++r) {
            float h = g_rows[r].header ? L::LetterH : L::RowH;
            if (g_rows[r].header) g_items.push_back({Kind::Letter, r, D2D1::RectF(lx0, y, lx1, y + h), RList});
            else g_items.push_back({Kind::AppRow, r, D2D1::RectF(lx0, y, lx1, y + h), RList});
            y += h;
        }
        g_contentH[RList] = y + g_scroll[RList] - list.top;
    }

    // Bottom bar
    float by = g_h - L::BottomH;
    g_items.push_back({Kind::User, 0, D2D1::RectF(L::Pad - 8, by + 11, L::Pad - 8 + 240, g_h - 11), RNone});
    float bx = g_w - L::Pad + 4;
    const Kind btns[] = {Kind::BtnPower, Kind::BtnSettings, Kind::BtnExplorer, Kind::BtnDownloads};
    for (Kind k : btns) {
        bx -= 40;
        g_items.push_back({k, 0, D2D1::RectF(bx, by + 11, bx + 40, g_h - 11), RNone});
        bx -= 4;
    }
}


static void ClampScroll(int r) {
    float viewH = g_regionRc[r].bottom - g_regionRc[r].top;
    float maxS = std::max(0.0f, g_contentH[r] - viewH);
    g_scroll[r] = std::clamp(g_scroll[r], 0.0f, maxS);
}

static void EnsureVisible(int itemIdx) {
    if (itemIdx < 0 || itemIdx >= (int)g_items.size()) return;
    const Item& it = g_items[itemIdx];
    if (it.region == RNone) return;
    const D2D1_RECT_F& reg = g_regionRc[it.region];
    float d = 0;
    if (it.rc.top < reg.top) d = it.rc.top - reg.top;
    else if (it.rc.bottom > reg.bottom) d = it.rc.bottom - reg.bottom;
    if (d != 0) {
        g_scroll[it.region] += d;
        ClampScroll(it.region);
        Kind k = it.kind; int idx = it.index;
        Layout();
        for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].kind == k && g_items[i].index == idx) { g_sel = i; break; }
    }
}

static int FindItem(Kind k, int index) {
    for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].kind == k && g_items[i].index == index) return i;
    return -1;
}

static int HitTest(float x, float y) {
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (it.region != RNone && !InRect(g_regionRc[it.region], x, y)) continue;
        if (InRect(it.rc, x, y)) return i;
    }
    return -1;
}

// ============================================================ drawing
static void FillRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    g_brush->SetColor(c);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_brush);
}

static void StrokeRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float w = 1.0f) {
    g_brush->SetColor(c);
    D2D1_RECT_F rr = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(rr, radius, radius), g_brush, w);
}

static void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    if (!f || s.empty()) return;
    g_brush->SetColor(c);
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void Glyph(wchar_t g, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    wchar_t s[2] = {g, 0};
    Text(s, g_fIcon, r, c);
}

static void DrawIcon(const std::wstring& id, const std::wstring& name, float cx, float cy, float size) {
    D2D1_RECT_F r = D2D1::RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2);
    if (ID2D1Bitmap* bmp = IconBitmap(id)) {
        g_rt->DrawBitmap(bmp, r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    // Placeholder while icons load (or for apps without one)
    FillRound(r, size * 0.22f, g_theme.surface);
    std::wstring initial = name.empty() ? L"?" : name.substr(0, 1);
    Text(initial, g_fAvatar, r, g_theme.subtext);
}

static void DrawItemBackground(int i, const Item& it, float radius) {
    if (i == g_sel) {
        FillRound(it.rc, radius, g_theme.hover);
        StrokeRound(it.rc, radius, g_theme.accent, 1.5f);
    } else if (i == g_hot) {
        FillRound(it.rc, radius, g_theme.hover);
    }
}

static void DrawScrollbar(int r) {
    const D2D1_RECT_F& reg = g_regionRc[r];
    float viewH = reg.bottom - reg.top;
    if (g_contentH[r] <= viewH + 1) return;
    float thumbH = std::max(24.0f, viewH * viewH / g_contentH[r]);
    float t = g_scroll[r] / (g_contentH[r] - viewH);
    float y = reg.top + t * (viewH - thumbH);
    D2D1_RECT_F bar = D2D1::RectF(reg.right - 3, y, reg.right, y + thumbH);
    FillRound(bar, 1.5f, D2D1::ColorF(g_theme.subtext.r, g_theme.subtext.g, g_theme.subtext.b, 0.45f));
}

static void DrawSearch() {
    D2D1_RECT_F box = D2D1::RectF(L::Pad, L::Pad, g_w - L::Pad, L::Pad + L::SearchH);
    FillRound(box, L::SearchH / 2, g_theme.surface);
    StrokeRound(box, L::SearchH / 2, Searching() ? g_theme.accent : g_theme.divider, Searching() ? 1.5f : 1.0f);
    Glyph(0xE721, D2D1::RectF(box.left + 10, box.top, box.left + 40, box.bottom), g_theme.subtext);

    D2D1_RECT_F tr = D2D1::RectF(box.left + 44, box.top, box.right - 16, box.bottom);
    if (g_query.empty()) {
        Text(L"Type to search apps", g_fSearch, tr, g_theme.subtext);
        if (g_caretOn) {
            g_brush->SetColor(g_theme.text);
            g_rt->FillRectangle(D2D1::RectF(tr.left, box.top + 12, tr.left + 1.5f, box.bottom - 12), g_brush);
        }
        return;
    }
    // Measure text so we can place the caret / selection highlight.
    IDWriteTextLayout* lay = nullptr;
    g_dw->CreateTextLayout(g_query.c_str(), (UINT32)g_query.size(), g_fSearch, 4000, tr.bottom - tr.top, &lay);
    float width = 0;
    if (lay) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); width = m.widthIncludingTrailingWhitespace; }
    float maxW = tr.right - tr.left;
    float offset = std::max(0.0f, width - maxW + 4);   // keep the end of long queries visible
    if (g_selectAll) FillRound(D2D1::RectF(tr.left - 2, box.top + 10, tr.left + std::min(width, maxW) + 2, box.bottom - 10), 3, D2D1::ColorF(g_theme.accent.r, g_theme.accent.g, g_theme.accent.b, 0.35f));
    if (lay) {
        g_rt->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_ALIASED);
        g_brush->SetColor(g_theme.text);
        g_rt->DrawTextLayout(D2D1::Point2F(tr.left - offset, tr.top), lay, g_brush);
        g_rt->PopAxisAlignedClip();
        lay->Release();
    }
    if (g_caretOn && !g_selectAll) {
        float cx = tr.left + width - offset + 1;
        g_brush->SetColor(g_theme.text);
        g_rt->FillRectangle(D2D1::RectF(cx, box.top + 12, cx + 1.5f, box.bottom - 12), g_brush);
    }
}

static void DrawJumpOverlay() {
    const D2D1_RECT_F& reg = g_regionRc[RList];
    D2D1_RECT_F all = D2D1::RectF(reg.left, reg.top - L::HeaderH, reg.right, reg.bottom);
    FillRound(all, g_theme.radius, g_theme.bg);
    const wchar_t* letters = L"#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    float cell = (reg.right - reg.left) / 6.0f;
    for (int i = 0; letters[i]; ++i) {
        wchar_t c = letters[i];
        bool present = std::any_of(g_rows.begin(), g_rows.end(), [&](const ListRow& r) { return r.header && r.letter == c; });
        float x = reg.left + (i % 6) * cell, y = all.top + 8 + (i / 6) * cell;
        D2D1_RECT_F r = D2D1::RectF(x + 3, y + 3, x + cell - 3, y + cell - 3);
        if (present) FillRound(r, g_theme.radius, g_theme.surface);
        wchar_t s[2] = {c, 0};
        Text(s, g_fJump, r, present ? g_theme.text : D2D1::ColorF(g_theme.subtext.r, g_theme.subtext.g, g_theme.subtext.b, 0.35f));
    }
}

static int JumpHit(float x, float y) {
    const D2D1_RECT_F& reg = g_regionRc[RList];
    float top = reg.top - L::HeaderH + 8;
    float cell = (reg.right - reg.left) / 6.0f;
    if (x < reg.left || x >= reg.right || y < top) return -1;
    int col = (int)((x - reg.left) / cell), row = (int)((y - top) / cell);
    int i = row * 6 + col;
    const wchar_t* letters = L"#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    return (i >= 0 && i < 27) ? letters[i] : -1;
}

static void Paint() {
    if (!EnsureTarget()) return;
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(g_theme.bg);

    DrawSearch();

    const float contentTop = L::Pad + L::SearchH + L::Gap;
    if (Searching()) {
        std::wstring hdr = g_results.empty() ? (g_appsLoaded ? L"No matches" : L"Loading apps\x2026") : L"Apps";
        Text(hdr, g_fHeader, D2D1::RectF(L::Pad + 4, contentTop, g_w - L::Pad, contentTop + L::HeaderH), g_theme.text);
        g_rt->PushAxisAlignedClip(g_regionRc[RResults], D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const Item& it = g_items[i];
            if (it.kind != Kind::Result || it.rc.bottom < g_regionRc[RResults].top || it.rc.top > g_regionRc[RResults].bottom) continue;
            const AppEntry& a = g_apps[g_results[it.index]];
            DrawItemBackground(i, it, g_theme.radius);
            DrawIcon(a.id, a.name, it.rc.left + 26, (it.rc.top + it.rc.bottom) / 2, 30);
            Text(a.name, g_fRow, D2D1::RectF(it.rc.left + 52, it.rc.top + 4, it.rc.right - 12, it.rc.top + 26), g_theme.text);
            Text(a.id.find(L'!') != std::wstring::npos ? L"Store app" : L"App", g_fSmall,
                 D2D1::RectF(it.rc.left + 52, it.rc.top + 24, it.rc.right - 12, it.rc.bottom - 4), g_theme.subtext);
        }
        g_rt->PopAxisAlignedClip();
        DrawScrollbar(RResults);
    } else {
        float x1 = L::Pad + L::Cols * L::TileW;
        Text(L"Pinned", g_fHeader, D2D1::RectF(L::Pad + 4, contentTop, x1, contentTop + L::HeaderH), g_theme.text);
        Text(L"All apps", g_fHeader, D2D1::RectF(x1 + L::ColGap + 4, contentTop, g_w - L::Pad, contentTop + L::HeaderH), g_theme.text);

        g_rt->PushAxisAlignedClip(g_regionRc[RPins], D2D1_ANTIALIAS_MODE_ALIASED);
        if (g_config.pins.empty()) {
            Text(L"Right-click any app to pin it here.", g_fSmall,
                 D2D1::RectF(g_regionRc[RPins].left + 4, g_regionRc[RPins].top, g_regionRc[RPins].right, g_regionRc[RPins].top + 30), g_theme.subtext);
        }
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const Item& it = g_items[i];
            if (it.kind != Kind::Pin) continue;
            const Pin& p = g_config.pins[it.index];
            std::wstring name = PinName(it.index);
            D2D1_RECT_F tile = D2D1::RectF(it.rc.left + 3, it.rc.top + 3, it.rc.right - 3, it.rc.bottom - 3);
            DrawItemBackground(i, Item{it.kind, it.index, tile, it.region}, g_theme.radius);
            DrawIcon(p.id, name, (it.rc.left + it.rc.right) / 2, it.rc.top + 30, 32);
            Text(name, g_fTile, D2D1::RectF(it.rc.left + 6, it.rc.top + 52, it.rc.right - 6, it.rc.top + 74), g_theme.text);
        }
        g_rt->PopAxisAlignedClip();
        DrawScrollbar(RPins);

        g_rt->PushAxisAlignedClip(g_regionRc[RList], D2D1_ANTIALIAS_MODE_ALIASED);
        if (!g_appsLoaded) Text(L"Loading apps\x2026", g_fSmall, D2D1::RectF(g_regionRc[RList].left + 12, g_regionRc[RList].top, g_regionRc[RList].right, g_regionRc[RList].top + 30), g_theme.subtext);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const Item& it = g_items[i];
            if (it.region != RList || it.rc.bottom < g_regionRc[RList].top || it.rc.top > g_regionRc[RList].bottom) continue;
            const ListRow& row = g_rows[it.index];
            if (row.header) {
                if (i == g_hot) FillRound(it.rc, g_theme.radius, g_theme.hover);
                wchar_t s[2] = {row.letter, 0};
                Text(s, g_fLetter, D2D1::RectF(it.rc.left + 12, it.rc.top, it.rc.right, it.rc.bottom), g_theme.accent);
                continue;
            }
            const AppEntry& a = g_apps[row.app];
            DrawItemBackground(i, it, g_theme.radius);
            DrawIcon(a.id, a.name, it.rc.left + 22, (it.rc.top + it.rc.bottom) / 2, 24);
            Text(a.name, g_fRow, D2D1::RectF(it.rc.left + 44, it.rc.top, it.rc.right - 10, it.rc.bottom), g_theme.text);
        }
        g_rt->PopAxisAlignedClip();
        DrawScrollbar(RList);
        if (g_jumpMode) DrawJumpOverlay();
    }

    // Bottom bar
    float by = g_h - L::BottomH;
    g_brush->SetColor(g_theme.bottomBg);
    g_rt->FillRectangle(D2D1::RectF(0, by, g_w, g_h), g_brush);
    g_brush->SetColor(g_theme.divider);
    g_rt->FillRectangle(D2D1::RectF(0, by, g_w, by + 1), g_brush);
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (it.region != RNone) continue;
        DrawItemBackground(i, it, g_theme.radius);
        float cy = (it.rc.top + it.rc.bottom) / 2;
        switch (it.kind) {
            case Kind::User: {
                D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(it.rc.left + 26, cy), 15, 15);
                g_brush->SetColor(g_theme.accent);
                g_rt->FillEllipse(e, g_brush);
                std::wstring initial = g_userName.empty() ? L"?" : g_userName.substr(0, 1);
                Text(initial, g_fAvatar, D2D1::RectF(it.rc.left + 11, cy - 15, it.rc.left + 41, cy + 15), g_theme.onAccent);
                Text(g_userName, g_fRow, D2D1::RectF(it.rc.left + 50, it.rc.top, it.rc.right - 8, it.rc.bottom), g_theme.text);
                break;
            }
            case Kind::BtnPower:     Glyph(0xE7E8, it.rc, g_theme.text); break;
            case Kind::BtnSettings:  Glyph(0xE713, it.rc, g_theme.text); break;
            case Kind::BtnExplorer:  Glyph(0xEC50, it.rc, g_theme.text); break;
            case Kind::BtnDownloads: Glyph(0xE896, it.rc, g_theme.text); break;
            default: break;
        }
    }

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

    if (dx != g_dpi) {
        g_dpi = dx;
        if (g_rt) { DiscardDeviceResources(); }
    }

    // Which edge is the taskbar on? Compare the work area with the monitor.
    const RECT& m = mi.rcMonitor;
    const RECT& w = mi.rcWork;
    enum { Bottom, Top, Left, Right } edge = Bottom;
    if (w.top > m.top) edge = Top;
    else if (w.left > m.left) edge = Left;
    else if (w.right < m.right) edge = Right;

    // Taskbar alignment: HKCU ... Explorer\Advanced\TaskbarAl (0 = left, 1/absent = center on Windows 11)
    DWORD al = 1, size = sizeof(al);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAl",
                     RRF_RT_REG_DWORD, nullptr, &al, &size) != ERROR_SUCCESS) {
        al = 1;  // value absent = Windows 11 default (centered)
    }

    int margin = Px(L::Margin);
    int width = Px(L::W);
    int height = std::min<int>(Px(L::H), (w.bottom - w.top) - 2 * margin);
    width = std::min<int>(width, (w.right - w.left) - 2 * margin);
    int x, y;
    switch (edge) {
        case Top:
            y = w.top + margin;
            x = (al == 0) ? w.left + margin : w.left + ((w.right - w.left) - width) / 2;
            break;
        case Left:
            x = w.left + margin; y = w.top + margin; break;
        case Right:
            x = w.right - width - margin; y = w.top + margin; break;
        default:
            y = w.bottom - height - margin;
            x = (al == 0) ? w.left + margin : w.left + ((w.right - w.left) - width) / 2;
            break;
    }
    g_w = Dip(width);
    g_h = Dip(height);
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

bool MenuVisible() { return g_hwnd && IsWindowVisible(g_hwnd); }

static void ResetState() {
    g_query.clear();
    g_selectAll = false;
    g_jumpMode = false;
    g_hot = g_sel = -1;
    for (float& s : g_scroll) s = 0;
    g_results.clear();
}

void MenuHide() {
    if (!MenuVisible()) return;
    KillTimer(g_hwnd, TIMER_CARET);
    ShowWindow(g_hwnd, SW_HIDE);
}

static void MenuShow(bool fromMouse, POINT pt) {
    DWORD t0 = GetTickCount();
    if (!fromMouse) GetCursorPos(&pt);
    else SendMaskKey();      // gives this process the right to take focus
    ResetState();
    PlaceWindow(pt);
    Layout();
    g_caretOn = true;
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    Paint();
    ValidateRect(g_hwnd, nullptr);
    ForceForeground(g_hwnd);
    if (GetForegroundWindow() != g_hwnd) Log(L"Warning: Windows did not give Foyer focus");
    SetTimer(g_hwnd, TIMER_CARET, GetCaretBlinkTime() ? GetCaretBlinkTime() : 530, nullptr);
    Log(L"Menu shown via %ls in %lu ms", fromMouse ? L"Start button" : L"Win key", GetTickCount() - t0);
    MaybeRefreshApps(false);
}

void MenuToggle(bool fromMouse, POINT pt) {
    if (MenuVisible()) MenuHide();
    else MenuShow(fromMouse, pt);
}

void MenuReloadConfig() {
    RebuildRows();
    if (MenuVisible()) Redraw();
}

// ============================================================ actions
enum Cmd : UINT {
    CMD_OPEN = 1, CMD_ADMIN, CMD_LOCATION, CMD_PIN, CMD_UNPIN, CMD_LEFT, CMD_RIGHT, CMD_HIDE, CMD_UNINSTALL,
    CMD_LOCK = 50, CMD_SIGNOUT, CMD_SWITCH, CMD_ACCOUNT,
    CMD_SLEEP = 60, CMD_HIBERNATE, CMD_SHUTDOWN, CMD_RESTART, CMD_UEFI,
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

static void ShowAppMenu(int itemIdx, bool useMouse) {
    const Item it = g_items[itemIdx];
    std::wstring name, target;
    std::wstring id = ItemAppId(it, &name, &target);
    if (id.empty()) return;
    bool pinned = g_config.IsPinned(id);
    bool isPinTile = it.kind == Kind::Pin;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, CMD_ADMIN, L"Run as administrator\tCtrl+Shift+Enter");
    AppendMenuW(m, MF_STRING, CMD_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (pinned) AppendMenuW(m, MF_STRING, CMD_UNPIN, L"Unpin from Foyer");
    else AppendMenuW(m, MF_STRING, CMD_PIN, L"Pin to Foyer");
    if (isPinTile) {
        AppendMenuW(m, MF_STRING | (it.index == 0 ? MF_GRAYED : 0), CMD_LEFT, L"Move left");
        AppendMenuW(m, MF_STRING | (it.index + 1 >= (int)g_config.pins.size() ? MF_GRAYED : 0), CMD_RIGHT, L"Move right");
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
    ClampScroll(RPins); ClampScroll(RList);
    Redraw();
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
    const Item& it = g_items[idx];
    switch (it.kind) {
        case Kind::Pin: case Kind::AppRow: case Kind::Result: Launch(it, admin); break;
        case Kind::Letter: g_jumpMode = true; Redraw(); break;
        case Kind::User: ShowAccountMenu(it); break;
        case Kind::BtnPower: ShowPowerMenu(it); break;
        case Kind::BtnSettings: ShellOpen(L"ms-settings:"); MenuHide(); break;
        case Kind::BtnExplorer: ShellOpen(L"explorer.exe"); MenuHide(); break;
        case Kind::BtnDownloads: ShellOpen(L"shell:Downloads"); MenuHide(); break;
    }
}

static void JumpTo(wchar_t letter) {
    g_jumpMode = false;
    float y = 0;
    for (auto& r : g_rows) {
        if (r.header && r.letter == letter) {
            g_scroll[RList] = y;
            ClampScroll(RList);
            break;
        }
        y += r.header ? L::LetterH : L::RowH;
    }
    Redraw();
}

// ============================================================ keyboard
static void OnQueryChanged() {
    g_jumpMode = false;
    g_scroll[RResults] = 0;
    RebuildResults();
    Layout();
    g_sel = Searching() && !g_results.empty() ? FindItem(Kind::Result, 0) : -1;
    g_caretOn = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void MoveSelection(int dir, bool horizontal) {
    // dir: -1/+1. Grid-aware for pinned tiles.
    if (Searching()) {
        if (g_results.empty()) return;
        int cur = (g_sel >= 0 && g_items[g_sel].kind == Kind::Result) ? g_items[g_sel].index : -1;
        if (horizontal) return;
        int next = std::clamp(cur + dir, 0, (int)g_results.size() - 1);
        g_sel = FindItem(Kind::Result, next);
        EnsureVisible(g_sel);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    int pinCount = (int)g_config.pins.size();
    auto firstAppRow = [&]() { for (int r = 0; r < (int)g_rows.size(); ++r) if (!g_rows[r].header) return r; return -1; };

    if (g_sel < 0 || (g_items[g_sel].kind != Kind::Pin && g_items[g_sel].kind != Kind::AppRow)) {
        g_sel = pinCount ? FindItem(Kind::Pin, 0) : FindItem(Kind::AppRow, firstAppRow());
    } else if (g_items[g_sel].kind == Kind::Pin) {
        int i = g_items[g_sel].index;
        if (horizontal) {
            int col = i % L::Cols;
            if (dir > 0 && (col == L::Cols - 1 || i + 1 >= pinCount)) {
                int r = firstAppRow();
                if (r >= 0) g_sel = FindItem(Kind::AppRow, r);
            } else {
                g_sel = FindItem(Kind::Pin, std::clamp(i + dir, 0, pinCount - 1));
            }
        } else {
            int n = i + dir * L::Cols;
            if (n >= 0 && n < pinCount) g_sel = FindItem(Kind::Pin, n);
            else if (dir > 0 && n >= pinCount && (i / L::Cols) < (pinCount - 1) / L::Cols) g_sel = FindItem(Kind::Pin, pinCount - 1);
        }
    } else {  // AppRow
        int r = g_items[g_sel].index;
        if (horizontal) {
            if (dir < 0 && pinCount) g_sel = FindItem(Kind::Pin, 0);
        } else {
            int n = r;
            do { n += dir; } while (n >= 0 && n < (int)g_rows.size() && g_rows[n].header);
            if (n >= 0 && n < (int)g_rows.size()) g_sel = FindItem(Kind::AppRow, n);
        }
    }
    EnsureVisible(g_sel);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void PageList(int dir) {
    int region = Searching() ? RResults : RList;
    g_scroll[region] += dir * (g_regionRc[region].bottom - g_regionRc[region].top) * 0.9f;
    ClampScroll(region);
    g_sel = -1;
    Redraw();
}

static void ToggleColumn() {
    if (Searching()) return;
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
            if (ctrl) {  // Ctrl+, : settings (Phase 3 brings the real window; for now open config.json)
                MenuHide();
                ShellOpen(L"notepad.exe", (L"\"" + DataDir() + L"\\config.json\"").c_str());
                return true;
            }
            break;
    }
    return false;
}

static void OnChar(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return;          // control chars handled in OnKeyDown
    if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) return;
    if (g_selectAll) { g_query.clear(); g_selectAll = false; }
    if (g_query.size() < 200) g_query += ch;
    OnQueryChanged();
}

// ============================================================ window proc
static void OnMouseMove(float x, float y) {
    if (!g_tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_hwnd, 0};
        TrackMouseEvent(&tme);
        g_tracking = true;
    }
    int hot = g_jumpMode ? -1 : HitTest(x, y);
    if (g_jumpMode) {
        // In jump mode only the bottom bar stays interactive
        int h = HitTest(x, y);
        if (h >= 0 && g_items[h].region == RNone) hot = h;
    }
    if (hot != g_hot) { g_hot = hot; InvalidateRect(g_hwnd, nullptr, FALSE); }
}

static void OnWheel(float x, float y, int delta) {
    int region = -1;
    for (int r = 1; r < 4; ++r) if (InRect(g_regionRc[r], x, y)) region = r;
    if (region < 0) region = Searching() ? RResults : RList;
    g_scroll[region] -= delta / 120.0f * L::RowH * 3;
    ClampScroll(region);
    Layout();
    g_hot = HitTest(x, y);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

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
            // We size and position ourselves explicitly in PlaceWindow(); just track the DPI.
            g_dpi = HIWORD(wParam);
            if (g_rt) g_rt->SetDpi((float)g_dpi, (float)g_dpi);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) MenuHide();
            return 0;
        case WM_CLOSE: MenuHide(); return 0;
        case WM_TIMER:
            if (wParam == TIMER_CARET) {
                // Safety net: if Windows refused to give us focus, we would never get a
                // "deactivated" message, so close once another app is in front.
                HWND fg = GetForegroundWindow();
                DWORD pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &pid);
                if (fg && pid != GetCurrentProcessId()) { MenuHide(); return 0; }
                g_caretOn = !g_caretOn;
                RECT r{0, 0, Px(g_w), Px(L::Pad + L::SearchH + 4)};
                InvalidateRect(hwnd, &r, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (OnKeyDown(wParam)) return 0;
            break;
        case WM_CHAR: OnChar((wchar_t)wParam); return 0;
        case WM_MOUSEMOVE: OnMouseMove(Dip((short)LOWORD(lParam)), Dip((short)HIWORD(lParam))); return 0;
        case WM_MOUSELEAVE: g_tracking = false; if (g_hot != -1) { g_hot = -1; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{(short)LOWORD(lParam), (short)HIWORD(lParam)};
            ScreenToClient(hwnd, &pt);
            OnWheel(Dip(pt.x), Dip(pt.y), GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            if (g_jumpMode) {
                int h = HitTest(x, y);
                if (h >= 0 && g_items[h].region == RNone) { g_jumpMode = false; Activate(h, false); return 0; }
                int letter = JumpHit(x, y);
                if (letter > 0) JumpTo((wchar_t)letter);
                else { g_jumpMode = false; Redraw(); }
                return 0;
            }
            int h = HitTest(x, y);
            if (h >= 0) Activate(h, (GetKeyState(VK_CONTROL) < 0) && (GetKeyState(VK_SHIFT) < 0));
            return 0;
        }
        case WM_RBUTTONUP: {
            int h = g_jumpMode ? -1 : HitTest(Dip((short)LOWORD(lParam)), Dip((short)HIWORD(lParam)));
            if (h >= 0) {
                Kind k = g_items[h].kind;
                if (k == Kind::Pin || k == Kind::AppRow || k == Kind::Result) ShowAppMenu(h, true);
            }
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
        case WM_FOYER_REFRESH_START:
            StartButtonRefreshAsync(nullptr);
            return 0;
        case WM_FOYER_START_FOUND: return 0;
        case WM_FOYER_APPS_READY: {
            auto* res = reinterpret_cast<AppListResult*>(lParam);
            g_apps = std::move(res->apps);
            delete res;
            g_appById.clear();
            for (int i = 0; i < (int)g_apps.size(); ++i) g_appById[ToLower(g_apps[i].id)] = i;
            // Refresh cached pin names so tiles have the right label next startup
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
        case WM_SETTINGCHANGE:
            g_theme = DarkTheme();  // picks up accent colour changes
            StartButtonRefreshAsync(nullptr);
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            g_theme = DarkTheme();
            return 0;
        case WM_DESTROY:
            TrayRemove();
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == taskbarCreated && taskbarCreated) {
                // Explorer restarted: re-add the tray icon and find the new Start button.
                TrayAdd(hwnd);
                StartButtonRefreshAsync(nullptr);
                return 0;
            }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND MenuCreate(HINSTANCE inst) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw));
    if (!g_d2d || !g_dw) { Log(L"Failed to create Direct2D/DirectWrite factories"); return nullptr; }
    g_theme = DarkTheme();
    CreateFormats();
    g_userName = CurrentUserDisplayName();
    SYSTEM_POWER_CAPABILITIES caps{};
    g_hibernateAvailable = GetPwrCapabilities(&caps) && caps.SystemS4 && caps.HiberFilePresent;
    g_iconPx = std::clamp(MulDiv(32, GetDpiForSystem(), 96), 32, 96);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MenuProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"FoyerMenu";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"FoyerMenu", L"Foyer", WS_POPUP,
                             0, 0, Px(L::W), Px(L::H), nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { Log(L"CreateWindowEx failed (%lu)", GetLastError()); return nullptr; }
    g_dpi = GetDpiForWindow(g_hwnd);

    // Windows 11 look: dark frame, rounded corners, subtle border.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2; /*DWMWCP_ROUND*/
    DwmSetWindowAttribute(g_hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    COLORREF border = RGB(0x3A, 0x3A, 0x40);
    DwmSetWindowAttribute(g_hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));

    EnsureTarget();   // pre-create so the first open is instant
    MaybeRefreshApps(true);
    return g_hwnd;
}
