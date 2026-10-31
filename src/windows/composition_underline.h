#pragma once
#include "module.h"
#include <wrl/client.h>
#include <cstdint>
#include <optional>
#include <string>

namespace nskkzime::windows {
enum class UnderlineStyle { Reading, Converted };
// Screen rectangle of the substitute underline for `text`, limited to the
// owner's client area in screen coordinates. Empty compositions get a stub.
std::optional<RECT> composition_underline_bounds(RECT text, RECT client, UINT dpi) noexcept;

// The common EDIT accepts TSF edits through its transitory parent but never
// paints display attributes. Draw only the underline, click-through, over the
// host's own text so that the document stays the single source of truth.
class CompositionUnderline {
public:
    CompositionUnderline() = default;
    CompositionUnderline(const CompositionUnderline&) = delete;
    CompositionUnderline& operator=(const CompositionUnderline&) = delete;
    ~CompositionUnderline() { close(); }
    void locate(ITfContext*, ITfComposition*, TfEditCookie, UnderlineStyle) noexcept;
    void hide() noexcept;
    void close() noexcept;
    bool visible() const noexcept { return window_ && IsWindowVisible(window_); }
private:
    void show(HWND owner, const RECT& bounds, UINT dpi, UnderlineStyle) noexcept;
    void watch(HWND owner) noexcept;
    void unwatch() noexcept;
    void paint(HDC) const noexcept;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK moved(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    HWND window_ = nullptr, owner_ = nullptr, root_ = nullptr;
    HWINEVENTHOOK hook_ = nullptr;
    std::wstring class_name_;
    UINT dpi_ = 96;
    UnderlineStyle style_ = UnderlineStyle::Reading;
    std::uint64_t generation_ = 0;
};
} // namespace nskkzime::windows
