// Foyer - shared graphics helpers and theme presets
#include "gfx.h"
#include "config.h"
#include <dwmapi.h>
#include <algorithm>

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

Theme BuildThemeNamed(const Config& c, const wchar_t* themeId) {
    std::wstring id = themeId;
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
    if (SystemBackdropSupported() && backdrop != L"solid") {
        t.backdrop = backdrop == L"mica" ? 2 : 3;
        t.opacity = opacity;
    } else {
        t.backdrop = 0;
        t.opacity = 1.0f;
    }
    return t;
}

Theme BuildTheme(const Config& c) { return BuildThemeNamed(c, c.theme.c_str()); }

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
