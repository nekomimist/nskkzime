#pragma once
#include "module.h"
#include "nskkzime/engine.h"
#include "tool_launcher.h"
#include <ctfutb.h>
#include <ctffunc.h>
#include <wrl/client.h>
#include <array>
#include <functional>

namespace nskkzime::windows {
struct ModeActions {
    std::function<void(Mode)> change;
    std::function<HRESULT(ImeTool)> open_tool;
    std::array<bool, 2> tools_available{};
};
inline constexpr UINT settings_menu_id = 10, dictionary_menu_id = 11;
inline constexpr GUID toggle_mode_key = {0xbddfce88,0xaad3,0x4b89,{0x98,0xef,0x76,0x88,0x67,0x6b,0x0f,0x16}};
inline constexpr GUID kanji_mode_key = {0x8a52ae66,0xd4ae,0x49cd,{0x95,0xbe,0xa3,0xf3,0xcb,0xd8,0xb6,0x29}};
inline constexpr std::array<TF_PRESERVEDKEY, 2> mode_keys{{{VK_SPACE, TF_MOD_CONTROL}, {VK_KANJI, 0}}};
inline constexpr std::array<GUID, 2> mode_key_ids{{toggle_mode_key, kanji_mode_key}};

class ModeButton final : public ITfLangBarItemButton, public ITfSource {
public:
    explicit ModeButton(std::weak_ptr<ModeActions> actions);
    ~ModeButton();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO*) override;
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD*) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL) override;
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR*) override;
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick, POINT, const RECT*) override;
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu*) override;
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT) override;
    HRESULT STDMETHODCALLTYPE GetIcon(HICON*) override;
    HRESULT STDMETHODCALLTYPE GetText(BSTR*) override;
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID, IUnknown*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD) override;
    void update(Mode mode) noexcept;
    void close() noexcept;
private:
    HRESULT select(Mode) noexcept;
    std::atomic<ULONG> refs_{1};
    std::weak_ptr<ModeActions> actions_;
    Microsoft::WRL::ComPtr<ITfLangBarItemSink> sink_;
    Mode mode_ = Mode::Hiragana;
    DWORD status_ = 0;
};

// Owns the thread-local Windows state and language bar binding. Disconnect
// before releasing it: compartment subscriptions otherwise keep it alive.
class InputMode final : public ITfCompartmentEventSink {
public:
    InputMode();
    ~InputMode();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE OnChange(REFGUID) override;
    void connect(ITfThreadMgr*, TfClientId, std::weak_ptr<ModeActions>, Mode) noexcept;
    void disconnect() noexcept;
    void publish(Mode) noexcept;
private:
    std::atomic<ULONG> refs_{1};
    std::weak_ptr<ModeActions> actions_;
    std::array<Microsoft::WRL::ComPtr<ITfCompartment>, 2> compartments_;
    std::array<DWORD, 2> cookies_{{TF_INVALID_COOKIE, TF_INVALID_COOKIE}};
    Microsoft::WRL::ComPtr<ITfLangBarItemMgr> bar_;
    Microsoft::WRL::ComPtr<ModeButton> button_;
    TfClientId client_ = TF_CLIENTID_NULL;
    bool writing_ = false;
    Mode mode_ = Mode::Hiragana;
};
} // namespace nskkzime::windows
