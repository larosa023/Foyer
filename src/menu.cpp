// Foyer - the menu window (Direct2D + DirectWrite, custom drawn)
//
// The window is created hidden at startup and stays alive with its render target, fonts and
// icons ready, so opening it is just "position, show, paint". Animations run on a short timer
// only while something moves; when the menu is still, Foyer uses no CPU at all.
#include "common.h"
#include "config.h"
#include "apps.h"
#include "gfx.h"
#include "updater.h"
#include "search.h"
#include "calc.h"
#include "recent.h"
#include "shortcuts.h"
#include "system.h"
#include "inputbox.h"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <powrprof.h>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>

using namespace gfx;

// ============================================================ metrics
struct Metrics {
    float W = 760, H = 660;           // panel size (DIPs)
    bool classic = false, full = false;
    int cols = 4;
    float tileW = 88, tileH = 88, tileIcon = 32;
    float rowH = 38, rowIcon = 24, letterH = 30, resultH = 48, resultIcon = 30, recentH = 44;
    float pad = 20, greetH = 0, searchH = 42, gap = 14, headerH = 34, bottomH = 62, colGap = 24;
    int recentMax = 6;
};

enum class Kind {
    Pin, GroupChild, GroupTitle, AppRow, Letter, Section, ResHeader, Result, RecentRow, RecentMore,
    User, UpdatePill, BtnFoyer, BtnShortcut, BtnPower
};
enum Region { RNone = 0, RPins, RList, RResults, RGroup, RegionCount };

struct Item {
    Kind kind;
    int index;            // depends on kind (tile, child, row, result, recent, shortcut)
    D2D1_RECT_F rc;       // window DIPs, scroll applied
    Region region;
};

struct ListRow { bool header; wchar_t letter; int app; };

struct IconData {
    int size = 0;
    std::vector<uint8_t> pixels;
    ID2D1Bitmap* bmp = nullptr;
};

// A tile in the pinned area: one of the user's pins, or the automatic "My Apps" folder.
struct Tile { int pin; bool myApps; };

// Search results
enum class RKind { Calc, App, Setting, File, Run };
struct Res {
    RKind kind;
    std::wstring title, sub, target;
    wchar_t glyph = 0;
    int app = -1;
    int score = 0;
    bool dir = false;
};
struct ResRow { bool header; std::wstring label; int res; };

// ============================================================ state
static HWND g_hwnd;
static Theme g_theme;
static Metrics g_m;
static UINT g_dpi = 96;
static D2D1_SIZE_F g_win = {760, 660};
static D2D1_RECT_F g_panel;

static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static ID2D1RadialGradientBrush* g_glow;
static ID2D1RadialGradientBrush* g_glow2;
static Canvas g_cv;
static IDWriteTextFormat *g_fGreet, *g_fDate, *g_fSearch, *g_fHeader, *g_fRow, *g_fTile, *g_fSmall, *g_fIcon, *g_fIconSm, *g_fIconLg,
    *g_fLetter, *g_fAvatar, *g_fJump, *g_fPill, *g_fTip, *g_fCalc, *g_fSection;
static float g_textScale = 1.0f;

static std::vector<AppEntry> g_apps;
static std::map<std::wstring, int> g_appById;
static std::map<std::wstring, IconData> g_icons;
static std::vector<ListRow> g_rows;
static std::vector<Tile> g_tiles;
static std::vector<int> g_suiteApps;          // app indices for "My Apps"
static DWORD g_lastRefresh = 0;
static bool g_appsLoaded = false;
static int g_iconPx = 48;
static bool g_iconsStale = false;

static std::vector<RecentItem> g_recent;
static DWORD g_lastRecent = 0;

static std::wstring g_query;
static search::Query g_q;
static std::vector<Res> g_res;
static std::vector<ResRow> g_resRows;
static std::vector<search::FileHit> g_fileHits;
static unsigned g_searchGen = 0;
static bool g_filesPending = false;
static std::wstring g_flash;                  // short message in the results header ("Copied")
static DWORD g_flashUntil = 0;

static bool g_selectAll = false;
static bool g_caretOn = true;
static bool g_jumpMode = false;
static bool g_preview = false;
static int g_openTile = -1;                   // folder overlay
static std::vector<Item> g_items;
static int g_hot = -1, g_sel = -1;
static bool g_tracking = false;
static float g_scroll[RegionCount] = {}, g_scrollTarget[RegionCount] = {};
static float g_contentH[RegionCount] = {};
static D2D1_RECT_F g_regionRc[RegionCount];
static D2D1_RECT_F g_groupCard;
static std::wstring g_userName, g_firstName;
static bool g_hibernateAvailable = false;
static bool g_reduceMotion = false;

// drag and drop
struct DragState {
    bool pressed = false, active = false;
    Kind kind; int index = -1;
    float startX = 0, startY = 0, x = 0, y = 0;
    int insertAt = -1;        // tile index to insert before (-1 none)
    int mergeTile = -1;       // tile to drop onto (make / add to folder)
    bool outOfGroup = false;  // a folder item dragged outside its folder
};
static DragState g_drag;

// animation
static LARGE_INTEGER g_qpf;
static double g_openStart = 0;
static double g_lastTick = 0;
static float g_openDur = 0.26f;
static bool g_animTimer = false;
static std::map<uint64_t, float> g_hoverA;
static uint64_t g_pressKey = ~0ull;
static float g_pressA = 0;

constexpr UINT_PTR TIMER_CARET = 1;
constexpr UINT_PTR TIMER_ANIM = 2;
constexpr UINT_PTR TIMER_SAVEICONS = 3;
constexpr UINT_PTR TIMER_FLASH = 4;

// ============================================================ helpers
static double Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / g_qpf.QuadPart; }
static float Dip(int px) { return px * 96.0f / g_dpi; }
static int Px(float dip) { return (int)std::lround(dip * g_dpi / 96.0f); }
static bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static bool Searching() { return !g_query.empty(); }
static bool Animated() { return g_config.animation != L"none" && !g_reduceMotion; }
static uint64_t Key(Kind k, int idx) { return ((uint64_t)k << 32) | (uint32_t)idx; }
static float EaseOut(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }
static D2D1_RECT_F Inset(const D2D1_RECT_F& r, float d) { return D2D1::RectF(r.left + d, r.top + d, r.right - d, r.bottom - d); }
static bool IsPathId(const std::wstring& id) { return id.size() > 3 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/'); }

HWND MenuWindow() { return g_hwnd; }

static const AppEntry* AppById(const std::wstring& id) {
    auto it = g_appById.find(ToLower(id));
    return it == g_appById.end() ? nullptr : &g_apps[it->second];
}

static float OpenProgress() {
    if (!Animated() || g_openDur <= 0) return 1.0f;
    return (float)std::clamp((Now() - g_openStart) / g_openDur, 0.0, 1.0);
}

static void StartAnim();
static void Redraw();
static void Layout();

// ============================================================ pins & tiles
static Pin* TilePin(int t) {
    if (t < 0 || t >= (int)g_tiles.size() || g_tiles[t].myApps) return nullptr;
    int p = g_tiles[t].pin;
    return (p >= 0 && p < (int)g_config.pins.size()) ? &g_config.pins[p] : nullptr;
}

static bool TileIsGroup(int t) {
    if (t < 0 || t >= (int)g_tiles.size()) return false;
    if (g_tiles[t].myApps) return true;
    Pin* p = TilePin(t);
    return p && p->type == PinType::Group;
}

static std::wstring PinName(const Pin& p) {
    if (p.label.empty() && p.type == PinType::App)
        if (const AppEntry* a = AppById(p.id)) return a->name;
    return PinDisplayName(p);
}

static std::wstring TileName(int t) {
    if (t < 0 || t >= (int)g_tiles.size()) return L"";
    if (g_tiles[t].myApps) return L"My Apps";
    Pin* p = TilePin(t);
    return p ? PinName(*p) : L"";
}

static void RebuildTiles() {
    g_tiles.clear();
    g_suiteApps.clear();
    for (int i = 0; i < (int)g_apps.size(); ++i) if (g_apps[i].suite && !g_config.IsHidden(g_apps[i].id)) g_suiteApps.push_back(i);
    if (g_config.myAppsGroup && !g_suiteApps.empty()) g_tiles.push_back({-1, true});
    for (int i = 0; i < (int)g_config.pins.size(); ++i) g_tiles.push_back({i, false});
    if (g_openTile >= (int)g_tiles.size()) g_openTile = -1;
}

static int FirstPinTile() { return (!g_tiles.empty() && g_tiles[0].myApps) ? 1 : 0; }

// Children of a folder tile, as pins (My Apps children are synthesized from apps)
static int GroupChildCount(int t) {
    if (t < 0 || t >= (int)g_tiles.size()) return 0;
    if (g_tiles[t].myApps) return (int)g_suiteApps.size();
    Pin* p = TilePin(t);
    return p ? (int)p->items.size() : 0;
}

static Pin GroupChild(int t, int c) {
    if (g_tiles[t].myApps) {
        const AppEntry& a = g_apps[g_suiteApps[c]];
        Pin p; p.id = a.id; p.name = a.name; p.type = PinType::App;
        return p;
    }
    return TilePin(t)->items[c];
}

// ============================================================ icons
static ID2D1Bitmap* IconBitmap(const std::wstring& key) {
    auto it = g_icons.find(key);
    if (it == g_icons.end()) return nullptr;
    IconData& d = it->second;
    if (!d.bmp && g_rt && d.size > 0) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        g_rt->CreateBitmap(D2D1::SizeU(d.size, d.size), d.pixels.data(), d.size * 4, bp, &d.bmp);
    }
    return d.bmp;
}

struct IconSpec { std::wstring key, parsing; wchar_t glyph = 0; bool thumb = false; };

static IconSpec IconForPin(const Pin& p) {
    IconSpec s;
    if (!p.icon.empty()) { s.key = L"custom:" + ToLower(p.icon); s.parsing = p.icon; s.thumb = true; return s; }
    switch (p.type) {
        case PinType::App:
            s.key = p.id;
            if (!AppById(p.id)) s.parsing = IsPathId(p.id) ? p.id : L"shell:AppsFolder\\" + p.id;
            break;
        case PinType::File: s.key = FileIconKey(p.id, false); s.parsing = p.id; break;
        case PinType::Folder: s.key = FileIconKey(p.id, true); s.parsing = p.id; break;
        case PinType::Url: s.glyph = 0xE774; break;
        case PinType::Group: s.glyph = 0xE8B7; break;
    }
    return s;
}

static void DrawGlyphIcon(wchar_t glyph, float cx, float cy, float size, float opacity) {
    D2D1_RECT_F r = D2D1::RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2);
    g_cv.FillRound(r, size * 0.26f, Alpha(Mix(g_theme.surfaceStrong, g_theme.accent, 0.22f), opacity));
    IDWriteTextFormat* f = size >= 30 ? g_fIconLg : g_fIcon;
    g_cv.Glyph(glyph, f, r, Alpha(g_theme.accent, opacity));
}

static void DrawIconSpec(const IconSpec& s, const std::wstring& name, float cx, float cy, float size, float opacity = 1.0f) {
    D2D1_RECT_F r = D2D1::RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2);
    if (!s.key.empty()) {
        if (ID2D1Bitmap* bmp = IconBitmap(s.key)) {
            g_rt->DrawBitmap(bmp, r, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            return;
        }
        if (!s.parsing.empty()) IconRequest(g_hwnd, s.key, s.parsing, g_iconPx, s.thumb);
    }
    if (s.glyph) { DrawGlyphIcon(s.glyph, cx, cy, size, opacity); return; }
    g_cv.FillRound(r, size * 0.24f, Alpha(Mix(g_theme.surfaceStrong, g_theme.accent, 0.18f), opacity));
    std::wstring initial = name.empty() ? L"?" : name.substr(0, 1);
    g_cv.Text(initial, g_fAvatar, r, Alpha(g_theme.text, opacity));
}

static void DrawAppIcon(const std::wstring& id, const std::wstring& name, float cx, float cy, float size, float opacity = 1.0f) {
    IconSpec s;
    s.key = id;
    if (!AppById(id)) s.parsing = IsPathId(id) ? id : L"shell:AppsFolder\\" + id;
    DrawIconSpec(s, name, cx, cy, size, opacity);
}

// A folder tile: up to four child icons in a rounded square.
static void DrawFolderIcon(int t, float cx, float cy, float size, float opacity) {
    D2D1_RECT_F r = D2D1::RectF(cx - size / 2 - 3, cy - size / 2 - 3, cx + size / 2 + 3, cy + size / 2 + 3);
    g_cv.FillRound(r, size * 0.28f, Alpha(Mix(g_theme.surfaceStrong, g_theme.accent, 0.12f), opacity));
    int n = std::min(4, GroupChildCount(t));
    float cell = (size + 6 - 9) / 2;
    for (int i = 0; i < n; ++i) {
        Pin c = GroupChild(t, i);
        float x = r.left + 3 + (i % 2) * (cell + 3) + cell / 2, y = r.top + 3 + (i / 2) * (cell + 3) + cell / 2;
        IconSpec s = IconForPin(c);
        DrawIconSpec(s, PinName(c), x, y, cell - 2, opacity);
    }
    if (n == 0) g_cv.Glyph(0xE8B7, g_fIcon, r, Alpha(g_theme.accent, opacity));
}

static std::vector<CachedIcon> CollectIcons() {
    std::vector<CachedIcon> out;
    for (auto& kv : g_icons) {
        if (kv.second.size <= 0) continue;
        // apps and file-type icons are worth keeping; individual files and custom icons are cheap to reload
        if (kv.first.compare(0, 5, L"path:") == 0 || kv.first.compare(0, 7, L"custom:") == 0) continue;
        out.push_back({kv.first, kv.second.size, kv.second.pixels});
        if (out.size() >= 2500) break;
    }
    return out;
}

// ============================================================ metrics from settings
static void ComputeMetrics(const RECT& work) {
    Metrics m;
    bool compact = g_config.density == L"compact";
    float s = 1.0f + (g_textScale - 1.0f) * 0.8f;   // rows grow with text size
    m.rowH = (compact ? 32.0f : 38.0f) * s;
    m.letterH = (compact ? 26.0f : 30.0f) * s;
    m.resultH = (compact ? 42.0f : 50.0f) * s;
    m.recentH = (compact ? 38.0f : 44.0f) * s;
    m.tileW = (compact ? 84.0f : 90.0f) * std::min(s, 1.25f);
    m.tileIcon = g_config.iconSize == L"small" ? 26.0f : g_config.iconSize == L"large" ? 40.0f : 32.0f;
    m.rowIcon = g_config.iconSize == L"small" ? 20.0f : g_config.iconSize == L"large" ? 28.0f : 24.0f;
    m.resultIcon = m.rowIcon + 6;
    m.tileH = ((compact ? 74.0f : 84.0f) + (m.tileIcon - 32)) * s;
    m.searchH = 42 * s;
    m.headerH = 34 * s;
    m.bottomH = 62 * std::min(s, 1.3f);
    m.greetH = g_config.showGreeting ? 44.0f * s : 0.0f;
    m.classic = g_config.layout == L"classic";
    m.full = g_config.layout == L"fullscreen";
    float workW = Dip(work.right - work.left), workH = Dip(work.bottom - work.top);
    if (m.classic) {
        m.W = 430 * std::min(s, 1.3f); m.H = 720;
        m.recentMax = 5;
    } else if (m.full) {
        m.W = std::min(1280.0f, workW - 120);
        m.H = std::min(900.0f, workH - 100);
        m.pad = 32;
        m.tileW += 8; m.tileH += 6;
        float leftW = (m.W - 2 * m.pad - m.colGap) * 0.6f;
        m.cols = std::max(4, (int)(leftW / m.tileW));
        m.recentMax = 8;
    } else {
        m.W = 4 * m.tileW + 2 * m.pad + m.colGap + 350;
        m.H = 700 + m.greetH;
        m.recentMax = g_config.recentEnabled ? 4 : 0;
    }
    if (!g_config.recentEnabled) m.recentMax = 0;
    m.H = std::min(m.H, workH - 24);
    m.W = std::min(m.W, workW - 24);
    g_m = m;
}

// ============================================================ fonts & device resources
static void ReleaseFormats() {
    IDWriteTextFormat** all[] = {&g_fGreet, &g_fDate, &g_fSearch, &g_fHeader, &g_fRow, &g_fTile, &g_fSmall, &g_fIcon, &g_fIconSm, &g_fIconLg,
                                 &g_fLetter, &g_fAvatar, &g_fJump, &g_fPill, &g_fTip, &g_fCalc, &g_fSection};
    for (auto f : all) SafeRelease(*f);
}

static void CreateFormats() {
    ReleaseFormats();
    g_textScale = std::clamp(g_config.fontScale * a11y::TextScale(), 0.85f, 2.0f);
    float k = g_textScale;
    const wchar_t* ui = MenuFont(g_config);
    const wchar_t* display = g_config.font.empty() ? DisplayFont() : ui;
    g_fGreet   = Format(display, 21.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fDate    = Format(ui, 13.0f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    g_fSearch  = Format(ui, 15.0f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fHeader  = Format(ui, 14.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fSection = Format(ui, 12.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fRow     = Format(ui, 13.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fTile    = Format(ui, 12.0f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fSmall   = Format(ui, 11.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fIcon    = Format(IconFont(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconSm  = Format(IconFont(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconLg  = Format(IconFont(), 20.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fLetter  = Format(ui, 14.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fAvatar  = Format(ui, 14.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fJump    = Format(ui, 17.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fPill    = Format(ui, 12.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fTip     = Format(ui, 12.0f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fCalc    = Format(display, 22.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
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
    RebuildTiles();
}

static void RebuildResultRows() {
    g_resRows.clear();
    if (g_res.empty()) return;
    bool scoped = g_q.scope != search::Scope::All;
    // calculator answer always first
    std::vector<int> calc, apps, settings, files, run;
    for (int i = 0; i < (int)g_res.size(); ++i) {
        switch (g_res[i].kind) {
            case RKind::Calc: calc.push_back(i); break;
            case RKind::App: apps.push_back(i); break;
            case RKind::Setting: settings.push_back(i); break;
            case RKind::File: files.push_back(i); break;
            case RKind::Run: run.push_back(i); break;
        }
    }
    auto byScore = [&](std::vector<int>& v) { std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return g_res[a].score > g_res[b].score; }); };
    byScore(apps); byScore(settings); byScore(files);
    for (int i : run) { g_resRows.push_back({true, L"Run command", -1}); g_resRows.push_back({false, L"", i}); }
    for (int i : calc) { g_resRows.push_back({true, g_res[i].sub.find(L"conversion") != std::wstring::npos ? L"Conversion" : L"Calculator", -1}); g_resRows.push_back({false, L"", i}); }
    int best = -1;
    if (!scoped && calc.empty()) {
        for (auto* v : {&apps, &settings, &files})
            if (!v->empty() && (best < 0 || g_res[(*v)[0]].score > g_res[best].score)) best = (*v)[0];
        if (best >= 0) { g_resRows.push_back({true, L"Best match", -1}); g_resRows.push_back({false, L"", best}); }
    }
    auto section = [&](const wchar_t* label, const std::vector<int>& v, size_t limit) {
        std::vector<int> shown;
        for (int i : v) if (i != best && shown.size() < limit) shown.push_back(i);
        if (shown.empty()) return;
        g_resRows.push_back({true, label, -1});
        for (int i : shown) g_resRows.push_back({false, L"", i});
    };
    section(L"Apps", apps, scoped ? 40 : 6);
    section(L"Settings", settings, scoped ? 40 : 4);
    section(L"Files & folders", files, scoped ? 40 : 8);
}

static void MergeFileResults() {
    g_res.erase(std::remove_if(g_res.begin(), g_res.end(), [](const Res& r) { return r.kind == RKind::File; }), g_res.end());
    for (auto& h : g_fileHits) {
        Res r;
        r.kind = RKind::File;
        r.title = h.name;
        r.sub = h.folder;
        r.target = h.path;
        r.dir = h.dir;
        r.score = h.score - 120;   // files rank a little below apps and settings with the same match
        g_res.push_back(std::move(r));
    }
    RebuildResultRows();
}

static void RebuildResults() {
    g_res.clear();
    g_fileHits.clear();
    g_filesPending = false;
    g_q = search::Parse(g_query);
    if (g_q.text.empty() && g_q.scope != search::Scope::Run) { RebuildResultRows(); return; }

    if (g_q.scope == search::Scope::Run) {
        if (!g_q.text.empty()) {
            Res r; r.kind = RKind::Run; r.title = g_q.text; r.sub = L"Press Enter to run  \x00B7  Ctrl+Shift+Enter as administrator";
            r.glyph = 0xE756; r.target = g_q.text;
            g_res.push_back(r);
        }
        RebuildResultRows();
        return;
    }
    bool all = g_q.scope == search::Scope::All;
    if (all) {
        calc::Answer a;
        if (calc::Evaluate(g_q.text, a)) {
            Res r; r.kind = RKind::Calc; r.title = a.display; r.sub = a.detail + L"  \x00B7  Enter copies " + a.value;
            r.target = a.value; r.glyph = 0xE8EF; r.score = 5000;
            g_res.push_back(r);
        }
    }
    if (all || g_q.scope == search::Scope::Apps) {
        for (int i = 0; i < (int)g_apps.size(); ++i) {
            if (g_config.IsHidden(g_apps[i].id)) continue;
            int s = search::MatchScore(g_apps[i].nameLower, g_q.lower);
            if (s < 0) continue;
            Res r; r.kind = RKind::App; r.title = g_apps[i].name; r.app = i; r.target = g_apps[i].id;
            r.sub = g_apps[i].suite ? L"App  \x00B7  larosa023" : (g_apps[i].id.find(L'!') != std::wstring::npos ? L"Store app" : L"App");
            r.score = s + search::UsageBoost(L"app:" + g_apps[i].id) + 40;
            g_res.push_back(std::move(r));
        }
    }
    if ((all && g_config.searchSettings) || g_q.scope == search::Scope::Settings) {
        std::vector<search::SettingHit> hits;
        search::SearchSettings(g_q.lower, hits, all ? 8 : 40);
        for (auto& h : hits) {
            Res r; r.kind = RKind::Setting; r.title = h.title; r.sub = h.section; r.target = h.target; r.glyph = h.glyph; r.score = h.score;
            g_res.push_back(std::move(r));
        }
    }
    if (((all && g_config.searchFiles) || g_q.scope == search::Scope::Files) && g_q.text.size() >= 2) {
        g_filesPending = true;
        search::FilesSearchAsync(++g_searchGen, g_q.text, all ? 10 : 40, !all);
    }
    RebuildResultRows();
}

// ============================================================ layout
static float ContentTop() { return g_panel.top + g_m.pad + g_m.greetH + g_m.searchH + g_m.gap; }
static float ContentBottom() { return g_panel.bottom - g_m.bottomH - 6; }
static int RecentShown() { return std::min<int>((int)g_recent.size(), g_m.recentMax); }

static void LayoutGroup() {
    if (g_openTile < 0) return;
    const Metrics& m = g_m;
    int n = GroupChildCount(g_openTile);
    int cols = std::max(3, std::min(5, (int)((g_panel.right - g_panel.left - 80) / m.tileW)));
    int rows = std::max(1, (n + cols - 1) / cols);
    float w = cols * m.tileW + 40;
    float maxH = (ContentBottom() - ContentTop()) - 10;
    float h = std::min(maxH, 70 + rows * m.tileH + 20);
    float cx = (g_panel.left + g_panel.right) / 2, cy = (ContentTop() + ContentBottom()) / 2;
    g_groupCard = D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    g_items.push_back({Kind::GroupTitle, 0, D2D1::RectF(g_groupCard.left + 16, g_groupCard.top + 12, g_groupCard.right - 16, g_groupCard.top + 50), RNone});
    D2D1_RECT_F reg = D2D1::RectF(g_groupCard.left + 20, g_groupCard.top + 62, g_groupCard.right - 20, g_groupCard.bottom - 12);
    g_regionRc[RGroup] = reg;
    for (int i = 0; i < n; ++i) {
        float x = reg.left + (i % cols) * m.tileW, y = reg.top + (i / cols) * m.tileH - g_scroll[RGroup];
        g_items.push_back({Kind::GroupChild, i, D2D1::RectF(x, y, x + m.tileW, y + m.tileH), RGroup});
    }
    g_contentH[RGroup] = rows * m.tileH;
}

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
        for (int k = 0; k < (int)g_resRows.size(); ++k) {
            const ResRow& row = g_resRows[k];
            float h = row.header ? 28.0f * g_textScale : (g_res[row.res].kind == RKind::Calc ? m.resultH + 16 : m.resultH);
            g_items.push_back({row.header ? Kind::ResHeader : Kind::Result, k, D2D1::RectF(reg.left, y, reg.right, y + h), RResults});
            y += h;
        }
        g_contentH[RResults] = y + g_scroll[RResults] - reg.top;
    } else if (m.classic) {
        D2D1_RECT_F reg = D2D1::RectF(ox + m.pad - 6, top, right + 6, bottom);
        g_regionRc[RList] = reg;
        float y = reg.top - g_scroll[RList];
        if (!g_tiles.empty()) {
            g_items.push_back({Kind::Section, 0, D2D1::RectF(reg.left, y, reg.right, y + m.headerH), RList});
            y += m.headerH;
            for (int i = 0; i < (int)g_tiles.size(); ++i) {
                g_items.push_back({Kind::Pin, i, D2D1::RectF(reg.left, y, reg.right, y + m.rowH), RList});
                y += m.rowH;
            }
            y += 8;
        }
        if (RecentShown() > 0) {
            g_items.push_back({Kind::Section, 2, D2D1::RectF(reg.left, y, reg.right, y + m.headerH), RList});
            y += m.headerH;
            for (int i = 0; i < RecentShown(); ++i) {
                g_items.push_back({Kind::RecentRow, i, D2D1::RectF(reg.left, y, reg.right, y + m.recentH), RList});
                y += m.recentH;
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
        float y0 = pins.top - g_scroll[RPins];
        for (int i = 0; i < (int)g_tiles.size(); ++i) {
            float x = x0 + (i % m.cols) * m.tileW;
            float y = y0 + (i / m.cols) * m.tileH;
            g_items.push_back({Kind::Pin, i, D2D1::RectF(x, y, x + m.tileW, y + m.tileH), RPins});
        }
        float y = y0 + ((g_tiles.size() + m.cols - 1) / m.cols) * m.tileH;
        if (g_tiles.empty()) y += 40;
        if (RecentShown() > 0) {
            y += 10;
            g_items.push_back({Kind::Section, 2, D2D1::RectF(x0, y, x1, y + m.headerH), RPins});
            y += m.headerH;
            for (int i = 0; i < RecentShown(); ++i) {
                g_items.push_back({Kind::RecentRow, i, D2D1::RectF(x0, y, x1, y + m.recentH), RPins});
                y += m.recentH;
            }
        }
        g_contentH[RPins] = y + g_scroll[RPins] - pins.top + 6;

        float lx0 = x1 + m.colGap, lx1 = right;
        D2D1_RECT_F list = D2D1::RectF(lx0, top + m.headerH, lx1, bottom);
        g_regionRc[RList] = list;
        y = list.top - g_scroll[RList];
        for (int r = 0; r < (int)g_rows.size(); ++r) {
            float h = g_rows[r].header ? m.letterH : m.rowH;
            g_items.push_back({g_rows[r].header ? Kind::Letter : Kind::AppRow, r, D2D1::RectF(lx0, y, lx1, y + h), RList});
            y += h;
        }
        g_contentH[RList] = y + g_scroll[RList] - list.top;
    }

    if (!Searching()) LayoutGroup();

    // Bottom bar
    float by = g_panel.bottom - m.bottomH;
    float ux = ox + m.pad - 8;
    float userW = std::min(220.0f, 50 + MeasureWidth(g_userName, g_fRow) + 16);
    float btnTop = by + (m.bottomH - 40) / 2, btnBottom = btnTop + 40;
    g_items.push_back({Kind::User, 0, D2D1::RectF(ux, btnTop, ux + userW, btnBottom), RNone});
    if (AutoUpdateAvailable()) {
        float px = ux + userW + 8;
        g_items.push_back({Kind::UpdatePill, 0, D2D1::RectF(px, by + m.bottomH / 2 - 14, px + 132, by + m.bottomH / 2 + 14), RNone});
    }
    float bx = g_panel.right - m.pad + 4;
    auto addBtn = [&](Kind k, int idx) { bx -= 40; g_items.push_back({k, idx, D2D1::RectF(bx, btnTop, bx + 40, btnBottom), RNone}); bx -= 2; };
    addBtn(Kind::BtnPower, 0);
    std::vector<int> sc;
    for (auto& id : g_config.shortcuts) {
        for (int i = 0; i < kQuickShortcutCount; ++i) if (id == kQuickShortcuts[i].id) sc.push_back(i);
        if ((int)sc.size() >= kMaxQuickShortcuts) break;
    }
    for (auto it = sc.rbegin(); it != sc.rend(); ++it) addBtn(Kind::BtnShortcut, *it);
    addBtn(Kind::BtnFoyer, 0);
}

static bool IsSelectable(Kind k) {
    return k == Kind::Pin || k == Kind::AppRow || k == Kind::Result || k == Kind::RecentRow || k == Kind::GroupChild;
}

static float MaxScroll(int r) {
    float viewH = g_regionRc[r].bottom - g_regionRc[r].top;
    return std::max(0.0f, g_contentH[r] - viewH);
}

static int FindItem(Kind k, int index) {
    for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].kind == k && g_items[i].index == index) return i;
    return -1;
}

static void EnsureVisible(int itemIdx) {
    if (itemIdx < 0 || itemIdx >= (int)g_items.size()) return;
    const Item& it = g_items[itemIdx];
    if (it.region == RNone) return;
    const D2D1_RECT_F& reg = g_regionRc[it.region];
    float delta = g_scrollTarget[it.region] - g_scroll[it.region];
    float top = it.rc.top - delta, bottom = it.rc.bottom - delta;
    // keep a result's section header visible too
    if (it.kind == Kind::Result && it.index > 0 && g_resRows[it.index - 1].header) top -= 28.0f * g_textScale;
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
    bool overlay = g_openTile >= 0 && !Searching();
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (overlay && it.region != RGroup && it.kind != Kind::GroupTitle && !InRect(D2D1::RectF(0, g_panel.bottom - g_m.bottomH, g_win.width, g_win.height), x, y)) continue;
        if (it.region != RNone && !InRect(g_regionRc[it.region], x, y)) continue;
        if (InRect(it.rc, x, y)) return i;
    }
    return -1;
}

// ============================================================ accessibility
static std::wstring ItemLabel(const Item& it, std::wstring* desc) {
    std::wstring d;
    std::wstring name;
    switch (it.kind) {
        case Kind::Pin: name = TileName(it.index); d = TileIsGroup(it.index) ? L"Pinned folder" : L"Pinned"; break;
        case Kind::GroupChild: name = PinName(GroupChild(g_openTile, it.index)); d = L"In folder " + TileName(g_openTile); break;
        case Kind::AppRow: name = g_apps[g_rows[it.index].app].name; d = L"App"; break;
        case Kind::RecentRow: name = g_recent[it.index].name; d = g_recent[it.index].dir ? L"Recent folder" : L"Recent file"; break;
        case Kind::Result: {
            const Res& r = g_res[g_resRows[it.index].res];
            name = r.title;
            d = r.kind == RKind::App ? L"App" : r.kind == RKind::Setting ? L"Setting, " + r.sub : r.kind == RKind::File ? (r.dir ? L"Folder" : L"File") + std::wstring(L", in ") + r.sub
                : r.kind == RKind::Calc ? L"Result. Press Enter to copy" : L"Run command";
            break;
        }
        default: break;
    }
    if (desc) *desc = d;
    return name;
}

static void Announce() {
    if (!g_hwnd || !IsWindowVisible(g_hwnd)) return;
    if (g_sel >= 0 && g_sel < (int)g_items.size()) {
        const Item& it = g_items[g_sel];
        std::wstring desc;
        std::wstring name = ItemLabel(it, &desc);
        RECT r{Px(it.rc.left), Px(it.rc.top), Px(it.rc.right), Px(it.rc.bottom)};
        MapWindowPoints(g_hwnd, nullptr, (POINT*)&r, 2);
        a11y::Focus(g_hwnd, name, desc, r, false, L"");
    } else {
        RECT r; GetWindowRect(g_hwnd, &r);
        std::wstring desc = Searching() ? (g_res.empty() ? L"No results" : std::to_wstring(g_res.size()) + L" results") : L"Type to search apps, settings and files";
        a11y::Focus(g_hwnd, L"Search", desc, r, true, g_query);
    }
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
    for (int r = 1; r < RegionCount; ++r) {
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
    g_cv.FillRound(Inset(box, -3), rad + 3, Alpha(g_theme.accent, 0.12f));
    g_cv.FillRound(box, rad, g_theme.surface);
    g_cv.StrokeRound(box, rad, Alpha(g_theme.accent, 0.9f), 1.5f);
    // scope chip ("Files", "Settings"...) replaces the magnifier when a prefix is typed
    const wchar_t* scope = nullptr;
    switch (g_q.scope) {
        case search::Scope::Files: scope = L"Files"; break;
        case search::Scope::Settings: scope = L"Settings"; break;
        case search::Scope::Apps: scope = L"Apps"; break;
        case search::Scope::Run: scope = L"Run"; break;
        default: break;
    }
    float textLeft = box.left + 46;
    if (scope && Searching()) {
        float w = MeasureWidth(scope, g_fPill) + 20;
        D2D1_RECT_F chip = D2D1::RectF(box.left + 10, box.top + 9, box.left + 10 + w, box.bottom - 9);
        g_cv.FillRound(chip, (chip.bottom - chip.top) / 2, g_theme.accent);
        g_cv.Text(scope, g_fPill, D2D1::RectF(chip.left + 10, chip.top, chip.right, chip.bottom), g_theme.onAccent);
        textLeft = chip.right + 10;
    } else {
        g_cv.Glyph(0xE721, g_fIcon, D2D1::RectF(box.left + 10, box.top, box.left + 42, box.bottom), g_theme.accent);
    }

    D2D1_RECT_F tr = D2D1::RectF(textLeft, box.top, box.right - 18, box.bottom);
    if (g_query.empty()) {
        g_cv.Text(L"Search apps, settings and files", g_fSearch, D2D1::RectF(tr.left + 4, tr.top, tr.right, tr.bottom), g_theme.subtext);
        if (g_caretOn) g_cv.Fill(D2D1::RectF(tr.left, box.top + 12, tr.left + 1.5f, box.bottom - 12), g_theme.accent);
        return;
    }
    // hide the typed prefix ("f:") behind the chip
    std::wstring shown = g_query;
    if (scope) {
        size_t lead = shown.find_first_not_of(L' ');
        shown = shown.substr(lead == std::wstring::npos ? 0 : lead);
        shown = shown.substr(g_q.scope == search::Scope::Run ? 1 : 2);
        size_t sp = shown.find_first_not_of(L' ');
        shown = sp == std::wstring::npos ? L"" : shown.substr(sp);
    }
    IDWriteTextLayout* lay = nullptr;
    if (!shown.empty()) dw->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), g_fSearch, 4000, tr.bottom - tr.top, &lay);
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
    D2D1_RECT_F all = D2D1::RectF(reg.left - 4, reg.top - (g_m.classic ? 0 : g_m.headerH), reg.right + 4, reg.bottom);
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
    float top = reg.top - (g_m.classic ? 0 : g_m.headerH) + 10;
    float cell = std::min(56.0f, (reg.right - reg.left) / 6.0f);
    float gx = reg.left + ((reg.right - reg.left) - cell * 6) / 2;
    if (x < gx || x >= gx + cell * 6 || y < top) return -1;
    int i = (int)((y - top) / cell) * 6 + (int)((x - gx) / cell);
    const wchar_t* letters = L"#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    return (i >= 0 && i < 27) ? letters[i] : -1;
}

static void DrawTooltip(const Item& it, const std::wstring& text, float alpha) {
    if (alpha <= 0.05f || text.empty()) return;
    float w = MeasureWidth(text, g_fTip) + 18;
    float cx = (it.rc.left + it.rc.right) / 2;
    float x = std::clamp(cx - w / 2, g_panel.left + 6, g_panel.right - w - 6);
    D2D1_RECT_F r = D2D1::RectF(x, it.rc.top - 32 + (1 - alpha) * 4, x + w, it.rc.top - 8 + (1 - alpha) * 4);
    g_cv.FillRound(r, 6, Alpha(g_theme.surfaceStrong, 0.97f * alpha));
    g_cv.StrokeRound(r, 6, Alpha(g_theme.divider, alpha));
    g_cv.Text(text, g_fTip, r, Alpha(g_theme.text, alpha));
}

// Pinned item drawn as a tile (grid) or a row (classic list)
static void DrawPinContent(int t, const Pin* pin, bool myApps, const D2D1_RECT_F& box, bool asRow, float appear, float hover) {
    std::wstring name = myApps ? L"My Apps" : PinName(*pin);
    bool group = myApps || (pin && pin->type == PinType::Group);
    if (asRow) {
        float icon = g_m.rowIcon;
        float cy = (box.top + box.bottom) / 2;
        float cx = box.left + 10 + icon / 2 + 2 * hover;
        if (group) DrawFolderIcon(t, cx, cy, icon - 4, appear);
        else DrawIconSpec(IconForPin(*pin), name, cx, cy, icon, appear);
        float tx = box.left + 22 + icon + 2 * hover;
        g_cv.Text(name, g_fRow, D2D1::RectF(tx, box.top, box.right - 34, box.bottom), Alpha(g_theme.text, appear));
        if (group) g_cv.Glyph(0xE76C, g_fIconSm, D2D1::RectF(box.right - 30, box.top, box.right - 8, box.bottom), g_theme.subtext);
        return;
    }
    float cx = (box.left + box.right) / 2;
    float lift = -2.5f * hover;
    float iy = box.top + 8 + g_m.tileIcon / 2 + 2 + lift;
    if (group) DrawFolderIcon(t, cx, iy, g_m.tileIcon - 2, appear);
    else DrawIconSpec(IconForPin(*pin), name, cx, iy, g_m.tileIcon, appear);
    float ty = box.top + 14 + g_m.tileIcon;
    g_cv.Text(name, g_fTile, D2D1::RectF(box.left + 4, ty, box.right - 4, ty + 22 * g_textScale), Alpha(g_theme.text, appear));
}

static void DrawPinItem(int i, const Item& it, float appear) {
    int t = it.index;
    bool myApps = g_tiles[t].myApps;
    Pin* pin = TilePin(t);
    if (!myApps && !pin) return;
    float h = HoverOf(Kind::Pin, t);
    bool pressed = g_pressKey == Key(Kind::Pin, t);
    bool dragging = g_drag.active && g_drag.kind == Kind::Pin && g_drag.index == t;
    bool asRow = g_m.classic;
    float scale = pressed ? 1.0f - (asRow ? 0.02f : 0.06f) * g_pressA : 1.0f;
    float dy = (1 - appear) * 12;
    D2D1_RECT_F box = asRow ? Inset(it.rc, 1) : D2D1::RectF(it.rc.left + 3, it.rc.top + 3 + dy, it.rc.right - 3, it.rc.bottom - 3 + dy);
    float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    D2D1_MATRIX_3X2_F saved;
    g_rt->GetTransform(&saved);
    if (scale != 1.0f) g_rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)) * saved);
    if (g_drag.active && g_drag.mergeTile == t) {
        g_cv.FillRound(box, g_theme.radius, Alpha(g_theme.accent, 0.22f));
        g_cv.StrokeRound(box, g_theme.radius, g_theme.accent, 2.0f);
    } else {
        DrawItemBg(box, Kind::Pin, t, i == g_sel, g_theme.radius);
    }
    DrawPinContent(t, pin, myApps, box, asRow, dragging ? 0.3f : appear, h);
    g_rt->SetTransform(saved);
}

static void DrawRowBase(int i, const Item& it, float* textLeft, float iconSize, const IconSpec* spec, const std::wstring& name, const std::wstring& appId) {
    D2D1_RECT_F rc = Inset(it.rc, 1);
    DrawItemBg(rc, it.kind, it.index, i == g_sel, g_theme.radius);
    float h = HoverOf(it.kind, it.index);
    float cy = (rc.top + rc.bottom) / 2;
    float cx = rc.left + 10 + iconSize / 2 + 2 * h;
    if (spec) DrawIconSpec(*spec, name, cx, cy, iconSize);
    else DrawAppIcon(appId, name, cx, cy, iconSize);
    *textLeft = rc.left + 22 + iconSize + 2 * h;
    if (i == g_sel) g_cv.FillRound(D2D1::RectF(rc.left + 2, cy - 8, rc.left + 5, cy + 8), 1.5f, g_theme.accent);
}

static void WithPress(const Item& it, float amount, const std::function<void()>& draw) {
    bool pressed = g_pressKey == Key(it.kind, it.index);
    D2D1_MATRIX_3X2_F saved;
    g_rt->GetTransform(&saved);
    if (pressed) {
        float s = 1.0f - amount * g_pressA;
        g_rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F((it.rc.left + it.rc.right) / 2, (it.rc.top + it.rc.bottom) / 2)) * saved);
    }
    draw();
    g_rt->SetTransform(saved);
}

static void DrawAppRow(int i, const Item& it) {
    const AppEntry& a = g_apps[g_rows[it.index].app];
    WithPress(it, 0.02f, [&] {
        float tx;
        DrawRowBase(i, it, &tx, g_m.rowIcon, nullptr, a.name, a.id);
        g_cv.Text(a.name, g_fRow, D2D1::RectF(tx, it.rc.top, it.rc.right - 12, it.rc.bottom), g_theme.text);
    });
}

static void DrawRecentRow(int i, const Item& it) {
    const RecentItem& r = g_recent[it.index];
    WithPress(it, 0.02f, [&] {
        IconSpec s; s.key = FileIconKey(r.path, r.dir); s.parsing = r.path;
        float tx;
        DrawRowBase(i, it, &tx, g_m.rowIcon, &s, r.name, L"");
        float cy = (it.rc.top + it.rc.bottom) / 2;
        std::wstring when = RelativeTime(r.when);
        float ww = MeasureWidth(when, g_fSmall) + 6;
        g_cv.Text(r.name, g_fRow, D2D1::RectF(tx, it.rc.top + 3, it.rc.right - 12 - ww, cy + 3), g_theme.text);
        std::wstring folder = r.folder;
        size_t sl = folder.find_last_of(L'\\');
        if (sl != std::wstring::npos && folder.size() > 3) folder = folder.substr(sl + 1);
        g_cv.Text(folder, g_fSmall, D2D1::RectF(tx, cy, it.rc.right - 12 - ww, it.rc.bottom - 3), g_theme.subtext);
        g_cv.Text(when, g_fSmall, D2D1::RectF(it.rc.right - 8 - ww, it.rc.top, it.rc.right - 8, it.rc.bottom), g_theme.subtext);
    });
}

static void DrawResult(int i, const Item& it) {
    const Res& r = g_res[g_resRows[it.index].res];
    WithPress(it, 0.02f, [&] {
        D2D1_RECT_F rc = Inset(it.rc, 1);
        float cy = (rc.top + rc.bottom) / 2;
        if (r.kind == RKind::Calc) {
            DrawItemBg(rc, it.kind, it.index, i == g_sel, g_theme.radius);
            g_cv.FillRound(rc, g_theme.radius, Alpha(g_theme.accent, 0.06f));
            float icon = g_m.resultIcon + 6;
            DrawGlyphIcon(r.glyph, rc.left + 12 + icon / 2, cy, icon, 1.0f);
            float tx = rc.left + 26 + icon;
            g_cv.Text(r.title, g_fCalc, D2D1::RectF(tx, rc.top + 4, rc.right - 12, cy + 6), g_theme.text);
            g_cv.Text(r.sub, g_fSmall, D2D1::RectF(tx, cy + 6, rc.right - 12, rc.bottom - 6), g_theme.subtext);
            return;
        }
        float tx;
        IconSpec s;
        if (r.kind == RKind::File) { s.key = FileIconKey(r.target, r.dir); s.parsing = r.target; }
        else if (r.kind != RKind::App) s.glyph = r.glyph;
        DrawRowBase(i, it, &tx, g_m.resultIcon, r.kind == RKind::App ? nullptr : &s, r.title, r.kind == RKind::App ? g_apps[r.app].id : L"");
        g_cv.Text(r.title, g_fRow, D2D1::RectF(tx, rc.top + 5, rc.right - 12, cy + 2), g_theme.text);
        g_cv.Text(r.sub, g_fSmall, D2D1::RectF(tx, cy, rc.right - 12, rc.bottom - 5), g_theme.subtext);
    });
}

static void DrawGroupOverlay() {
    if (g_openTile < 0 || Searching()) return;
    const Theme& t = g_theme;
    g_cv.Fill(D2D1::RectF(g_panel.left, ContentTop() - 6, g_panel.right, ContentBottom() + 4), Alpha(t.bg, 0.55f));
    D2D1_RECT_F card = g_groupCard;
    g_cv.FillRound(Inset(card, -6), t.radius * 2 + 6, Alpha(D2D1::ColorF(0, 0, 0, 1), t.dark ? 0.25f : 0.08f));
    g_cv.FillRound(card, t.radius * 2, Mix(t.bg, t.surface, 0.7f));
    g_cv.StrokeRound(card, t.radius * 2, t.divider);
    int ti = FindItem(Kind::GroupTitle, 0);
    if (ti >= 0) {
        const Item& title = g_items[ti];
        float h = HoverOf(Kind::GroupTitle, 0);
        bool editable = !g_tiles[g_openTile].myApps;
        if (h > 0 && editable) g_cv.FillRound(title.rc, t.radius, Alpha(t.hover, h));
        g_cv.Text(TileName(g_openTile), g_fGreet, D2D1::RectF(title.rc.left + 8, title.rc.top, title.rc.right - 40, title.rc.bottom), t.text);
        if (editable) g_cv.Glyph(0xE70F, g_fIconSm, D2D1::RectF(title.rc.right - 36, title.rc.top, title.rc.right - 8, title.rc.bottom), Alpha(t.subtext, 0.4f + 0.6f * h));
    }
    g_rt->PushAxisAlignedClip(g_regionRc[RGroup], D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (it.kind != Kind::GroupChild) continue;
        Pin c = GroupChild(g_openTile, it.index);
        float h = HoverOf(Kind::GroupChild, it.index);
        bool dragging = g_drag.active && g_drag.kind == Kind::GroupChild && g_drag.index == it.index;
        D2D1_RECT_F box = D2D1::RectF(it.rc.left + 3, it.rc.top + 3, it.rc.right - 3, it.rc.bottom - 3);
        WithPress(it, 0.06f, [&] {
            DrawItemBg(box, Kind::GroupChild, it.index, i == g_sel, t.radius);
            float cx = (box.left + box.right) / 2;
            DrawIconSpec(IconForPin(c), PinName(c), cx, box.top + 8 + g_m.tileIcon / 2 + 2 - 2.5f * h, g_m.tileIcon, dragging ? 0.3f : 1.0f);
            float ty = box.top + 14 + g_m.tileIcon;
            g_cv.Text(PinName(c), g_fTile, D2D1::RectF(box.left + 4, ty, box.right - 4, ty + 22 * g_textScale), t.text);
        });
    }
    g_rt->PopAxisAlignedClip();
    DrawScrollbar(RGroup);
}

static void DrawDragGhost() {
    if (!g_drag.active) return;
    std::wstring name;
    IconSpec spec;
    bool isApp = false, folder = false;
    std::wstring appId;
    switch (g_drag.kind) {
        case Kind::Pin:
            if (g_tiles[g_drag.index].myApps) { name = L"My Apps"; folder = true; }
            else if (Pin* p = TilePin(g_drag.index)) { name = PinName(*p); folder = p->type == PinType::Group; spec = IconForPin(*p); }
            break;
        case Kind::GroupChild: { Pin c = GroupChild(g_openTile, g_drag.index); name = PinName(c); spec = IconForPin(c); break; }
        case Kind::AppRow: { const AppEntry& a = g_apps[g_rows[g_drag.index].app]; name = a.name; isApp = true; appId = a.id; break; }
        case Kind::RecentRow: { const RecentItem& r = g_recent[g_drag.index]; name = r.name; spec.key = FileIconKey(r.path, r.dir); spec.parsing = r.path; break; }
        case Kind::Result: {
            const Res& r = g_res[g_resRows[g_drag.index].res];
            name = r.title;
            if (r.kind == RKind::App) { isApp = true; appId = r.target; }
            else { spec.key = FileIconKey(r.target, r.dir); spec.parsing = r.target; }
            break;
        }
        default: return;
    }
    float w = g_m.tileW - 6, h = g_m.tileH - 6;
    D2D1_RECT_F box = D2D1::RectF(g_drag.x - w / 2, g_drag.y - h / 2, g_drag.x + w / 2, g_drag.y + h / 2);
    g_cv.FillRound(Inset(box, -4), g_theme.radius + 4, Alpha(D2D1::ColorF(0, 0, 0, 1), 0.25f));
    g_cv.FillRound(box, g_theme.radius, Alpha(g_theme.surfaceStrong, 0.95f));
    g_cv.StrokeRound(box, g_theme.radius, Alpha(g_theme.accent, 0.8f), 1.5f);
    float cx = (box.left + box.right) / 2, iy = box.top + 8 + g_m.tileIcon / 2 + 2;
    if (folder) DrawFolderIcon(g_drag.index, cx, iy, g_m.tileIcon - 2, 1.0f);
    else if (isApp) DrawAppIcon(appId, name, cx, iy, g_m.tileIcon);
    else DrawIconSpec(spec, name, cx, iy, g_m.tileIcon);
    float ty = box.top + 14 + g_m.tileIcon;
    g_cv.Text(name, g_fTile, D2D1::RectF(box.left + 4, ty, box.right - 4, ty + 22 * g_textScale), g_theme.text);
}

static void DrawInsertMarker() {
    if (!g_drag.active || g_drag.insertAt < 0 || g_drag.mergeTile >= 0) return;
    int n = (int)g_tiles.size();
    int at = g_drag.insertAt;
    if (g_m.classic) {
        float y;
        if (at < n) { int ii = FindItem(Kind::Pin, at); if (ii < 0) return; y = g_items[ii].rc.top; }
        else { int ii = FindItem(Kind::Pin, n - 1); if (ii < 0) return; y = g_items[ii].rc.bottom; }
        const D2D1_RECT_F& reg = g_regionRc[RList];
        g_cv.FillRound(D2D1::RectF(reg.left + 6, y - 1.5f, reg.right - 6, y + 1.5f), 1.5f, g_theme.accent);
        return;
    }
    float x, top, bottom;
    if (at < n) {
        int ii = FindItem(Kind::Pin, at); if (ii < 0) return;
        x = g_items[ii].rc.left; top = g_items[ii].rc.top; bottom = g_items[ii].rc.bottom;
    } else if (n > 0) {
        int ii = FindItem(Kind::Pin, n - 1); if (ii < 0) return;
        x = g_items[ii].rc.right; top = g_items[ii].rc.top; bottom = g_items[ii].rc.bottom;
    } else {
        x = g_regionRc[RPins].left; top = g_regionRc[RPins].top; bottom = top + g_m.tileH;
    }
    g_cv.FillRound(D2D1::RectF(x - 1.5f, top + 10, x + 1.5f, bottom - 10), 1.5f, g_theme.accent);
}

static void DrawBackground() {
    const Theme& t = g_theme;
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    float bgA = t.backdrop ? t.opacity : 1.0f;
    if (g_m.full) {
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
    g_cv.Fill(D2D1::RectF(g_panel.left + 12, g_panel.top, g_panel.right - 12, g_panel.top + 1), Alpha(D2D1::ColorF(1, 1, 1, 1), t.dark ? 0.07f : 0.5f));
}

static void DrawSectionHeader(const D2D1_RECT_F& rc, const wchar_t* text, const wchar_t* right = nullptr) {
    g_cv.Text(text, g_fHeader, D2D1::RectF(rc.left + 6, rc.top, rc.right - 8, rc.bottom), g_theme.text);
    if (right) {
        IDWriteTextFormat* f = g_fSmall;
        float w = MeasureWidth(right, f);
        g_cv.Text(right, f, D2D1::RectF(rc.right - 8 - w, rc.top, rc.right - 8, rc.bottom), g_theme.subtext);
    }
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
        if (layer) g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), p), layer);
        float s = m.full ? 0.97f + 0.03f * p : 1.0f;
        D2D1_POINT_2F c = D2D1::Point2F((g_panel.left + g_panel.right) / 2, (g_panel.top + g_panel.bottom) / 2);
        g_rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * D2D1::Matrix3x2F::Translation(0, (1 - p) * 18));
    }

    if (m.greetH > 0) {
        float gy = g_panel.top + m.pad - 4;
        D2D1_RECT_F gr = D2D1::RectF(g_panel.left + m.pad + 2, gy, g_panel.right - m.pad - 2, gy + m.greetH - 8);
        g_cv.Text(GreetingText(), g_fGreet, gr, t.text);
        if (!m.classic) g_cv.Text(DateText(), g_fDate, gr, t.subtext);
    }
    DrawSearch();

    const float top = ContentTop();
    if (Searching()) {
        std::wstring hdr;
        if (!g_flash.empty() && GetTickCount() < g_flashUntil) hdr = g_flash;
        else if (g_res.empty()) hdr = (!g_appsLoaded || g_filesPending) ? L"Searching\x2026" : (g_q.scope == search::Scope::Run ? L"Type a command to run" : L"No results");
        else hdr = L"Results";
        std::wstring hint = L"f: files   s: settings   a: apps   > run";
        if (g_filesPending && !g_res.empty()) hint = L"Searching files\x2026";
        DrawSectionHeader(D2D1::RectF(g_panel.left + m.pad - 2, top, g_panel.right - m.pad, top + m.headerH), hdr.c_str(), hint.c_str());
        g_rt->PushAxisAlignedClip(g_regionRc[RResults], D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const Item& it = g_items[i];
            if (it.region != RResults || it.rc.bottom < g_regionRc[RResults].top || it.rc.top > g_regionRc[RResults].bottom) continue;
            if (it.kind == Kind::ResHeader) {
                g_cv.Text(g_resRows[it.index].label, g_fSection, D2D1::RectF(it.rc.left + 8, it.rc.top + 4, it.rc.right, it.rc.bottom), Alpha(t.accent, 0.95f));
                continue;
            }
            DrawResult(i, it);
        }
        g_rt->PopAxisAlignedClip();
        DrawScrollbar(RResults);
    } else {
        if (!m.classic) {
            float x1 = g_panel.left + m.pad + m.cols * m.tileW;
            DrawSectionHeader(D2D1::RectF(g_panel.left + m.pad, top, x1, top + m.headerH), L"Pinned");
            DrawSectionHeader(D2D1::RectF(x1 + m.colGap, top, g_panel.right - m.pad, top + m.headerH), L"All apps",
                              g_appsLoaded ? (std::to_wstring(g_apps.size() - g_config.hidden.size()) + L" apps").c_str() : nullptr);
            g_rt->PushAxisAlignedClip(g_regionRc[RPins], D2D1_ANTIALIAS_MODE_ALIASED);
            if (g_tiles.empty())
                g_cv.Text(L"Right-click or drag any app here to pin it.", g_fSmall,
                          D2D1::RectF(g_regionRc[RPins].left + 6, g_regionRc[RPins].top - g_scroll[RPins], g_regionRc[RPins].right, g_regionRc[RPins].top + 30 - g_scroll[RPins]), t.subtext);
            for (int i = 0; i < (int)g_items.size(); ++i) {
                const Item& it = g_items[i];
                if (it.region != RPins || it.rc.bottom < g_regionRc[RPins].top || it.rc.top > g_regionRc[RPins].bottom) continue;
                if (it.kind == Kind::Pin) {
                    float appear = Animated() ? EaseOut((raw * g_openDur - it.index * 0.018f) / std::max(0.08f, g_openDur * 0.7f)) : 1.0f;
                    DrawPinItem(i, it, appear);
                } else if (it.kind == Kind::Section) {
                    DrawSectionHeader(it.rc, L"Recent");
                } else if (it.kind == Kind::RecentRow) {
                    DrawRecentRow(i, it);
                }
            }
            DrawInsertMarker();
            g_rt->PopAxisAlignedClip();
            DrawScrollbar(RPins);
        }

        g_rt->PushAxisAlignedClip(g_regionRc[RList], D2D1_ANTIALIAS_MODE_ALIASED);
        if (!g_appsLoaded && g_rows.empty() && !m.classic)
            g_cv.Text(L"Loading apps\x2026", g_fSmall, D2D1::RectF(g_regionRc[RList].left + 12, g_regionRc[RList].top, g_regionRc[RList].right, g_regionRc[RList].top + 30), t.subtext);
        for (int i = 0; i < (int)g_items.size(); ++i) {
            const Item& it = g_items[i];
            if (it.region != RList || it.rc.bottom < g_regionRc[RList].top || it.rc.top > g_regionRc[RList].bottom) continue;
            switch (it.kind) {
                case Kind::Section:
                    DrawSectionHeader(D2D1::RectF(it.rc.left + 4, it.rc.top, it.rc.right, it.rc.bottom), it.index == 0 ? L"Pinned" : it.index == 2 ? L"Recent" : L"All apps");
                    break;
                case Kind::Pin: DrawPinItem(i, it, 1.0f); break;
                case Kind::RecentRow: DrawRecentRow(i, it); break;
                case Kind::Letter: {
                    const ListRow& row = g_rows[it.index];
                    float h = HoverOf(Kind::Letter, it.index);
                    if (h > 0) g_cv.FillRound(Inset(it.rc, 1), t.radius, Alpha(t.hover, h));
                    D2D1_RECT_F badge = D2D1::RectF(it.rc.left + 8, it.rc.top + 4, it.rc.left + 30, it.rc.bottom - 4);
                    g_cv.FillRound(badge, 6, Alpha(t.accent, 0.14f + 0.1f * h));
                    wchar_t s[2] = {row.letter, 0};
                    g_cv.Text(s, g_fAvatar, badge, t.accent);
                    g_cv.Fill(D2D1::RectF(it.rc.left + 38, (it.rc.top + it.rc.bottom) / 2, it.rc.right - 10, (it.rc.top + it.rc.bottom) / 2 + 1), Alpha(t.divider, 0.8f));
                    break;
                }
                case Kind::AppRow: DrawAppRow(i, it); break;
                default: break;
            }
        }
        if (m.classic) DrawInsertMarker();
        g_rt->PopAxisAlignedClip();
        DrawScrollbar(RList);
        if (g_jumpMode) DrawJumpOverlay();
        DrawGroupOverlay();
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
    std::wstring tipText;
    float tipA = 0;
    for (int i = 0; i < (int)g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (it.region != RNone || it.kind == Kind::GroupTitle) continue;
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
        std::wstring tip;
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
            case Kind::BtnPower: g_cv.Glyph(0xE7E8, g_fIcon, icon, t.text); tip = L"Power"; break;
            case Kind::BtnFoyer: g_cv.Glyph(0xE771, g_fIcon, icon, t.accent); tip = L"Foyer settings"; break;
            case Kind::BtnShortcut: {
                const QuickShortcut& q = kQuickShortcuts[it.index];
                g_cv.Glyph(q.glyph, g_fIcon, icon, t.text);
                tip = q.name;
                break;
            }
            default: break;
        }
        if (!tip.empty() && h > tipA) { tipA = h; tipItem = &it; tipText = tip; }
    }
    if (tipItem) DrawTooltip(*tipItem, tipText, tipA);
    DrawDragGhost();

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
        if (HWND sw = FindWindowW(L"FoyerSettings", nullptr)) {
            RECT sr; GetWindowRect(sw, &sr);
            int gap = Px(16);
            if (sr.right + gap + width <= w.right) x = sr.right + gap;
            else if (sr.left - gap - width >= w.left) x = sr.left - gap - width;
            else if ((sr.right - sr.left) + gap + width <= (w.right - w.left)) {
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
    if (!g_iconsStale) for (auto& kv : g_icons) have.push_back(kv.first);
    g_iconsStale = false;
    AppsRefreshAsync(g_hwnd, std::move(have), g_iconPx);
}

static void MaybeRefreshRecent(bool force) {
    if (!g_config.recentEnabled) { g_recent.clear(); return; }
    DWORD now = GetTickCount();
    if (!force && now - g_lastRecent < 3000) return;
    g_lastRecent = now;
    RecentRefreshAsync(g_hwnd, 12, g_config.hiddenRecent);
}

void MenuRefreshApps() { MaybeRefreshApps(true); }

bool MenuVisible() { return g_hwnd && IsWindowVisible(g_hwnd); }

static void ResetState() {
    g_query.clear();
    g_q = search::Query{};
    g_res.clear();
    g_resRows.clear();
    g_fileHits.clear();
    g_filesPending = false;
    g_selectAll = false;
    g_jumpMode = false;
    g_openTile = -1;
    g_hot = g_sel = -1;
    for (int r = 0; r < RegionCount; ++r) g_scroll[r] = g_scrollTarget[r] = 0;
    g_hoverA.clear();
    g_pressKey = ~0ull;
    g_pressA = 0;
    g_drag = DragState{};
}

void MenuHide() {
    if (!MenuVisible()) return;
    if (GetCapture() == g_hwnd) ReleaseCapture();
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
    g_reduceMotion = a11y::ReduceMotion();
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
    Announce();
    MaybeRefreshApps(false);
    MaybeRefreshRecent(false);
    search::FilesTouch();
}

void MenuToggle(bool fromMouse, POINT pt) {
    if (MenuVisible() && !g_preview) MenuHide();
    else { if (g_preview) MenuHide(); MenuShow(fromMouse, pt, false); }
}

static POINT SettingsCenter() {
    POINT pt; GetCursorPos(&pt);
    if (HWND s = FindWindowW(L"FoyerSettings", nullptr)) {
        RECT r; GetWindowRect(s, &r);
        pt = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    }
    return pt;
}

void MenuSetPreview(bool on) {
    if (on) MenuShow(true, SettingsCenter(), true);
    else if (g_preview) MenuHide();
}

void MenuReloadConfig() {
    RebuildRows();
    if (MenuVisible()) Redraw();
}

void MenuApplySettings() {
    g_theme = BuildTheme(g_config);
    ApplyWindowTheme(g_hwnd, g_theme, true);
    ApplyMenuTheme(g_theme.dark);
    CreateFormats();
    DiscardDeviceResources();
    RebuildRows();
    if (!g_config.recentEnabled) g_recent.clear();
    if (MenuVisible()) {
        POINT pt;
        if (g_preview) pt = SettingsCenter();
        else { RECT r; GetWindowRect(g_hwnd, &r); pt = {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }
        PlaceWindow(pt);
        Redraw();
    }
}

// ============================================================ actions
enum Cmd : UINT {
    CMD_OPEN = 1, CMD_ADMIN, CMD_LOCATION, CMD_PIN, CMD_UNPIN, CMD_LEFT, CMD_RIGHT, CMD_HIDE, CMD_UNINSTALL,
    CMD_RENAME, CMD_ICON, CMD_ICON_RESET, CMD_UNGROUP, CMD_OUT_OF_FOLDER, CMD_COPY_PATH, CMD_REMOVE_RECENT, CMD_RECENT_OFF,
    CMD_HIDE_MYAPPS, CMD_ADD_URL, CMD_ADD_FILE, CMD_ADD_FOLDER, CMD_NEW_FOLDER,
    CMD_LOCK = 50, CMD_SIGNOUT, CMD_SWITCH, CMD_ACCOUNT,
    CMD_SLEEP = 60, CMD_HIBERNATE, CMD_SHUTDOWN, CMD_RESTART, CMD_UEFI,
    CMD_FOYER_SETTINGS = 70, CMD_REFRESH, CMD_SHORTCUTS,
};

static void LaunchPin(const Pin& p, bool admin) {
    switch (p.type) {
        case PinType::App: {
            search::UsageRecord(L"app:" + p.id);
            const AppEntry* a = AppById(p.id);
            if (admin) AppRunAsAdmin(g_hwnd, p.id, a ? a->target : (IsPathId(p.id) ? p.id : L""));
            else AppLaunch(p.id);
            break;
        }
        case PinType::File:
        case PinType::Folder:
            search::UsageRecord(L"file:" + p.id);
            OpenPath(p.id);
            break;
        case PinType::Url:
            OpenPath(p.id);
            break;
        case PinType::Group: return;
    }
    MenuHide();
}

static void LaunchApp(int app, bool admin) {
    const AppEntry& a = g_apps[app];
    search::UsageRecord(L"app:" + a.id);
    if (admin) AppRunAsAdmin(g_hwnd, a.id, a.target);
    else AppLaunch(a.id);
    MenuHide();
}

static void CopyText(const std::wstring& text) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) { memcpy(p, text.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
        else GlobalFree(h);
    }
    CloseClipboard();
}

static void Flash(const std::wstring& text) {
    g_flash = text;
    g_flashUntil = GetTickCount() + 1600;
    SetTimer(g_hwnd, TIMER_FLASH, 1650, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ActivateResult(int row, bool admin) {
    const Res& r = g_res[g_resRows[row].res];
    switch (r.kind) {
        case RKind::Calc:
            CopyText(r.target);
            Flash(L"Copied " + r.target);
            return;
        case RKind::App: LaunchApp(r.app, admin); return;
        case RKind::Setting: search::LaunchSetting(r.target); MenuHide(); return;
        case RKind::File:
            search::UsageRecord(L"file:" + r.target);
            OpenPath(r.target);
            MenuHide();
            return;
        case RKind::Run:
            if (search::RunCommand(r.target, admin)) MenuHide();
            else Flash(L"Couldn't run that command");
            return;
    }
}

static void OpenGroup(int tile) {
    g_openTile = tile;
    g_scroll[RGroup] = g_scrollTarget[RGroup] = 0;
    g_jumpMode = false;
    Layout();
    g_sel = GroupChildCount(tile) ? FindItem(Kind::GroupChild, 0) : -1;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    Announce();
}

static void CloseGroup() {
    int tile = g_openTile;
    g_openTile = -1;
    Layout();
    g_sel = FindItem(Kind::Pin, tile);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    Announce();
}

static void SaveAndRefresh() {
    ConfigSave();
    RebuildTiles();
    g_sel = -1;
    Redraw();
}

static std::wstring PickFileOrFolder(bool folder) {
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    dlg->SetTitle(folder ? L"Pin a folder to Foyer" : L"Pin a file to Foyer");
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

static std::wstring PickIcon() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Icons and pictures (*.ico;*.png;*.jpg;*.exe;*.dll)\0*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.exe;*.dll\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose an icon";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? std::wstring(path) : L"";
}

static Pin PinForPath(const std::wstring& path) {
    Pin p;
    p.id = path;
    DWORD attr = GetFileAttributesW(path.c_str());
    p.type = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? PinType::Folder : PinType::File;
    std::wstring lower = ToLower(path);
    if (p.type == PinType::File && lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0) p.type = PinType::App;
    return p;
}

static void AddPin(Pin p, int beforeTile = -1) {
    if (p.type != PinType::Group && g_config.IsPinned(p.id)) { Flash(L"Already pinned"); return; }
    int at = (beforeTile < 0 || beforeTile >= (int)g_tiles.size()) ? (int)g_config.pins.size() : std::max(0, g_tiles[beforeTile].pin);
    if (beforeTile >= 0 && beforeTile < (int)g_tiles.size() && g_tiles[beforeTile].myApps) at = 0;
    at = std::clamp(at, 0, (int)g_config.pins.size());
    g_config.pins.insert(g_config.pins.begin() + at, std::move(p));
    SaveAndRefresh();
}

static void AddWebsite() {
    std::wstring url = L"https://";
    if (!InputBox(g_hwnd, L"Pin a website", L"Website address:", url, g_theme.dark)) return;
    url = url.substr(0, url.find_last_not_of(L" ") + 1);
    if (url.size() < 9 || url == L"https://") return;
    if (url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    Pin p; p.id = url; p.type = PinType::Url;
    std::wstring name = PinDisplayName(p);
    if (InputBox(g_hwnd, L"Pin a website", L"Name for the tile:", name, g_theme.dark) && !name.empty()) p.label = name;
    AddPin(p);
}

static void RenamePin(Pin& p, const wchar_t* title) {
    std::wstring name = PinName(p);
    if (!InputBox(g_hwnd, title, L"Name:", name, g_theme.dark)) return;
    p.label = name == PinDisplayName(Pin{p.id, p.name, p.type}) ? L"" : name;
    if (p.type == PinType::Group) p.label = name.empty() ? L"Folder" : name;
    SaveAndRefresh();
}

static POINT ScreenPointFor(const Item& it, bool useMouse) {
    POINT pt;
    if (useMouse) { GetCursorPos(&pt); return pt; }
    pt.x = Px(it.rc.left + 16); pt.y = Px(it.rc.bottom);
    ClientToScreen(g_hwnd, &pt);
    return pt;
}

static UINT TrackMenu(HMENU m, POINT pt, UINT flags = 0) {
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | flags, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    return cmd;
}

static void OpenFoyerSettings(int page) {
    MenuHide();
    SettingsOpen(page);
}

// Context menu for an app (from the list or search results)
static void ShowAppMenu(int app, POINT pt) {
    const AppEntry a = g_apps[app];
    bool pinned = g_config.IsPinned(a.id);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, CMD_ADMIN, L"Run as administrator\tCtrl+Shift+Enter");
    AppendMenuW(m, MF_STRING, CMD_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, pinned ? CMD_UNPIN : CMD_PIN, pinned ? L"Unpin from Foyer" : L"Pin to Foyer");
    AppendMenuW(m, MF_STRING, CMD_HIDE, L"Hide from Foyer");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_UNINSTALL, L"Uninstall\x2026");
    switch (TrackMenu(m, pt)) {
        case CMD_OPEN: LaunchApp(app, false); return;
        case CMD_ADMIN: LaunchApp(app, true); return;
        case CMD_LOCATION: AppOpenLocation(g_hwnd, a.id, a.target); MenuHide(); return;
        case CMD_PIN: { Pin p; p.id = a.id; p.name = a.name; AddPin(p); return; }
        case CMD_UNPIN:
            for (auto it = g_config.pins.begin(); it != g_config.pins.end();) {
                if (it->type != PinType::Group && _wcsicmp(it->id.c_str(), a.id.c_str()) == 0) { it = g_config.pins.erase(it); continue; }
                auto& items = it->items;
                items.erase(std::remove_if(items.begin(), items.end(), [&](const Pin& c) { return _wcsicmp(c.id.c_str(), a.id.c_str()) == 0; }), items.end());
                ++it;
            }
            SaveAndRefresh();
            return;
        case CMD_HIDE: g_config.hidden.push_back(a.id); ConfigSave(); RebuildRows(); RebuildResults(); Redraw(); return;
        case CMD_UNINSTALL: AppUninstall(a.id); MenuHide(); return;
    }
}

static void ShowFileMenu(const std::wstring& path, bool dir, POINT pt, int recentIndex) {
    bool pinned = g_config.IsPinned(path);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open");
    AppendMenuW(m, MF_STRING, CMD_LOCATION, L"Open file location");
    AppendMenuW(m, MF_STRING, CMD_COPY_PATH, L"Copy path");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (pinned ? MF_GRAYED : 0), CMD_PIN, pinned ? L"Pinned" : L"Pin to Foyer");
    if (recentIndex >= 0) {
        AppendMenuW(m, MF_STRING, CMD_REMOVE_RECENT, L"Remove from Recent");
        AppendMenuW(m, MF_STRING, CMD_RECENT_OFF, L"Turn off Recent\x2026");
    }
    switch (TrackMenu(m, pt)) {
        case CMD_OPEN: search::UsageRecord(L"file:" + path); OpenPath(path); MenuHide(); return;
        case CMD_LOCATION: OpenFileLocation(path); MenuHide(); return;
        case CMD_COPY_PATH: CopyText(path); return;
        case CMD_PIN: { Pin p = PinForPath(path); if (dir) p.type = PinType::Folder; AddPin(p); return; }
        case CMD_REMOVE_RECENT:
            g_config.hiddenRecent.push_back(path);
            if (g_config.hiddenRecent.size() > 300) g_config.hiddenRecent.erase(g_config.hiddenRecent.begin());
            ConfigSave();
            g_recent.erase(g_recent.begin() + recentIndex);
            g_sel = -1;
            Redraw();
            return;
        case CMD_RECENT_OFF: OpenFoyerSettings(PageApps); return;
    }
}

static void ShowPinMenu(int tile, POINT pt) {
    if (g_tiles[tile].myApps) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open folder");
        AppendMenuW(m, MF_STRING, CMD_HIDE_MYAPPS, L"Hide My Apps");
        UINT cmd = TrackMenu(m, pt);
        if (cmd == CMD_OPEN) OpenGroup(tile);
        else if (cmd == CMD_HIDE_MYAPPS) { g_config.myAppsGroup = false; SaveAndRefresh(); }
        return;
    }
    Pin* pp = TilePin(tile);
    if (!pp) return;
    int pi = g_tiles[tile].pin;
    bool group = pp->type == PinType::Group;
    bool app = pp->type == PinType::App;
    bool path = pp->type == PinType::File || pp->type == PinType::Folder || (app && IsPathId(pp->id));
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, group ? L"Open folder" : L"Open");
    if (app) AppendMenuW(m, MF_STRING, CMD_ADMIN, L"Run as administrator");
    if (app || path) AppendMenuW(m, MF_STRING, CMD_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_RENAME, group ? L"Rename folder\x2026" : L"Rename\x2026");
    if (!group) {
        AppendMenuW(m, MF_STRING, CMD_ICON, L"Change icon\x2026");
        if (!pp->icon.empty()) AppendMenuW(m, MF_STRING, CMD_ICON_RESET, L"Reset icon");
    } else {
        AppendMenuW(m, MF_STRING, CMD_UNGROUP, L"Ungroup");
    }
    bool first = pi == 0, last = pi + 1 >= (int)g_config.pins.size();
    AppendMenuW(m, MF_STRING | (first ? MF_GRAYED : 0), CMD_LEFT, g_m.classic ? L"Move up" : L"Move left");
    AppendMenuW(m, MF_STRING | (last ? MF_GRAYED : 0), CMD_RIGHT, g_m.classic ? L"Move down" : L"Move right");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_UNPIN, group ? L"Unpin folder" : L"Unpin");
    UINT cmd = TrackMenu(m, pt);
    pp = TilePin(tile);
    if (!pp) return;
    switch (cmd) {
        case CMD_OPEN: if (group) OpenGroup(tile); else LaunchPin(*pp, false); return;
        case CMD_ADMIN: LaunchPin(*pp, true); return;
        case CMD_LOCATION: {
            const AppEntry* a = AppById(pp->id);
            if (app && !IsPathId(pp->id)) AppOpenLocation(g_hwnd, pp->id, a ? a->target : L"");
            else OpenFileLocation(pp->id);
            MenuHide();
            return;
        }
        case CMD_RENAME: RenamePin(*pp, group ? L"Rename folder" : L"Rename"); return;
        case CMD_ICON: { std::wstring ic = PickIcon(); if (!ic.empty()) { pp->icon = ic; SaveAndRefresh(); } return; }
        case CMD_ICON_RESET: pp->icon.clear(); SaveAndRefresh(); return;
        case CMD_UNGROUP: {
            std::vector<Pin> items = pp->items;
            g_config.pins.erase(g_config.pins.begin() + pi);
            g_config.pins.insert(g_config.pins.begin() + pi, items.begin(), items.end());
            SaveAndRefresh();
            return;
        }
        case CMD_LEFT: std::swap(g_config.pins[pi], g_config.pins[pi - 1]); SaveAndRefresh(); return;
        case CMD_RIGHT: std::swap(g_config.pins[pi], g_config.pins[pi + 1]); SaveAndRefresh(); return;
        case CMD_UNPIN:
            if (group && !pp->items.empty() &&
                MessageBoxW(g_hwnd, (L"Unpin the folder \"" + PinName(*pp) + L"\" and everything in it?").c_str(), L"Foyer", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
            g_config.pins.erase(g_config.pins.begin() + pi);
            SaveAndRefresh();
            return;
    }
}

static void ShowGroupChildMenu(int child, POINT pt) {
    if (g_tiles[g_openTile].myApps) {
        ShowAppMenu(g_suiteApps[child], pt);
        return;
    }
    Pin* g = TilePin(g_openTile);
    if (!g || child >= (int)g->items.size()) return;
    Pin& c = g->items[child];
    bool app = c.type == PinType::App;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"Open");
    if (app) AppendMenuW(m, MF_STRING, CMD_ADMIN, L"Run as administrator");
    if (c.type != PinType::Url) AppendMenuW(m, MF_STRING, CMD_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_RENAME, L"Rename\x2026");
    AppendMenuW(m, MF_STRING, CMD_ICON, L"Change icon\x2026");
    if (!c.icon.empty()) AppendMenuW(m, MF_STRING, CMD_ICON_RESET, L"Reset icon");
    AppendMenuW(m, MF_STRING, CMD_OUT_OF_FOLDER, L"Move out of folder");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_UNPIN, L"Unpin");
    UINT cmd = TrackMenu(m, pt);
    g = TilePin(g_openTile);
    if (!g || child >= (int)g->items.size()) return;
    Pin& cc = g->items[child];
    switch (cmd) {
        case CMD_OPEN: LaunchPin(cc, false); return;
        case CMD_ADMIN: LaunchPin(cc, true); return;
        case CMD_LOCATION: {
            const AppEntry* a = AppById(cc.id);
            if (app && !IsPathId(cc.id)) AppOpenLocation(g_hwnd, cc.id, a ? a->target : L"");
            else OpenFileLocation(cc.id);
            MenuHide();
            return;
        }
        case CMD_RENAME: RenamePin(cc, L"Rename"); return;
        case CMD_ICON: { std::wstring ic = PickIcon(); if (!ic.empty()) { cc.icon = ic; SaveAndRefresh(); } return; }
        case CMD_ICON_RESET: cc.icon.clear(); SaveAndRefresh(); return;
        case CMD_OUT_OF_FOLDER: {
            Pin moved = cc;
            int gi = g_tiles[g_openTile].pin;
            g->items.erase(g->items.begin() + child);
            g_config.pins.insert(g_config.pins.begin() + gi + 1, moved);
            if (g_config.pins[gi].items.empty()) { g_config.pins.erase(g_config.pins.begin() + gi); g_openTile = -1; }
            SaveAndRefresh();
            return;
        }
        case CMD_UNPIN: {
            int gi = g_tiles[g_openTile].pin;
            g->items.erase(g->items.begin() + child);
            if (g_config.pins[gi].items.empty()) { g_config.pins.erase(g_config.pins.begin() + gi); g_openTile = -1; }
            SaveAndRefresh();
            return;
        }
    }
}

static void ShowPinsAreaMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_ADD_URL, L"Pin a website\x2026");
    AppendMenuW(m, MF_STRING, CMD_ADD_FILE, L"Pin a file\x2026");
    AppendMenuW(m, MF_STRING, CMD_ADD_FOLDER, L"Pin a folder\x2026");
    AppendMenuW(m, MF_STRING, CMD_NEW_FOLDER, L"New pin folder");
    if (!g_config.myAppsGroup) AppendMenuW(m, MF_STRING, CMD_HIDE_MYAPPS, L"Show My Apps");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_FOYER_SETTINGS, L"Foyer settings\tCtrl+,");
    switch (TrackMenu(m, pt)) {
        case CMD_ADD_URL: AddWebsite(); return;
        case CMD_ADD_FILE: { std::wstring p = PickFileOrFolder(false); if (!p.empty()) AddPin(PinForPath(p)); return; }
        case CMD_ADD_FOLDER: { std::wstring p = PickFileOrFolder(true); if (!p.empty()) { Pin pin = PinForPath(p); pin.type = PinType::Folder; AddPin(pin); } return; }
        case CMD_NEW_FOLDER: { Pin g; g.type = PinType::Group; g.label = L"Folder"; AddPin(g); return; }
        case CMD_HIDE_MYAPPS: g_config.myAppsGroup = true; SaveAndRefresh(); return;
        case CMD_FOYER_SETTINGS: OpenFoyerSettings(-1); return;
    }
}

static void ShowBackgroundMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_FOYER_SETTINGS, L"Foyer settings\tCtrl+,");
    AppendMenuW(m, MF_STRING, CMD_SHORTCUTS, L"Choose bottom bar shortcuts\x2026");
    AppendMenuW(m, MF_STRING, CMD_REFRESH, L"Refresh app list");
    UINT cmd = TrackMenu(m, pt);
    if (cmd == CMD_FOYER_SETTINGS) OpenFoyerSettings(-1);
    else if (cmd == CMD_SHORTCUTS) OpenFoyerSettings(PageLayout);
    else if (cmd == CMD_REFRESH) { MaybeRefreshApps(true); MaybeRefreshRecent(true); }
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
    UINT cmd = TrackMenu(m, pt, TPM_BOTTOMALIGN);
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
    UINT cmd = TrackMenu(m, pt, TPM_BOTTOMALIGN | TPM_RIGHTALIGN);
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

static void ContextMenuFor(int idx, bool useMouse) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    const Item it = g_items[idx];
    POINT pt = ScreenPointFor(it, useMouse);
    switch (it.kind) {
        case Kind::Pin: ShowPinMenu(it.index, pt); break;
        case Kind::GroupChild: ShowGroupChildMenu(it.index, pt); break;
        case Kind::AppRow: ShowAppMenu(g_rows[it.index].app, pt); break;
        case Kind::RecentRow: ShowFileMenu(g_recent[it.index].path, g_recent[it.index].dir, pt, it.index); break;
        case Kind::Result: {
            const Res& r = g_res[g_resRows[it.index].res];
            if (r.kind == RKind::App) ShowAppMenu(r.app, pt);
            else if (r.kind == RKind::File) ShowFileMenu(r.target, r.dir, pt, -1);
            break;
        }
        default: break;
    }
}

static void Activate(int idx, bool admin) {
    if (idx < 0 || idx >= (int)g_items.size()) return;
    const Item it = g_items[idx];
    switch (it.kind) {
        case Kind::Pin:
            if (TileIsGroup(it.index)) OpenGroup(it.index);
            else if (Pin* p = TilePin(it.index)) LaunchPin(*p, admin);
            break;
        case Kind::GroupChild:
            if (g_tiles[g_openTile].myApps) LaunchApp(g_suiteApps[it.index], admin);
            else { Pin c = GroupChild(g_openTile, it.index); LaunchPin(c, admin); }
            break;
        case Kind::GroupTitle:
            if (!g_tiles[g_openTile].myApps) if (Pin* p = TilePin(g_openTile)) RenamePin(*p, L"Rename folder");
            break;
        case Kind::AppRow: LaunchApp(g_rows[it.index].app, admin); break;
        case Kind::Result: ActivateResult(it.index, admin); break;
        case Kind::RecentRow:
            search::UsageRecord(L"file:" + g_recent[it.index].path);
            OpenPath(g_recent[it.index].path);
            MenuHide();
            break;
        case Kind::Letter: g_jumpMode = true; Redraw(); break;
        case Kind::User: ShowAccountMenu(it); break;
        case Kind::UpdatePill: OpenFoyerSettings(PageUpdates); break;
        case Kind::BtnFoyer: OpenFoyerSettings(-1); break;
        case Kind::BtnPower: ShowPowerMenu(it); break;
        case Kind::BtnShortcut: {
            const QuickShortcut& q = kQuickShortcuts[it.index];
            ShellOpen(q.target, q.args);
            MenuHide();
            break;
        }
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

// ============================================================ drag and drop
static bool Draggable(Kind k, int index) {
    switch (k) {
        case Kind::Pin: return !g_tiles[index].myApps;
        case Kind::GroupChild: return g_openTile >= 0 && !g_tiles[g_openTile].myApps;
        case Kind::AppRow: case Kind::RecentRow: return true;
        case Kind::Result: { const Res& r = g_res[g_resRows[index].res]; return r.kind == RKind::App || r.kind == RKind::File; }
        default: return false;
    }
}

static bool InPinsArea(float x, float y) {
    if (g_m.classic) {
        // the pinned rows (and just below them)
        int first = FindItem(Kind::Pin, 0), last = FindItem(Kind::Pin, (int)g_tiles.size() - 1);
        if (first < 0) { int sec = FindItem(Kind::Section, 1); return sec >= 0 && InRect(g_regionRc[RList], x, y) && y < g_items[sec].rc.top + 10; }
        return InRect(g_regionRc[RList], x, y) && y >= g_items[first].rc.top - 10 && y <= g_items[last].rc.bottom + 14;
    }
    return InRect(g_regionRc[RPins], x, y);
}

static void UpdateDropTarget() {
    g_drag.insertAt = -1;
    g_drag.mergeTile = -1;
    g_drag.outOfGroup = false;
    float x = g_drag.x, y = g_drag.y;
    if (g_drag.kind == Kind::GroupChild) {
        g_drag.outOfGroup = !InRect(g_groupCard, x, y);
        if (!g_drag.outOfGroup) {
            // reorder inside the folder
            for (const Item& it : g_items) if (it.kind == Kind::GroupChild && InRect(it.rc, x, y)) {
                float f = (x - it.rc.left) / (it.rc.right - it.rc.left);
                g_drag.insertAt = it.index + (f > 0.5f ? 1 : 0);
            }
        }
        return;
    }
    if (!InPinsArea(x, y)) return;
    bool srcIsGroup = g_drag.kind == Kind::Pin && TileIsGroup(g_drag.index);
    int n = (int)g_tiles.size();
    for (const Item& it : g_items) {
        if (it.kind != Kind::Pin || !InRect(it.rc, x, y)) continue;
        float f = g_m.classic ? (y - it.rc.top) / (it.rc.bottom - it.rc.top) : (x - it.rc.left) / (it.rc.right - it.rc.left);
        bool self = g_drag.kind == Kind::Pin && g_drag.index == it.index;
        bool canMerge = !self && !srcIsGroup && !g_tiles[it.index].myApps;
        if (canMerge && f > 0.28f && f < 0.72f) { g_drag.mergeTile = it.index; return; }
        g_drag.insertAt = it.index + (f >= 0.5f ? 1 : 0);
        break;
    }
    if (g_drag.insertAt < 0) g_drag.insertAt = n;
    g_drag.insertAt = std::max(g_drag.insertAt, FirstPinTile());   // My Apps stays first
}

static Pin DragSourceAsPin() {
    Pin p;
    switch (g_drag.kind) {
        case Kind::AppRow: { const AppEntry& a = g_apps[g_rows[g_drag.index].app]; p.id = a.id; p.name = a.name; break; }
        case Kind::RecentRow: p = PinForPath(g_recent[g_drag.index].path); if (g_recent[g_drag.index].dir) p.type = PinType::Folder; break;
        case Kind::Result: {
            const Res& r = g_res[g_resRows[g_drag.index].res];
            if (r.kind == RKind::App) { p.id = r.target; p.name = r.title; }
            else { p = PinForPath(r.target); if (r.dir) p.type = PinType::Folder; }
            break;
        }
        default: break;
    }
    return p;
}

static int TileToPinIndex(int tile) {
    if (tile >= (int)g_tiles.size()) return (int)g_config.pins.size();
    return std::max(0, g_tiles[tile].pin);
}

static void FinishDrop() {
    DragState d = g_drag;
    g_drag = DragState{};
    if (!d.active) return;
    if (d.kind == Kind::GroupChild) {
        Pin* g = TilePin(g_openTile);
        if (!g) return;
        int gi = g_tiles[g_openTile].pin;
        if (d.outOfGroup) {
            Pin moved = g->items[d.index];
            g->items.erase(g->items.begin() + d.index);
            g_config.pins.insert(g_config.pins.begin() + gi + 1, moved);
            if (g_config.pins[gi].items.empty()) g_config.pins.erase(g_config.pins.begin() + gi);
            g_openTile = -1;
        } else if (d.insertAt >= 0) {
            int to = d.insertAt;
            Pin moved = g->items[d.index];
            g->items.erase(g->items.begin() + d.index);
            if (to > d.index) --to;
            to = std::clamp(to, 0, (int)g->items.size());
            g->items.insert(g->items.begin() + to, moved);
        } else return;
        SaveAndRefresh();
        return;
    }
    if (d.mergeTile < 0 && d.insertAt < 0) { Redraw(); return; }

    // the item being dropped
    Pin moving;
    int fromPin = -1;
    if (d.kind == Kind::Pin) {
        fromPin = g_tiles[d.index].pin;
        moving = g_config.pins[fromPin];
    } else {
        moving = DragSourceAsPin();
        if (moving.id.empty()) return;
        if (g_config.IsPinned(moving.id)) { Flash(L"Already pinned"); Redraw(); return; }
    }

    if (d.mergeTile >= 0) {
        int target = g_tiles[d.mergeTile].pin;
        if (target < 0) return;
        Pin& t = g_config.pins[target];
        if (t.type == PinType::Group) {
            t.items.push_back(moving);
        } else {
            Pin group;
            group.type = PinType::Group;
            group.label = L"Folder";
            group.items = {t, moving};
            t = group;
        }
        if (fromPin >= 0) g_config.pins.erase(g_config.pins.begin() + fromPin);
        SaveAndRefresh();
        return;
    }

    int to = TileToPinIndex(d.insertAt);
    if (fromPin >= 0) {
        if (to == fromPin || to == fromPin + 1) { Redraw(); return; }
        g_config.pins.erase(g_config.pins.begin() + fromPin);
        if (to > fromPin) --to;
    }
    to = std::clamp(to, 0, (int)g_config.pins.size());
    g_config.pins.insert(g_config.pins.begin() + to, moving);
    SaveAndRefresh();
}

// ============================================================ keyboard
static void OnQueryChanged() {
    g_jumpMode = false;
    g_openTile = -1;
    g_scroll[RResults] = g_scrollTarget[RResults] = 0;
    RebuildResults();
    Layout();
    g_sel = -1;
    for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].kind == Kind::Result) { g_sel = i; break; }
    g_caretOn = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    Announce();
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

static void MoveGrid(int dir, bool horizontal, Kind kind, int count, int cols) {
    int i = g_items[g_sel].index;
    int n = horizontal ? i + dir : i + dir * cols;
    if (n >= 0 && n < count) g_sel = FindItem(kind, n);
    else if (!horizontal && dir > 0 && (i / cols) < (count - 1) / cols) g_sel = FindItem(kind, count - 1);
}

static void MoveSelection(int dir, bool horizontal) {
    if (Searching()) {
        if (!horizontal) MoveLinear(dir, RResults);
    } else if (g_openTile >= 0) {
        int n = GroupChildCount(g_openTile);
        int cols = std::max(3, std::min(5, (int)((g_panel.right - g_panel.left - 80) / g_m.tileW)));
        if (g_sel < 0 || g_items[g_sel].kind != Kind::GroupChild) g_sel = n ? FindItem(Kind::GroupChild, 0) : -1;
        else MoveGrid(dir, horizontal, Kind::GroupChild, n, cols);
    } else if (g_m.classic) {
        if (!horizontal) MoveLinear(dir, RList);
    } else {
        int tiles = (int)g_tiles.size();
        int cols = g_m.cols;
        auto firstAppRow = [&]() { for (int r = 0; r < (int)g_rows.size(); ++r) if (!g_rows[r].header) return r; return -1; };
        Kind k = g_sel >= 0 ? g_items[g_sel].kind : Kind::User;
        if (g_sel < 0 || (k != Kind::Pin && k != Kind::AppRow && k != Kind::RecentRow)) {
            g_sel = tiles ? FindItem(Kind::Pin, 0) : (RecentShown() ? FindItem(Kind::RecentRow, 0) : FindItem(Kind::AppRow, firstAppRow()));
        } else if (k == Kind::Pin) {
            int i = g_items[g_sel].index;
            if (horizontal) {
                int col = i % cols;
                if (dir > 0 && (col == cols - 1 || i + 1 >= tiles)) { int r = firstAppRow(); if (r >= 0) g_sel = FindItem(Kind::AppRow, r); }
                else g_sel = FindItem(Kind::Pin, std::clamp(i + dir, 0, tiles - 1));
            } else {
                int nx = i + dir * cols;
                if (nx >= 0 && nx < tiles) g_sel = FindItem(Kind::Pin, nx);
                else if (dir > 0 && (i / cols) < (tiles - 1) / cols) g_sel = FindItem(Kind::Pin, tiles - 1);
                else if (dir > 0 && RecentShown()) g_sel = FindItem(Kind::RecentRow, 0);
            }
        } else if (k == Kind::RecentRow) {
            int i = g_items[g_sel].index;
            if (horizontal) { if (dir > 0) { int r = firstAppRow(); if (r >= 0) g_sel = FindItem(Kind::AppRow, r); } }
            else if (dir < 0 && i == 0 && tiles) g_sel = FindItem(Kind::Pin, ((tiles - 1) / cols) * cols);
            else g_sel = FindItem(Kind::RecentRow, std::clamp(i + dir, 0, RecentShown() - 1));
        } else {
            int r = g_items[g_sel].index;
            if (horizontal) { if (dir < 0) g_sel = tiles ? FindItem(Kind::Pin, 0) : FindItem(Kind::RecentRow, 0); }
            else {
                int nx = r;
                do { nx += dir; } while (nx >= 0 && nx < (int)g_rows.size() && g_rows[nx].header);
                if (nx >= 0 && nx < (int)g_rows.size()) g_sel = FindItem(Kind::AppRow, nx);
            }
        }
    }
    EnsureVisible(g_sel);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    Announce();
}

static void PageList(int dir) {
    int region = Searching() ? RResults : (g_openTile >= 0 ? RGroup : RList);
    g_scrollTarget[region] = std::clamp(g_scrollTarget[region] + dir * (g_regionRc[region].bottom - g_regionRc[region].top) * 0.9f, 0.0f, MaxScroll(region));
    if (!Animated()) g_scroll[region] = g_scrollTarget[region];
    g_sel = -1;
    Redraw();
    StartAnim();
}

static void ToggleColumn() {
    if (Searching() || g_m.classic || g_openTile >= 0) { MoveSelection(+1, false); return; }
    bool inLeft = g_sel >= 0 && (g_items[g_sel].kind == Kind::Pin || g_items[g_sel].kind == Kind::RecentRow);
    if (inLeft || (g_tiles.empty() && !RecentShown())) {
        for (int r = 0; r < (int)g_rows.size(); ++r) if (!g_rows[r].header) { g_sel = FindItem(Kind::AppRow, r); break; }
    } else {
        g_sel = g_tiles.empty() ? FindItem(Kind::RecentRow, 0) : FindItem(Kind::Pin, 0);
    }
    EnsureVisible(g_sel);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    Announce();
}

static bool OnKeyDown(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
        case VK_ESCAPE:
            if (g_drag.active) { g_drag = DragState{}; ReleaseCapture(); Redraw(); }
            else if (g_jumpMode) { g_jumpMode = false; Redraw(); }
            else if (g_openTile >= 0 && !Searching()) CloseGroup();
            else if (!g_query.empty()) { g_query.clear(); g_selectAll = false; OnQueryChanged(); }
            else MenuHide();
            return true;
        case VK_RETURN: {
            int idx = g_sel;
            if (idx < 0 && Searching()) for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].kind == Kind::Result) { idx = i; break; }
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
        case VK_F10:
            if (vk == VK_F10 && !shift) break;
            if (g_sel >= 0) ContextMenuFor(g_sel, false);
            return true;
        case VK_F2:
            if (g_sel >= 0 && g_items[g_sel].kind == Kind::Pin) { if (Pin* p = TilePin(g_items[g_sel].index)) RenamePin(*p, L"Rename"); }
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
        case 'C':
            if (ctrl && g_sel >= 0 && g_items[g_sel].kind == Kind::Result) {
                const Res& r = g_res[g_resRows[g_items[g_sel].index].res];
                if (r.kind == RKind::Calc || r.kind == RKind::File) { CopyText(r.target); Flash(L"Copied"); }
                return true;
            }
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
static void OnMouseMove(float x, float y, WPARAM keys) {
    if (!g_tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_hwnd, 0};
        TrackMouseEvent(&tme);
        g_tracking = true;
    }
    if (g_drag.pressed && (keys & MK_LBUTTON)) {
        g_drag.x = x; g_drag.y = y;
        if (!g_drag.active && (std::fabs(x - g_drag.startX) > 6 || std::fabs(y - g_drag.startY) > 6) && Draggable(g_drag.kind, g_drag.index)) {
            g_drag.active = true;
            g_pressKey = ~0ull;
        }
        if (g_drag.active) {
            UpdateDropTarget();
            // auto-scroll near the edges of the pinned area
            Region r = g_m.classic ? RList : RPins;
            const D2D1_RECT_F& reg = g_regionRc[r];
            if (y < reg.top + 20 && InRect(reg, reg.left + 1, reg.top + 1)) g_scrollTarget[r] = std::max(0.0f, g_scrollTarget[r] - 12);
            else if (y > reg.bottom - 20 && y < reg.bottom + 10) g_scrollTarget[r] = std::min(MaxScroll(r), g_scrollTarget[r] + 12);
            StartAnim();
            return;
        }
    }
    int h = HitTest(x, y);
    if (g_jumpMode && h >= 0 && g_items[h].region != RNone) h = -1;
    if (h >= 0 && (g_items[h].kind == Kind::Section || g_items[h].kind == Kind::ResHeader)) h = -1;
    if (h != g_hot) {
        g_hot = h;
        if (Animated()) StartAnim();
        else { AnimTick(); InvalidateRect(g_hwnd, nullptr, FALSE); }
    }
}

static void OnWheel(float x, float y, int delta) {
    int region = -1;
    for (int r = 1; r < RegionCount; ++r) if (InRect(g_regionRc[r], x, y)) region = r;
    if (g_openTile >= 0 && !Searching()) region = RGroup;
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

// ============================================================ icon cache
static void LoadIconCache() {
    std::vector<CachedIcon> cached;
    bool stale = false;
    if (!IconCacheLoad(g_iconPx, cached, &stale)) return;
    for (auto& c : cached) {
        IconData& d = g_icons[c.id];
        d.size = c.size;
        d.pixels = std::move(c.pixels);
    }
    Log(L"Icon cache: %u icons%ls", (unsigned)cached.size(), stale ? L" (refreshing, older than a week)" : L"");
    g_iconsStale = stale;
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
        case WM_GETOBJECT: return a11y::OnGetObject(hwnd, wParam, lParam);
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            return 0;
        case WM_DPICHANGED:
            g_dpi = HIWORD(wParam);
            if (g_rt) g_rt->SetDpi((float)g_dpi, (float)g_dpi);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !g_preview) {
                // our own popups (input box, file pickers) take focus briefly; keep the menu for those
                HWND to = (HWND)lParam;
                DWORD pid = 0;
                if (to) GetWindowThreadProcessId(to, &pid);
                if (!to || pid != GetCurrentProcessId()) MenuHide();
            }
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
                if (!AnimTick() && !g_drag.active) { KillTimer(hwnd, TIMER_ANIM); g_animTimer = false; }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wParam == TIMER_SAVEICONS) {
                KillTimer(hwnd, TIMER_SAVEICONS);
                IconCacheSave(g_iconPx, CollectIcons());
            } else if (wParam == TIMER_FLASH) {
                KillTimer(hwnd, TIMER_FLASH);
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
        case WM_MOUSEMOVE: OnMouseMove(Dip((short)LOWORD(lParam)), Dip((short)HIWORD(lParam)), wParam); return 0;
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
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            int h = HitTest(x, y);
            g_drag = DragState{};
            if (h >= 0 && !g_jumpMode) {
                g_pressKey = Key(g_items[h].kind, g_items[h].index);
                g_drag.pressed = true;
                g_drag.kind = g_items[h].kind;
                g_drag.index = g_items[h].index;
                g_drag.startX = g_drag.x = x;
                g_drag.startY = g_drag.y = y;
                StartAnim();
            }
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            uint64_t pressed = g_pressKey;
            g_pressKey = ~0ull;
            if (g_drag.active) { FinishDrop(); return 0; }
            g_drag = DragState{};
            if (g_m.full && !InRect(g_panel, x, y)) { MenuHide(); return 0; }
            if (g_jumpMode) {
                int h = HitTest(x, y);
                if (h >= 0 && g_items[h].region == RNone) { g_jumpMode = false; Activate(h, false); return 0; }
                int letter = JumpHit(x, y);
                if (letter > 0) JumpTo((wchar_t)letter);
                else { g_jumpMode = false; Redraw(); }
                return 0;
            }
            int h = HitTest(x, y);
            if (g_openTile >= 0 && !Searching() && h < 0 && !InRect(g_groupCard, x, y)) { CloseGroup(); return 0; }
            if (h >= 0 && Key(g_items[h].kind, g_items[h].index) == pressed)
                Activate(h, (GetKeyState(VK_CONTROL) < 0) && (GetKeyState(VK_SHIFT) < 0));
            else StartAnim();
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            int h = g_jumpMode ? -1 : HitTest(x, y);
            POINT pt; GetCursorPos(&pt);
            if (h >= 0 && IsSelectable(g_items[h].kind)) ContextMenuFor(h, true);
            else if (!Searching() && g_openTile < 0 && InPinsArea(x, y)) ShowPinsAreaMenu(pt);
            else if (InRect(g_panel, x, y)) ShowBackgroundMenu(pt);
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
        case WM_FOYER_OFFER_INSTALL: MaybeOfferInstall(nullptr); return 0;
        case WM_FOYER_REFRESH_START: StartButtonRefreshAsync(nullptr); return 0;
        case WM_FOYER_START_FOUND: return 0;
        case WM_FOYER_APPS_READY: {
            auto* res = reinterpret_cast<AppListResult*>(lParam);
            g_apps = std::move(res->apps);
            delete res;
            g_appById.clear();
            for (int i = 0; i < (int)g_apps.size(); ++i) g_appById[ToLower(g_apps[i].id)] = i;
            bool changed = false;
            auto refreshName = [&](Pin& p) {
                if (p.type != PinType::App) return;
                if (const AppEntry* a = AppById(p.id)) if (a->name != p.name) { p.name = a->name; changed = true; }
            };
            for (auto& p : g_config.pins) { refreshName(p); for (auto& c : p.items) refreshName(c); }
            if (changed) ConfigSave();
            g_appsLoaded = true;
            RebuildRows();
            if (Searching()) RebuildResults();
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
            SetTimer(hwnd, TIMER_SAVEICONS, 15000, nullptr);
            if (MenuVisible()) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_FOYER_FILES_READY: {
            auto* fr = reinterpret_cast<search::FileResults*>(lParam);
            if (fr->gen == g_searchGen && Searching()) {
                g_fileHits = std::move(fr->hits);
                g_filesPending = false;
                int selKind = g_sel >= 0 ? (int)g_items[g_sel].kind : -1;
                int selRes = (g_sel >= 0 && g_items[g_sel].kind == Kind::Result) ? g_resRows[g_items[g_sel].index].res : -1;
                MergeFileResults();
                Layout();
                // keep the selection on the same result when files arrive
                g_sel = -1;
                if (selKind == (int)Kind::Result && selRes >= 0)
                    for (int i = 0; i < (int)g_items.size(); ++i)
                        if (g_items[i].kind == Kind::Result && g_resRows[g_items[i].index].res == selRes) { g_sel = i; break; }
                if (g_sel < 0) for (int i = 0; i < (int)g_items.size(); ++i) if (g_items[i].kind == Kind::Result) { g_sel = i; break; }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            delete fr;
            return 0;
        }
        case WM_FOYER_RECENT_READY: {
            auto* rr = reinterpret_cast<RecentResult*>(lParam);
            if (g_config.recentEnabled) {
                bool same = rr->items.size() == g_recent.size();
                for (size_t i = 0; same && i < g_recent.size(); ++i) same = rr->items[i].path == g_recent[i].path;
                g_recent = std::move(rr->items);
                if (!same && MenuVisible() && !g_drag.active) {
                    int selKind = g_sel >= 0 ? (int)g_items[g_sel].kind : -1, selIdx = g_sel >= 0 ? g_items[g_sel].index : -1;
                    Layout();
                    g_sel = selKind >= 0 ? FindItem((Kind)selKind, selIdx) : -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            delete rr;
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
            if (lParam && (wcscmp((const wchar_t*)lParam, L"ImmersiveColorSet") == 0 || wcscmp((const wchar_t*)lParam, L"WindowsThemeElement") == 0))
                MenuApplySettings();   // light/dark or high contrast switched
            if (wParam == SPI_SETHIGHCONTRAST || wParam == SPI_SETCLIENTAREAANIMATION) MenuApplySettings();
            StartButtonRefreshAsync(nullptr);
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            MenuApplySettings();
            return 0;
        case WM_DESTROY:
            IconCacheSave(g_iconPx, CollectIcons());
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
    a11y::Attach(g_hwnd, [] { if (g_sel >= 0) Activate(g_sel, false); });

    g_theme = BuildTheme(g_config);
    ApplyWindowTheme(g_hwnd, g_theme, true);
    ApplyMenuTheme(g_theme.dark);
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    ComputeMetrics(work);
    EnsureTarget();
    RebuildTiles();          // pins show right away, before the app list has loaded
    LoadIconCache();
    MaybeRefreshApps(true);
    MaybeRefreshRecent(true);
    return g_hwnd;
}
