#pragma once
#include "module.h"
#include "input_timing.h"
#include "candidate_settings.h"
#include "ui_settings.h"
#include "ui_theme.h"
#include "nskkzime/engine.h"
#include <wrl/client.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nskkzime::windows {
inline constexpr GUID candidate_ui_guid = {0x680bc7bf, 0xbe19, 0x42e9, {0xa4, 0x38, 0x0d, 0x8d, 0xf2, 0x99, 0xf5, 0x0b}};

// UI snapshots hold only weak references to this service-owned endpoint.
struct CandidateActions {
    std::function<void(std::size_t, std::uint64_t)> select;
    std::function<void(std::uint64_t)> interact; // Freeze asynchronous reordering on pointer down.
    std::function<void(int, std::uint64_t)> step; // Move candidate selection on wheel scroll.
};
struct CandidatePointer {
    std::optional<std::size_t> pressed;
    void down(std::optional<std::size_t> row) { pressed = row; }
    std::optional<std::size_t> up(std::optional<std::size_t> row) {
        const auto before = std::exchange(pressed, {});
        return before && before == row ? row : std::nullopt;
    }
    void cancel() { pressed.reset(); }
};
struct CandidateWheel {
    int remainder = 0;
    int accumulate(int delta) noexcept {
        if (!delta) return 0;
        const long long total = static_cast<long long>(remainder) + delta;
        const auto notches = static_cast<int>(total / WHEEL_DELTA);
        remainder = static_cast<int>(total % WHEEL_DELTA);
        return -notches;
    }
    void reset() noexcept { remainder = 0; }
};
std::optional<std::size_t> candidate_row_at_top(POINT, SIZE client, int top, int line_height,
    std::size_t first, std::size_t end) noexcept;
std::optional<std::size_t> candidate_row_at(POINT, SIZE client, int padding, int line_height,
    bool registration, std::size_t first, std::size_t end) noexcept;
inline constexpr GUID registration_ui_guid = {0x5422d011, 0x5654, 0x440d, {0x97, 0xdd, 0x64, 0xa2, 0xfa, 0xef, 0x4a, 0x2a}};
inline constexpr GUID notice_ui_guid = {0x136083bb, 0x0951, 0x45e4, {0x97, 0x1d, 0xc0, 0x59, 0x97, 0xfb, 0xd0, 0x4d}};
enum class PopupKind { Candidates, Editor, Notice };
// Count accepted user conversion steps, independently of candidate reordering
// and async generation. A failed host edit must not advance this state.
class CandidateVisibility {
public:
    void reset() noexcept { steps_ = 0; explicit_list_ = false; }
    void advance(Key key, const Result& result, const Engine& next) noexcept {
        if (next.registering() || next.forgetting()) { reset(); return; }
        if (result.conversion_started) { steps_ = 1; explicit_list_ = false; }
        else if (next.stage() == Stage::Selecting) {
            if (key.code == KeyCode::Space && steps_ < 99) ++steps_;
            if (key.code == KeyCode::PageDown || key.code == KeyCode::PageUp) explicit_list_ = true;
        } else reset();
    }
    bool visible(const CandidateSettings& settings) const noexcept {
        return settings.show_after == 1 || explicit_list_ || steps_ >= settings.show_after;
    }
private:
    unsigned int steps_ = 0;
    bool explicit_list_ = false;
};
class CandidateList final : public ITfCandidateListUIElement {
public:
    explicit CandidateList(PopupKind = PopupKind::Candidates, std::weak_ptr<CandidateActions> actions = {},
        CandidateKeys = CandidateKeys::Numbers, std::wstring font_face = default_ui_font, bool show_input_mode = false);
    ~CandidateList();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR*) override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID*) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL) override;
    HRESULT STDMETHODCALLTYPE IsShown(BOOL*) override;
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD*) override;
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr**) override;
    HRESULT STDMETHODCALLTYPE GetCount(UINT*) override;
    HRESULT STDMETHODCALLTYPE GetSelection(UINT*) override;
    HRESULT STDMETHODCALLTYPE GetString(UINT, BSTR*) override;
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT*, UINT, UINT*) override;
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT*, UINT) override;
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT*) override;

    void update(const Engine&, ITfDocumentMgr*);
    void notice(std::wstring_view, ITfDocumentMgr*);
    void locate(HWND owner, const RECT* anchor) noexcept;
    void close() noexcept;
    std::size_t shortcut_index(std::size_t slot) const noexcept;
    void request_selection(std::size_t index) noexcept;
    void request_step(int delta) noexcept;
    void begin_interaction() noexcept;
    PopupKind kind() const { return kind_; }
    CandidateKeys keys() const { return keys_; }
    std::wstring_view font_face() const { return font_face_; }
    bool shows_input_mode() const { return show_input_mode_; }
    bool presentation_available() const { return !closed_ && (!requested_visible_ || (window_ && IsWindowVisible(window_))); }
private:
    struct Row {
        std::wstring text, annotation;
        CandidateSource source = CandidateSource::Dictionary;
        bool operator==(const Row&) const = default;
    };
    struct Layout {
        int pad_x = 8;
        int pad_y = 6;
        int header_height = 0;
        int editor_height = 0;
        int rows_top = 6;
        int row_height = 34;
        int footer_height = 28;
        int keycap_size = 22;
        int badge_height = 18;
        int footer_top(int client_height) const noexcept { return (std::max)(0, client_height - footer_height); }
        int rows_bottom(int client_height) const noexcept { return (std::max)(rows_top, footer_top(client_height)); }
    };
    int scale(int px) const noexcept { return MulDiv(px, static_cast<int>(dpi_ ? dpi_ : 96), 96); }
    Layout layout() const noexcept;
    UINT current_page() const;
    std::pair<UINT, UINT> visible_rows() const;
    std::wstring compact_footer_hint() const;
    std::wstring footer_counter() const;
    void refresh_theme() noexcept;
    void ensure_fonts(UINT dpi) noexcept;
    void release_fonts() noexcept;
    SIZE measure();
    void render() noexcept;
    void paint(HDC) const;
    void paint_candidates(SurfaceCanvas&, const Layout&, int width, int top, int bottom) const;
    void paint_registration_header(SurfaceCanvas&, const Layout&, int width) const;
    void paint_forget_body(SurfaceCanvas&, const Layout&, int width) const;
    void paint_notice(SurfaceCanvas&, const Layout&, int width, int height) const;
    void paint_footer(SurfaceCanvas&, const Layout&, int width, int top) const;
    std::optional<std::size_t> hit_test(POINT) const noexcept;
    void cancel_pointer() noexcept;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    std::atomic<ULONG> refs_{1};
    Microsoft::WRL::ComPtr<ITfDocumentMgr> document_;
    std::shared_ptr<const std::vector<Candidate>> source_rows_;
    std::u32string source_suffix_;
    std::vector<Row> rows_;
    std::vector<UINT> pages_;
    const PopupKind kind_;
    const CandidateKeys keys_;
    InputMethod input_method_ = InputMethod::Skk;
    const std::wstring font_face_;
    const bool show_input_mode_;
    std::wstring heading_, draft_, hint_;
    Mode editor_mode_ = Mode::Hiragana;
    std::size_t registration_depth_ = 0;
    std::wstring registration_reading_;
    bool registration_limit_ = false;
    bool editor_selecting_ = false;
    bool forgetting_ = false;
    bool has_forget_target_ = false;
    std::wstring forget_word_, forget_scope_, forget_answer_;
    UINT selection_ = 0;
    DWORD updated_ = 0;
    bool redraw_ = true;
    std::optional<UINT> previous_selection_;
    std::optional<std::size_t> hovered_;
    bool tracking_mouse_ = false;
    int row_width_ = 0;
    SIZE measured_{};
    bool requested_visible_ = false, located_ = false, closed_ = false, ime_visible_ = false;
    bool dwm_framed_ = false;
    HWND window_ = nullptr, owner_ = nullptr;
    HFONT font_ = nullptr, title_font_ = nullptr, sub_font_ = nullptr, badge_font_ = nullptr;
    UiTheme theme_{};
    RECT anchor_{};
    std::wstring class_name_;
    UINT dpi_ = 0;
    std::weak_ptr<CandidateActions> actions_;
    std::uint64_t revision_ = 0;
    CandidatePointer pointer_;
    CandidateWheel wheel_;
    InputTimingToken paint_timing_;
};

class CandidatePresenter {
public:
    ~CandidatePresenter() { close(); }
    void update(ITfThreadMgr*, ITfContext*, const Engine&, std::weak_ptr<CandidateActions> = {}, bool show_candidates = true, CandidateKeys keys = CandidateKeys::Numbers, std::wstring_view notice = {},
        std::wstring_view font_face = default_ui_font, bool show_input_mode = false) noexcept;
    // Also captures a position before a text mutation, even without a popup.
    void locate(ITfContext*, ITfComposition*, TfEditCookie, std::uint64_t session = 0, bool capture_position = false) noexcept;
    void dismiss_notice() noexcept;
    void invalidate_position() noexcept;
    void hide() noexcept;
    void close() noexcept;
    Key selection_key(Key, const Engine&) const noexcept;
    bool presentation_available() const { return list_ && list_->presentation_available(); }
private:
    void close_ui() noexcept;
    Microsoft::WRL::ComPtr<ITfUIElementMgr> manager_;
    Microsoft::WRL::ComPtr<CandidateList> list_;
    Microsoft::WRL::ComPtr<ITfContext> position_context_;
    Microsoft::WRL::ComPtr<ITfComposition> position_composition_;
    Microsoft::WRL::ComPtr<IUnknown> position_view_;
    HWND position_owner_ = nullptr;
    RECT position_{};
    std::uint64_t position_session_ = 0, generation_ = 0;
    DWORD id_ = 0;
    bool begun_ = false;
};
} // namespace nskkzime::windows
