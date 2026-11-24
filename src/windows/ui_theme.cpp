#include "ui_theme.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace nskkzime::windows {
namespace {

constexpr DWORD dwm_immersive_dark_mode = 20;
constexpr DWORD dwm_window_corner_preference = 33;
constexpr DWORD dwm_border_color = 34;
constexpr DWORD dwm_corner_round = 2;
constexpr DWORD dwm_corner_round_small = 3;

// Load dwmapi dynamically so minimal AppContainer processes never fail DLL
// initialization if DWM cannot initialize inside the sandbox.
class DwmLibrary {
public:
    DwmLibrary() noexcept
        : module_(LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {}
    ~DwmLibrary() { if (module_) FreeLibrary(module_); }
    DwmLibrary(const DwmLibrary&) = delete;
    DwmLibrary& operator=(const DwmLibrary&) = delete;

    HRESULT set_window_attribute(HWND window, DWORD attribute, LPCVOID value, DWORD size) const noexcept {
        if (!module_) return E_NOTIMPL;
        using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        const auto fn = reinterpret_cast<Fn>(GetProcAddress(module_, "DwmSetWindowAttribute"));
        return fn ? fn(window, attribute, value, size) : E_NOTIMPL;
    }
    HRESULT get_colorization_color(DWORD* color, BOOL* opaque) const noexcept {
        if (!module_) return E_NOTIMPL;
        using Fn = HRESULT(WINAPI*)(DWORD*, BOOL*);
        const auto fn = reinterpret_cast<Fn>(GetProcAddress(module_, "DwmGetColorizationColor"));
        return fn ? fn(color, opaque) : E_NOTIMPL;
    }

private:
    HMODULE module_ = nullptr;
};

double relative_luminance(COLORREF color) noexcept {
    const auto channel = [](int value) {
        const double s = static_cast<double>(value) / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(GetRValue(color)) + 0.7152 * channel(GetGValue(color)) + 0.0722 * channel(GetBValue(color));
}

bool read_hkcu_dword(const wchar_t* subkey, const wchar_t* value, DWORD& out) noexcept {
    DWORD size = sizeof(DWORD);
    return RegGetValueW(HKEY_CURRENT_USER, subkey, value, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS &&
        size == sizeof(DWORD);
}

COLORREF adjust_accent(COLORREF color, bool dark) noexcept {
    const double lum = relative_luminance(color);
    if (dark) {
        if (lum < 0.22) return blend_color(color, RGB(255, 255, 255), 115);
        if (lum > 0.78) return blend_color(color, RGB(0, 0, 0), 40);
        return color;
    }
    if (lum > 0.42) return blend_color(color, RGB(0, 0, 0), 95);
    if (lum < 0.04) return blend_color(color, RGB(255, 255, 255), 55);
    return color;
}

float rounded_rect_sdf(float px, float py, float cx, float cy, float hx, float hy, float radius) noexcept {
    const float qx = std::fabs(px - cx) - (hx - radius);
    const float qy = std::fabs(py - cy) - (hy - radius);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::hypot(ox, oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

std::uint32_t to_bgra(COLORREF color) noexcept {
    return 0xff000000u | (static_cast<std::uint32_t>(GetRValue(color)) << 16) |
        (static_cast<std::uint32_t>(GetGValue(color)) << 8) | static_cast<std::uint32_t>(GetBValue(color));
}

} // namespace

COLORREF blend_color(COLORREF base, COLORREF overlay, std::uint8_t alpha) noexcept {
    const int a = alpha;
    const int inv = 255 - a;
    const int r = (GetRValue(base) * inv + GetRValue(overlay) * a + 127) / 255;
    const int g = (GetGValue(base) * inv + GetGValue(overlay) * a + 127) / 255;
    const int b = (GetBValue(base) * inv + GetBValue(overlay) * a + 127) / 255;
    return RGB(r, g, b);
}

COLORREF readable_text_on(COLORREF background) noexcept {
    return relative_luminance(background) > 0.34 ? RGB(24, 24, 24) : RGB(255, 255, 255);
}

bool system_high_contrast() noexcept {
    HIGHCONTRASTW hc{sizeof(HIGHCONTRASTW)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) != FALSE &&
        (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool apps_use_dark_theme() noexcept {
    DWORD light = 1;
    return read_hkcu_dword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", light) && light == 0;
}

bool taskbar_uses_dark_theme() noexcept {
    DWORD light = 1;
    return read_hkcu_dword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", light) && light == 0;
}

COLORREF system_accent_color(bool dark) noexcept {
    DWORD value = 0;
    if (read_hkcu_dword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", value)) {
        // AccentColor is stored as ABGR; its lower 24 bits match COLORREF (0x00BBGGRR).
        return adjust_accent(static_cast<COLORREF>(value & 0x00ffffffu), dark);
    }
    DWORD argb = 0;
    BOOL opaque = FALSE;
    const DwmLibrary dwm;
    if (SUCCEEDED(dwm.get_colorization_color(&argb, &opaque))) {
        const COLORREF rgb = RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
        return adjust_accent(rgb, dark);
    }
    return dark ? RGB(96, 205, 255) : RGB(0, 103, 192);
}

UiTheme make_ui_theme(bool dark, bool high_contrast, COLORREF accent) noexcept {
    UiTheme theme{};
    theme.dark = dark;
    theme.high_contrast = high_contrast;
    if (high_contrast) {
        const auto window = GetSysColor(COLOR_WINDOW);
        const auto window_text = GetSysColor(COLOR_WINDOWTEXT);
        const auto highlight = GetSysColor(COLOR_HIGHLIGHT);
        const auto highlight_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
        const auto gray_text = GetSysColor(COLOR_GRAYTEXT);
        theme.surface = window;
        theme.surface_secondary = window;
        theme.input_surface = window;
        theme.border = window_text;
        theme.separator = window_text;
        theme.text = window_text;
        theme.text_secondary = gray_text;
        theme.text_muted = gray_text;
        theme.accent = highlight;
        theme.accent_text = highlight_text;
        theme.selection_bg = highlight;
        theme.selection_text = highlight_text;
        theme.hover_bg = blend_color(window, highlight, 45);
        theme.keycap_bg = window;
        theme.keycap_border = window_text;
        theme.keycap_text = window_text;
        theme.ai_badge_bg = window;
        theme.ai_badge_border = highlight;
        theme.ai_badge_text = window_text;
        theme.warning_accent = highlight;
        theme.warning_bg = window;
        theme.notice_accent = highlight;
        return theme;
    }

    theme.accent = accent;
    theme.accent_text = readable_text_on(accent);
    if (dark) {
        theme.surface = RGB(44, 44, 44);
        theme.surface_secondary = RGB(36, 36, 36);
        theme.input_surface = RGB(30, 30, 30);
        theme.border = RGB(72, 72, 72);
        theme.separator = RGB(62, 62, 62);
        theme.text = RGB(245, 245, 245);
        theme.text_secondary = RGB(180, 180, 180);
        theme.text_muted = RGB(150, 150, 150);
        theme.selection_bg = blend_color(theme.surface, accent, 42);
        theme.selection_text = theme.text;
        theme.hover_bg = RGB(56, 56, 56);
        theme.keycap_bg = RGB(56, 56, 56);
        theme.keycap_border = RGB(84, 84, 84);
        theme.keycap_text = RGB(214, 214, 214);
        theme.ai_badge_bg = blend_color(theme.surface, accent, 48);
        theme.ai_badge_border = blend_color(theme.surface, accent, 120);
        theme.ai_badge_text = blend_color(accent, RGB(255, 255, 255), 80);
        theme.warning_accent = RGB(255, 153, 164);
        theme.warning_bg = RGB(68, 39, 38);
        theme.notice_accent = accent;
    } else {
        theme.surface = RGB(252, 252, 252);
        theme.surface_secondary = RGB(243, 243, 243);
        theme.input_surface = RGB(255, 255, 255);
        theme.border = RGB(214, 214, 214);
        theme.separator = RGB(230, 230, 230);
        theme.text = RGB(27, 27, 27);
        theme.text_secondary = RGB(96, 96, 96);
        theme.text_muted = RGB(118, 118, 118);
        theme.selection_bg = blend_color(theme.surface, accent, 28);
        theme.selection_text = theme.text;
        theme.hover_bg = RGB(240, 240, 240);
        theme.keycap_bg = RGB(244, 244, 244);
        theme.keycap_border = RGB(204, 204, 204);
        theme.keycap_text = RGB(74, 74, 74);
        theme.ai_badge_bg = blend_color(theme.surface, accent, 28);
        theme.ai_badge_border = blend_color(theme.surface, accent, 96);
        theme.ai_badge_text = blend_color(accent, RGB(0, 0, 0), 36);
        theme.warning_accent = RGB(196, 43, 28);
        theme.warning_bg = RGB(253, 238, 236);
        theme.notice_accent = accent;
    }
    return theme;
}

UiTheme current_ui_theme() noexcept {
    const bool hc = system_high_contrast();
    const bool dark = !hc && apps_use_dark_theme();
    return make_ui_theme(dark, hc, hc ? GetSysColor(COLOR_HIGHLIGHT) : system_accent_color(dark));
}

ModePalette mode_palette(const UiTheme& theme, Mode mode) noexcept {
    if (theme.high_contrast) {
        if (mode == Mode::Latin) {
            return {theme.surface, theme.border, theme.text};
        }
        return {theme.accent, theme.border, theme.accent_text};
    }
    switch (mode) {
    case Mode::Hiragana:
        return {theme.accent, blend_color(theme.accent, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 36),
            theme.accent_text};
    case Mode::Katakana: {
        const COLORREF bg = theme.dark ? RGB(64, 184, 166) : RGB(15, 123, 108);
        return {bg, blend_color(bg, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 36), readable_text_on(bg)};
    }
    case Mode::FullWidthLatin: {
        const COLORREF bg = theme.dark ? RGB(176, 145, 232) : RGB(116, 77, 169);
        return {bg, blend_color(bg, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 36), readable_text_on(bg)};
    }
    default:
        return theme.dark ? ModePalette{RGB(56, 56, 56), RGB(92, 92, 92), RGB(220, 220, 220)}
                          : ModePalette{RGB(240, 240, 240), RGB(196, 196, 196), RGB(72, 72, 72)};
    }
}

bool apply_popup_frame(HWND window, const UiTheme& theme, PopupCorner corner) noexcept {
    if (!window) return false;
    const DwmLibrary dwm;
    const BOOL dark_mode = theme.dark ? TRUE : FALSE;
    dwm.set_window_attribute(window, dwm_immersive_dark_mode, &dark_mode, sizeof(dark_mode));
    const COLORREF border_color = theme.border;
    dwm.set_window_attribute(window, dwm_border_color, &border_color, sizeof(border_color));
    const DWORD preference = corner == PopupCorner::RoundSmall ? dwm_corner_round_small : dwm_corner_round;
    return SUCCEEDED(dwm.set_window_attribute(window, dwm_window_corner_preference, &preference, sizeof(preference)));
}

SurfaceCanvas::SurfaceCanvas(HDC target, int width, int height) noexcept
    : target_(target), width_(width), height_(height) {
    if (!target || width <= 0 || height <= 0) return;
    dc_ = CreateCompatibleDC(target);
    if (!dc_) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // Top-down DIB.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_ || !bits) {
        if (bitmap_) { DeleteObject(bitmap_); bitmap_ = nullptr; }
        DeleteDC(dc_); dc_ = nullptr;
        return;
    }
    previous_bitmap_ = SelectObject(dc_, bitmap_);
    pixels_ = static_cast<std::uint32_t*>(bits);
}

SurfaceCanvas::~SurfaceCanvas() {
    if (dc_) {
        if (previous_bitmap_) SelectObject(dc_, previous_bitmap_);
        DeleteDC(dc_);
    }
    if (bitmap_) DeleteObject(bitmap_);
}

void SurfaceCanvas::clear(COLORREF color) noexcept {
    if (!valid()) return;
    std::fill_n(pixels_, static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), to_bgra(color));
}

void SurfaceCanvas::blend_pixel(int x, int y, COLORREF color, std::uint8_t alpha) noexcept {
    if (alpha == 0 || x < 0 || y < 0 || x >= width_ || y >= height_) return;
    auto& dst = pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    if (alpha == 255) {
        dst = to_bgra(color);
        return;
    }
    const int a = alpha;
    const int inv = 255 - a;
    const int sr = GetRValue(color);
    const int sg = GetGValue(color);
    const int sb = GetBValue(color);
    const int dr = static_cast<int>((dst >> 16) & 0xff);
    const int dg = static_cast<int>((dst >> 8) & 0xff);
    const int db = static_cast<int>(dst & 0xff);
    const auto r = static_cast<std::uint32_t>((dr * inv + sr * a + 127) / 255);
    const auto g = static_cast<std::uint32_t>((dg * inv + sg * a + 127) / 255);
    const auto b = static_cast<std::uint32_t>((db * inv + sb * a + 127) / 255);
    dst = 0xff000000u | (r << 16) | (g << 8) | b;
}

void SurfaceCanvas::fill_rect(RECT rect, COLORREF color, std::uint8_t alpha) noexcept {
    if (!valid() || alpha == 0) return;
    const int x0 = std::max<int>(0, rect.left);
    const int y0 = std::max<int>(0, rect.top);
    const int x1 = std::min<int>(width_, rect.right);
    const int y1 = std::min<int>(height_, rect.bottom);
    if (x0 >= x1 || y0 >= y1) return;
    if (alpha == 255) {
        const auto value = to_bgra(color);
        for (int y = y0; y < y1; ++y) {
            std::fill_n(pixels_ + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + x0,
                static_cast<std::size_t>(x1 - x0), value);
        }
        return;
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) blend_pixel(x, y, color, alpha);
    }
}

void SurfaceCanvas::fill_rounded_rect(RECT rect, float radius, COLORREF color, std::uint8_t alpha) noexcept {
    if (!valid() || alpha == 0 || rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (radius <= 0.5f) { fill_rect(rect, color, alpha); return; }
    const float hx = static_cast<float>(rect.right - rect.left) * 0.5f;
    const float hy = static_cast<float>(rect.bottom - rect.top) * 0.5f;
    const float cx = static_cast<float>(rect.left) + hx;
    const float cy = static_cast<float>(rect.top) + hy;
    const float r = std::min(radius, std::min(hx, hy));
    const int x0 = std::max<int>(0, rect.left - 1);
    const int y0 = std::max<int>(0, rect.top - 1);
    const int x1 = std::min<int>(width_, rect.right + 1);
    const int y1 = std::min<int>(height_, rect.bottom + 1);
    const int inner_left = static_cast<int>(std::ceil(static_cast<float>(rect.left) + r));
    const int inner_right = static_cast<int>(std::floor(static_cast<float>(rect.right) - r));
    const int inner_top = static_cast<int>(std::ceil(static_cast<float>(rect.top) + r));
    const int inner_bottom = static_cast<int>(std::floor(static_cast<float>(rect.bottom) - r));
    const auto solid = to_bgra(color);
    for (int y = y0; y < y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        const bool in_mid_y = y >= inner_top && y < inner_bottom;
        for (int x = x0; x < x1; ++x) {
            if (in_mid_y && x >= inner_left && x < inner_right) {
                if (alpha == 255) {
                    pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)] = solid;
                } else {
                    blend_pixel(x, y, color, alpha);
                }
                continue;
            }
            const float d = rounded_rect_sdf(static_cast<float>(x) + 0.5f, py, cx, cy, hx, hy, r);
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            const auto eff = static_cast<std::uint8_t>(std::lround(cov * static_cast<float>(alpha)));
            blend_pixel(x, y, color, eff);
        }
    }
}

void SurfaceCanvas::stroke_rounded_rect(RECT rect, float radius, float thickness, COLORREF color, std::uint8_t alpha) noexcept {
    if (!valid() || alpha == 0 || thickness <= 0.0f || rect.right <= rect.left || rect.bottom <= rect.top) return;
    const float hx = static_cast<float>(rect.right - rect.left) * 0.5f;
    const float hy = static_cast<float>(rect.bottom - rect.top) * 0.5f;
    const float cx = static_cast<float>(rect.left) + hx;
    const float cy = static_cast<float>(rect.top) + hy;
    const float r = std::min(std::max(radius, 0.0f), std::min(hx, hy));
    const int x0 = std::max<int>(0, rect.left - 1);
    const int y0 = std::max<int>(0, rect.top - 1);
    const int x1 = std::min<int>(width_, rect.right + 1);
    const int y1 = std::min<int>(height_, rect.bottom + 1);
    const int margin = static_cast<int>(std::ceil(thickness + r)) + 1;
    const int skip_left = rect.left + margin;
    const int skip_right = rect.right - margin;
    const int skip_top = rect.top + margin;
    const int skip_bottom = rect.bottom - margin;
    for (int y = y0; y < y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        const bool skip_y = y >= skip_top && y < skip_bottom;
        for (int x = x0; x < x1; ++x) {
            if (skip_y && x >= skip_left && x < skip_right) {
                x = skip_right - 1;
                continue;
            }
            const float d = rounded_rect_sdf(static_cast<float>(x) + 0.5f, py, cx, cy, hx, hy, r);
            const float outer_cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            const float inner_cov = std::clamp(0.5f - (d + thickness), 0.0f, 1.0f);
            const float cov = std::clamp(outer_cov - inner_cov, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            const auto eff = static_cast<std::uint8_t>(std::lround(cov * static_cast<float>(alpha)));
            blend_pixel(x, y, color, eff);
        }
    }
}

void SurfaceCanvas::present() const noexcept {
    if (!valid()) return;
    BitBlt(target_, 0, 0, width_, height_, dc_, 0, 0, SRCCOPY);
}

HICON create_mode_tray_icon(std::wstring_view text, std::wstring_view font_face,
    int width, int height, COLORREF foreground) noexcept {
    if (width <= 0 || height <= 0 || text.empty()) return nullptr;
    const HDC screen = GetDC(nullptr);
    const HDC work_dc = CreateCompatibleDC(screen);
    if (screen) ReleaseDC(nullptr, screen);
    if (!work_dc) return nullptr;

    auto create_dib = [](HDC dc, int w, int h, std::uint32_t*& bits) -> HBITMAP {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* raw = nullptr;
        const HBITMAP bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
        bits = static_cast<std::uint32_t*>(raw);
        return bmp;
    };

    std::uint32_t* src_pixels = nullptr;
    const HBITMAP src_bmp = create_dib(work_dc, width, height, src_pixels);
    if (!src_bmp || !src_pixels) {
        if (src_bmp) DeleteObject(src_bmp);
        DeleteDC(work_dc);
        return nullptr;
    }

    // Render at native 1x resolution with ClearType on an opaque black surface:
    // TrueType hinting snaps horizontal strokes to integer pixel rows while
    // ClearType's 3x horizontal subpixels provide smooth curves once averaged
    // to monochrome alpha (no color fringing).
    const HGDIOBJ old_bmp = SelectObject(work_dc, src_bmp);
    std::fill_n(src_pixels, static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0xff000000u);

    const std::wstring face(font_face);
    const int font_height = -std::max(10, MulDiv(height, 98, 100));
    const HFONT font = CreateFontW(font_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face.c_str());
    const HGDIOBJ old_font = SelectObject(work_dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(work_dc, RGB(255, 255, 255));
    SetBkColor(work_dc, RGB(0, 0, 0));
    SetBkMode(work_dc, OPAQUE);
    RECT draw_rect{0, 0, width, height};
    DrawTextW(work_dc, text.data(), static_cast<int>(text.size()), &draw_rect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    SelectObject(work_dc, old_font);
    if (font) DeleteObject(font);
    SelectObject(work_dc, old_bmp);

    std::uint32_t* out_pixels = nullptr;
    const HBITMAP color_bmp = create_dib(work_dc, width, height, out_pixels);
    const HBITMAP mask_bmp = CreateBitmap(width, height, 1, 1, nullptr);
    HICON icon = nullptr;
    if (color_bmp && out_pixels && mask_bmp) {
        const int fr = GetRValue(foreground);
        const int fg = GetGValue(foreground);
        const int fb = GetBValue(foreground);
        // Gentle gamma boost prevents thin strokes (e.g. horizontal bars of "あ")
        // from washing out against dark taskbars while preserving smooth edges.
        const double gamma = relative_luminance(foreground) > 0.5 ? 0.68 : 0.82;
        const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = src_pixels[i];
            const int r = static_cast<int>((p >> 16) & 0xff);
            const int g = static_cast<int>((p >> 8) & 0xff);
            const int b = static_cast<int>(p & 0xff);
            const double coverage = static_cast<double>(r + g + b) / (3.0 * 255.0);
            const double boosted = coverage > 0.0 ? std::pow(coverage, gamma) : 0.0;
            const int alpha = std::clamp(static_cast<int>(std::lround(boosted * 255.0)), 0, 255);
            const auto pr = static_cast<std::uint32_t>((fr * alpha + 127) / 255);
            const auto pg = static_cast<std::uint32_t>((fg * alpha + 127) / 255);
            const auto pb = static_cast<std::uint32_t>((fb * alpha + 127) / 255);
            out_pixels[i] = (static_cast<std::uint32_t>(alpha) << 24) | (pr << 16) | (pg << 8) | pb;
        }
        // Zero mask lets the 32bpp alpha channel drive transparency cleanly.
        const HGDIOBJ prev_mask = SelectObject(work_dc, mask_bmp);
        PatBlt(work_dc, 0, 0, width, height, BLACKNESS);
        SelectObject(work_dc, prev_mask);

        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmMask = mask_bmp;
        info.hbmColor = color_bmp;
        icon = CreateIconIndirect(&info);
    }

    if (color_bmp) DeleteObject(color_bmp);
    if (mask_bmp) DeleteObject(mask_bmp);
    DeleteObject(src_bmp);
    DeleteDC(work_dc);
    return icon;
}

} // namespace nskkzime::windows
