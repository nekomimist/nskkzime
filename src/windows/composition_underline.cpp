#include "composition_underline.h"
#include <algorithm>
#include <vector>

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
namespace {
constexpr DWORD underline_style = WS_POPUP | WS_DISABLED;
// Layered + transparent keeps every click on the host's text underneath.
constexpr DWORD underline_ex_style = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
constexpr COLORREF transparent_key = RGB(255, 0, 255);
thread_local std::vector<CompositionUnderline*> watching;

int band(UINT dpi) noexcept { return std::max(1, MulDiv(1, dpi ? static_cast<int>(dpi) : 96, 96)); }
COLORREF opaque(COLORREF color) noexcept { return color == transparent_key ? RGB(254, 0, 255) : color; }
}

std::optional<RECT> composition_underline_bounds(RECT text, RECT client, UINT dpi) noexcept {
    if (text.bottom <= text.top || text.right < text.left || client.right <= client.left || client.bottom <= client.top) return {};
    // Two bands of the window text and background colors stay visible on
    // both light and dark fields without sampling the host's pixels.
    const auto height = band(dpi) * 2;
    // A fresh abbrev or okuri start has no text yet but is still composing.
    const auto width = std::max(text.right - text.left, (text.bottom - text.top) / 2);
    const RECT line{text.left, text.bottom - height, text.left + width, text.bottom};
    RECT visible{};
    if (!IntersectRect(&visible, &line, &client)) return {};
    return visible;
}

void CompositionUnderline::locate(ITfContext* context, ITfComposition* composition, TfEditCookie cookie, UnderlineStyle style) noexcept {
    const auto generation = ++generation_;
    // Host callbacks can end the composition while answering a layout query.
    const ComPtr<ITfContext> target(context);
    const ComPtr<ITfComposition> current(composition);
    ComPtr<ITfContextView> view;
    ComPtr<ITfRange> range;
    if (!target || !current || FAILED(target->GetActiveView(&view)) || !view || FAILED(current->GetRange(&range)) || !range) {
        if (generation == generation_) hide();
        return;
    }
    HWND owner = nullptr;
    if (FAILED(view->GetWnd(&owner)) || !owner) owner = GetFocus();
    RECT text{}; BOOL clipped = FALSE;
    const auto hr = view->GetTextExt(cookie, range.Get(), &text, &clipped);
    if (generation != generation_) return;
    // SetText can invalidate layout until OnLayoutChange refreshes it.
    if (hr == TF_E_NOLAYOUT && visible() && owner == owner_) return;
    RECT client{};
    POINT origin{};
    if (FAILED(hr) || !owner || !IsWindow(owner) || !IsWindowVisible(owner) || !GetClientRect(owner, &client) || !ClientToScreen(owner, &origin)) {
        hide(); return;
    }
    OffsetRect(&client, origin.x, origin.y);
    const auto dpi = GetDpiForWindow(owner);
    const auto bounds = composition_underline_bounds(text, client, dpi);
    if (!bounds) { hide(); return; }
    show(owner, *bounds, dpi, style);
}

void CompositionUnderline::show(HWND owner, const RECT& bounds, UINT dpi, UnderlineStyle style) noexcept {
    if (window_ && owner_ != owner) close();
    if (!window_) {
        if (!class_name_.empty()) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); }
        try {
            wchar_t name[96]{}; swprintf_s(name, L"nskkzime.CompositionUnderline.%p", static_cast<void*>(this));
            class_name_ = name;
        } catch (...) { return; }
        WNDCLASSW cls{}; cls.style = CS_IME | CS_HREDRAW | CS_VREDRAW; cls.lpfnWndProc = window_proc; cls.hInstance = module_instance;
        cls.lpszClassName = class_name_.c_str();
        if (!RegisterClassW(&cls)) { class_name_.clear(); return; }
        // Owning the window by the host field keeps it just above that field.
        window_ = CreateWindowExW(underline_ex_style, class_name_.c_str(), L"nskkzime 未確定下線", underline_style,
            0, 0, 0, 0, owner, nullptr, module_instance, this);
        if (!window_) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); return; }
        if (!SetLayeredWindowAttributes(window_, transparent_key, 255, LWA_COLORKEY)) { close(); return; }
        owner_ = owner;
    }
    if (!dpi) dpi = 96;
    const bool restyled = dpi_ != dpi || style_ != style;
    dpi_ = dpi; style_ = style;
    SetWindowPos(window_, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (restyled) InvalidateRect(window_, nullptr, FALSE);
    watch(owner);
}

void CompositionUnderline::watch(HWND owner) noexcept {
    const auto root = GetAncestor(owner, GA_ROOT);
    if (hook_ && root_ == root) return;
    unwatch();
    DWORD process = 0;
    const auto thread = root ? GetWindowThreadProcessId(root, &process) : 0;
    if (!thread) return;
    try { watching.push_back(this); } catch (...) { return; }
    // A moved or resized field would leave the line floating at the old
    // place. Hide it until the next edit locates the composition again.
    hook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, moved,
        process, thread, WINEVENT_OUTOFCONTEXT);
    if (!hook_) { std::erase(watching, this); return; }
    root_ = root;
}

void CompositionUnderline::unwatch() noexcept {
    if (hook_) UnhookWinEvent(hook_);
    hook_ = nullptr; root_ = nullptr;
    std::erase(watching, this);
}

void CALLBACK CompositionUnderline::moved(HWINEVENTHOOK hook, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (object != OBJID_WINDOW || child != CHILDID_SELF) return;
    const auto found = std::find_if(watching.begin(), watching.end(),
        [&](const auto* item) { return item->hook_ == hook && (window == item->root_ || window == item->owner_); });
    // hide() unregisters the item, so leave the iteration first.
    if (found != watching.end()) (*found)->hide();
}

void CompositionUnderline::hide() noexcept {
    ++generation_;
    unwatch();
    if (window_) ShowWindow(window_, SW_HIDE);
}

void CompositionUnderline::close() noexcept {
    hide();
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    if (!class_name_.empty()) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); }
    owner_ = nullptr;
}

void CompositionUnderline::paint(HDC dc) const noexcept {
    RECT area{};
    if (!GetClientRect(window_, &area)) return;
    const auto fill = [dc](RECT rect, COLORREF color) {
        SetBkColor(dc, color); ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rect, nullptr, 0, nullptr);
    };
    fill(area, transparent_key);
    const auto unit = band(dpi_);
    const bool converted = style_ == UnderlineStyle::Converted;
    // Like the usual IME attributes: dotted while reading, solid once converted.
    const auto dark_height = unit;
    const auto dash = converted ? std::max<LONG>(area.right, 1) : 2 * unit;
    const auto step = converted ? dash : 4 * unit;
    const auto dark = opaque(GetSysColor(COLOR_WINDOWTEXT)), light = opaque(GetSysColor(COLOR_WINDOW));
    for (LONG x = 0; x < area.right; x += step) {
        const auto right = std::min(area.right, x + dash);
        fill({x, 0, right, dark_height}, dark);
        fill({x, dark_height, right, area.bottom}, light);
    }
}

LRESULT CALLBACK CompositionUnderline::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CompositionUnderline*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<CompositionUnderline*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && self) {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        if (dc) self->paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY && self) { self->window_ = nullptr; SetWindowLongPtrW(window, GWLP_USERDATA, 0); }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace nskkzime::windows
