#include "candidate_ui.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT page_size = static_cast<UINT>(candidate_page_size);
constexpr DWORD all_updates = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
    TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
struct SelectedFont {
    HDC dc;
    HGDIOBJ previous;
    SelectedFont(HDC target, HFONT font) : dc(target), previous(SelectObject(target, font ? font : GetStockObject(DEFAULT_GUI_FONT))) {}
    ~SelectedFont() { SelectObject(dc, previous); }
};
struct WindowDC {
    HWND window;
    HDC value;
    explicit WindowDC(HWND target) : window(target), value(GetDC(target)) {}
    ~WindowDC() { if (value) ReleaseDC(window, value); }
};
std::wstring wide(std::u32string_view text) {
    std::wstring value;
    for (auto ch : text) {
        if (ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) ch = 0xfffd;
        if (ch <= 0xffff) value += static_cast<wchar_t>(ch);
        else { ch -= 0x10000; value += static_cast<wchar_t>(0xd800 + (ch >> 10)); value += static_cast<wchar_t>(0xdc00 + (ch & 1023)); }
    }
    return value;
}
std::wstring label(std::wstring_view text) {
    auto value = std::wstring(text.substr(0, 256));
    if (value.size() < text.size()) {
        if (!value.empty() && value.back() >= 0xd800 && value.back() <= 0xdbff) value.pop_back();
        value += L'…';
    }
    for (auto& ch : value) if (ch < L' ' || ch == 0x7f) ch = L' ';
    return value;
}
}
CandidateList::CandidateList(bool registration) : registration_(registration) { ++module_objects; }
CandidateList::~CandidateList() { close(); --module_objects; }
HRESULT CandidateList::QueryInterface(REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (registration_ && iid == IID_ITfCandidateListUIElement) return E_NOINTERFACE;
    if (iid != IID_IUnknown && iid != IID_ITfUIElement && iid != IID_ITfCandidateListUIElement) return E_NOINTERFACE;
    *output = static_cast<ITfCandidateListUIElement*>(this); AddRef(); return S_OK;
}
ULONG CandidateList::AddRef() { return ++refs_; }
ULONG CandidateList::Release() { const auto count = --refs_; if (!count) delete this; return count; }
HRESULT CandidateList::GetDescription(BSTR* value) {
    if (!value) return E_POINTER;
    *value = nullptr;
    try {
        const auto description = registration_ ? heading_ + L"\n" + draft_ + L"\n" + hint_ : L"nskkzime 候補一覧";
        *value = SysAllocString(description.c_str()); return *value ? S_OK : E_OUTOFMEMORY;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT CandidateList::GetGUID(GUID* value) { if (!value) return E_POINTER; *value = registration_ ? registration_ui_guid : candidate_ui_guid; return S_OK; }
HRESULT CandidateList::Show(BOOL show) { requested_visible_ = show != FALSE; render(); return S_OK; }
HRESULT CandidateList::IsShown(BOOL* value) { if (!value) return E_POINTER; *value = window_ && IsWindowVisible(window_); return S_OK; }
HRESULT CandidateList::GetUpdatedFlags(DWORD* value) { if (!value) return E_POINTER; *value = updated_; return S_OK; }
HRESULT CandidateList::GetDocumentMgr(ITfDocumentMgr** value) { if (!value) return E_POINTER; return document_.CopyTo(value); }
HRESULT CandidateList::GetCount(UINT* value) { if (!value) return E_POINTER; *value = static_cast<UINT>(rows_.size()); return S_OK; }
HRESULT CandidateList::GetSelection(UINT* value) { if (!value) return E_POINTER; *value = selection_; return S_OK; }
HRESULT CandidateList::GetString(UINT index, BSTR* value) {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (index >= rows_.size()) return E_INVALIDARG;
    *value = SysAllocStringLen(rows_[index].text.data(), static_cast<UINT>(rows_[index].text.size()));
    return *value ? S_OK : E_OUTOFMEMORY;
}
HRESULT CandidateList::GetPageIndex(UINT* indices, UINT capacity, UINT* count) {
    if (!count || (capacity && !indices)) return E_POINTER;
    *count = static_cast<UINT>(pages_.size());
    if (capacity) std::copy_n(pages_.begin(), std::min<std::size_t>(capacity, pages_.size()), indices);
    return capacity < pages_.size() ? S_FALSE : S_OK;
}
HRESULT CandidateList::SetPageIndex(UINT* indices, UINT count) {
    if (!indices) return E_POINTER;
    if (!count || count > rows_.size() || indices[0] != 0) return E_INVALIDARG;
    for (UINT i = 0; i < count; ++i) if (indices[i] >= rows_.size() || (i && indices[i] <= indices[i - 1])) return E_INVALIDARG;
    try { pages_.assign(indices, indices + count); updated_ |= TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE; render(); return S_OK; }
    catch (...) { return E_OUTOFMEMORY; }
}
UINT CandidateList::current_page() const {
    return pages_.empty() ? 0 : static_cast<UINT>(std::upper_bound(pages_.begin(), pages_.end(), selection_) - pages_.begin() - 1);
}
std::pair<UINT, UINT> CandidateList::visible_rows() const {
    if (pages_.empty()) return {0, 0};
    const auto page = current_page();
    auto first = pages_[page];
    const auto end = page + 1 < pages_.size() ? pages_[page + 1] : static_cast<UINT>(rows_.size());
    // A host can choose pages larger than our popup. Keep the selected row visible.
    if (selection_ - first >= page_size) first = selection_ - page_size + 1;
    return {first, first + std::min(page_size, end - first)};
}
HRESULT CandidateList::GetCurrentPage(UINT* value) { if (!value) return E_POINTER; *value = current_page(); return S_OK; }
void CandidateList::update(const Engine& engine, ITfDocumentMgr* document) {
    if (closed_) return;
    if (engine.candidates().size() > std::numeric_limits<UINT>::max()) throw std::length_error("too many candidates");
    std::vector<Row> rows;
    rows.reserve(engine.candidates().size());
    for (std::size_t i = 0; i < engine.candidates().size(); ++i)
        rows.push_back({wide(engine.candidate_text(i)), wide(engine.candidates()[i].annotation)});
    updated_ = 0;
    if (registration_) {
        const auto heading = L"単語登録 " + std::to_wstring(engine.registration_depth()) + L"  「" + wide(engine.registration_reading()) + L"」";
        const auto draft = wide(engine.registration_text());
        const std::wstring hint = engine.registration_limit() ? L"入力上限: 1024文字・再帰8段まで" :
            L"語幹を入力  Enter: 登録  C-j: かな  Esc: 取消";
        if (heading != heading_ || draft != draft_ || hint != hint_) updated_ |= all_updates;
        heading_ = heading; draft_ = draft; hint_ = hint;
    }
    if (document_.Get() != document) updated_ |= TF_CLUIE_DOCUMENTMGR;
    document_ = document;
    if (rows != rows_) {
        std::vector<UINT> pages;
        for (std::size_t index = 0; index < rows.size(); index += page_size) pages.push_back(static_cast<UINT>(index));
        rows_ = std::move(rows); pages_ = std::move(pages);
        updated_ |= all_updates;
    }
    const auto selection = static_cast<UINT>(engine.candidate_index());
    if (selection != selection_) updated_ |= TF_CLUIE_SELECTION | TF_CLUIE_CURRENTPAGE;
    selection_ = selection;
}
void CandidateList::locate(HWND owner, const RECT* anchor) noexcept {
    owner_ = owner;
    located_ = anchor && anchor->bottom > anchor->top;
    if (located_) anchor_ = *anchor;
    render();
}
void CandidateList::close() noexcept {
    closed_ = true; located_ = false; requested_visible_ = false;
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    if (!class_name_.empty()) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); }
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    owner_ = nullptr; document_.Reset();
}
void CandidateList::render() noexcept {
    if (closed_ || !located_ || !requested_visible_ || (rows_.empty() && !registration_)) {
        if (window_) ShowWindow(window_, SW_HIDE);
        return;
    }
    try {
        if (!window_) {
            if (!class_name_.empty()) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); }
            wchar_t name[96]{}; swprintf_s(name, L"nskkzime.Candidates.%p", static_cast<void*>(this));
            class_name_ = name;
            WNDCLASSW cls{}; cls.lpfnWndProc = window_proc; cls.hInstance = module_instance;
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = class_name_.c_str();
            cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
            if (!RegisterClassW(&cls)) { class_name_.clear(); return; }
            window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                class_name_.c_str(), registration_ ? L"nskkzime 単語登録" : L"nskkzime 候補", WS_POPUP | WS_BORDER, 0, 0, 0, 0,
                owner_ ? GetAncestor(owner_, GA_ROOT) : nullptr, nullptr, module_instance, this);
            if (!window_) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); return; }
        }
        const auto dpi = owner_ ? GetDpiForWindow(owner_) : 96U;
        if (dpi_ != dpi || !font_) {
            dpi_ = dpi ? dpi : 96;
            if (font_) DeleteObject(font_);
            font_ = CreateFontW(-MulDiv(18, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
            line_height_ = MulDiv(32, dpi_, 96); padding_ = MulDiv(10, dpi_, 96);
        }
        // Keep measurement bounded even if the host supplies a custom large page.
        const auto [first, visible_end] = visible_rows();
        WindowDC dc(window_);
        if (!dc.value) return;
        SelectedFont selected(dc.value, font_);
        const auto footer_text = footer(); SIZE footer_size{};
        GetTextExtentPoint32W(dc.value, footer_text.c_str(), static_cast<int>(footer_text.size()), &footer_size);
        int width = std::max(MulDiv(360, dpi_, 96), static_cast<int>(footer_size.cx) + padding_ * 2);
        if (registration_) width = std::max(width, MulDiv(580, dpi_, 96));
        for (UINT i = first; i < visible_end; ++i) {
            const auto row = std::to_wstring(i + 1) + L"  " + label(rows_[i].text) + L"  " + label(rows_[i].annotation);
            SIZE size{}; GetTextExtentPoint32W(dc.value, row.c_str(), static_cast<int>(row.size()), &size);
            width = std::max(width, static_cast<int>(size.cx) + padding_ * 2);
        }
        width = std::min(width, MulDiv(680, dpi_, 96));
        int height = (static_cast<int>(visible_end - first + 1) + (registration_ ? 2 : 0)) * line_height_ + padding_ * 2;
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        const auto x = std::clamp(anchor_.left, monitor.rcWork.left, monitor.rcWork.right - width);
        auto y = anchor_.bottom;
        if (y + height > monitor.rcWork.bottom) y = anchor_.top - height;
        y = std::clamp(y, monitor.rcWork.top, monitor.rcWork.bottom - height);
        SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(window_, nullptr, FALSE);
    } catch (...) { if (window_) ShowWindow(window_, SW_HIDE); }
}
std::wstring CandidateList::footer() const {
    return registration_ ? hint_ : std::to_wstring(selection_ + 1) + L" / " + std::to_wstring(rows_.size()) +
        L"   Space/x: 次/前  PgDn/PgUp: 7件  Enter: 確定";
}
void CandidateList::paint(HDC dc) const {
    RECT area{}; GetClientRect(window_, &area);
    FillRect(dc, &area, GetSysColorBrush(COLOR_WINDOW));
    SelectedFont selected(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    int top = padding_;
    const auto line = [&](std::wstring_view value, UINT format = DT_END_ELLIPSIS) {
        RECT rect{padding_, top, area.right - padding_, top + line_height_};
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | format);
        top += line_height_;
    };
    if (registration_) {
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT)); line(label(heading_));
        // Keep the currently edited tail visible for a long registration.
        const auto offset = draft_.size() > 160 ? draft_.size() - 160 : 0;
        auto tail = draft_.substr(offset);
        if (!tail.empty() && tail.front() >= 0xdc00 && tail.front() <= 0xdfff) tail.erase(0, 1);
        const auto text = (offset ? L"…" : L"") + tail + L"▏";
        SIZE size{}; GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        line(text, size.cx > area.right - padding_ * 2 ? DT_RIGHT : DT_END_ELLIPSIS);
    }
    if (!pages_.empty()) {
        const auto [first, end] = visible_rows();
        for (UINT i = first; i < end; ++i, top += line_height_) {
            RECT row{0, top, area.right, top + line_height_};
            if (i == selection_) FillRect(dc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
            SetTextColor(dc, GetSysColor(i == selection_ ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            row.left += padding_; row.right -= padding_;
            const auto text = std::to_wstring(i + 1) + L"  " + label(rows_[i].text) +
                (rows_[i].annotation.empty() ? L"" : L"  — " + label(rows_[i].annotation));
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &row, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    line(footer());
}
LRESULT CALLBACK CandidateList::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CandidateList*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<CandidateList*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_PAINT && self) {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        try { self->paint(dc); } catch (...) { /* Never throw through the window procedure. */ }
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_NCDESTROY && self) { self->window_ = nullptr; SetWindowLongPtrW(window, GWLP_USERDATA, 0); }
    return DefWindowProcW(window, message, wparam, lparam);
}
void CandidatePresenter::update(ITfThreadMgr* thread, ITfContext* context, const Engine& engine) noexcept {
    if (!engine.registering() && (engine.stage() != Stage::Selecting || engine.candidates().empty())) { close(); return; }
    if (list_ && list_->registration() != engine.registering()) close();
    try {
        ComPtr<ITfDocumentMgr> document;
        if (!context || FAILED(context->GetDocumentMgr(&document)) || !document) { close(); return; }
        if (!list_) {
            if (!thread || FAILED(thread->QueryInterface(IID_PPV_ARGS(&manager_)))) return;
            list_.Attach(new CandidateList(engine.registering()));
            list_->update(engine, document.Get());
            BOOL show = TRUE;
            const auto list = list_; const auto manager = manager_;
            DWORD id = 0;
            const auto hr = manager->BeginUIElement(list.Get(), &show, &id);
            if (list_.Get() != list.Get()) { if (SUCCEEDED(hr)) manager->EndUIElement(id); return; }
            if (FAILED(hr)) { close(); return; }
            id_ = id; begun_ = true; list->Show(show);
        } else {
            list_->update(engine, document.Get());
            DWORD flags = 0; list_->GetUpdatedFlags(&flags);
            const auto list = list_; const auto manager = manager_;
            if (flags && FAILED(manager->UpdateUIElement(id_)) && list_.Get() == list.Get()) close();
        }
    } catch (...) { close(); }
}
void CandidatePresenter::locate(ITfContext* context, ITfComposition* composition, TfEditCookie cookie) noexcept {
    const auto list = list_;
    if (!list) return;
    // Host callbacks can end the composition while answering a layout query.
    const ComPtr<ITfContext> target(context);
    const ComPtr<ITfComposition> current(composition);
    ComPtr<ITfContextView> view;
    ComPtr<ITfRange> range, caret;
    RECT rect{}; BOOL clipped = FALSE; HWND owner = nullptr;
    if (!target || !current || FAILED(target->GetActiveView(&view)) || !view ||
        FAILED(current->GetRange(&range)) || !range || FAILED(range->Clone(&caret)) || !caret) {
        list->locate(nullptr, nullptr); return;
    }
    auto hr = caret->Collapse(cookie, TF_ANCHOR_START);
    if (SUCCEEDED(hr)) hr = view->GetTextExt(cookie, caret.Get(), &rect, &clipped);
    if (FAILED(hr) || rect.bottom <= rect.top) hr = view->GetTextExt(cookie, range.Get(), &rect, &clipped);
    view->GetWnd(&owner);
    if (FAILED(hr) || clipped || rect.bottom <= rect.top) { list->locate(nullptr, nullptr); return; }
    if (list_.Get() == list.Get()) list->locate(owner, &rect);
}
void CandidatePresenter::hide() noexcept { if (list_) list_->locate(nullptr, nullptr); }
void CandidatePresenter::close() noexcept {
    // Invalidate before notifying the host; retained elements cannot reopen UI.
    auto list = std::move(list_); auto manager = std::move(manager_);
    const auto id = id_; const auto begun = begun_; begun_ = false;
    if (list) list->close();
    if (begun && manager) manager->EndUIElement(id);
}
} // namespace nskkzime::windows
