#pragma once
#include "nskkzime/engine.h"
#include <windows.h>
#include <cstdint>
#include <string_view>

namespace nskkzime::windows {

struct ModePalette {
    COLORREF background = RGB(0, 103, 192);
    COLORREF border = RGB(0, 84, 158);
    COLORREF text = RGB(255, 255, 255);
};

struct UiTheme {
    bool dark = false;
    bool high_contrast = false;
    COLORREF surface = RGB(252, 252, 252);
    COLORREF surface_secondary = RGB(243, 243, 243);
    COLORREF input_surface = RGB(255, 255, 255);
    COLORREF border = RGB(214, 214, 214);
    COLORREF separator = RGB(230, 230, 230);
    COLORREF text = RGB(27, 27, 27);
    COLORREF text_secondary = RGB(96, 96, 96);
    COLORREF text_muted = RGB(118, 118, 118);
    COLORREF accent = RGB(0, 103, 192);
    COLORREF accent_text = RGB(255, 255, 255);
    COLORREF selection_bg = RGB(232, 242, 252);
    COLORREF selection_text = RGB(27, 27, 27);
    COLORREF hover_bg = RGB(241, 241, 241);
    COLORREF keycap_bg = RGB(243, 243, 243);
    COLORREF keycap_border = RGB(206, 206, 206);
    COLORREF keycap_text = RGB(72, 72, 72);
    COLORREF ai_badge_bg = RGB(230, 242, 253);
    COLORREF ai_badge_border = RGB(166, 208, 245);
    COLORREF ai_badge_text = RGB(0, 90, 170);
    COLORREF warning_accent = RGB(196, 43, 28);
    COLORREF warning_bg = RGB(253, 238, 236);
    COLORREF notice_accent = RGB(0, 103, 192);
};

enum class PopupCorner { Round, RoundSmall };

COLORREF blend_color(COLORREF base, COLORREF overlay, std::uint8_t alpha) noexcept;
COLORREF readable_text_on(COLORREF background) noexcept;

bool system_high_contrast() noexcept;
bool apps_use_dark_theme() noexcept;
bool taskbar_uses_dark_theme() noexcept;
COLORREF system_accent_color(bool dark) noexcept;

UiTheme make_ui_theme(bool dark, bool high_contrast, COLORREF accent) noexcept;
UiTheme current_ui_theme() noexcept;
ModePalette mode_palette(const UiTheme& theme, Mode mode) noexcept;

// Applies Windows 11 rounded corners, border color and dark-mode frame hint.
// Returns true when DWM rounded corners are active; false means fall back to a
// software border inside the client area.
bool apply_popup_frame(HWND window, const UiTheme& theme, PopupCorner corner) noexcept;

// Offscreen 32bpp BGRA surface for flicker-free double-buffered GDI painting
// with analytically anti-aliased rounded rectangles and strokes.
class SurfaceCanvas {
public:
    SurfaceCanvas(HDC target, int width, int height) noexcept;
    ~SurfaceCanvas();
    SurfaceCanvas(const SurfaceCanvas&) = delete;
    SurfaceCanvas& operator=(const SurfaceCanvas&) = delete;

    bool valid() const noexcept { return dc_ != nullptr && pixels_ != nullptr; }
    HDC dc() const noexcept { return dc_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    void clear(COLORREF color) noexcept;
    void fill_rect(RECT rect, COLORREF color, std::uint8_t alpha = 255) noexcept;
    void fill_rounded_rect(RECT rect, float radius, COLORREF color, std::uint8_t alpha = 255) noexcept;
    void stroke_rounded_rect(RECT rect, float radius, float thickness, COLORREF color, std::uint8_t alpha = 255) noexcept;
    void present() const noexcept;

private:
    void blend_pixel(int x, int y, COLORREF color, std::uint8_t alpha) noexcept;

    HDC target_ = nullptr;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

// Creates an anti-aliased 32bpp ARGB icon for the taskbar input-mode indicator.
HICON create_mode_tray_icon(std::wstring_view text, std::wstring_view font_face,
    int width, int height, COLORREF foreground) noexcept;

} // namespace nskkzime::windows
