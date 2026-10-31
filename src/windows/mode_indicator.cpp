#include "mode_indicator.h"
#include "mode_label.h"
#include "tsf_edit.h"
#include <algorithm>
#include <utility>

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT_PTR expiry_timer = 1;
constexpr UINT_PTR position_retry_timer = 2;
constexpr unsigned int position_retry_delays[] = {25, 50, 100, 200};
constexpr DWORD window_style = WS_POPUP | WS_BORDER;
constexpr DWORD window_ex_style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
}

ModeIndicator::ModeIndicator(Mode mode, const UiSettings& settings)
    : mode_(mode), font_face_(settings.font), duration_ms_(settings.input_mode_duration_ms) { ++module_objects; }
ModeIndicator::~ModeIndicator() { close(); --module_objects; }
HRESULT ModeIndicator::QueryInterface(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfUIElement) *out = static_cast<ITfUIElement*>(this);
    else if (iid == IID_ITfTextLayoutSink) *out = static_cast<ITfTextLayoutSink*>(this);
    else if (iid == IID_ITfTextEditSink) *out = static_cast<ITfTextEditSink*>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
}
ULONG ModeIndicator::AddRef() { return ++refs_; }
ULONG ModeIndicator::Release() { const auto count = --refs_; if (!count) delete this; return count; }
HRESULT ModeIndicator::GetDescription(BSTR* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        const auto description = std::wstring(L"nskkzime 入力モード: ") + mode_name(mode_) + L" [" + mode_label(mode_) + L"]";
        *out = SysAllocString(description.c_str());
    } catch (...) { return E_OUTOFMEMORY; }
    return *out ? S_OK : E_OUTOFMEMORY;
}
HRESULT ModeIndicator::GetGUID(GUID* out) {
    if (!out) return E_POINTER;
    *out = mode_indicator_ui_guid; return S_OK;
}
HRESULT ModeIndicator::Show(BOOL show) {
    const ComPtr<ModeIndicator> self(this);
    if (!closed_) {
        requested_visible_ = show != FALSE;
        if (!show) cancel_retry();
        render();
        if (show && !located_) request_position();
    }
    return S_OK;
}
HRESULT ModeIndicator::IsShown(BOOL* out) {
    if (!out) return E_POINTER;
    *out = window_ && IsWindowVisible(window_); return S_OK;
}

void ModeIndicator::start(ITfThreadMgr* thread, ITfContext* context, TfClientId client, ITfContext* origin) noexcept {
    const ComPtr<ModeIndicator> self(this);
    if (started_ || closed_) return;
    started_ = true;
    if (!thread || !context || client == TF_CLIENTID_NULL || duration_ms_ < 1 || duration_ms_ > 60000) { close(); return; }
    try {
        thread_ = thread; context_ = context; client_ = client;
        deadline_ = GetTickCount64() + duration_ms_;
        retry_deadline_ = std::min(deadline_, GetTickCount64() + 500);
        ComPtr<ITfDocumentMgr> document;
        ComPtr<ITfContext> top;
        if (FAILED(thread->GetFocus(&document)) || !document || FAILED(document->GetTop(&top)) ||
            top.Get() != (origin ? origin : context) || closed_) { close(); return; }
        focus_document_ = document; focus_context_ = top;
        ComPtr<ITfContextView> view;
        if (FAILED(context->GetActiveView(&view)) || !view || closed_) { close(); return; }
        HWND owner = nullptr;
        if (FAILED(view->GetWnd(&owner)) || !owner) owner = GetFocus();
        if (closed_ || !owner || !IsWindow(owner)) { close(); return; }
        owner_ = owner;

        wchar_t name[96]{};
        swprintf_s(name, L"nskkzime.InputMode.%p", static_cast<void*>(this));
        class_name_ = name;
        WNDCLASSW cls{};
        cls.style = CS_IME; cls.lpfnWndProc = window_proc; cls.hInstance = module_instance;
        cls.lpszClassName = class_name_.c_str(); cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        if (!RegisterClassW(&cls)) { class_name_.clear(); close(); return; }
        const auto window = CreateWindowExW(window_ex_style, class_name_.c_str(), L"nskkzime 入力モード", window_style,
            0, 0, 0, 0, owner, nullptr, module_instance, this);
        if (closed_) { if (window) DestroyWindow(window); close(); return; }
        window_ = window;
        // A hidden window bounds the notification lifetime even if the host
        // never services our asynchronous position request.
        if (!window_ || closed_ || !SetTimer(window_, expiry_timer, duration_ms_, nullptr)) { close(); return; }

        ComPtr<ITfSource> source;
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&source))) || !source || closed_) { close(); return; }
        DWORD cookie = TF_INVALID_COOKIE;
        auto hr = source->AdviseSink(IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink*>(this), &cookie);
        if (closed_) { if (SUCCEEDED(hr)) source->UnadviseSink(cookie); return; }
        if (FAILED(hr)) { close(); return; }
        layout_cookie_ = cookie;
        cookie = TF_INVALID_COOKIE;
        hr = source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &cookie);
        if (closed_) { if (SUCCEEDED(hr)) source->UnadviseSink(cookie); return; }
        if (FAILED(hr)) { close(); return; }
        edit_cookie_ = cookie;

        ComPtr<ITfUIElementMgr> manager;
        if (FAILED(thread->QueryInterface(IID_PPV_ARGS(&manager))) || !manager || !current()) { close(); return; }
        manager_ = manager;
        BOOL show = TRUE;
        DWORD id = 0;
        hr = manager->BeginUIElement(static_cast<ITfUIElement*>(this), &show, &id);
        if (closed_) { if (SUCCEEDED(hr)) manager->EndUIElement(id); return; }
        if (FAILED(hr)) { close(); return; }
        ui_id_ = id; begun_ = true; requested_visible_ = show != FALSE;
        if (!show && FAILED(manager->UpdateUIElement(id))) { close(); return; }
        if (!closed_) request_position();
    } catch (...) { close(); }
}

bool ModeIndicator::current() noexcept {
    if (closed_ || !thread_ || !context_ || GetTickCount64() >= deadline_) return false;
    // Keep local references: focus queries may re-enter and close this element.
    const auto thread = thread_;
    const auto expected_document = focus_document_;
    const auto expected_context = focus_context_;
    ComPtr<ITfDocumentMgr> document;
    ComPtr<ITfContext> context;
    return SUCCEEDED(thread->GetFocus(&document)) && document && document.Get() == expected_document.Get() &&
        SUCCEEDED(document->GetTop(&context)) && context.Get() == expected_context.Get() &&
        !closed_ && GetTickCount64() < deadline_;
}
void ModeIndicator::request_position() noexcept {
    if (closed_ || position_pending_) return;
    try {
        if (!current()) { close(); return; }
        cancel_retry();
        const ComPtr<ModeIndicator> self(this);
        const auto context = context_;
        position_pending_ = true;
        const auto hr = request_edit(context.Get(), client_, TF_ES_ASYNC | TF_ES_READ,
            [self](TfEditCookie cookie) {
                self->position_pending_ = false;
                if (!self->closed_) self->locate(cookie);
                return S_OK;
            });
        if (FAILED(hr)) {
            position_pending_ = false;
            if (hr == TF_E_LOCKED) retry_position();
            else close();
        }
    } catch (...) { close(); }
}
void ModeIndicator::cancel_retry() noexcept {
    if (window_) KillTimer(window_, position_retry_timer);
    retry_scheduled_ = false;
}
void ModeIndicator::retry_position() noexcept {
    if (closed_ || !window_ || !requested_visible_ || retry_scheduled_ ||
        retries_ >= std::size(position_retry_delays) || GetTickCount64() >= retry_deadline_) return;
    if (!current()) { close(); return; }
    if (closed_ || !window_ || !requested_visible_) return;
    // Mode-only keys need not produce OnEndEdit/OnLayoutChange. Retry a few
    // times while the same focused text store finishes its initial layout.
    const auto delay = position_retry_delays[retries_++];
    if (GetTickCount64() + delay >= retry_deadline_) return;
    retry_scheduled_ = SetTimer(window_, position_retry_timer, delay, nullptr) != 0;
}
void ModeIndicator::locate(TfEditCookie cookie) noexcept {
    if (closed_ || locating_) return;
    locating_ = true;
    try {
        const auto context = context_;
        if (!current()) { close(); locating_ = false; return; }
        ComPtr<ITfContextView> view;
        TF_SELECTION selection{};
        ULONG count = 0;
        auto hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count);
        ComPtr<ITfRange> range;
        range.Attach(selection.range);
        if (SUCCEEDED(hr) && count == 1 && range && !selection.style.fInterimChar && !closed_) {
            hr = range->Collapse(cookie, selection.style.ase == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END);
            if (SUCCEEDED(hr)) hr = context->GetActiveView(&view);
        } else hr = E_FAIL;
        RECT rect{};
        BOOL clipped = FALSE;
        HWND owner = nullptr;
        if (SUCCEEDED(hr) && view && !closed_) {
            if (FAILED(view->GetWnd(&owner)) || !owner) owner = GetFocus();
            hr = view->GetTextExt(cookie, range.Get(), &rect, &clipped);
        }
        ComPtr<ITfContextView> latest_view;
        ComPtr<IUnknown> identity, latest_identity;
        if (SUCCEEDED(hr) && !closed_ && (FAILED(context->GetActiveView(&latest_view)) || !latest_view ||
            FAILED(view.As(&identity)) || FAILED(latest_view.As(&latest_identity)) || identity != latest_identity)) hr = E_FAIL;
        if (!closed_ && current()) {
            located_ = SUCCEEDED(hr) && !clipped && rect.bottom > rect.top && owner == owner_ && IsWindow(owner);
            if (located_) anchor_ = rect;
            // No cached coordinates: a missing layout must hide the old marker.
            render();
            if (located_) cancel_retry();
            else if (!selection.style.fInterimChar && !clipped &&
                (FAILED(hr) || (owner == owner_ && rect.bottom <= rect.top))) retry_position();
        } else close();
    } catch (...) { close(); }
    locating_ = false;
}
HRESULT ModeIndicator::OnLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView*) {
    const ComPtr<ModeIndicator> self(this);
    if (closed_ || context != context_.Get()) return S_OK;
    if (code == TF_LC_DESTROY) close();
    else { located_ = false; render(); request_position(); }
    return S_OK;
}
HRESULT ModeIndicator::OnEndEdit(ITfContext* context, TfEditCookie cookie, ITfEditRecord*) {
    const ComPtr<ModeIndicator> self(this);
    if (!closed_ && context == context_.Get()) locate(cookie);
    return S_OK;
}

void ModeIndicator::render() noexcept {
    if (closed_) return;
    if (!current()) { close(); return; }
    if (!located_ || !requested_visible_) {
        if (window_) ShowWindow(window_, SW_HIDE);
        return;
    }
    try {
        const auto dpi = GetDpiForWindow(owner_);
        if (!font_ || dpi_ != dpi) {
            dpi_ = dpi ? dpi : 96;
            if (font_) DeleteObject(font_);
            font_ = CreateFontW(-MulDiv(18, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, font_face_.c_str());
        }
        const auto side = MulDiv(32, dpi_, 96);
        RECT outer{0, 0, side, side};
        if (!AdjustWindowRectExForDpi(&outer, window_style, FALSE, window_ex_style, dpi_)) { close(); return; }
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &monitor)) { close(); return; }
        const auto width = std::min(outer.right - outer.left, monitor.rcWork.right - monitor.rcWork.left);
        const auto height = std::min(outer.bottom - outer.top, monitor.rcWork.bottom - monitor.rcWork.top);
        const auto gap = MulDiv(2, dpi_, 96);
        const auto x = std::clamp(anchor_.left, monitor.rcWork.left, monitor.rcWork.right - width);
        auto y = anchor_.bottom + gap;
        if (y + height > monitor.rcWork.bottom) y = anchor_.top - height - gap;
        y = std::clamp(y, monitor.rcWork.top, monitor.rcWork.bottom - height);
        if (!SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW)) { close(); return; }
        if (!closed_ && window_) InvalidateRect(window_, nullptr, FALSE);
    } catch (...) { close(); }
}
void ModeIndicator::close() noexcept {
    if (closed_) {
        // An owner-driven DestroyWindow may still have been dispatching its
        // final messages when we first tried to unregister the class.
        if (!window_ && !class_name_.empty() && UnregisterClassW(class_name_.c_str(), module_instance)) class_name_.clear();
        return;
    }
    closed_ = true; located_ = false; requested_visible_ = false;
    auto context = std::move(context_);
    auto manager = std::move(manager_);
    const auto begun = begun_; begun_ = false;
    const auto layout_cookie = layout_cookie_, edit_cookie = edit_cookie_;
    layout_cookie_ = edit_cookie_ = TF_INVALID_COOKIE;
    thread_.Reset(); focus_document_.Reset(); focus_context_.Reset();
    cancel_retry();
    if (window_) { KillTimer(window_, expiry_timer); DestroyWindow(window_); window_ = nullptr; }
    if (!class_name_.empty() && UnregisterClassW(class_name_.c_str(), module_instance)) class_name_.clear();
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    ComPtr<ITfSource> source;
    if (context && SUCCEEDED(context.As(&source))) {
        if (layout_cookie != TF_INVALID_COOKIE) source->UnadviseSink(layout_cookie);
        if (edit_cookie != TF_INVALID_COOKIE) source->UnadviseSink(edit_cookie);
    }
    if (begun && manager) manager->EndUIElement(ui_id_);
}
LRESULT CALLBACK ModeIndicator::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ModeIndicator*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ModeIndicator*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    const ComPtr<ModeIndicator> keep(self);
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_TIMER && wparam == expiry_timer && self) {
        if (GetTickCount64() >= self->deadline_) self->close();
        return 0;
    }
    if (message == WM_TIMER && wparam == position_retry_timer && self) {
        self->cancel_retry();
        if (!self->closed_ && GetTickCount64() < self->retry_deadline_) self->request_position();
        return 0;
    }
    if (message == WM_DPICHANGED && self) { self->render(); return 0; }
    if (message == WM_PAINT && self) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        if (dc) {
            RECT rect{}; GetClientRect(window, &rect);
            FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW));
            const auto previous = SelectObject(dc, self->font_ ? self->font_ : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT)); SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, mode_label(self->mode_), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, previous);
        }
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_WINDOWPOSCHANGED && self) {
        const bool visible = IsWindowVisible(window) != FALSE;
        const bool was_visible = std::exchange(self->ime_visible_, visible);
        const auto position = reinterpret_cast<const WINDOWPOS*>(lparam);
        if (visible != was_visible)
            NotifyWinEvent(visible ? EVENT_OBJECT_IME_SHOW : EVENT_OBJECT_IME_HIDE, window, OBJID_CLIENT, CHILDID_SELF);
        else if (visible && position && (position->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
            NotifyWinEvent(EVENT_OBJECT_IME_CHANGE, window, OBJID_CLIENT, CHILDID_SELF);
    }
    if (message == WM_DESTROY && self) {
        if (std::exchange(self->ime_visible_, false)) NotifyWinEvent(EVENT_OBJECT_IME_HIDE, window, OBJID_CLIENT, CHILDID_SELF);
        if (!self->closed_) { self->window_ = nullptr; self->close(); }
    }
    if (message == WM_NCDESTROY && self) { self->window_ = nullptr; SetWindowLongPtrW(window, GWLP_USERDATA, 0); }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace nskkzime::windows
