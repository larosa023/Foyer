// Foyer - shared Direct2D / DirectWrite helpers and theme
#pragma once
#include "common.h"
#include <d2d1.h>
#include <dwrite.h>

struct Config;

namespace gfx {

extern ID2D1Factory* d2d;
extern IDWriteFactory* dw;
bool Init();
const wchar_t* IconFont();      // "Segoe Fluent Icons" on Windows 11, "Segoe MDL2 Assets" on Windows 10
const wchar_t* UiFont();        // "Segoe UI Variable Text" when present, else "Segoe UI"
const wchar_t* DisplayFont();   // "Segoe UI Variable Display" when present, else "Segoe UI"

IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                          DWRITE_TEXT_ALIGNMENT align, bool ellipsis, bool wrap = false);

inline D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}
inline D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
inline D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Canvas {
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;

    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c) { br->SetColor(c); rt->FillRectangle(r, br); }
    void FillRound(const D2D1_RECT_F& r, float rad, D2D1_COLOR_F c) {
        br->SetColor(c);
        if (rad <= 0.01f) rt->FillRectangle(r, br);
        else rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    }
    void StrokeRound(const D2D1_RECT_F& r, float rad, D2D1_COLOR_F c, float w = 1.0f) {
        br->SetColor(c);
        D2D1_RECT_F rr = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
        if (rad <= 0.01f) rt->DrawRectangle(rr, br, w);
        else rt->DrawRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), br, w);
    }
    void Circle(float cx, float cy, float r, D2D1_COLOR_F c) {
        br->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br);
    }
    void Ring(float cx, float cy, float r, D2D1_COLOR_F c, float w) {
        br->SetColor(c);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br, w);
    }
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
        if (!f || s.empty()) return;
        br->SetColor(c);
        rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Glyph(wchar_t g, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
        wchar_t s[2] = {g, 0};
        Text(s, f, r, c);
    }
};

float MeasureWidth(const std::wstring& s, IDWriteTextFormat* f);
float MeasureHeight(const std::wstring& s, IDWriteTextFormat* f, float width);

// ---------------------------------------------------------------- theme
struct Theme {
    bool dark = true;
    D2D1_COLOR_F bg, bottomBg, surface, surfaceStrong, hover, divider, text, subtext, accent, onAccent, scrim;
    float radius = 8.0f;
    int backdrop = 0;       // 0 none (solid), 2 Mica, 3 acrylic  (DWMSBT_* values)
    float opacity = 1.0f;   // alpha of bg when a backdrop is active
    bool glow = true;
};

struct ThemePreset { const wchar_t* id; const wchar_t* name; };
extern const ThemePreset kThemePresets[];
extern const int kThemePresetCount;

bool SystemBackdropSupported();      // Windows 11 22H2+
bool WindowsUsesLightTheme();
uint32_t WindowsAccentRgb();
Theme BuildTheme(const Config& c);
Theme BuildThemeNamed(const Config& c, const wchar_t* themeId);  // for previews
void ApplyWindowTheme(HWND hwnd, const Theme& t, bool popup);
void ApplyMenuTheme(bool dark);      // dark/light native popup menus

} // namespace gfx
