#pragma once
#include "module.h"
#include "nskkzime/engine.h"
#include "ui_settings.h"
#include <wrl/client.h>
#include <string>

namespace nskkzime::windows {
inline constexpr GUID mode_indicator_ui_guid = {0x7c81cbf2, 0x820d, 0x4d11, {0x8c, 0x36, 0x1b, 0x1e, 0x1a, 0xb8, 0xf1, 0x9c}};

// One immutable mode notification. Closing it also invalidates queued layout
// reads; a retained UI element or edit session cannot reopen an old notice.
class ModeIndicator final : public ITfUIElement, public ITfTextLayoutSink, public ITfTextEditSink {
public:
    ModeIndicator(Mode, const UiSettings&);
    ~ModeIndicator();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR*) override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID*) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL) override;
    HRESULT STDMETHODCALLTYPE IsShown(BOOL*) override;
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext*, TfLayoutCode, ITfContextView*) override;
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext*, TfEditCookie, ITfEditRecord*) override;

    void start(ITfThreadMgr*, ITfContext*, TfClientId, ITfContext* origin = nullptr) noexcept;
    void close() noexcept;
    bool matches_focus(ITfDocumentMgr* document, ITfContext* context) const noexcept {
        return !closed_ && focus_document_.Get() == document && focus_context_.Get() == context;
    }
private:
    bool current() noexcept;
    void request_position() noexcept;
    void retry_position() noexcept;
    void cancel_retry() noexcept;
    void locate(TfEditCookie) noexcept;
    void render() noexcept;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);

    std::atomic<ULONG> refs_{1};
    const Mode mode_;
    const std::wstring font_face_;
    const unsigned int duration_ms_;
    Microsoft::WRL::ComPtr<ITfThreadMgr> thread_;
    Microsoft::WRL::ComPtr<ITfContext> context_, focus_context_;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> focus_document_;
    Microsoft::WRL::ComPtr<ITfUIElementMgr> manager_;
    TfClientId client_ = TF_CLIENTID_NULL;
    DWORD layout_cookie_ = TF_INVALID_COOKIE, edit_cookie_ = TF_INVALID_COOKIE, ui_id_ = 0;
    ULONGLONG deadline_ = 0, retry_deadline_ = 0;
    HWND window_ = nullptr, owner_ = nullptr;
    HFONT font_ = nullptr;
    RECT anchor_{};
    std::wstring class_name_;
    UINT dpi_ = 0;
    unsigned int retries_ = 0;
    bool started_ = false, closed_ = false, begun_ = false, requested_visible_ = false;
    bool located_ = false, locating_ = false, position_pending_ = false, ime_visible_ = false, retry_scheduled_ = false;
};
} // namespace nskkzime::windows
