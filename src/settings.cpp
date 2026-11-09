// Foyer - settings window (custom drawn, Mica, changes apply live)
#include "common.h"
#include "config.h"
#include "gfx.h"
#include "updater.h"
#include "apps.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <functional>
#include <map>
#include <algorithm>
#include <cmath>
#include <ctime>

using namespace gfx;

// ============================================================ control model
enum class CType { Header, Toggle, Choice, Slider, Swatches, Buttons, Info, ThemeGrid, LayoutGrid, PinRow, HiddenRow, Notes, UpdateCard, Hero };

struct Ctl {
    CType type;
    std::wstring label, desc;
    std::vector<std::wstring> options;
    std::function<int()> geti;
    std::function<void(int)> seti;
    std::function<bool()> getb;
    std::function<void(bool)> setb;
    std::function<float()> getf;
    std::function<void(float)> setf;
    float fmin = 0, fmax = 1;
    std::function<std::wstring(float)> fmt;
    std::vector<std::pair<std::wstring, std::function<void()>>> buttons;
    int primary = -1;              // index of the accent button
    int index = 0;                 // pin / hidden index
    bool enabled = true;
};

struct Zone { int ctl; int part; D2D1_RECT_F rc; };

static HWND g_hwnd;
static UINT g_dpi = 96;
static Theme g_theme;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static Canvas g_cv;
static IDWriteTextFormat *g_fTitle, *g_fBrand, *g_fNav, *g_fLabel, *g_fDesc, *g_fHeader, *g_fBtn, *g_fIcon, *g_fIconBig,
    *g_fSmallC, *g_fNotes, *g_fHero, *g_fCard;
static int g_page = PageGeneral;
static std::vector<Ctl> g_ctls;
static std::vector<Zone> g_zones;
static int g_hotCtl = -1, g_hotPart = -1;
static int g_dragCtl = -1;
static float g_scroll = 0, g_contentH = 0, g_viewH = 0;
static bool g_tracking = false;
static bool g_previewOn = false;
static std::map<std::wstring, float> g_anim;   // toggle knob positions
static bool g_animTimer = false;
constexpr UINT_PTR TIMER_SANIM = 1;

static const float kSidebarW = 240;
static const wchar_t* kPageNames[PageCount] = {L"General", L"Appearance", L"Layout", L"Apps", L"Updates", L"About"};
static const wchar_t kPageGlyphs[PageCount] = {0xE80F, 0xE790, 0xF0E2, 0xE71D, 0xE895, 0xE946};

static float Dip(int px) { return px * 96.0f / g_dpi; }
static int Px(float d) { return (int)std::lround(d * g_dpi / 96.0f); }
static bool InR(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

bool SettingsIsOpen() { return g_hwnd && IsWindow(g_hwnd); }

// ============================================================ applying changes
static void ApplyTheme();

static void Changed(bool save = true) {
    if (save) ConfigSave();
    g_interceptWinKey = g_config.interceptWinKey;
    g_interceptStartButton = g_config.interceptStartButton;
    MenuApplySettings();
    AutoUpdateApplySettings();
    ApplyTheme();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SetPreview(bool on) {
    g_previewOn = on;
    MenuSetPreview(on);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static std::wstring PickFile(bool save) {
    wchar_t path[MAX_PATH] = L"Foyer-backup.json";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Foyer backup (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    if (!save) path[0] = 0;
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? std::wstring(path) : L"";
}

// ============================================================ pages
static Ctl Header(const wchar_t* t) { Ctl c{CType::Header}; c.label = t; return c; }

static Ctl Toggle(const wchar_t* label, const wchar_t* desc, std::function<bool()> get, std::function<void(bool)> set) {
    Ctl c{CType::Toggle}; c.label = label; c.desc = desc ? desc : L""; c.getb = get; c.setb = set; return c;
}

static Ctl Choice(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts, std::function<int()> get, std::function<void(int)> set) {
    Ctl c{CType::Choice}; c.label = label; c.desc = desc ? desc : L""; c.options = std::move(opts); c.geti = get; c.seti = set; return c;
}

static Ctl Buttons(const wchar_t* label, const wchar_t* desc, std::vector<std::pair<std::wstring, std::function<void()>>> b, int primary = -1) {
    Ctl c{CType::Buttons}; c.label = label ? label : L""; c.desc = desc ? desc : L""; c.buttons = std::move(b); c.primary = primary; return c;
}

static Ctl Info(const std::wstring& text) { Ctl c{CType::Info}; c.desc = text; return c; }

static int IndexOf(const std::wstring& v, std::initializer_list<const wchar_t*> list) {
    int i = 0;
    for (auto s : list) { if (v == s) return i; ++i; }
    return 0;
}

static Ctl PreviewButton() {
    return Buttons(L"Live preview", L"Shows Foyer next to this window and updates it as you change things.",
                   {{g_previewOn ? L"Hide preview" : L"Show preview", [] { SetPreview(!g_previewOn); }}}, g_previewOn ? -1 : 0);
}

static void BuildGeneral(std::vector<Ctl>& v) {
    v.push_back(Header(L"Open Foyer with"));
    v.push_back(Toggle(L"Windows key", L"Tap Win to open Foyer. Shortcuts like Win+E, Win+D and Win+R keep working.",
                       [] { return g_config.interceptWinKey; }, [](bool b) { g_config.interceptWinKey = b; Changed(); }));
    v.push_back(Toggle(L"Start button", L"Clicking the taskbar Start button opens Foyer.",
                       [] { return g_config.interceptStartButton; }, [](bool b) { g_config.interceptStartButton = b; Changed(); }));
    v.push_back(Toggle(L"Pause Foyer", L"Hands the Windows key and Start button back to Windows until you turn this off.",
                       [] { return g_paused.load(); }, [](bool b) { g_paused = b; TrayUpdateTip(); Changed(false); }));
    v.push_back(Header(L"Startup"));
    v.push_back(Toggle(L"Start with Windows", L"Foyer starts quietly in the tray when you sign in.",
                       [] { return StartWithWindowsEnabled(); }, [](bool b) { SetStartWithWindows(b); Changed(false); }));
    v.push_back(Header(L"Personal touch"));
    v.push_back(Toggle(L"Greeting", L"\"Good evening\" with your name and today's date at the top of the menu.",
                       [] { return g_config.showGreeting; }, [](bool b) { g_config.showGreeting = b; Changed(); }));
    v.push_back(Header(L"Safety net"));
    v.push_back(Buttons(L"Original Start menu", L"Ctrl+Win, Ctrl+Esc or Shift+click on the Start button always open the Windows Start menu.",
                        {{L"Open it now", [] { PostMessageW(MenuWindow(), WM_FOYER_ORIGINAL, 0, 0); }}}));
}

static void BuildAppearance(std::vector<Ctl>& v) {
    v.push_back(PreviewButton());
    v.push_back(Header(L"Theme"));
    v.push_back(Ctl{CType::ThemeGrid});
    v.push_back(Header(L"Color"));
    Ctl sw{CType::Swatches};
    sw.label = L"Accent color";
    sw.desc = L"The first swatch follows your Windows accent color.";
    v.push_back(sw);
    v.push_back(Toggle(L"Accent glow", L"A soft wash of your accent color behind the menu.",
                       [] { return g_config.accentGlow; }, [](bool b) { g_config.accentGlow = b; Changed(); }));
    v.push_back(Header(L"Material"));
    bool supported = SystemBackdropSupported();
    Ctl bg = Choice(L"Background", supported ? L"Acrylic blurs what's behind the menu, like the Windows Start menu. Mica tints it with your wallpaper."
                                             : L"Mica and acrylic need Windows 11 version 22H2 or newer.",
                    {L"Solid", L"Mica", L"Acrylic"},
                    [] { return IndexOf(g_config.backdrop, {L"solid", L"mica", L"acrylic"}); },
                    [](int i) { const wchar_t* o[] = {L"solid", L"mica", L"acrylic"}; g_config.backdrop = o[i]; Changed(); });
    bg.enabled = supported;
    v.push_back(bg);
    Ctl tint{CType::Slider};
    tint.label = L"Tint";
    tint.desc = L"Lower values let more of the background show through.";
    tint.fmin = 0.4f; tint.fmax = 1.0f;
    tint.getf = [] { return g_config.opacity; };
    tint.setf = [](float f) { g_config.opacity = f; Changed(false); };
    tint.fmt = [](float f) { return std::to_wstring((int)std::lround(f * 100)) + L"%"; };
    tint.enabled = supported && g_config.backdrop != L"solid";
    v.push_back(tint);
    Ctl rad{CType::Slider};
    rad.label = L"Corner radius";
    rad.desc = L"Roundness of tiles, rows and the menu itself.";
    rad.fmin = 0; rad.fmax = 16;
    rad.getf = [] { return g_config.cornerRadius; };
    rad.setf = [](float f) { g_config.cornerRadius = std::round(f); Changed(false); };
    rad.fmt = [](float f) { return std::to_wstring((int)std::lround(f)) + L" px"; };
    v.push_back(rad);
    v.push_back(Header(L"Motion"));
    v.push_back(Choice(L"Animations", L"Smooth adds a slide-in, staggered tiles and gliding hover. None is the fastest.",
                       {L"None", L"Fast", L"Smooth"},
                       [] { return IndexOf(g_config.animation, {L"none", L"fast", L"smooth"}); },
                       [](int i) { const wchar_t* o[] = {L"none", L"fast", L"smooth"}; g_config.animation = o[i]; Changed(); }));
}

static void BuildLayout(std::vector<Ctl>& v) {
    v.push_back(PreviewButton());
    v.push_back(Header(L"Style"));
    v.push_back(Ctl{CType::LayoutGrid});
    v.push_back(Header(L"Size and placement"));
    v.push_back(Choice(L"Position", L"Where the menu opens (Full screen always fills the screen).", {L"Next to taskbar", L"Center"},
                       [] { return IndexOf(g_config.position, {L"taskbar", L"center"}); },
                       [](int i) { g_config.position = i ? L"center" : L"taskbar"; Changed(); }));
    v.push_back(Choice(L"Density", L"Compact fits more apps on screen.", {L"Compact", L"Comfortable"},
                       [] { return IndexOf(g_config.density, {L"compact", L"comfortable"}); },
                       [](int i) { g_config.density = i ? L"comfortable" : L"compact"; Changed(); }));
    v.push_back(Choice(L"Icon size", nullptr, {L"Small", L"Medium", L"Large"},
                       [] { return IndexOf(g_config.iconSize, {L"small", L"medium", L"large"}); },
                       [](int i) { const wchar_t* o[] = {L"small", L"medium", L"large"}; g_config.iconSize = o[i]; Changed(); }));
}

static void BuildApps(std::vector<Ctl>& v) {
    v.push_back(Header(L"Pinned apps"));
    if (g_config.pins.empty()) v.push_back(Info(L"Nothing pinned yet. Right-click any app in Foyer and choose Pin to Foyer."));
    for (int i = 0; i < (int)g_config.pins.size(); ++i) {
        Ctl c{CType::PinRow};
        c.label = g_config.pins[i].name.empty() ? g_config.pins[i].id : g_config.pins[i].name;
        c.index = i;
        v.push_back(c);
    }
    v.push_back(Buttons(nullptr, nullptr, {{L"Reset pins to defaults", [] {
        if (MessageBoxW(g_hwnd, L"Replace your pinned apps with Foyer's defaults?", L"Foyer", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            ConfigDefaultPins(); Changed();
        }
    }}}));
    v.push_back(Header(L"Hidden apps"));
    if (g_config.hidden.empty()) v.push_back(Info(L"No hidden apps. Right-click an app in Foyer and choose Hide from Foyer to declutter."));
    for (int i = 0; i < (int)g_config.hidden.size(); ++i) {
        Ctl c{CType::HiddenRow};
        std::wstring id = g_config.hidden[i];
        size_t bang = id.find(L'!');
        size_t slash = id.find_last_of(L"\\/");
        c.label = bang != std::wstring::npos ? id.substr(0, std::min(id.find(L'_'), bang)) : slash != std::wstring::npos ? id.substr(slash + 1) : id;
        c.desc = id;
        c.index = i;
        v.push_back(c);
    }
    v.push_back(Header(L"App list"));
    v.push_back(Buttons(L"Refresh", L"Foyer refreshes automatically; use this right after installing something.",
                        {{L"Refresh app list", [] { MenuRefreshApps(); }}}));
}

static std::wstring CleanNotes(const std::wstring& md) {
    std::wstring out;
    size_t i = 0;
    while (i < md.size()) {
        size_t nl = md.find(L'\n', i);
        std::wstring line = md.substr(i, nl == std::wstring::npos ? std::wstring::npos : nl - i);
        i = nl == std::wstring::npos ? md.size() : nl + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t k = 0;
        while (k < line.size() && line[k] == L'#') ++k;
        if (k) line = line.substr(line.find_first_not_of(L' ', k) == std::wstring::npos ? line.size() : line.find_first_not_of(L' ', k));
        if (line.size() > 1 && (line[0] == L'-' || line[0] == L'*') && line[1] == L' ') line = L"\x2022 " + line.substr(2);
        std::wstring clean;
        for (wchar_t c : line) if (c != L'*' && c != L'`') clean += c;
        out += clean + L"\n";
    }
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    return out;
}

static void BuildUpdates(std::vector<Ctl>& v) {
    v.push_back(Ctl{CType::UpdateCard});
    v.push_back(Header(L"Automatic updates"));
    v.push_back(Toggle(L"Check for updates automatically", L"Foyer quietly checks GitHub Releases in the background.",
                       [] { return g_config.autoCheck; }, [](bool b) { g_config.autoCheck = b; Changed(); }));
    v.push_back(Choice(L"Check every", nullptr, {L"Hour", L"6 hours", L"Day"},
                       [] { return g_config.checkHours <= 1 ? 0 : g_config.checkHours <= 6 ? 1 : 2; },
                       [](int i) { const int h[] = {1, 6, 24}; g_config.checkHours = h[i]; Changed(); }));
    v.push_back(Toggle(L"Install updates automatically", L"Downloads, verifies and installs in the background. Never while you're gaming, watching fullscreen video or presenting.",
                       [] { return g_config.autoInstall; }, [](bool b) { g_config.autoInstall = b; Changed(); }));
    v.push_back(Choice(L"Channel", L"Beta gets new features first and may be less polished.", {L"Stable", L"Beta"},
                       [] { return g_config.channel == L"beta" ? 1 : 0; },
                       [](int i) { g_config.channel = i ? L"beta" : L"stable"; Changed(); }));
    updater::Status st = updater::Get();
    if (!st.release.notes.empty()) {
        v.push_back(Header((L"What's new in " + st.release.version).c_str()));
        Ctl n{CType::Notes};
        n.desc = CleanNotes(st.release.notes);
        v.push_back(n);
    }
}

static void BuildAbout(std::vector<Ctl>& v) {
    v.push_back(Ctl{CType::Hero});
    v.push_back(Buttons(L"Project", L"Source code, releases and issue tracker on GitHub.",
                        {{L"Open GitHub", [] { ShellOpen(L"https://github.com/larosa023/Foyer"); }},
                         {L"Release notes", [] { ShellOpen(L"https://github.com/larosa023/Foyer/releases"); }}}));
    v.push_back(Header(L"Your data"));
    v.push_back(Buttons(L"Backup", L"Save all settings, pins, hidden apps and theme choices to one file.",
                        {{L"Back up\x2026", [] {
                              std::wstring p = PickFile(true);
                              if (!p.empty()) MessageBoxW(g_hwnd, ConfigExportTo(p) ? L"Backup saved." : L"Couldn't save the backup.", L"Foyer", MB_ICONINFORMATION);
                          }},
                         {L"Restore\x2026", [] {
                              std::wstring p = PickFile(false);
                              if (p.empty()) return;
                              std::wstring err;
                              if (ConfigImportFrom(p, &err)) { Changed(false); MenuReloadConfig(); }
                              else MessageBoxW(g_hwnd, err.c_str(), L"Foyer", MB_ICONWARNING);
                          }}}));
    v.push_back(Buttons(L"Files", L"Settings live in config.json; foyer.log helps diagnose problems.",
                        {{L"Open data folder", [] { ShellOpen(DataDir().c_str()); }},
                         {L"Open log", [] { ShellOpen((DataDir() + L"\\foyer.log").c_str()); }}}));
    v.push_back(Header(L"Keyboard"));
    v.push_back(Info(L"Type to search  \x00B7  Arrows to move  \x00B7  Tab to switch columns  \x00B7  Enter to open\n"
                     L"Ctrl+Shift+Enter run as admin  \x00B7  Menu key for options  \x00B7  Ctrl+, settings  \x00B7  Esc to clear or close\n"
                     L"Ctrl+Win, Ctrl+Esc or Shift+click Start  \x2192  original Windows Start menu"));
}

static void BuildPage() {
    g_ctls.clear();
    switch (g_page) {
        case PageGeneral: BuildGeneral(g_ctls); break;
        case PageAppearance: BuildAppearance(g_ctls); break;
        case PageLayout: BuildLayout(g_ctls); break;
        case PageApps: BuildApps(g_ctls); break;
        case PageUpdates: BuildUpdates(g_ctls); break;
        case PageAbout: BuildAbout(g_ctls); break;
    }
}

// ============================================================ resources
static void ReleaseFormats() {
    IDWriteTextFormat** all[] = {&g_fTitle, &g_fBrand, &g_fNav, &g_fLabel, &g_fDesc, &g_fHeader, &g_fBtn, &g_fIcon, &g_fIconBig, &g_fSmallC, &g_fNotes, &g_fHero, &g_fCard};
    for (auto f : all) SafeRelease(*f);
}

static void CreateFormats() {
    ReleaseFormats();
    const wchar_t* ui = UiFont();
    g_fTitle   = Format(DisplayFont(), 28, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fBrand   = Format(DisplayFont(), 18, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fNav     = Format(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fLabel   = Format(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fDesc    = Format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
    g_fHeader  = Format(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fBtn     = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIcon    = Format(IconFont(), 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconBig = Format(IconFont(), 26, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fSmallC  = Format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fNotes   = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
    g_fHero    = Format(DisplayFont(), 34, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fCard    = Format(ui, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
}

static void Discard() { SafeRelease(g_br); SafeRelease(g_rt); }

static bool EnsureTarget() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    props.dpiX = props.dpiY = (float)g_dpi;
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom))), &g_rt)))
        return false;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(g_theme.text, &g_br);
    g_cv.rt = g_rt;
    g_cv.br = g_br;
    return g_br != nullptr;
}

static void ApplyTheme() {
    if (!g_hwnd) return;
    g_theme = BuildTheme(g_config);
    Theme win = g_theme;
    win.backdrop = SystemBackdropSupported() ? 2 : 0;   // settings always use Mica when available
    ApplyWindowTheme(g_hwnd, win, false);
    g_theme.backdrop = win.backdrop;
    Discard();
}

// ============================================================ drawing helpers
static void AddZone(int ctl, int part, D2D1_RECT_F rc) { g_zones.push_back({ctl, part, rc}); }
static bool Hot(int ctl, int part) { return g_hotCtl == ctl && g_hotPart == part; }

static float AnimValue(const std::wstring& key, float target) {
    auto it = g_anim.find(key);
    if (it == g_anim.end()) { g_anim[key] = target; return target; }
    return it->second;
}

static void StartAnim() {
    if (!g_animTimer && g_hwnd) { SetTimer(g_hwnd, TIMER_SANIM, 10, nullptr); g_animTimer = true; }
}

static bool StepAnims(std::map<std::wstring, float>& targets) {
    bool moving = false;
    for (auto& kv : targets) {
        float& v = g_anim[kv.first];
        float d = kv.second - v;
        if (std::fabs(d) < 0.01f) v = kv.second;
        else { v += d * 0.35f; moving = true; }
    }
    return moving;
}

static std::map<std::wstring, float> g_animTargets;

static float DrawButton(float right, float cy, const std::wstring& text, bool primary, int ctl, int part, bool enabled = true) {
    float w = MeasureWidth(text, g_fBtn) + 32;
    D2D1_RECT_F r = D2D1::RectF(right - w, cy - 16, right, cy + 16);
    bool hot = Hot(ctl, part) && enabled;
    const Theme& t = g_theme;
    if (primary) {
        g_cv.FillRound(r, 16, Alpha(hot ? Mix(t.accent, D2D1::ColorF(1, 1, 1, 1), 0.12f) : t.accent, enabled ? 1.0f : 0.4f));
        g_cv.Text(text, g_fBtn, r, t.onAccent);
    } else {
        g_cv.FillRound(r, 16, hot ? t.surfaceStrong : Alpha(t.surface, 0.9f));
        g_cv.StrokeRound(r, 16, t.divider);
        g_cv.Text(text, g_fBtn, r, enabled ? t.text : Alpha(t.subtext, 0.6f));
    }
    if (enabled) AddZone(ctl, part, r);
    return w;
}

static float CardHeight(const Ctl& c, float width) {
    switch (c.type) {
        case CType::Header: return 40;
        case CType::ThemeGrid: { int perRow = std::max(1, (int)((width + 12) / 124)); int rows = (kThemePresetCount + perRow - 1) / perRow; return rows * 112.0f; }
        case CType::LayoutGrid: return 150;
        case CType::PinRow: case CType::HiddenRow: return 52;
        case CType::Info: return std::max(52.0f, MeasureHeight(c.desc, g_fDesc, width - 40) + 32);
        case CType::Notes: return MeasureHeight(c.desc, g_fNotes, width - 40) + 32;
        case CType::UpdateCard: return 148;
        case CType::Hero: return 128;
        default: {
            float descH = c.desc.empty() ? 0 : MeasureHeight(c.desc, g_fDesc, width * 0.55f);
            return std::max(56.0f, 30 + descH + 14);
        }
    }
}

static void DrawThemeCard(float x, float y, float w, float h, int idx, int ctl) {
    const ThemePreset& p = kThemePresets[idx];
    Theme t = BuildThemeNamed(g_config, p.id);
    bool sel = g_config.theme == p.id;
    bool hot = Hot(ctl, idx);
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h - 26);
    float lift = hot ? -2.0f : 0.0f;
    card.top += lift; card.bottom += lift;
    g_cv.FillRound(card, 10, t.bg);
    if (std::wstring(p.id) == L"system") {
        // Split preview: half light, half dark
        Theme lt = BuildThemeNamed(g_config, L"light");
        g_rt->PushAxisAlignedClip(D2D1::RectF(card.left, card.top, (card.left + card.right) / 2, card.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        g_cv.FillRound(card, 10, lt.bg);
        g_rt->PopAxisAlignedClip();
    }
    if (std::wstring(p.id) == L"glass") g_cv.FillRound(card, 10, Alpha(D2D1::ColorF(0.6f, 0.75f, 1.0f, 1.0f), 0.18f));
    // mini menu: glow, search pill, tiles
    g_cv.Circle(card.left + 18, card.top + 14, 22, Alpha(t.accent, 0.22f));
    g_cv.FillRound(D2D1::RectF(card.left + 10, card.top + 10, card.right - 10, card.top + 20), 5, t.surface);
    for (int i = 0; i < 4; ++i) {
        float tx = card.left + 12 + i * ((w - 24) / 4.0f);
        g_cv.FillRound(D2D1::RectF(tx, card.top + 30, tx + 14, card.top + 44), 4, i == 0 ? t.accent : Alpha(t.subtext, 0.5f));
    }
    g_cv.FillRound(D2D1::RectF(card.left + 12, card.top + 52, card.right - 30, card.top + 56), 2, Alpha(t.subtext, 0.35f));
    g_cv.StrokeRound(card, 10, sel ? g_theme.accent : (hot ? Alpha(g_theme.text, 0.35f) : g_theme.divider), sel ? 2.5f : 1.0f);
    if (sel) {
        g_cv.Circle(card.right - 10, card.top + 10, 8, g_theme.accent);
        g_cv.Glyph(0xE73E, g_fSmallC, D2D1::RectF(card.right - 18, card.top + 2, card.right - 2, card.top + 18), g_theme.onAccent);
    }
    g_cv.Text(p.name, g_fSmallC, D2D1::RectF(x, card.bottom + 4, x + w, y + h), sel ? g_theme.text : g_theme.subtext);
    AddZone(ctl, idx, D2D1::RectF(x, y, x + w, y + h));
}

static D2D1_RECT_F Inset_(const D2D1_RECT_F& r, float d) { return D2D1::RectF(r.left + d, r.top + d, r.right - d, r.bottom - d); }

static void DrawLayoutCard(float x, float y, float w, float h, int idx, int ctl) {
    const wchar_t* ids[] = {L"modern", L"classic", L"fullscreen"};
    const wchar_t* names[] = {L"Modern", L"Classic", L"Full screen"};
    bool sel = g_config.layout == ids[idx];
    bool hot = Hot(ctl, idx);
    const Theme& t = g_theme;
    D2D1_RECT_F card = D2D1::RectF(x, y + (hot ? -2.0f : 0.0f), x + w, y + h - 28 + (hot ? -2.0f : 0.0f));
    g_cv.FillRound(card, 10, Alpha(t.surface, 0.9f));
    D2D1_RECT_F s = Inset_(card, 12);
    auto bar = [&](float l, float tp, float r, float b, D2D1_COLOR_F c) { g_cv.FillRound(D2D1::RectF(l, tp, r, b), 3, c); };
    D2D1_COLOR_F dim = Alpha(t.subtext, 0.45f);
    if (idx == 0) {          // two columns: grid + list
        float mid = s.left + (s.right - s.left) * 0.55f;
        bar(s.left, s.top, s.right, s.top + 8, Alpha(t.accent, 0.6f));
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
            float cx = s.left + c * ((mid - s.left) / 3), cy = s.top + 16 + r * 22;
            bar(cx, cy, cx + 14, cy + 14, (r == 0 && c == 0) ? t.accent : dim);
        }
        for (int r = 0; r < 5; ++r) bar(mid + 8, s.top + 16 + r * 13, s.right, s.top + 22 + r * 13, dim);
    } else if (idx == 1) {   // single column list
        float cx0 = s.left + (s.right - s.left) * 0.25f, cx1 = s.left + (s.right - s.left) * 0.75f;
        bar(cx0, s.top, cx1, s.top + 8, Alpha(t.accent, 0.6f));
        for (int r = 0; r < 6; ++r) bar(cx0, s.top + 16 + r * 11, cx1, s.top + 21 + r * 11, r == 0 ? t.accent : dim);
    } else {                 // full screen overlay
        bar(s.left - 6, s.top - 6, s.right + 6, s.bottom + 6, Alpha(t.accent, 0.12f));
        bar(s.left + 14, s.top + 2, s.right - 14, s.top + 10, Alpha(t.accent, 0.6f));
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 6; ++c) {
            float cx = s.left + 14 + c * ((s.right - s.left - 28) / 6), cy = s.top + 18 + r * 20;
            bar(cx, cy, cx + 12, cy + 12, (r == 0 && c == 0) ? t.accent : dim);
        }
    }
    g_cv.StrokeRound(card, 10, sel ? t.accent : (hot ? Alpha(t.text, 0.35f) : t.divider), sel ? 2.5f : 1.0f);
    g_cv.Text(names[idx], g_fSmallC, D2D1::RectF(x, card.bottom + 4, x + w, y + h), sel ? t.text : t.subtext);
    AddZone(ctl, idx, D2D1::RectF(x, y, x + w, y + h));
}

static void DrawUpdateCard(const D2D1_RECT_F& card, int ci) {
    const Theme& t = g_theme;
    updater::Status st = updater::Get();
    using S = updater::State;
    wchar_t glyph = 0xE895;
    D2D1_COLOR_F gc = t.accent;
    std::wstring title = st.message.empty() ? L"Check for updates" : st.message;
    std::wstring sub = L"You're running Foyer " FOYER_VERSION;
    if (st.state == S::UpToDate) { glyph = 0xE930; title = L"You're up to date"; }
    if (st.state == S::Error) { glyph = 0xE7BA; gc = D2D1::ColorF(0.95f, 0.55f, 0.35f, 1.0f); }
    if (st.state == S::Available || st.state == S::Ready) { glyph = 0xE896; sub = L"Installed: " FOYER_VERSION L"   \x2192   New: " + st.release.version + (st.release.prerelease ? L" (beta)" : L""); }
    if (g_config.lastCheck > 0 && (st.state == S::UpToDate || st.state == S::Idle || st.state == S::Error)) {
        time_t tt = (time_t)g_config.lastCheck;
        tm lt{}; localtime_s(&lt, &tt);
        wchar_t b[64]; wcsftime(b, 64, L"%b %d, %I:%M %p", &lt);
        sub += L"   \x00B7   Last checked " + std::wstring(b);
    }
    D2D1_RECT_F badge = D2D1::RectF(card.left + 20, card.top + 22, card.left + 72, card.top + 74);
    g_cv.FillRound(badge, 14, Alpha(gc, 0.16f));
    bool spinning = st.state == S::Checking || st.state == S::Downloading || st.state == S::Verifying || st.state == S::Installing;
    if (spinning) {
        // simple animated arc: rotate the sync glyph
        float angle = (float)fmod(GetTickCount64() / 4.0, 360.0);
        D2D1_MATRIX_3X2_F saved; g_rt->GetTransform(&saved);
        g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F((badge.left + badge.right) / 2, (badge.top + badge.bottom) / 2)) * saved);
        g_cv.Glyph(0xE895, g_fIconBig, badge, gc);
        g_rt->SetTransform(saved);
        g_animTargets[L"__spin"] = (float)GetTickCount64();   // keeps the timer alive
    } else {
        g_cv.Glyph(glyph, g_fIconBig, badge, gc);
    }
    g_cv.Text(title, g_fCard, D2D1::RectF(card.left + 88, card.top + 22, card.right - 20, card.top + 48), t.text);
    g_cv.Text(sub, g_fDesc, D2D1::RectF(card.left + 88, card.top + 50, card.right - 20, card.top + 72), t.subtext);
    if (st.state == S::Downloading) {
        D2D1_RECT_F track = D2D1::RectF(card.left + 88, card.top + 80, card.right - 24, card.top + 84);
        g_cv.FillRound(track, 2, Alpha(t.subtext, 0.25f));
        g_cv.FillRound(D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * st.percent / 100.0f, track.bottom), 2, t.accent);
    }
    float bx = card.right - 20, by = card.bottom - 32;
    if (st.state == S::Ready) {
        bx -= DrawButton(bx, by, L"Restart and update", true, ci, 2) + 8;
    } else if (st.state == S::Available || (st.state == S::Error && !st.release.exeUrl.empty())) {
        bx -= DrawButton(bx, by, L"Download and install", true, ci, 1) + 8;
    }
    if (!st.release.page.empty()) bx -= DrawButton(bx, by, L"View on GitHub", false, ci, 3) + 8;
    DrawButton(bx, by, L"Check now", st.state != S::Available && st.state != S::Ready, ci, 0, !spinning);
}

static void Paint() {
    if (!EnsureTarget()) return;
    const Theme& t = g_theme;
    RECT rc; GetClientRect(g_hwnd, &rc);
    float W = Dip(rc.right), H = Dip(rc.bottom);
    g_zones.clear();
    g_animTargets.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    bool glass = t.backdrop != 0;

    // Sidebar
    g_cv.Fill(D2D1::RectF(0, 0, kSidebarW, H), glass ? Alpha(t.bg, 0.25f) : t.bottomBg);
    g_cv.Fill(D2D1::RectF(kSidebarW, 0, W, H), glass ? Alpha(t.bg, 0.45f) : t.bg);
    D2D1_RECT_F mark = D2D1::RectF(22, 22, 54, 54);
    g_cv.FillRound(mark, 9, t.accent);
    g_cv.Glyph(0xE80F, g_fIcon, mark, t.onAccent);
    g_cv.Text(L"Foyer", g_fBrand, D2D1::RectF(64, 18, kSidebarW - 10, 40), t.text);
    g_cv.Text(L"Version " FOYER_VERSION, g_fDesc, D2D1::RectF(64, 40, kSidebarW - 10, 58), t.subtext);
    float ny = 84;
    for (int p = 0; p < PageCount; ++p) {
        D2D1_RECT_F r = D2D1::RectF(12, ny, kSidebarW - 12, ny + 40);
        bool sel = p == g_page;
        if (sel) g_cv.FillRound(r, 6, Alpha(t.surface, glass ? 0.8f : 1.0f));
        else if (Hot(-2, p)) g_cv.FillRound(r, 6, t.hover);
        if (sel) g_cv.FillRound(D2D1::RectF(r.left + 2, r.top + 11, r.left + 5, r.bottom - 11), 1.5f, t.accent);
        g_cv.Glyph(kPageGlyphs[p], g_fIcon, D2D1::RectF(r.left + 12, r.top, r.left + 40, r.bottom), sel ? t.accent : t.text);
        g_cv.Text(kPageNames[p], g_fNav, D2D1::RectF(r.left + 48, r.top, r.right - 8, r.bottom), t.text);
        if (p == PageUpdates && AutoUpdateAvailable()) g_cv.Circle(r.right - 16, (r.top + r.bottom) / 2, 4, t.accent);
        AddZone(-2, p, r);
        ny += 44;
    }

    // Content
    float cx0 = kSidebarW + 36, cx1 = W - 36;
    float width = cx1 - cx0;
    g_cv.Text(kPageNames[g_page], g_fTitle, D2D1::RectF(cx0, 22, cx1, 64), t.text);
    float top = 80;
    g_viewH = H - top;
    D2D1_RECT_F clip = D2D1::RectF(kSidebarW, top, W, H);
    g_rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = top - g_scroll;
    for (int i = 0; i < (int)g_ctls.size(); ++i) {
        Ctl& c = g_ctls[i];
        float h = CardHeight(c, width);
        D2D1_RECT_F card = D2D1::RectF(cx0, y, cx1, y + h);
        if (y + h < top - 4 || y > H + 4) { y += h + (c.type == CType::Header ? 0 : 4); continue; }
        float alpha = c.enabled ? 1.0f : 0.5f;
        bool isCard = c.type != CType::Header && c.type != CType::ThemeGrid && c.type != CType::LayoutGrid && c.type != CType::Hero;
        if (isCard) {
            g_cv.FillRound(card, std::min(8.0f, std::max(4.0f, t.radius)), Alpha(t.surface, glass ? 0.72f : 1.0f));
            g_cv.StrokeRound(card, std::min(8.0f, std::max(4.0f, t.radius)), Alpha(t.divider, 0.8f));
        }
        float midY = (card.top + card.bottom) / 2;
        D2D1_RECT_F labelR = D2D1::RectF(card.left + 20, card.top + 12, card.left + width * 0.58f, card.top + 32);
        D2D1_RECT_F descR = D2D1::RectF(card.left + 20, card.top + 33, card.left + width * 0.58f, card.bottom - 8);
        if (c.desc.empty() && (c.type == CType::Toggle || c.type == CType::Choice || c.type == CType::Slider || c.type == CType::Buttons || c.type == CType::Swatches))
            labelR = D2D1::RectF(card.left + 20, card.top, card.left + width * 0.58f, card.bottom);

        switch (c.type) {
            case CType::Header:
                g_cv.Text(c.label, g_fHeader, D2D1::RectF(cx0 + 2, card.top + 14, cx1, card.bottom - 2), t.text);
                break;
            case CType::Toggle: {
                g_cv.Text(c.label, g_fLabel, labelR, Alpha(t.text, alpha));
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                bool on = c.getb ? c.getb() : false;
                float k = AnimValue(c.label, on ? 1.0f : 0.0f);
                g_animTargets[c.label] = on ? 1.0f : 0.0f;
                D2D1_RECT_F tr = D2D1::RectF(cx1 - 64, midY - 11, cx1 - 20, midY + 11);
                bool hot = Hot(i, 0);
                g_cv.FillRound(tr, 11, Mix(Alpha(t.surfaceStrong, 1.0f), t.accent, k));
                if (k < 0.99f) g_cv.StrokeRound(tr, 11, Alpha(t.subtext, (1 - k) * 0.8f));
                float knob = tr.left + 11 + k * (tr.right - tr.left - 22);
                g_cv.Circle(knob, midY, hot ? 7.5f : 6.5f, Mix(t.subtext, t.onAccent, k));
                g_cv.Text(on ? L"On" : L"Off", g_fDesc, D2D1::RectF(tr.left - 44, midY - 10, tr.left - 10, midY + 10), t.subtext);
                AddZone(i, 0, D2D1::RectF(tr.left - 50, card.top, cx1, card.bottom));
                break;
            }
            case CType::Choice: {
                g_cv.Text(c.label, g_fLabel, labelR, Alpha(t.text, alpha));
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                int sel = c.geti ? c.geti() : 0;
                float total = 6;
                std::vector<float> ws;
                for (auto& o : c.options) { float w = MeasureWidth(o, g_fBtn) + 26; ws.push_back(w); total += w; }
                float x = cx1 - 20 - total;
                D2D1_RECT_F pill = D2D1::RectF(x, midY - 17, cx1 - 20, midY + 17);
                g_cv.FillRound(pill, 17, Alpha(t.surfaceStrong, alpha));
                x += 3;
                for (int o = 0; o < (int)c.options.size(); ++o) {
                    D2D1_RECT_F r = D2D1::RectF(x, midY - 14, x + ws[o], midY + 14);
                    if (o == sel) g_cv.FillRound(r, 14, Alpha(t.accent, alpha));
                    else if (Hot(i, o)) g_cv.FillRound(r, 14, t.hover);
                    g_cv.Text(c.options[o], g_fBtn, r, o == sel ? t.onAccent : Alpha(t.text, alpha));
                    if (c.enabled) AddZone(i, o, r);
                    x += ws[o];
                }
                break;
            }
            case CType::Slider: {
                g_cv.Text(c.label, g_fLabel, labelR, Alpha(t.text, alpha));
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                float v = c.getf ? c.getf() : 0;
                float f = std::clamp((v - c.fmin) / (c.fmax - c.fmin), 0.0f, 1.0f);
                D2D1_RECT_F tr = D2D1::RectF(cx1 - 220, midY - 2, cx1 - 24, midY + 2);
                g_cv.FillRound(tr, 2, Alpha(t.subtext, 0.35f * alpha));
                float kx = tr.left + f * (tr.right - tr.left);
                g_cv.FillRound(D2D1::RectF(tr.left, tr.top, kx, tr.bottom), 2, Alpha(t.accent, alpha));
                bool hot = Hot(i, 0) || g_dragCtl == i;
                g_cv.Circle(kx, midY, 10, Alpha(t.surface, 1.0f));
                g_cv.Ring(kx, midY, 10, Alpha(t.divider, 1.0f), 1.0f);
                g_cv.Circle(kx, midY, hot ? 6.5f : 5.0f, Alpha(t.accent, alpha));
                if (c.fmt) g_cv.Text(c.fmt(v), g_fDesc, D2D1::RectF(tr.left - 70, midY - 10, tr.left - 14, midY + 10), t.subtext);
                if (c.enabled) AddZone(i, 0, D2D1::RectF(tr.left - 10, card.top, tr.right + 10, card.bottom));
                break;
            }
            case CType::Swatches: {
                g_cv.Text(c.label, g_fLabel, labelR, t.text);
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                static const uint32_t colors[] = {0, 0x4CC2FF, 0x7C83FD, 0xC86DD7, 0xFF6B9A, 0xFF7A45, 0xF5B83D, 0x3DD68C, 0x2EC4B6};
                const int n = (int)(sizeof(colors) / sizeof(colors[0]));
                float x = cx1 - 24 - n * 30;
                for (int s = 0; s < n; ++s) {
                    uint32_t col = s == 0 ? WindowsAccentRgb() : colors[s];
                    bool sel = s == 0 ? g_config.useWindowsAccent : (!g_config.useWindowsAccent && g_config.accentColor == colors[s]);
                    float scx = x + s * 30 + 15;
                    if (sel) g_cv.Ring(scx, midY, 14, t.text, 2.0f);
                    else if (Hot(i, s)) g_cv.Ring(scx, midY, 14, Alpha(t.text, 0.4f), 1.5f);
                    g_cv.Circle(scx, midY, 10.5f, Rgb(col));
                    if (s == 0) g_cv.Glyph(0xE770, g_fSmallC, D2D1::RectF(scx - 8, midY - 8, scx + 8, midY + 8), D2D1::ColorF(1, 1, 1, 0.9f));
                    AddZone(i, s, D2D1::RectF(scx - 15, midY - 15, scx + 15, midY + 15));
                }
                break;
            }
            case CType::Buttons: {
                if (!c.label.empty()) {
                    g_cv.Text(c.label, g_fLabel, labelR, t.text);
                    g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                }
                float bx = cx1 - 20;
                for (int b = (int)c.buttons.size() - 1; b >= 0; --b)
                    bx -= DrawButton(bx, midY, c.buttons[b].first, b == c.primary, i, b) + 8;
                break;
            }
            case CType::Info:
                g_cv.Text(c.desc, g_fDesc, D2D1::RectF(card.left + 20, card.top + 16, card.right - 20, card.bottom - 8), t.subtext);
                break;
            case CType::Notes:
                g_cv.Text(c.desc, g_fNotes, D2D1::RectF(card.left + 20, card.top + 16, card.right - 20, card.bottom - 8), t.text);
                break;
            case CType::ThemeGrid: {
                int perRow = std::max(1, (int)((width + 12) / 124));
                for (int p = 0; p < kThemePresetCount; ++p)
                    DrawThemeCard(cx0 + (p % perRow) * 124, card.top + (p / perRow) * 112, 112, 104, p, i);
                break;
            }
            case CType::LayoutGrid: {
                float w = std::min(200.0f, (width - 24) / 3);
                for (int p = 0; p < 3; ++p) DrawLayoutCard(cx0 + p * (w + 12), card.top + 4, w, 144, p, i);
                break;
            }
            case CType::PinRow:
            case CType::HiddenRow: {
                bool pin = c.type == CType::PinRow;
                g_cv.Glyph(pin ? 0xE718 : 0xED1A, g_fIcon, D2D1::RectF(card.left + 12, card.top, card.left + 44, card.bottom), t.subtext);
                g_cv.Text(c.label, g_fLabel, D2D1::RectF(card.left + 50, card.top, cx1 - 200, card.bottom), t.text);
                if (pin) {
                    const wchar_t glyphs[3] = {0xE70E, 0xE70D, 0xE711};
                    for (int b = 0; b < 3; ++b) {
                        D2D1_RECT_F r = D2D1::RectF(cx1 - 20 - (3 - b) * 40, midY - 16, cx1 - 20 - (3 - b) * 40 + 36, midY + 16);
                        bool enabled = !(b == 0 && c.index == 0) && !(b == 1 && c.index + 1 >= (int)g_config.pins.size());
                        if (Hot(i, b) && enabled) g_cv.FillRound(r, 6, t.hover);
                        g_cv.Glyph(glyphs[b], g_fIcon, r, enabled ? (b == 2 ? D2D1::ColorF(0.95f, 0.45f, 0.4f, 1) : t.text) : Alpha(t.subtext, 0.4f));
                        if (enabled) AddZone(i, b, r);
                    }
                } else {
                    DrawButton(cx1 - 20, midY, L"Unhide", false, i, 0);
                }
                break;
            }
            case CType::UpdateCard:
                DrawUpdateCard(card, i);
                break;
            case CType::Hero: {
                D2D1_RECT_F big = D2D1::RectF(cx0, card.top + 10, cx0 + 92, card.top + 102);
                g_cv.Circle(cx0 + 46, card.top + 56, 70, Alpha(t.accent, 0.12f));
                g_cv.FillRound(big, 24, t.accent);
                g_cv.Glyph(0xE80F, g_fIconBig, big, t.onAccent);
                g_cv.Text(L"Foyer", g_fHero, D2D1::RectF(cx0 + 116, card.top + 12, cx1, card.top + 56), t.text);
                g_cv.Text(L"Version " FOYER_VERSION L"  \x00B7  A fast, clean Start menu. Part of the larosa023 suite.", g_fDesc,
                          D2D1::RectF(cx0 + 118, card.top + 60, cx1, card.top + 96), t.subtext);
                break;
            }
        }
        y += h + (c.type == CType::Header ? 0 : 4);
    }
    g_contentH = y + g_scroll - top + 24;
    g_rt->PopAxisAlignedClip();

    // scrollbar
    if (g_contentH > g_viewH) {
        float thumbH = std::max(30.0f, g_viewH * g_viewH / g_contentH);
        float ty = top + (g_scroll / (g_contentH - g_viewH)) * (g_viewH - thumbH);
        g_cv.FillRound(D2D1::RectF(W - 8, ty, W - 4, ty + thumbH), 2, Alpha(t.subtext, 0.4f));
    }

    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { Discard(); InvalidateRect(g_hwnd, nullptr, FALSE); }

    // Keep animating while toggles slide or something spins.
    bool moving = false;
    for (auto& kv : g_animTargets) {
        if (kv.first == L"__spin") { moving = true; continue; }
        auto it = g_anim.find(kv.first);
        if (it != g_anim.end() && std::fabs(it->second - kv.second) > 0.01f) moving = true;
    }
    if (moving) StartAnim();
}

// ============================================================ interaction
static void SetScroll(float s) {
    g_scroll = std::clamp(s, 0.0f, std::max(0.0f, g_contentH - g_viewH));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void GoToPage(int p) {
    if (p < 0 || p >= PageCount) return;
    if (g_page != p && g_previewOn && p != PageAppearance && p != PageLayout) SetPreview(false);
    g_page = p;
    g_scroll = 0;
    BuildPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SliderSet(int ci, float x) {
    Ctl& c = g_ctls[ci];
    for (const Zone& z : g_zones) {
        if (z.ctl != ci) continue;
        float l = z.rc.left + 10, r = z.rc.right - 10;
        float f = std::clamp((x - l) / (r - l), 0.0f, 1.0f);
        if (c.setf) c.setf(c.fmin + f * (c.fmax - c.fmin));
        break;
    }
}

static void Click(int ci, int part, float x) {
    if (ci == -2) { GoToPage(part); return; }
    if (ci < 0 || ci >= (int)g_ctls.size()) return;
    Ctl& c = g_ctls[ci];
    switch (c.type) {
        case CType::Toggle: if (c.setb) c.setb(!c.getb()); StartAnim(); break;
        case CType::Choice: if (c.seti) c.seti(part); break;
        case CType::Slider: SliderSet(ci, x); break;
        case CType::Swatches: {
            static const uint32_t colors[] = {0, 0x4CC2FF, 0x7C83FD, 0xC86DD7, 0xFF6B9A, 0xFF7A45, 0xF5B83D, 0x3DD68C, 0x2EC4B6};
            if (part == 0) g_config.useWindowsAccent = true;
            else { g_config.useWindowsAccent = false; g_config.accentColor = colors[part]; }
            Changed();
            break;
        }
        case CType::Buttons: if (part < (int)c.buttons.size() && c.buttons[part].second) c.buttons[part].second(); break;
        case CType::ThemeGrid: g_config.theme = kThemePresets[part].id; Changed(); break;
        case CType::LayoutGrid: { const wchar_t* ids[] = {L"modern", L"classic", L"fullscreen"}; g_config.layout = ids[part]; Changed(); break; }
        case CType::PinRow: {
            int i = c.index;
            if (part == 0 && i > 0) std::swap(g_config.pins[i], g_config.pins[i - 1]);
            else if (part == 1 && i + 1 < (int)g_config.pins.size()) std::swap(g_config.pins[i], g_config.pins[i + 1]);
            else if (part == 2) g_config.pins.erase(g_config.pins.begin() + i);
            Changed();
            MenuReloadConfig();
            break;
        }
        case CType::HiddenRow:
            g_config.hidden.erase(g_config.hidden.begin() + c.index);
            Changed();
            MenuReloadConfig();
            break;
        case CType::UpdateCard:
            if (part == 0) AutoUpdateCheckNow();
            else if (part == 1) AutoUpdateDownloadNow();
            else if (part == 2) AutoUpdateInstallNow();
            else if (part == 3) ShellOpen(updater::Get().release.page.c_str());
            break;
        default: break;
    }
    BuildPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static bool HitZone(float x, float y, int* ci, int* part) {
    for (auto it = g_zones.rbegin(); it != g_zones.rend(); ++it) {
        if (InR(it->rc, x, y)) {
            // zones in the content area are clipped to the visible region
            if (it->ctl != -2 && y < 80) continue;
            *ci = it->ctl; *part = it->part;
            return true;
        }
    }
    return false;
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); Paint(); EndPaint(hwnd, &ps); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            SetScroll(g_scroll);
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wParam);
            RECT* r = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Discard();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lParam;
            mm->ptMinTrackSize = {Px(760), Px(540)};
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_SANIM) {
                bool spin = g_animTargets.count(L"__spin") > 0;
                std::map<std::wstring, float> targets = g_animTargets;
                targets.erase(L"__spin");
                if (!StepAnims(targets) && !spin) { KillTimer(hwnd, TIMER_SANIM); g_animTimer = false; }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            if (!g_tracking) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t); g_tracking = true; }
            if (g_dragCtl >= 0) { SliderSet(g_dragCtl, x); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            int ci = -1, part = -1;
            HitZone(x, y, &ci, &part);
            if (ci != g_hotCtl || part != g_hotPart) { g_hotCtl = ci; g_hotPart = part; InvalidateRect(hwnd, nullptr, FALSE); }
            SetCursor(LoadCursorW(nullptr, ci == -1 && part == -1 ? IDC_ARROW : IDC_HAND));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, (g_hotCtl != -1 || g_hotPart != -1) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE: g_tracking = false; g_hotCtl = g_hotPart = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            int ci = -1, part = -1;
            if (HitZone(x, y, &ci, &part) && ci >= 0 && g_ctls[ci].type == CType::Slider) {
                g_dragCtl = ci;
                SetCapture(hwnd);
                SliderSet(ci, x);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            if (g_dragCtl >= 0) {
                g_dragCtl = -1;
                ReleaseCapture();
                ConfigSave();   // sliders save once, when released
                Changed(false);
                return 0;
            }
            int ci = -1, part = -1;
            if (HitZone(x, y, &ci, &part)) Click(ci, part, x);
            return 0;
        }
        case WM_MOUSEWHEEL:
            SetScroll(g_scroll - GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 60);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            if (wParam == VK_PRIOR) { SetScroll(g_scroll - g_viewH * 0.9f); return 0; }
            if (wParam == VK_NEXT) { SetScroll(g_scroll + g_viewH * 0.9f); return 0; }
            if (wParam == VK_TAB && GetKeyState(VK_CONTROL) < 0) { GoToPage((g_page + (GetKeyState(VK_SHIFT) < 0 ? PageCount - 1 : 1)) % PageCount); return 0; }
            break;
        case WM_SETTINGCHANGE:
            ApplyTheme();
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_DESTROY:
            if (g_previewOn) { g_previewOn = false; MenuSetPreview(false); }
            Discard();
            g_hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SettingsOnUpdateStatus() {
    if (!SettingsIsOpen()) return;
    BuildPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SettingsOpen(int page) {
    MenuHide();
    if (SettingsIsOpen()) {
        if (page >= 0) GoToPage(page);
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return;
    }
    gfx::Init();
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wc.lpszClassName = L"FoyerSettings";
        RegisterClassExW(&wc);
        registered = true;
    }
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    g_dpi = dx;
    int w = std::min<int>(Px(980), mi.rcWork.right - mi.rcWork.left - 40);
    int h = std::min<int>(Px(700), mi.rcWork.bottom - mi.rcWork.top - 40);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    if (page >= 0) g_page = page;
    g_scroll = 0;
    CreateFormats();
    g_hwnd = CreateWindowExW(0, L"FoyerSettings", L"Foyer Settings", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return;
    g_dpi = GetDpiForWindow(g_hwnd);
    ApplyTheme();
    BuildPage();
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (g_page == PageUpdates && updater::Get().state == updater::State::Idle) AutoUpdateCheckNow();
}
