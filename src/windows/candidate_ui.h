#pragma once
#include "module.h"
#include "input_timing.h"
#include "nskkzime/engine.h"
#include <wrl/client.h>
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
std::optional<std::size_t> candidate_row_at(POINT, SIZE client, int padding, int line_height,
    bool registration, std::size_t first, std::size_t end) noexcept;
inline constexpr GUID registration_ui_guid = {0x5422d011, 0x5654, 0x440d, {0x97, 0xdd, 0x64, 0xa2, 0xfa, 0xef, 0x4a, 0x2a}};
inline constexpr GUID reading_ui_guid = {0x3eb96c26, 0x5b51, 0x4a45, {0x8c, 0x5a, 0x26, 0x29, 0x80, 0xa3, 0x19, 0x0b}};
enum class PopupKind { Candidates, Editor, Reading };
class CandidateList final : public ITfCandidateListUIElement {
public:
    explicit CandidateList(PopupKind = PopupKind::Candidates, std::weak_ptr<CandidateActions> actions = {});
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
    void locate(HWND owner, const RECT* anchor) noexcept;
    void close() noexcept;
    std::size_t shortcut_index(std::size_t slot) const noexcept;
    void request_selection(std::size_t index) noexcept;
    void begin_interaction() noexcept;
    PopupKind kind() const { return kind_; }
    bool presentation_available() const { return !closed_ && (!requested_visible_ || (window_ && IsWindowVisible(window_))); }
private:
    struct Row { std::wstring text, annotation; bool operator==(const Row&) const = default; };
    UINT current_page() const;
    std::pair<UINT, UINT> visible_rows() const;
    std::wstring footer() const;
    void render() noexcept;
    void paint(HDC) const;
    std::optional<std::size_t> hit_test(POINT) const noexcept;
    void cancel_pointer() noexcept;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    std::atomic<ULONG> refs_{1};
    Microsoft::WRL::ComPtr<ITfDocumentMgr> document_;
    std::vector<Row> rows_;
    std::vector<UINT> pages_;
    const PopupKind kind_;
    std::wstring heading_, draft_, hint_;
    UINT selection_ = 0;
    DWORD updated_ = 0;
    bool requested_visible_ = false, located_ = false, closed_ = false, ime_visible_ = false;
    HWND window_ = nullptr, owner_ = nullptr;
    HFONT font_ = nullptr;
    RECT anchor_{};
    std::wstring class_name_;
    int line_height_ = 28, padding_ = 10;
    UINT dpi_ = 0;
    std::weak_ptr<CandidateActions> actions_;
    std::uint64_t revision_ = 0;
    CandidatePointer pointer_;
    InputTimingToken paint_timing_;
};

class CandidatePresenter {
public:
    ~CandidatePresenter() { close(); }
    void update(ITfThreadMgr*, ITfContext*, const Engine&, std::weak_ptr<CandidateActions> = {}, bool show_reading = false) noexcept;
    void locate(ITfContext*, ITfComposition*, TfEditCookie) noexcept;
    void hide() noexcept;
    void close() noexcept;
    Key selection_key(Key, const Engine&) const noexcept;
    bool presentation_available() const { return list_ && list_->presentation_available(); }
private:
    Microsoft::WRL::ComPtr<ITfUIElementMgr> manager_;
    Microsoft::WRL::ComPtr<CandidateList> list_;
    DWORD id_ = 0;
    bool begun_ = false;
};
} // namespace nskkzime::windows
