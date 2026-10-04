#pragma once
#include "module.h"
#include "nskkzime/engine.h"
#include <wrl/client.h>
#include <string>
#include <utility>
#include <vector>

namespace nskkzime::windows {
inline constexpr GUID candidate_ui_guid = {0x680bc7bf, 0xbe19, 0x42e9, {0xa4, 0x38, 0x0d, 0x8d, 0xf2, 0x99, 0xf5, 0x0b}};

// Snapshot owned by the UI manager. It never points back to the text service.
inline constexpr GUID registration_ui_guid = {0x5422d011, 0x5654, 0x440d, {0x97, 0xdd, 0x64, 0xa2, 0xfa, 0xef, 0x4a, 0x2a}};
class CandidateList final : public ITfCandidateListUIElement {
public:
    explicit CandidateList(bool registration = false);
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
    bool registration() const { return registration_; }
    bool presentation_available() const { return !closed_ && (!requested_visible_ || (window_ && IsWindowVisible(window_))); }
private:
    struct Row { std::wstring text, annotation; bool operator==(const Row&) const = default; };
    UINT current_page() const;
    std::pair<UINT, UINT> visible_rows() const;
    std::wstring footer() const;
    void render() noexcept;
    void paint(HDC) const;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    std::atomic<ULONG> refs_{1};
    Microsoft::WRL::ComPtr<ITfDocumentMgr> document_;
    std::vector<Row> rows_;
    std::vector<UINT> pages_;
    const bool registration_;
    std::wstring heading_, draft_, hint_;
    UINT selection_ = 0;
    DWORD updated_ = 0;
    bool requested_visible_ = false, located_ = false, closed_ = false;
    HWND window_ = nullptr, owner_ = nullptr;
    HFONT font_ = nullptr;
    RECT anchor_{};
    std::wstring class_name_;
    int line_height_ = 28, padding_ = 10;
    UINT dpi_ = 0;
};

class CandidatePresenter {
public:
    ~CandidatePresenter() { close(); }
    void update(ITfThreadMgr*, ITfContext*, const Engine&) noexcept;
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
