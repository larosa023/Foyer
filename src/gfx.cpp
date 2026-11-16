// Foyer - shared graphics helpers and theme presets
#include "gfx.h"
#include "config.h"
#include <dwmapi.h>
#include <shlobj.h>
#include <algorithm>
#include "json.h"
#include "system.h"
#include <map>

namespace gfx {

ID2D1Factory* d2d = nullptr;
IDWriteFactory* dw = nullptr;
static std::wstring s_iconFont = L"Segoe MDL2 Assets";
static std::wstring s_uiFont = L"Segoe UI";
static std::wstring s_displayFont = L"Segoe UI";

static bool FontExists(IDWriteFontCollection* sys, const wchar_t* name) {
    UINT32 idx; BOOL exists = FALSE;
    return sys && SUCCEEDED(sys->FindFamilyName(name, &idx, &exists)) && exists;
}

bool Init() {
    if (d2d && dw) return true;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw));
    if (!d2d || !dw) return false;
    IDWriteFontCollection* sys = nullptr;
    dw->GetSystemFontCollection(&sys, FALSE);
    if (FontExists(sys, L"Segoe Fluent Icons")) s_iconFont = L"Segoe Fluent Icons";
    if (FontExists(sys, L"Segoe UI Variable Text")) s_uiFont = L"Segoe UI Variable Text";
    if (FontExists(sys, L"Segoe UI Variable Display")) s_displayFont = L"Segoe UI Variable Display";
    SafeRelease(sys);
    return true;
}

const wchar_t* IconFont() { return s_iconFont.c_str(); }
const wchar_t* UiFont() { return s_uiFont.c_str(); }
const wchar_t* DisplayFont() { return s_displayFont.c_str(); }

IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                          DWRITE_TEXT_ALIGNMENT align, bool ellipsis, bool wrap) {
    IDWriteTextFormat* f = nullptr;
    dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        IDWriteInlineObject* sign = nullptr;
        dw->CreateEllipsisTrimmingSign(f, &sign);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, sign);
        SafeRelease(sign);
    }
    return f;
}

float MeasureWidth(const std::wstring& s, IDWriteTextFormat* f) {
    if (!f || s.empty()) return 0;
    IDWriteTextLayout* lay = nullptr;
    dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 1000, &lay);
    if (!lay) return 0;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    lay->Release();
    return m.widthIncludingTrailingWhitespace;
}

float MeasureHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    if (!f || s.empty()) return 0;
    IDWriteTextLayout* lay = nullptr;
    dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 100000, &lay);
    if (!lay) return 0;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    lay->Release();
    return m.height;
}

// ---------------------------------------------------------------- theme
const ThemePreset kThemePresets[] = {
    {L"system", L"Follow Windows"}, {L"dark", L"Dark"}, {L"light", L"Light"}, {L"midnight", L"Midnight"},
    {L"glass", L"Glass"}, {L"ember", L"Ember"}, {L"ocean", L"Ocean"},
};
const int kThemePresetCount = (int)(sizeof(kThemePresets) / sizeof(kThemePresets[0]));

static DWORD WindowsBuild() {
    static DWORD build = 0;
    if (!build) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW v{sizeof(v)};
        if (fn && fn(&v) == 0) build = v.dwBuildNumber;
    }
    return build;
}

bool SystemBackdropSupported() { return WindowsBuild() >= 22621; }

bool WindowsUsesLightTheme() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    return v != 0;
}

uint32_t WindowsAccentRgb() {
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &abgr, &size) != ERROR_SUCCESS) return 0x4CC2FF;
    return ((abgr & 0xFF) << 16) | (abgr & 0xFF00) | ((abgr >> 16) & 0xFF);
}

static float Luminance(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// Make the accent readable on the theme's background.
static D2D1_COLOR_F TuneAccent(D2D1_COLOR_F a, bool dark) {
    float lum = Luminance(a);
    if (dark && lum < 0.45f) return Mix(a, D2D1::ColorF(1, 1, 1, 1), (0.45f - lum) / 0.45f * 0.6f);
    if (!dark && lum > 0.55f) return Mix(a, D2D1::ColorF(0, 0, 0, 1), (lum - 0.55f) / 0.45f * 0.5f);
    return a;
}

// ---------------------------------------------------------------- theme files
struct ThemeFile {
    std::wstring name, base = L"dark", backdrop, font;
    uint32_t accent = 0; bool hasAccent = false;
    float opacity = -1, radius = -1; int glow = -1;
    std::map<std::wstring, uint32_t> colors;
};

static bool ParseHex(const std::wstring& s, uint32_t& out) {
    std::wstring t = s;
    if (!t.empty() && t[0] == L'#') t = t.substr(1);
    if (t.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(t.c_str(), &end, 16);
    if (!end || *end) return false;
    out = (uint32_t)v;
    return true;
}

static bool ReadUtf8(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr), read = 0;
    std::string buf(size < 1024 * 1024 ? size : 0, '\0');
    bool ok = !buf.empty() && ReadFile(h, &buf[0], size, &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    size_t skip = (read >= 3 && (unsigned char)buf[0] == 0xEF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data() + skip, (int)(read - skip), nullptr, 0);
    out.assign(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, buf.data() + skip, (int)(read - skip), &out[0], n);
    return true;
}

static bool ParseThemeFile(const std::wstring& text, ThemeFile& tf) {
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) return false;
    tf.name = root.GetStr(L"name");
    tf.base = root.GetStr(L"base", L"dark");
    if (tf.base != L"light" && tf.base != L"midnight" && tf.base != L"ember" && tf.base != L"ocean" && tf.base != L"glass") tf.base = L"dark";
    tf.backdrop = root.GetStr(L"backdrop");
    tf.font = root.GetStr(L"font");
    tf.hasAccent = ParseHex(root.GetStr(L"accent"), tf.accent);
    tf.opacity = (float)root.GetNum(L"opacity", -1);
    tf.radius = (float)root.GetNum(L"radius", -1);
    if (const json::Value* g = root.Get(L"glow"); g && g->type == json::Value::Bool) tf.glow = g->b ? 1 : 0;
    if (const json::Value* cols = root.Get(L"colors"); cols && cols->type == json::Value::Object)
        for (auto& kv : cols->obj) { uint32_t v; if (kv.second.type == json::Value::String && ParseHex(kv.second.s, v)) tf.colors[kv.first] = v; }
    return true;
}

static std::wstring ThemeFilePath(const std::wstring& name) { return ThemesDir() + L"\\" + name + L".json"; }

static bool LoadThemeFile(const std::wstring& name, ThemeFile& tf) {
    std::wstring text;
    return ReadUtf8(ThemeFilePath(name), text) && ParseThemeFile(text, tf);
}

std::vector<ThemeEntry> ThemeList() {
    std::vector<ThemeEntry> out;
    for (int i = 0; i < kThemePresetCount; ++i) out.push_back({kThemePresets[i].id, kThemePresets[i].name});
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((ThemesDir() + L"\\*.json").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring file = fd.cFileName;
            std::wstring stem = file.substr(0, file.size() - 5);
            ThemeFile tf;
            if (!LoadThemeFile(stem, tf)) continue;
            out.push_back({L"file:" + stem, tf.name.empty() ? stem : tf.name});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return out;
}

static std::wstring Hex(uint32_t c) { wchar_t b[16]; swprintf(b, 16, L"#%06X", c & 0xFFFFFF); return b; }

static bool WriteUtf8(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string buf(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr) && w == buf.size();
    CloseHandle(h);
    return ok;
}

static uint32_t ToRgb(D2D1_COLOR_F c) {
    auto b = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255); };
    return (b(c.r) << 16) | (b(c.g) << 8) | b(c.b);
}

bool ThemeExport(const Config& c, const std::wstring& path, const std::wstring& name) {
    Theme t = BuildThemeNamed(c, c.theme.c_str());
    std::wstring base = c.theme;
    if (base.compare(0, 5, L"file:") == 0) { ThemeFile tf; base = LoadThemeFile(base.substr(5), tf) ? tf.base : L"dark"; }
    if (base == L"system") base = WindowsUsesLightTheme() ? L"light" : L"dark";
    json::Value root = json::Value::MakeObject();
    root.Set(L"name", json::Value::Str(name));
    root.Set(L"base", json::Value::Str(base));
    root.Set(L"accent", json::Value::Str(Hex(ToRgb(t.accent))));
    root.Set(L"backdrop", json::Value::Str(c.backdrop));
    root.Set(L"opacity", json::Value::Num(std::round(c.opacity * 100) / 100));
    root.Set(L"radius", json::Value::Num(std::round(c.cornerRadius)));
    root.Set(L"glow", json::Value::Boolean(c.accentGlow));
    if (!c.font.empty()) root.Set(L"font", json::Value::Str(c.font));
    json::Value cols = json::Value::MakeObject();
    cols.Set(L"background", json::Value::Str(Hex(ToRgb(t.bg))));
    cols.Set(L"bottom", json::Value::Str(Hex(ToRgb(t.bottomBg))));
    cols.Set(L"surface", json::Value::Str(Hex(ToRgb(t.surface))));
    cols.Set(L"text", json::Value::Str(Hex(ToRgb(t.text))));
    cols.Set(L"subtext", json::Value::Str(Hex(ToRgb(t.subtext))));
    root.Set(L"colors", cols);
    std::wstring text;
    json::Write(root, text);
    return WriteUtf8(path, text + L"\n");
}

bool ThemeImport(const std::wstring& path, std::wstring* newId, std::wstring* error) {
    std::wstring text;
    ThemeFile tf;
    if (!ReadUtf8(path, text) || !ParseThemeFile(text, tf)) { if (error) *error = L"That file isn't a Foyer theme."; return false; }
    std::wstring stem = path.substr(path.find_last_of(L"\\/") + 1);
    if (stem.size() > 5 && _wcsicmp(stem.c_str() + stem.size() - 5, L".json") == 0) stem.resize(stem.size() - 5);
    if (!tf.name.empty()) stem = tf.name;
    std::wstring clean;
    for (wchar_t ch : stem) clean += (wcschr(L"\\/:*?\"<>|", ch) ? L'-' : ch);
    if (clean.empty()) clean = L"Theme";
    SHCreateDirectoryExW(nullptr, ThemesDir().c_str(), nullptr);
    if (!WriteUtf8(ThemeFilePath(clean), text)) { if (error) *error = L"Couldn't save the theme."; return false; }
    if (newId) *newId = L"file:" + clean;
    return true;
}

bool ThemeDelete(const std::wstring& id) {
    if (id.compare(0, 5, L"file:") != 0) return false;
    return DeleteFileW(ThemeFilePath(id.substr(5)).c_str()) != FALSE;
}

const wchar_t* MenuFont(const Config& c) {
    static std::wstring font;
    font = c.font;
    if (c.theme.compare(0, 5, L"file:") == 0) { ThemeFile tf; if (LoadThemeFile(c.theme.substr(5), tf) && !tf.font.empty() && c.font.empty()) font = tf.font; }
    if (font.empty()) return UiFont();
    // fall back if the font isn't installed
    IDWriteFontCollection* sys = nullptr;
    bool ok = false;
    if (dw && SUCCEEDED(dw->GetSystemFontCollection(&sys, FALSE)) && sys) { ok = FontExists(sys, font.c_str()); sys->Release(); }
    return ok ? font.c_str() : UiFont();
}

Theme BuildThemeNamed(const Config& c, const wchar_t* themeId) {
    std::wstring id = themeId;
    ThemeFile tf;
    bool fromFile = id.compare(0, 5, L"file:") == 0 && LoadThemeFile(id.substr(5), tf);
    if (id.compare(0, 5, L"file:") == 0) id = fromFile ? tf.base : L"dark";
    if (id == L"system") id = WindowsUsesLightTheme() ? L"light" : L"dark";
    Theme t;
    uint32_t presetAccent = 0;
    if (id == L"light") {
        t.dark = false;
        t.bg = Rgb(0xF6F6F8); t.bottomBg = Rgb(0xECECF0); t.surface = Rgb(0xFFFFFF); t.surfaceStrong = Rgb(0xE6E6EB);
        t.hover = Rgb(0x000000, 0.055f); t.divider = Rgb(0x000000, 0.09f);
        t.text = Rgb(0x1A1A1F); t.subtext = Rgb(0x5F5F6B); t.onAccent = Rgb(0xFFFFFF); t.scrim = Rgb(0xE8E8EE, 0.55f);
    } else if (id == L"midnight") {
        t.bg = Rgb(0x000000); t.bottomBg = Rgb(0x000000); t.surface = Rgb(0x111114); t.surfaceStrong = Rgb(0x1C1C21);
        t.hover = Rgb(0xFFFFFF, 0.08f); t.divider = Rgb(0xFFFFFF, 0.07f);
        t.text = Rgb(0xF5F5F7); t.subtext = Rgb(0x9A9AA4); t.onAccent = Rgb(0x000000); t.scrim = Rgb(0x000000, 0.7f);
    } else if (id == L"ember") {
        t.bg = Rgb(0x1E1513); t.bottomBg = Rgb(0x18110F); t.surface = Rgb(0x2E211D); t.surfaceStrong = Rgb(0x3A2A25);
        t.hover = Rgb(0xFFB08A, 0.09f); t.divider = Rgb(0xFFD2BE, 0.08f);
        t.text = Rgb(0xFFF3EE); t.subtext = Rgb(0xC2A79C); t.onAccent = Rgb(0x1E1513); t.scrim = Rgb(0x120B09, 0.6f);
        presetAccent = 0xFF7A45;
    } else if (id == L"ocean") {
        t.bg = Rgb(0x0E1823); t.bottomBg = Rgb(0x0B131C); t.surface = Rgb(0x172636); t.surfaceStrong = Rgb(0x203347);
        t.hover = Rgb(0x8FD8FF, 0.09f); t.divider = Rgb(0xBFE6FF, 0.08f);
        t.text = Rgb(0xEEF7FF); t.subtext = Rgb(0x9DB4C8); t.onAccent = Rgb(0x0E1823); t.scrim = Rgb(0x060C12, 0.6f);
        presetAccent = 0x3FC8E4;
    } else {  // dark, glass
        t.bg = Rgb(0x202024); t.bottomBg = Rgb(0x1A1A1E); t.surface = Rgb(0x2C2C31); t.surfaceStrong = Rgb(0x38383E);
        t.hover = Rgb(0xFFFFFF, 0.075f); t.divider = Rgb(0xFFFFFF, 0.08f);
        t.text = Rgb(0xF3F3F5); t.subtext = Rgb(0xA4A4AE); t.onAccent = Rgb(0x101014); t.scrim = Rgb(0x000000, 0.55f);
    }

    uint32_t accentRgb = c.useWindowsAccent ? (presetAccent ? presetAccent : WindowsAccentRgb()) : c.accentColor;
    t.accent = TuneAccent(Rgb(accentRgb), t.dark);
    if (Luminance(t.accent) < 0.5f) t.onAccent = Rgb(0xFFFFFF);
    else t.onAccent = Rgb(0x101014);
    t.radius = c.cornerRadius;
    t.glow = c.accentGlow;

    std::wstring backdrop = c.backdrop;
    float opacity = c.opacity;
    if (id == L"glass") { backdrop = L"acrylic"; opacity = std::min(opacity, 0.5f); }
    if (id == L"midnight" && backdrop != L"solid") opacity = std::max(opacity, 0.85f);
    if (fromFile) {
        if (!tf.backdrop.empty()) backdrop = tf.backdrop;
        if (tf.opacity >= 0.3f && tf.opacity <= 1.0f) opacity = tf.opacity;
        if (tf.radius >= 0 && tf.radius <= 24) t.radius = tf.radius;
        if (tf.glow >= 0) t.glow = tf.glow == 1;
        auto col = [&](const wchar_t* k, D2D1_COLOR_F& dst) { auto it = tf.colors.find(k); if (it != tf.colors.end()) dst = Rgb(it->second); };
        col(L"background", t.bg); col(L"bottom", t.bottomBg); col(L"surface", t.surface);
        col(L"text", t.text); col(L"subtext", t.subtext);
        if (tf.colors.count(L"surface")) t.surfaceStrong = Mix(t.surface, t.text, 0.08f);
        if (c.useWindowsAccent && tf.hasAccent) t.accent = TuneAccent(Rgb(tf.accent), t.dark);
        if (tf.colors.count(L"accent")) t.accent = Rgb(tf.colors[L"accent"]);
        t.onAccent = Luminance(t.accent) < 0.5f ? Rgb(0xFFFFFF) : Rgb(0x101014);
        t.dark = Luminance(t.bg) < 0.5f;
        t.hover = t.dark ? Rgb(0xFFFFFF, 0.075f) : Rgb(0x000000, 0.055f);
        t.divider = t.dark ? Rgb(0xFFFFFF, 0.08f) : Rgb(0x000000, 0.09f);
    }
    if (SystemBackdropSupported() && backdrop != L"solid") {
        t.backdrop = backdrop == L"mica" ? 2 : 3;
        t.opacity = opacity;
    } else {
        t.backdrop = 0;
        t.opacity = 1.0f;
    }
    return t;
}

// High contrast: use the user's system colors and nothing decorative.
static Theme HighContrastTheme(const Config& c) {
    auto sys = [](int idx) { COLORREF v = GetSysColor(idx); return D2D1::ColorF(GetRValue(v) / 255.f, GetGValue(v) / 255.f, GetBValue(v) / 255.f, 1.0f); };
    Theme t;
    t.bg = sys(COLOR_WINDOW); t.bottomBg = t.bg; t.surface = t.bg; t.surfaceStrong = sys(COLOR_BTNFACE);
    t.text = sys(COLOR_WINDOWTEXT); t.subtext = sys(COLOR_GRAYTEXT);
    t.accent = sys(COLOR_HIGHLIGHT); t.onAccent = sys(COLOR_HIGHLIGHTTEXT);
    t.hover = Alpha(sys(COLOR_HIGHLIGHT), 0.25f); t.divider = sys(COLOR_WINDOWTEXT);
    t.scrim = Alpha(t.bg, 0.9f);
    t.dark = Luminance(t.bg) < 0.5f;
    t.radius = std::min(c.cornerRadius, 4.0f);
    t.glow = false;
    t.backdrop = 0;
    t.opacity = 1.0f;
    return t;
}

Theme BuildTheme(const Config& c) {
    if (a11y::HighContrast()) return HighContrastTheme(c);
    return BuildThemeNamed(c, c.theme.c_str());
}

void ApplyWindowTheme(HWND hwnd, const Theme& t, bool popup) {
    BOOL dark = t.dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = t.radius < 1.0f ? 1 /*DONOTROUND*/ : (t.radius <= 5.0f ? 3 /*ROUNDSMALL*/ : 2 /*ROUND*/);
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    if (popup) {
        COLORREF border = t.dark ? RGB(0x3C, 0x3C, 0x44) : RGB(0xD0, 0xD0, 0xD8);
        DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    }
    if (SystemBackdropSupported()) {
        int type = t.backdrop ? t.backdrop : 1 /*DWMSBT_NONE*/;
        DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &type, sizeof(type));
        MARGINS m = t.backdrop ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hwnd, &m);
    }
}

void ApplyMenuTheme(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

} // namespace gfx
