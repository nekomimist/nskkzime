#include "candidate_ui.h"
#include "mode_label.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <windowsx.h>

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT page_size = static_cast<UINT>(candidate_page_size);
constexpr DWORD popup_style = WS_POPUP;
constexpr DWORD popup_ex_style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
constexpr DWORD all_updates = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
    TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;

// Debugger-only host diagnostics. Never include text, candidate contents,
// window titles, coordinates, or document identifiers.
void trace(const wchar_t* event, long first = 0, long second = 0, long third = 0) noexcept {
    if (!IsDebuggerPresent()) return;
    wchar_t line[160]{};
    swprintf_s(line, L"nskkzime.ui %s %ld %ld %ld\n", event, first, second, third);
    OutputDebugStringW(line);
}
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
SIZE measure_text(HDC dc, HFONT font, std::wstring_view text) noexcept {
    SelectedFont selected(dc, font);
    SIZE size{};
    if (!text.empty()) GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    return size;
}
bool is_generated_source(CandidateSource source) noexcept {
    return source == CandidateSource::GeneratedLong || source == CandidateSource::GeneratedFallback;
}
} // namespace

std::optional<std::size_t> candidate_row_at_top(POINT point, SIZE client, int top, int line_height,
    std::size_t first, std::size_t end) noexcept {
    if (line_height <= 0 || point.x < 0 || point.x >= client.cx || point.y < top || point.y >= client.cy || first >= end) return {};
    const auto row = static_cast<std::size_t>((point.y - top) / line_height);
    return row < end - first ? std::optional{first + row} : std::nullopt;
}

std::optional<std::size_t> candidate_row_at(POINT point, SIZE client, int padding, int line_height,
    bool registration, std::size_t first, std::size_t end) noexcept {
    const auto top = padding + (registration ? 2 * line_height : 0);
    return candidate_row_at_top(point, client, top, line_height, first, end);
}

CandidateList::CandidateList(PopupKind kind, std::weak_ptr<CandidateActions> actions, CandidateKeys keys, std::wstring font_face, bool show_input_mode)
    : kind_(kind), keys_(keys), font_face_(std::move(font_face)), show_input_mode_(show_input_mode), actions_(std::move(actions)) { ++module_objects; }
CandidateList::~CandidateList() { close(); --module_objects; }
HRESULT CandidateList::QueryInterface(REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (kind_ != PopupKind::Candidates && iid == IID_ITfCandidateListUIElement) return E_NOINTERFACE;
    if (iid != IID_IUnknown && iid != IID_ITfUIElement && iid != IID_ITfCandidateListUIElement) return E_NOINTERFACE;
    *output = static_cast<ITfCandidateListUIElement*>(this); AddRef(); return S_OK;
}
ULONG CandidateList::AddRef() { return ++refs_; }
ULONG CandidateList::Release() { const auto count = --refs_; if (!count) delete this; return count; }
HRESULT CandidateList::GetDescription(BSTR* value) {
    if (!value) return E_POINTER;
    *value = nullptr;
    try {
        const auto description = kind_ != PopupKind::Candidates ? heading_ + L"\n" + draft_ + L"\n" + hint_ : L"nskkzime 候補一覧";
        *value = SysAllocString(description.c_str()); return *value ? S_OK : E_OUTOFMEMORY;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT CandidateList::GetGUID(GUID* value) { if (!value) return E_POINTER; *value = kind_ == PopupKind::Notice ? notice_ui_guid : kind_ == PopupKind::Editor ? registration_ui_guid : candidate_ui_guid; return S_OK; }
HRESULT CandidateList::Show(BOOL show) {
    if (!show) { cancel_pointer(); hovered_.reset(); wheel_.reset(); discard_candidate_timing(paint_timing_); }
    requested_visible_ = show != FALSE;
    render();
    return S_OK;
}
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
    try { cancel_pointer(); hovered_.reset(); wheel_.reset(); pages_.assign(indices, indices + count); redraw_ = true; previous_selection_.reset(); updated_ |= TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE; render(); return S_OK; }
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
std::size_t CandidateList::shortcut_index(std::size_t slot) const noexcept {
    const auto [first, end] = visible_rows();
    return !closed_ && slot < end - first ? first + slot : std::numeric_limits<std::size_t>::max();
}
void CandidateList::request_selection(std::size_t index) noexcept {
    if (closed_ || index >= rows_.size()) return;
    if (const auto actions = actions_.lock(); actions && actions->select) {
        try { actions->select(index, revision_); } catch (...) { /* Preserve the existing selection on failure. */ }
    }
}
void CandidateList::request_step(int delta) noexcept {
    if (closed_ || !delta || rows_.empty()) return;
    if (const auto actions = actions_.lock(); actions && actions->step) {
        try { actions->step(delta, revision_); } catch (...) {}
    }
}
void CandidateList::begin_interaction() noexcept {
    if (closed_) return;
    if (const auto actions = actions_.lock(); actions && actions->interact) {
        try { actions->interact(revision_); } catch (...) {}
    }
}
CandidateList::Layout CandidateList::layout() const noexcept {
    Layout m{};
    m.pad_x = scale(8);
    m.pad_y = scale(6);
    m.row_height = scale(34);
    m.footer_height = scale(28);
    m.keycap_size = scale(22);
    m.badge_height = scale(18);
    if (kind_ == PopupKind::Candidates) {
        m.rows_top = m.pad_y;
    } else if (kind_ == PopupKind::Editor) {
        m.header_height = scale(34);
        if (forgetting_) {
            m.editor_height = has_forget_target_ ? scale(72) : scale(36);
            m.rows_top = m.header_height + m.editor_height;
        } else {
            m.editor_height = scale(52);
            m.rows_top = m.header_height + m.editor_height + (!pages_.empty() ? scale(5) : 0);
        }
    } else {
        m.header_height = 0;
        m.editor_height = 0;
        m.rows_top = 0;
    }
    return m;
}
std::optional<std::size_t> CandidateList::hit_test(POINT point) const noexcept {
    RECT area{};
    if (closed_ || !located_ || !requested_visible_ || !window_ || !IsWindowVisible(window_) || !GetClientRect(window_, &area)) return {};
    if (kind_ == PopupKind::Notice || forgetting_) return {};
    const auto [first, end] = visible_rows();
    const auto m = layout();
    return candidate_row_at_top(point, {area.right, m.rows_bottom(area.bottom)}, m.rows_top, m.row_height, first, end);
}
void CandidateList::cancel_pointer() noexcept {
    pointer_.cancel();
    if (window_ && GetCapture() == window_) ReleaseCapture();
}
HRESULT CandidateList::GetCurrentPage(UINT* value) { if (!value) return E_POINTER; *value = current_page(); return S_OK; }

void CandidateList::refresh_theme() noexcept {
    theme_ = current_ui_theme();
    if (!window_) return;
    dwm_framed_ = apply_popup_frame(window_, theme_, PopupCorner::Round);
}

void CandidateList::release_fonts() noexcept {
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (title_font_) { DeleteObject(title_font_); title_font_ = nullptr; }
    if (sub_font_) { DeleteObject(sub_font_); sub_font_ = nullptr; }
    if (badge_font_) { DeleteObject(badge_font_); badge_font_ = nullptr; }
}

void CandidateList::ensure_fonts(UINT dpi) noexcept {
    const UINT resolved = dpi ? dpi : 96;
    if (dpi_ == resolved && font_ && title_font_ && sub_font_ && badge_font_) return;
    previous_selection_.reset();
    redraw_ = true;
    dpi_ = resolved;
    release_fonts();
    const auto create = [&](int px, int weight) {
        return CreateFontW(-MulDiv(px, static_cast<int>(dpi_), 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, font_face_.c_str());
    };
    font_ = create(16, FW_NORMAL);
    title_font_ = create(13, FW_SEMIBOLD);
    sub_font_ = create(12, FW_NORMAL);
    badge_font_ = create(11, FW_SEMIBOLD);
}

void CandidateList::update(const Engine& engine, ITfDocumentMgr* document) {
    const auto old_rows = visible_rows();
    redraw_ |= input_method_ != engine.input_method();
    input_method_ = engine.input_method();
    if (closed_) return;
    if (revision_ != engine.revision()) { cancel_pointer(); hovered_.reset(); }
    revision_ = engine.revision();
    if (engine.candidates().size() > std::numeric_limits<UINT>::max()) throw std::length_error("too many candidates");
    updated_ = 0;
    if (engine.forgetting()) {
        const auto& target = engine.forget_target();
        const auto word = target ? wide(target->candidate.text) : std::wstring{};
        const auto scope = target ? L"読み: " + wide(target->key.reading) + (target->key.okuri ?
            L"  /  送り: " + (target->okuri.empty() ? std::wstring(L"共通") : wide(target->okuri)) : L"  /  送りなし") :
            std::wstring(L"学習が無効、未同期、または読み取り専用の候補です");
        const auto answer = target ? wide(engine.forget_answer()) : std::wstring{};
        const auto heading = target ? L"学習語の削除  「" + word + L"」" : L"削除できる学習語がありません";
        const auto draft = scope + (target ? L"    確認: " + answer : L"");
        const std::wstring hint = target ? L"yes + Enter: この登録を削除  Esc / no + Enter: 取消" : L"Enter / Esc: 候補へ戻る";
        if (!forgetting_ || has_forget_target_ != target.has_value() || heading != heading_ || draft != draft_ || hint != hint_)
            updated_ |= all_updates;
        forgetting_ = true;
        has_forget_target_ = target.has_value();
        forget_word_ = word;
        forget_scope_ = scope;
        forget_answer_ = answer;
        heading_ = heading; draft_ = draft; hint_ = hint;
    } else if (kind_ == PopupKind::Editor) {
        const auto mode = show_input_mode_ ? L" [" + std::wstring(mode_label(engine.mode())) +
            (engine.mode() == Mode::Latin ? L" 英数]" : L"]") : std::wstring{};
        const auto reading = wide(engine.registration_reading());
        const auto depth = engine.registration_depth();
        const auto heading = L"単語登録" + mode + L" " + std::to_wstring(depth) + L"  「" + reading + L"」";
        const auto draft = wide(engine.registration_text());
        const bool selecting = engine.input_stage() == Stage::Selecting;
        const bool limit = engine.registration_limit();
        const std::wstring hint = limit ? L"入力上限: 1024文字・再帰8段まで" :
            selecting ? L"クリック/" + std::wstring(candidate_labels(keys_)) + L"/Enter: 欄へ追加  PgDn/PgUp: 7件  Esc: 戻る" :
            L"語幹を入力  Enter: 登録  C-j: かな  Esc: 取消";
        if (forgetting_ || editor_mode_ != engine.mode() || registration_depth_ != depth || registration_reading_ != reading ||
            registration_limit_ != limit || editor_selecting_ != selecting || heading != heading_ || draft != draft_ || hint != hint_)
            updated_ |= all_updates;
        forgetting_ = false;
        has_forget_target_ = false;
        editor_mode_ = engine.mode();
        registration_depth_ = depth;
        registration_reading_ = reading;
        registration_limit_ = limit;
        editor_selecting_ = selecting;
        heading_ = heading; draft_ = draft; hint_ = hint;
    } else {
        forgetting_ = false;
        has_forget_target_ = false;
    }
    if (document_.Get() != document) updated_ |= TF_CLUIE_DOCUMENTMGR;
    document_ = document;
    const auto source = engine.forgetting() ? nullptr : engine.candidate_snapshot();
    auto suffix = source ? engine.candidate_suffix() : std::u32string{};
    if (source != source_rows_ || suffix != source_suffix_) {
        std::vector<Row> rows;
        if (source) {
            rows.reserve(source->size());
            const auto ending = wide(suffix);
            for (const auto& candidate : *source) {
                auto text = wide(candidate.text);
                text += ending;
                rows.push_back({std::move(text), wide(candidate.annotation), candidate.source});
            }
        }
        if (rows != rows_) {
            std::vector<UINT> pages;
            for (std::size_t index = 0; index < rows.size(); index += page_size) pages.push_back(static_cast<UINT>(index));
            rows_ = std::move(rows); pages_ = std::move(pages);
            wheel_.reset();
            updated_ |= all_updates;
        }
        source_rows_ = source;
        source_suffix_ = std::move(suffix);
    }
    const auto selection = static_cast<UINT>(engine.candidate_index());
    if (selection != selection_) updated_ |= TF_CLUIE_SELECTION | TF_CLUIE_CURRENTPAGE;
    const auto old_selection = std::exchange(selection_, selection);
    if (updated_ & ~TF_CLUIE_DOCUMENTMGR) {
        if (!redraw_ && kind_ == PopupKind::Candidates && !(updated_ & TF_CLUIE_STRING) && old_rows == visible_rows())
            previous_selection_ = old_selection;
        else previous_selection_.reset();
        redraw_ = true;
    }
    if (updated_ && kind_ == PopupKind::Candidates) replace_candidate_timing(paint_timing_);
}
void CandidateList::notice(std::wstring_view reason, ITfDocumentMgr* document) {
    if (closed_ || kind_ != PopupKind::Notice) return;
    redraw_ = true; previous_selection_.reset(); hovered_.reset(); wheel_.reset();
    forgetting_ = false; has_forget_target_ = false;
    heading_ = L"単語登録"; draft_ = reason;
    hint_ = L"別の入力欄で登録してください。入力を続けると閉じます。";
    rows_.clear(); pages_.clear(); selection_ = 0;
    document_ = document; updated_ = all_updates;
}
void CandidateList::locate(HWND owner, const RECT* anchor) noexcept {
    const bool moved = !anchor || !located_ || owner_ != owner || !EqualRect(&anchor_, anchor);
    if (moved) cancel_pointer();
    owner_ = owner;
    located_ = anchor && anchor->bottom > anchor->top;
    if (!located_) discard_candidate_timing(paint_timing_);
    if (located_) anchor_ = *anchor;
    if (!moved && !redraw_ && window_ && IsWindowVisible(window_) &&
        dpi_ == (owner ? GetDpiForWindow(owner) : 96U)) return;
    render();
}
void CandidateList::close() noexcept {
    discard_candidate_timing(paint_timing_);
    closed_ = true; located_ = false; requested_visible_ = false;
    hovered_.reset(); tracking_mouse_ = false; wheel_.reset();
    actions_.reset(); cancel_pointer();
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    if (!class_name_.empty()) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); }
    release_fonts();
    source_rows_.reset(); source_suffix_.clear();
    owner_ = nullptr; document_.Reset();
}

std::wstring CandidateList::footer_counter() const {
    return std::to_wstring(selection_ + 1) + L" / " + std::to_wstring(rows_.size());
}

std::wstring CandidateList::compact_footer_hint() const {
    if (kind_ == PopupKind::Notice) return hint_;
    if (forgetting_) return has_forget_target_ ? L"yes + Enter 削除   Esc 取消" : L"Enter / Esc 戻る";
    if (kind_ == PopupKind::Editor) {
        if (registration_limit_) return L"入力上限: 1024文字・再帰8段まで";
        if (editor_selecting_) return L"Enter 欄へ追加   PgDn/Up 頁   Esc 戻る";
        return L"Enter 登録   C-j かな   Esc 取消";
    }
    return input_method_ == InputMethod::Skk ? L"Space 次   PgDn 頁   X/Del 削除" : L"Space 次   PgDn 頁   Del 削除";
}

SIZE CandidateList::measure() {
    const auto [first, visible_end] = visible_rows();
    WindowDC dc(window_);
    if (!dc.value) return {};
    const auto m = layout();

    if (kind_ == PopupKind::Notice) {
        const auto title_sz = measure_text(dc.value, title_font_, L"単語登録できません");
        const auto body_sz = measure_text(dc.value, font_, label(draft_));
        const auto hint_sz = measure_text(dc.value, sub_font_, hint_);
        const int content_w = std::max({title_sz.cx, body_sz.cx, hint_sz.cx});
        const int width = std::clamp(content_w + scale(32), scale(300), scale(540));
        const int height = scale(92);
        RECT outer{0, 0, width, height};
        if (!AdjustWindowRectExForDpi(&outer, popup_style, FALSE, popup_ex_style, dpi_)) return {};
        return {outer.right - outer.left, outer.bottom - outer.top};
    }

    int width = scale(kind_ == PopupKind::Editor ? 420 : 280);
    if (kind_ == PopupKind::Candidates) {
        const auto counter_sz = measure_text(dc.value, sub_font_, footer_counter());
        const auto hint_sz = measure_text(dc.value, sub_font_, compact_footer_hint());
        width = std::max(width, static_cast<int>(counter_sz.cx + hint_sz.cx) + scale(40));
    } else if (forgetting_) {
        const auto scope_sz = measure_text(dc.value, sub_font_, forget_scope_);
        const auto title_sz = measure_text(dc.value, title_font_, has_forget_target_ ? L"学習語の削除  " + label(forget_word_) : L"削除できる学習語がありません");
        width = std::max({width, static_cast<int>(scope_sz.cx) + scale(32), static_cast<int>(title_sz.cx) + scale(56)});
    } else {
        const auto reading_sz = measure_text(dc.value, title_font_, L"単語登録  " + label(registration_reading_));
        const auto hint_sz = measure_text(dc.value, sub_font_, compact_footer_hint());
        width = std::max({width, static_cast<int>(reading_sz.cx) + scale(110), static_cast<int>(hint_sz.cx) + scale(32)});
    }

    if (!previous_selection_) {
        row_width_ = 0;
        const int row_chrome = m.pad_x * 2 + scale(16) + m.keycap_size + scale(12);
        const int ai_width = scale(34);
        for (UINT i = first; i < visible_end; ++i) {
            const auto main_sz = measure_text(dc.value, font_, label(rows_[i].text));
            int needed = row_chrome + static_cast<int>(main_sz.cx);
            if (!rows_[i].annotation.empty()) {
                const auto ann_sz = measure_text(dc.value, sub_font_, label(rows_[i].annotation));
                needed += scale(14) + static_cast<int>(ann_sz.cx);
            }
            if (is_generated_source(rows_[i].source)) needed += ai_width;
            row_width_ = std::max(row_width_, needed);
        }
    }
    width = std::max(width, row_width_);
    width = std::min(width, scale(580));

    const int rows_height = static_cast<int>(visible_end - first) * m.row_height;
    int height = 0;
    if (kind_ == PopupKind::Candidates) {
        height = m.rows_top + rows_height + scale(6) + m.footer_height;
    } else {
        height = m.rows_top + rows_height + (rows_height > 0 ? scale(6) : 0) + m.footer_height;
    }
    RECT outer{0, 0, width, height};
    if (!AdjustWindowRectExForDpi(&outer, popup_style, FALSE, popup_ex_style, dpi_)) return {};
    return {outer.right - outer.left, outer.bottom - outer.top};
}

void CandidateList::render() noexcept {
    if (closed_ || !located_ || !requested_visible_ || (rows_.empty() && kind_ == PopupKind::Candidates)) {
        trace(L"hidden", closed_, located_, requested_visible_);
        cancel_pointer();
        hovered_.reset();
        wheel_.reset();
        if (window_) ShowWindow(window_, SW_HIDE);
        return;
    }
    try {
        if (!window_) {
            if (!class_name_.empty()) { UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); }
            wchar_t name[96]{}; swprintf_s(name, L"nskkzime.Candidates.%p", static_cast<void*>(this));
            class_name_ = name;
            // Identify the owned popup as IME UI to immersive window hosts.
            // A normal desktop popup can be hidden by the Store/XAML shell.
            WNDCLASSW cls{}; cls.style = CS_IME; cls.lpfnWndProc = window_proc; cls.hInstance = module_instance;
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = class_name_.c_str();
            if (!RegisterClassW(&cls)) { trace(L"class-failed", GetLastError()); class_name_.clear(); return; }
            // Keep the TSF-provided owner. Walking to GA_ROOT can cross from
            // an AppContainer's CoreWindow into the desktop shell's frame.
            if (IsDebuggerPresent()) {
                DWORD owner_process = 0, root_process = 0;
                const auto root = owner_ ? GetAncestor(owner_, GA_ROOT) : nullptr;
                if (owner_) GetWindowThreadProcessId(owner_, &owner_process);
                if (root) GetWindowThreadProcessId(root, &root_process);
                trace(L"owner-context", owner_process == GetCurrentProcessId(), root_process == GetCurrentProcessId(), owner_ == root);
            }
            window_ = CreateWindowExW(popup_ex_style,
                class_name_.c_str(), kind_ == PopupKind::Notice ? L"nskkzime 通知" : kind_ == PopupKind::Editor ? L"nskkzime 単語登録" : L"nskkzime 候補", popup_style, 0, 0, 0, 0,
                owner_, nullptr, module_instance, this);
            if (!window_) { trace(L"window-failed", GetLastError()); UnregisterClassW(class_name_.c_str(), module_instance); class_name_.clear(); return; }
            refresh_theme();
        }
        const auto dpi = owner_ ? GetDpiForWindow(owner_) : 96U;
        ensure_fonts(dpi);
        if (redraw_ || !measured_.cx) measured_ = measure();
        if (!measured_.cx) return;
        int width = measured_.cx, height = measured_.cy;
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        const int gap = scale(4);
        const auto x = std::clamp(anchor_.left, monitor.rcWork.left, monitor.rcWork.right - width);
        auto y = anchor_.bottom + gap;
        if (y + height > monitor.rcWork.bottom) y = anchor_.top - height - gap;
        y = std::clamp(y, monitor.rcWork.top, monitor.rcWork.bottom - height);
        const RECT expected{x, y, x + width, y + height};
        RECT actual{};
        const bool place = !IsWindowVisible(window_) || !GetWindowRect(window_, &actual) || !EqualRect(&expected, &actual);
        const bool redraw = std::exchange(redraw_, false);
        previous_selection_.reset();
        if (place) {
            const auto placed = SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            const auto error = placed ? 0 : GetLastError();
            if (IsDebuggerPresent()) trace(L"placed", placed, IsWindowVisible(window_), error);
            if (!placed) { redraw_ = true; return; }
        }
        if (!window_ || closed_) return;
        if (redraw || place) InvalidateRect(window_, nullptr, FALSE);
    } catch (...) { if (window_) ShowWindow(window_, SW_HIDE); }
}

void CandidateList::paint_candidates(SurfaceCanvas& canvas, const Layout& m, int width, int top, int bottom) const {
    if (pages_.empty() || top >= bottom) return;
    const auto [first, end] = visible_rows();
    const HDC dc = canvas.dc();
    const auto labels = candidate_labels(keys_);
    const int inset_x = m.pad_x;
    const int inset_y = scale(2);
    const float row_radius = static_cast<float>(scale(5));
    const float keycap_radius = static_cast<float>(scale(4));
    const float pill_radius = static_cast<float>(scale(2));
    const int pill_w = std::max(3, scale(3));
    const int pill_h = scale(16);

    const int saved_dc = SaveDC(dc);
    IntersectClipRect(dc, 0, top, width, bottom);
    for (UINT i = first; i < end && top < bottom; ++i, top += m.row_height) {
        const bool selected = i == selection_;
        const bool hovered = !selected && hovered_ && *hovered_ == i;
        const RECT row_box{inset_x, top + inset_y, width - inset_x, top + m.row_height - inset_y};

        if (selected) {
            canvas.fill_rounded_rect(row_box, row_radius, theme_.selection_bg);
            const int pill_top = top + (m.row_height - pill_h) / 2;
            const RECT pill{row_box.left + scale(2), pill_top, row_box.left + scale(2) + pill_w, pill_top + pill_h};
            canvas.fill_rounded_rect(pill, pill_radius, theme_.high_contrast ? theme_.selection_text : theme_.accent);
        } else if (hovered) {
            canvas.fill_rounded_rect(row_box, row_radius, theme_.hover_bg);
        }

        // Shortcut keycap badge.
        const int keycap_left = row_box.left + scale(9);
        const int keycap_top = top + (m.row_height - m.keycap_size) / 2;
        const RECT keycap{keycap_left, keycap_top, keycap_left + m.keycap_size, keycap_top + m.keycap_size};
        const COLORREF kc_bg = selected && !theme_.high_contrast
            ? blend_color(theme_.selection_bg, theme_.accent, 34)
            : theme_.keycap_bg;
        const COLORREF kc_border = selected && !theme_.high_contrast
            ? blend_color(theme_.selection_bg, theme_.accent, 110)
            : theme_.keycap_border;
        const COLORREF kc_text = selected && !theme_.high_contrast ? theme_.text : theme_.keycap_text;
        canvas.fill_rounded_rect(keycap, keycap_radius, kc_bg);
        canvas.stroke_rounded_rect(keycap, keycap_radius, 1.0f, kc_border);
        {
            SelectedFont f(dc, badge_font_);
            SetTextColor(dc, kc_text);
            wchar_t ch[2]{labels[i - first], 0};
            RECT kc_rect = keycap;
            DrawTextW(dc, ch, 1, &kc_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        int right_limit = row_box.right - scale(10);
        // Optional AI generation pill on the right edge.
        if (is_generated_source(rows_[i].source)) {
            const int badge_w = scale(26);
            const int badge_top = top + (m.row_height - m.badge_height) / 2;
            const RECT badge{right_limit - badge_w, badge_top, right_limit, badge_top + m.badge_height};
            const float br = static_cast<float>(m.badge_height) * 0.5f;
            canvas.fill_rounded_rect(badge, br, theme_.ai_badge_bg);
            canvas.stroke_rounded_rect(badge, br, 1.0f, theme_.ai_badge_border);
            {
                SelectedFont f(dc, badge_font_);
                SetTextColor(dc, theme_.ai_badge_text);
                RECT text_rect = badge;
                DrawTextW(dc, L"AI", 2, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            right_limit = badge.left - scale(8);
        }

        const int text_left = keycap.right + scale(10);
        const auto main_text = label(rows_[i].text);
        const auto ann_text = rows_[i].annotation.empty() ? std::wstring{} : label(rows_[i].annotation);
        const auto main_sz = measure_text(dc, font_, main_text);
        const int main_right = ann_text.empty()
            ? right_limit
            : std::min(right_limit, text_left + static_cast<int>(main_sz.cx) + scale(4));

        {
            SelectedFont f(dc, font_);
            SetTextColor(dc, selected ? theme_.selection_text : theme_.text);
            RECT main_rect{text_left, top, main_right, top + m.row_height};
            DrawTextW(dc, main_text.c_str(), static_cast<int>(main_text.size()), &main_rect,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        if (!ann_text.empty() && main_right + scale(10) < right_limit) {
            SelectedFont f(dc, sub_font_);
            SetTextColor(dc, selected && theme_.high_contrast ? theme_.selection_text : theme_.text_secondary);
            RECT ann_rect{main_right + scale(10), top, right_limit, top + m.row_height};
            DrawTextW(dc, ann_text.c_str(), static_cast<int>(ann_text.size()), &ann_rect,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    if (saved_dc) RestoreDC(dc, saved_dc);
}

void CandidateList::paint_registration_header(SurfaceCanvas& canvas, const Layout& m, int width) const {
    const HDC dc = canvas.dc();
    // Header band.
    const RECT header_rect{0, 0, width, m.header_height};
    canvas.fill_rect(header_rect, theme_.surface_secondary);
    canvas.fill_rect({0, m.header_height - 1, width, m.header_height}, theme_.separator);

    int right_cursor = width - scale(10);
    // Optional input mode pill on the right of the header.
    if (show_input_mode_) {
        const auto palette = mode_palette(theme_, editor_mode_);
        const std::wstring text = editor_mode_ == Mode::Latin
            ? std::wstring(mode_label(editor_mode_)) + L" 英数"
            : std::wstring(mode_label(editor_mode_));
        const auto sz = measure_text(dc, badge_font_, text);
        const int pill_w = std::max(scale(22), static_cast<int>(sz.cx) + scale(12));
        const int pill_h = scale(20);
        const int pill_top = (m.header_height - pill_h) / 2;
        const RECT pill{right_cursor - pill_w, pill_top, right_cursor, pill_top + pill_h};
        canvas.fill_rounded_rect(pill, static_cast<float>(scale(4)), palette.background);
        canvas.stroke_rounded_rect(pill, static_cast<float>(scale(4)), 1.0f, palette.border);
        {
            SelectedFont f(dc, badge_font_);
            SetTextColor(dc, palette.text);
            RECT r = pill;
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        right_cursor = pill.left - scale(6);
    }

    // Recursion depth pill (only shown when nested >= 2).
    if (registration_depth_ >= 2) {
        const std::wstring depth_text = std::to_wstring(registration_depth_) + L"/8";
        const auto sz = measure_text(dc, badge_font_, depth_text);
        const int pill_w = static_cast<int>(sz.cx) + scale(10);
        const int pill_h = scale(18);
        const int pill_top = (m.header_height - pill_h) / 2;
        const RECT pill{right_cursor - pill_w, pill_top, right_cursor, pill_top + pill_h};
        canvas.fill_rounded_rect(pill, static_cast<float>(scale(4)), theme_.keycap_bg);
        canvas.stroke_rounded_rect(pill, static_cast<float>(scale(4)), 1.0f, theme_.keycap_border);
        {
            SelectedFont f(dc, badge_font_);
            SetTextColor(dc, theme_.text_secondary);
            RECT r = pill;
            DrawTextW(dc, depth_text.c_str(), static_cast<int>(depth_text.size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        right_cursor = pill.left - scale(6);
    }

    // Title + reading chip.
    const std::wstring title = L"単語登録";
    const auto title_sz = measure_text(dc, title_font_, title);
    const int title_left = scale(12);
    {
        SelectedFont f(dc, title_font_);
        SetTextColor(dc, theme_.text);
        RECT r{title_left, 0, title_left + title_sz.cx + scale(4), m.header_height};
        DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    const auto reading = label(registration_reading_);
    if (!reading.empty() && title_left + title_sz.cx + scale(20) < right_cursor) {
        const int chip_left = title_left + title_sz.cx + scale(10);
        const auto read_sz = measure_text(dc, sub_font_, reading);
        const int chip_w = std::min(right_cursor - chip_left, static_cast<int>(read_sz.cx) + scale(14));
        const int chip_h = scale(22);
        const int chip_top = (m.header_height - chip_h) / 2;
        const RECT chip{chip_left, chip_top, chip_left + chip_w, chip_top + chip_h};
        canvas.fill_rounded_rect(chip, static_cast<float>(scale(4)), theme_.surface);
        canvas.stroke_rounded_rect(chip, static_cast<float>(scale(4)), 1.0f, theme_.border);
        SelectedFont f(dc, sub_font_);
        SetTextColor(dc, theme_.text);
        RECT text_rect{chip.left + scale(7), chip.top, chip.right - scale(7), chip.bottom};
        DrawTextW(dc, reading.c_str(), static_cast<int>(reading.size()), &text_rect,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    // Input box area.
    const int box_pad_x = scale(10);
    const int box_pad_y = scale(8);
    const RECT box{box_pad_x, m.header_height + box_pad_y, width - box_pad_x, m.header_height + m.editor_height - box_pad_y};
    const float box_radius = static_cast<float>(scale(5));
    const COLORREF line_color = registration_limit_ ? theme_.warning_accent : theme_.accent;
    canvas.fill_rounded_rect(box, box_radius, theme_.input_surface);
    canvas.stroke_rounded_rect(box, box_radius, 1.0f, theme_.border);
    // Win11 bottom accent focus line inside the input field.
    const int accent_h = std::max(2, scale(2));
    canvas.fill_rect({box.left + scale(4), box.bottom - accent_h, box.right - scale(4), box.bottom}, line_color);

    const int inner_left = box.left + scale(10);
    const int inner_right = box.right - scale(10);
    const int inner_width = std::max(0, inner_right - inner_left);
    const int caret_w = std::max(2, scale(2));
    const int caret_h = std::min<int>(box.bottom - box.top - scale(10), scale(18));
    const int caret_top = (box.top + box.bottom - caret_h) / 2;

    if (draft_.empty()) {
        canvas.fill_rect({inner_left, caret_top, inner_left + caret_w, caret_top + caret_h}, line_color);
        SelectedFont f(dc, sub_font_);
        SetTextColor(dc, theme_.text_muted);
        RECT placeholder{inner_left + caret_w + scale(6), box.top, inner_right, box.bottom - accent_h};
        const std::wstring guide = L"登録する語幹を入力…";
        DrawTextW(dc, guide.c_str(), static_cast<int>(guide.size()), &placeholder,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        // Keep the currently edited tail visible for a long registration.
        const auto offset = draft_.size() > 160 ? draft_.size() - 160 : 0;
        auto tail = draft_.substr(offset);
        if (!tail.empty() && tail.front() >= 0xdc00 && tail.front() <= 0xdfff) tail.erase(0, 1);
        const auto text = (offset ? L"…" : L"") + tail;
        const auto sz = measure_text(dc, font_, text);
        const bool overflow = sz.cx + caret_w + scale(2) > inner_width;
        RECT text_rect{inner_left, box.top, inner_right - caret_w - scale(2), box.bottom - accent_h};
        {
            SelectedFont f(dc, font_);
            SetTextColor(dc, theme_.text);
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_rect,
                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (overflow ? DT_RIGHT : DT_LEFT));
        }
        const int caret_x = overflow ? text_rect.right + scale(1) : inner_left + static_cast<int>(sz.cx) + scale(1);
        canvas.fill_rect({caret_x, caret_top, caret_x + caret_w, caret_top + caret_h}, line_color);
    }

    if (!pages_.empty()) {
        const int sep_y = m.header_height + m.editor_height;
        canvas.fill_rect({scale(10), sep_y, width - scale(10), sep_y + 1}, theme_.separator);
    }
}

void CandidateList::paint_forget_body(SurfaceCanvas& canvas, const Layout& m, int width) const {
    const HDC dc = canvas.dc();
    const RECT header_rect{0, 0, width, m.header_height};
    canvas.fill_rect(header_rect, has_forget_target_ && !theme_.high_contrast ? theme_.warning_bg : theme_.surface_secondary);
    canvas.fill_rect({0, m.header_height - 1, width, m.header_height}, theme_.separator);
    if (has_forget_target_) {
        const int bar_w = std::max(3, scale(3));
        canvas.fill_rect({0, 0, bar_w, m.header_height}, theme_.warning_accent);
    }

    const int left = scale(12);
    const std::wstring title = has_forget_target_ ? L"学習語の削除" : L"削除できる学習語がありません";
    const auto title_sz = measure_text(dc, title_font_, title);
    {
        SelectedFont f(dc, title_font_);
        SetTextColor(dc, has_forget_target_ ? theme_.warning_accent : theme_.text);
        RECT r{left, 0, has_forget_target_ ? left + title_sz.cx + scale(4) : width - left, m.header_height};
        DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &r,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (has_forget_target_) {
        const auto word = label(forget_word_);
        const int chip_left = left + title_sz.cx + scale(10);
        const int max_w = width - chip_left - scale(10);
        if (max_w > scale(24)) {
            const auto word_sz = measure_text(dc, title_font_, word);
            const int chip_w = std::min(max_w, static_cast<int>(word_sz.cx) + scale(14));
            const int chip_h = scale(22);
            const int chip_top = (m.header_height - chip_h) / 2;
            const RECT chip{chip_left, chip_top, chip_left + chip_w, chip_top + chip_h};
            canvas.fill_rounded_rect(chip, static_cast<float>(scale(4)), theme_.surface);
            canvas.stroke_rounded_rect(chip, static_cast<float>(scale(4)), 1.0f, theme_.border);
            SelectedFont f(dc, title_font_);
            SetTextColor(dc, theme_.text);
            RECT r{chip.left + scale(7), chip.top, chip.right - scale(7), chip.bottom};
            DrawTextW(dc, word.c_str(), static_cast<int>(word.size()), &r,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    // Scope line.
    const int body_top = m.header_height + scale(6);
    {
        SelectedFont f(dc, sub_font_);
        SetTextColor(dc, theme_.text_secondary);
        RECT scope_rect{left, body_top, width - left, body_top + scale(22)};
        DrawTextW(dc, forget_scope_.c_str(), static_cast<int>(forget_scope_.size()), &scope_rect,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    if (!has_forget_target_) return;

    // Confirmation input row ("yes").
    const int row_top = body_top + scale(26);
    const int box_h = scale(30);
    const std::wstring prompt_label = L"確認 (yes):";
    const auto prompt_sz = measure_text(dc, sub_font_, prompt_label);
    {
        SelectedFont f(dc, sub_font_);
        SetTextColor(dc, theme_.text);
        RECT r{left, row_top, left + prompt_sz.cx + scale(4), row_top + box_h};
        DrawTextW(dc, prompt_label.c_str(), static_cast<int>(prompt_label.size()), &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    const bool confirmed = forget_answer_ == L"yes";
    const RECT box{left + prompt_sz.cx + scale(10), row_top, std::min<int>(width - left, left + prompt_sz.cx + scale(160)), row_top + box_h};
    const COLORREF accent = confirmed ? theme_.warning_accent : theme_.accent;
    canvas.fill_rounded_rect(box, static_cast<float>(scale(4)), theme_.input_surface);
    canvas.stroke_rounded_rect(box, static_cast<float>(scale(4)), 1.0f, confirmed ? theme_.warning_accent : theme_.border);
    const int line_h = std::max(2, scale(2));
    canvas.fill_rect({box.left + scale(3), box.bottom - line_h, box.right - scale(3), box.bottom}, accent);

    const int inner_x = box.left + scale(8);
    const int caret_w = std::max(2, scale(2));
    const int caret_h = scale(14);
    const int caret_y = (box.top + box.bottom - caret_h) / 2;
    const auto answer_sz = measure_text(dc, font_, forget_answer_);
    if (!forget_answer_.empty()) {
        SelectedFont f(dc, font_);
        SetTextColor(dc, confirmed ? theme_.warning_accent : theme_.text);
        RECT r{inner_x, box.top, box.right - scale(8), box.bottom - line_h};
        DrawTextW(dc, forget_answer_.c_str(), static_cast<int>(forget_answer_.size()), &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    const int caret_x = std::min<int>(box.right - scale(6), inner_x + static_cast<int>(answer_sz.cx) + scale(1));
    canvas.fill_rect({caret_x, caret_y, caret_x + caret_w, caret_y + caret_h}, accent);
}

void CandidateList::paint_notice(SurfaceCanvas& canvas, const Layout&, int width, int height) const {
    const HDC dc = canvas.dc();
    const int bar_w = std::max(3, scale(3));
    canvas.fill_rect({0, 0, bar_w, height}, theme_.notice_accent);
    const int left = scale(14);
    const int right = width - scale(14);
    {
        SelectedFont f(dc, title_font_);
        SetTextColor(dc, theme_.text);
        RECT r{left, scale(10), right, scale(32)};
        const std::wstring title = L"単語登録できません";
        DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    {
        SelectedFont f(dc, font_);
        SetTextColor(dc, theme_.text_secondary);
        const auto text = label(draft_);
        RECT r{left, scale(34), right, scale(60)};
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    {
        SelectedFont f(dc, sub_font_);
        SetTextColor(dc, theme_.text_muted);
        RECT r{left, scale(62), right, height - scale(8)};
        DrawTextW(dc, hint_.c_str(), static_cast<int>(hint_.size()), &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void CandidateList::paint_footer(SurfaceCanvas& canvas, const Layout& m, int width, int top) const {
    const HDC dc = canvas.dc();
    canvas.fill_rect({0, top, width, top + m.footer_height}, theme_.surface);
    canvas.fill_rect({scale(8), top, width - scale(8), top + 1}, theme_.separator);
    const RECT rect{scale(12), top + 1, width - scale(12), top + m.footer_height};
    SelectedFont f(dc, sub_font_);
    const auto hint_text = compact_footer_hint();
    if (kind_ == PopupKind::Candidates) {
        const auto counter = footer_counter();
        const auto counter_sz = measure_text(dc, sub_font_, counter);
        SetTextColor(dc, theme_.text_secondary);
        RECT left_rect = rect;
        left_rect.right = std::min(rect.right, rect.left + static_cast<int>(counter_sz.cx) + scale(6));
        DrawTextW(dc, counter.c_str(), static_cast<int>(counter.size()), &left_rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        if (left_rect.right + scale(8) < rect.right) {
            SetTextColor(dc, theme_.text_muted);
            RECT right_rect{left_rect.right + scale(8), rect.top, rect.right, rect.bottom};
            DrawTextW(dc, hint_text.c_str(), static_cast<int>(hint_text.size()), &right_rect,
                DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    } else {
        SetTextColor(dc, registration_limit_ ? theme_.warning_accent : theme_.text_muted);
        RECT r = rect;
        DrawTextW(dc, hint_text.c_str(), static_cast<int>(hint_text.size()), &r,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void CandidateList::paint(HDC target_dc) const {
    RECT area{};
    if (!GetClientRect(window_, &area)) return;
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width <= 0 || height <= 0) return;

    SurfaceCanvas canvas(target_dc, width, height);
    if (!canvas.valid()) {
        FillRect(target_dc, &area, GetSysColorBrush(COLOR_WINDOW));
        return;
    }

    canvas.clear(theme_.surface);
    SetBkMode(canvas.dc(), TRANSPARENT);
    const auto m = layout();

    if (kind_ == PopupKind::Notice) {
        paint_notice(canvas, m, width, height);
    } else if (kind_ == PopupKind::Editor) {
        if (forgetting_) {
            paint_forget_body(canvas, m, width);
        } else {
            paint_registration_header(canvas, m, width);
            paint_candidates(canvas, m, width, m.rows_top, m.rows_bottom(height));
        }
        paint_footer(canvas, m, width, m.footer_top(height));
    } else {
        paint_candidates(canvas, m, width, m.rows_top, m.rows_bottom(height));
        paint_footer(canvas, m, width, m.footer_top(height));
    }

    if (!dwm_framed_ || theme_.high_contrast) {
        const float thickness = std::max(1.0f, static_cast<float>(scale(1)));
        canvas.stroke_rounded_rect(area, 0.0f, thickness, theme_.border);
    }
    canvas.present();
}

LRESULT CALLBACK CandidateList::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CandidateList*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<CandidateList*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_ERASEBKGND) return 1;
    if (self && (message == WM_SETTINGCHANGE || message == WM_DISPLAYCHANGE || message == WM_DPICHANGED)) {
        self->refresh_theme();
        self->redraw_ = true;
        self->previous_selection_.reset();
        InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    // Eat popup clicks, including its header/footer, without activating it or
    // letting them move the caret in the document underneath.
    if (message == WM_NCHITTEST) return HTCLIENT;
    if (message == WM_SETCURSOR && self) {
        POINT point{};
        const bool row = GetCursorPos(&point) && ScreenToClient(window, &point) && self->hit_test(point).has_value();
        SetCursor(LoadCursorW(nullptr, row ? IDC_HAND : IDC_ARROW)); return TRUE;
    }
    if (message == WM_MOUSEMOVE && self) {
        if (!self->tracking_mouse_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            self->tracking_mouse_ = TrackMouseEvent(&track) != FALSE;
        }
        const auto row = self->hit_test({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (row != self->hovered_) {
            self->hovered_ = row;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    if (message == WM_MOUSELEAVE && self) {
        self->tracking_mouse_ = false;
        if (self->hovered_) {
            self->hovered_.reset();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    if (message == WM_MOUSEWHEEL && self) {
        const ComPtr<CandidateList> keep(self);
        const int wheel = GET_WHEEL_DELTA_WPARAM(wparam);
        if (wheel != 0 && !self->rows_.empty()) {
            const int steps = self->wheel_.accumulate(wheel);
            if (steps != 0) {
                self->begin_interaction();
                self->request_step(steps);
            }
        }
        return 0;
    }
    if (message == WM_LBUTTONDOWN && self) {
        const ComPtr<CandidateList> keep(self);
        self->cancel_pointer();
        self->pointer_.down(self->hit_test({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}));
        if (self->pointer_.pressed) { self->begin_interaction(); SetCapture(window); }
        return 0;
    }
    if (message == WM_LBUTTONUP && self) {
        const ComPtr<CandidateList> keep(self);
        const auto revision = self->revision_;
        const auto row = self->pointer_.up(self->hit_test({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}));
        self->cancel_pointer();
        if (row && self->revision_ == revision) self->request_selection(*row);
        return 0;
    }
    if ((message == WM_CAPTURECHANGED || message == WM_CANCELMODE) && self) {
        if (message == WM_CANCELMODE) self->cancel_pointer(); else self->pointer_.cancel();
        return 0;
    }
    if (message == WM_PAINT && self) {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        bool painted = false;
        try { if (dc) { self->paint(dc); painted = true; } } catch (...) { /* Never throw through the window procedure. */ }
        EndPaint(window, &paint);
        if (painted && IsWindowVisible(window)) finish_candidate_timing(self->paint_timing_);
        else discard_candidate_timing(self->paint_timing_);
        return 0;
    }
    // Let XAML light-dismiss surfaces track our non-activating IME popups.
    // Use the actual window transitions, including host/owner-driven hiding.
    if (message == WM_WINDOWPOSCHANGED && self) {
        const bool visible = IsWindowVisible(window) != FALSE;
        const bool was_visible = std::exchange(self->ime_visible_, visible);
        const auto position = reinterpret_cast<const WINDOWPOS*>(lparam);
        if (visible != was_visible)
            NotifyWinEvent(visible ? EVENT_OBJECT_IME_SHOW : EVENT_OBJECT_IME_HIDE, window, OBJID_CLIENT, CHILDID_SELF);
        else if (visible && position && (position->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
            NotifyWinEvent(EVENT_OBJECT_IME_CHANGE, window, OBJID_CLIENT, CHILDID_SELF);
    }
    if (message == WM_DESTROY && self && std::exchange(self->ime_visible_, false))
        NotifyWinEvent(EVENT_OBJECT_IME_HIDE, window, OBJID_CLIENT, CHILDID_SELF);
    if (message == WM_NCDESTROY && self) { self->window_ = nullptr; SetWindowLongPtrW(window, GWLP_USERDATA, 0); }
    return DefWindowProcW(window, message, wparam, lparam);
}
void CandidatePresenter::update(ITfThreadMgr* thread, ITfContext* context, const Engine& engine, std::weak_ptr<CandidateActions> actions,
    bool show_candidates, CandidateKeys keys, std::wstring_view notice, std::wstring_view font_face, bool show_input_mode) noexcept {
    try {
        ++generation_;
        if (position_context_ && position_context_.Get() != context) invalidate_position();
        const bool notifying = !notice.empty();
        if (!notifying && !engine.registering() && (engine.stage() != Stage::Selecting || engine.candidates().empty())) { close_ui(); return; }
        const auto kind = notifying ? PopupKind::Notice : engine.registering() || engine.forgetting() ? PopupKind::Editor : PopupKind::Candidates;
        if (kind == PopupKind::Candidates && !show_candidates) { close_ui(); return; }
        if (list_ && (list_->kind() != kind || list_->keys() != keys || list_->font_face() != font_face || list_->shows_input_mode() != show_input_mode)) {
            const auto closing_generation = generation_ + 1;
            close_ui();
            if (generation_ != closing_generation) return;
        }
        const auto generation = generation_;
        ComPtr<ITfDocumentMgr> document;
        const auto document_status = context ? context->GetDocumentMgr(&document) : E_INVALIDARG;
        if (generation != generation_) return;
        if (FAILED(document_status) || !document) { close(); return; }
        if (!list_) {
            ComPtr<ITfUIElementMgr> ui_manager;
            const auto manager_status = thread ? thread->QueryInterface(IID_PPV_ARGS(&ui_manager)) : E_INVALIDARG;
            if (generation != generation_) return;
            if (FAILED(manager_status) || !ui_manager) return;
            manager_ = std::move(ui_manager);
            list_.Attach(new CandidateList(kind, notifying ? std::weak_ptr<CandidateActions>{} : std::move(actions), keys,
                std::wstring(font_face), show_input_mode));
            if (notifying) list_->notice(notice, document.Get());
            else list_->update(engine, document.Get());
            BOOL show = TRUE;
            const auto list = list_; const auto manager = manager_;
            DWORD id = 0;
            const auto hr = manager->BeginUIElement(list.Get(), &show, &id);
            trace(L"begin", hr, show);
            if (list_.Get() != list.Get()) { if (SUCCEEDED(hr)) manager->EndUIElement(id); return; }
            if (FAILED(hr)) { close(); return; }
            id_ = id; begun_ = true; list->Show(show);
            // A UILess host starts drawing at UpdateUIElement, not Begin.
            if (!show && list_.Get() == list.Get() && FAILED(manager->UpdateUIElement(id_)) && list_.Get() == list.Get()) close();
        } else {
            if (notifying) list_->notice(notice, document.Get());
            else list_->update(engine, document.Get());
            DWORD flags = 0; list_->GetUpdatedFlags(&flags);
            const auto list = list_; const auto manager = manager_;
            if (flags && FAILED(manager->UpdateUIElement(id_)) && list_.Get() == list.Get()) close();
        }
    } catch (...) { close(); }
}
void CandidatePresenter::locate(ITfContext* context, ITfComposition* composition, TfEditCookie cookie, std::uint64_t session, bool capture_position) noexcept {
    if (!list_ && !capture_position) return;
    const auto generation = ++generation_;
    const auto list = list_;
    // Host callbacks can end the composition while answering a layout query.
    const ComPtr<ITfContext> target(context);
    const ComPtr<ITfComposition> current(composition);
    const auto unavailable = [&] {
        if (generation != generation_) return;
        invalidate_position();
        if (list && list_.Get() == list.Get()) list->locate(nullptr, nullptr);
    };
    ComPtr<ITfContextView> view;
    ComPtr<IUnknown> view_identity;
    ComPtr<ITfRange> range, caret;
    RECT rect{}; BOOL clipped = FALSE; HWND owner = nullptr;
    if (!target || !current || FAILED(target->GetActiveView(&view)) || !view ||
        FAILED(view.As(&view_identity)) || FAILED(current->GetRange(&range)) || !range || FAILED(range->Clone(&caret)) || !caret) {
        trace(L"no-view-or-range");
        unavailable(); return;
    }
    if (FAILED(view->GetWnd(&owner)) || !owner) owner = GetFocus();
    auto hr = caret->Collapse(cookie, TF_ANCHOR_START);
    if (SUCCEEDED(hr)) hr = view->GetTextExt(cookie, caret.Get(), &rect, &clipped);
    if (FAILED(hr) || rect.bottom <= rect.top) {
        rect = {}; clipped = FALSE;
        hr = view->GetTextExt(cookie, range.Get(), &rect, &clipped);
    }
    trace(L"anchor", hr, clipped, rect.bottom > rect.top);
    trace(L"owner", owner != nullptr);
    if (generation != generation_) return;
    if (owner && !IsWindow(owner)) { unavailable(); return; }
    if (hr == TF_E_NOLAYOUT && !clipped && position_context_.Get() == target.Get() &&
        position_composition_.Get() == current.Get() && position_session_ == session &&
        position_view_.Get() == view_identity.Get() && position_owner_ == owner) {
        // SetText can invalidate layout until OnLayoutChange. The position
        // captured before that write still belongs to this composition/view.
        if (list && list_.Get() == list.Get()) list->locate(owner, &position_);
        return;
    }
    if (FAILED(hr) || clipped || rect.bottom <= rect.top) { unavailable(); return; }
    position_context_ = target; position_composition_ = current; position_view_ = view_identity;
    position_owner_ = owner; position_ = rect; position_session_ = session;
    if (list && list_.Get() == list.Get()) list->locate(owner, &rect);
}
void CandidatePresenter::invalidate_position() noexcept {
    ++generation_;
    position_context_.Reset(); position_composition_.Reset(); position_view_.Reset();
    position_owner_ = nullptr; position_ = {}; position_session_ = 0;
}
void CandidatePresenter::hide() noexcept { invalidate_position(); if (list_) list_->locate(nullptr, nullptr); }
void CandidatePresenter::dismiss_notice() noexcept { if (list_ && list_->kind() == PopupKind::Notice) close_ui(); }
Key CandidatePresenter::selection_key(Key key, const Engine& engine) const noexcept {
    if (presentation_available() && list_->kind() != PopupKind::Notice &&
        !engine.forgetting() && engine.input_stage() == Stage::Selecting && key.code == KeyCode::Character) {
        const auto slot = candidate_keys(list_->keys()).find(key.character);
        if (slot != std::string_view::npos) return {KeyCode::SelectCandidate, 0, list_->shortcut_index(slot)};
    }
    return key;
}
void CandidatePresenter::close() noexcept {
    invalidate_position(); close_ui();
}
void CandidatePresenter::close_ui() noexcept {
    ++generation_;
    // Invalidate before notifying the host; retained elements cannot reopen UI.
    auto list = std::move(list_); auto manager = std::move(manager_);
    const auto id = id_; const auto begun = begun_; begun_ = false;
    if (list) list->close();
    if (begun && manager) manager->EndUIElement(id);
}
} // namespace nskkzime::windows
