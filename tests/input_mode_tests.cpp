#include "dictionary_wait.h"
#include "runtime_dictionary.h"
#include "tsf_mode_host.h"
#include "learning_ipc.h"
#include "candidate_ui.h"
#include <iostream>
#include <stdexcept>
namespace nskkzime::windows {
HINSTANCE module_instance = nullptr;
std::atomic<long> module_objects{0};
std::atomic<long> server_locks{0};
}
using namespace nskkzime;
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Menu final : public ITfMenu {
public:
    struct Item { UINT id; DWORD flags; std::wstring text; };
    std::vector<Item> items;
    ULONG refs = 1;
    bool reject = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfMenu) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value = --refs; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE AddMenuItem(UINT id, DWORD flags, HBITMAP, HBITMAP, const WCHAR* text, ULONG size, ITfMenu**) override {
        if (reject) return E_FAIL;
        items.push_back({id, flags, text ? std::wstring(text, size) : std::wstring{}}); return S_OK;
    }
};
struct Session {
    ComPtr<test_host::ModeManager> host;
    ComPtr<test_host::Context> context;
    ComPtr<ITfTextInputProcessorEx> service;
    ComPtr<ITfKeyEventSink> keys;
    Session(bool closed = false, bool reject_bar = false) {
        host.Attach(new test_host::ModeManager); context.Attach(new test_host::Context);
        if (closed) host->open->value = 0;
        host->reject_bar = reject_bar;
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "create service"); service.As(&keys);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, 0)), "activate service"); wait_for_dictionary(nskkzime::windows::runtime_dictionary_ready);
    }
    ~Session() { service->Deactivate(); context->pump(); }
    bool key(WPARAM code, bool control = false, bool shift = false, bool repeat = false) {
        BYTE state[256]{};
        if (control) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
        if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
        SetKeyboardState(state);
        LPARAM parameter = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
        if (repeat) parameter |= static_cast<LPARAM>(1) << 30;
        BOOL eaten = FALSE;
        const auto text = context->doc->text; const auto mode = host->open->value;
        keys->OnTestKeyDown(context.Get(), code, parameter, &eaten);
        check(text == context->doc->text && mode == host->open->value, "key preflight changed mode/text");
        if (eaten) keys->OnKeyDown(context.Get(), code, parameter, &eaten);
        const bool result = eaten != FALSE; keys->OnKeyUp(context.Get(), code, parameter, &eaten); return result;
    }
    void type(std::string_view text) {
        for (const char ch : text) {
            const auto vk = VkKeyScanExW(static_cast<wchar_t>(ch), GetKeyboardLayout(0));
            check(vk != -1 && !(HIBYTE(vk) & 6), "unavailable key");
            check(key(LOBYTE(vk), false, (HIBYTE(vk) & 1) != 0), "key not consumed");
        }
    }
    ComPtr<ITfLangBarItemButton> button() { ComPtr<ITfLangBarItemButton> value; if (host->button) host->button.As(&value); return value; }
    std::wstring label() { BSTR value = nullptr; check(button() && SUCCEEDED(button()->GetText(&value)), "no label"); std::wstring result(value); SysFreeString(value); return result; }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::current_path() / ("tsf-mode-" + learning_client_id());
    std::filesystem::create_directory(root); const auto file = root / L"user.nskkdb";
    SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", argv[1]); SetEnvironmentVariableW(L"NSKKZIME_LEARNING", L"1");
    SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", file.c_str());
    BYTE previous[256]{}; GetKeyboardState(previous); CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        {
            auto actions = std::make_shared<ModeActions>(); unsigned mode_changes = 0;
            std::vector<ImeTool> opened;
            actions->change = [&](Mode) { ++mode_changes; };
            actions->tools_available = {true, true};
            actions->open_tool = [&](ImeTool tool) { opened.push_back(tool); return S_OK; };
            ComPtr<ModeButton> button; button.Attach(new ModeButton(actions));
            ComPtr<Menu> menu; menu.Attach(new Menu);
            check(button->InitMenu(menu.Get()) == S_OK && menu->items.size() == 6, "missing mode/tool menu items");
            check(menu->items[0].flags == TF_LBMENUF_CHECKED && menu->items[3].flags == TF_LBMENUF_SEPARATOR &&
                menu->items[4].id == settings_menu_id && menu->items[4].text == L"設定…" &&
                menu->items[5].id == dictionary_menu_id && menu->items[5].text == L"学習辞書の管理…", "tool menu identity incorrect");
            check(button->OnMenuSelect(settings_menu_id) == S_OK && button->OnMenuSelect(dictionary_menu_id) == S_OK &&
                opened == std::vector<ImeTool>{ImeTool::Settings, ImeTool::Dictionary} && !mode_changes, "tool selection changed input mode or picked wrong GUI");
            actions->tools_available[1] = false; menu->items.clear(); button->InitMenu(menu.Get());
            check(menu->items[5].flags == TF_LBMENUF_GRAYED && button->OnMenuSelect(dictionary_menu_id) == S_FALSE, "invalid dictionary path remained actionable");
            menu->reject = true; check(button->InitMenu(menu.Get()) == E_FAIL, "host menu error ignored");
            check(button->OnMenuSelect(99) == E_INVALIDARG, "unknown menu command accepted");
            button->close(); check(button->OnMenuSelect(settings_menu_id) == S_FALSE && opened.size() == 2, "closed menu launched GUI");
        }
        {
            Session s; check(s.label() == L"あ" && s.host->open->value == 1, "initial hiragana");
            TF_LANGBARITEMINFO info{}; s.button()->GetInfo(&info); check(info.guidItem == GUID_LBI_INPUTMODE, "shell item id");
            HICON icon = nullptr; check(SUCCEEDED(s.button()->GetIcon(&icon)) && icon, "mode icon"); DestroyIcon(icon);
            s.type("q"); check(s.label() == L"ア" && (*s.host->conversion->value & TF_CONVERSIONMODE_KATAKANA), "katakana sync");
            s.type("a"); check(s.context->doc->text == L"ア", "katakana input");
            s.type("l"); check(s.label() == L"A" && s.host->open->value == 0 && !s.key('A'), "Latin passthrough");
            check(s.key('J', true) && s.label() == L"あ", "C-j did not reopen");
            s.host->open->set(0); check(s.label() == L"A" && !s.key('A'), "external close");
            s.host->open->set(1); s.host->conversion->set(TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_KATAKANA);
            check(s.label() == L"ア", "external katakana");
            s.host->conversion->set(0); check(s.label() == L"A" && s.host->open->value == 0, "external alphanumeric");
            s.host->conversion->set(TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_KATAKANA);
            check(s.label() == L"A", "conversion opened a closed IME");
            s.host->open->set(1); check(s.label() == L"ア", "conversion-before-open was lost");
            s.host->open->set(0);
            check(s.key(VK_SPACE, true) && s.label() == L"あ", "toggle on");
            check(s.key(VK_SPACE, true, false, true) && s.label() == L"あ", "repeat toggled twice");
            check(!s.key(VK_SPACE, true, true), "modified shortcut captured");
            BOOL eaten = FALSE; s.keys->OnPreservedKey(s.context.Get(), toggle_mode_key, &eaten);
            check(eaten && s.label() == L"A", "preserved toggle");
            check(s.key(VK_IME_ON) && s.label() == L"あ", "IME on key");
            check(s.key(VK_IME_OFF) && s.label() == L"A", "IME off key");
            s.button()->OnMenuSelect(2); check(s.label() == L"ア", "mode menu");
            s.button()->OnClick(TF_LBI_CLK_LEFT, {}, nullptr); check(s.label() == L"A", "button toggle");
            s.context->readonly = true; check(!s.key(VK_SPACE, true), "readonly shortcut captured");
            auto retained = s.button(); s.service->Deactivate();
            check(!s.host->button && !s.host->open->sink && !s.host->conversion->sink && s.host->preserved.empty(), "mode bindings leaked");
            check(retained->OnMenuSelect(1) == S_FALSE, "retired button remained live");
            check(retained->OnMenuSelect(settings_menu_id) == S_FALSE && retained->OnMenuSelect(dictionary_menu_id) == S_FALSE,
                "retired service could open tools");
        }
        {
            Session s(true); check(s.label() == L"A" && !s.key('A'), "existing closed state ignored");
            s.key(VK_KANJI); s.type("Kanji"); s.key(VK_SPACE);
            ComPtr<CandidateList> candidates(static_cast<CandidateList*>(s.host->ui.Get()));
            candidates->request_selection(1);
            auto old = s.context->composition; s.host->open->set(0);
            check(!s.host->ui && s.label() == L"A" && s.context->doc->text == L"漢字", "close overwrote active candidate");
            s.key('J', true); s.type("Ka"); s.context->pump();
            check(old->ended && s.context->doc->text == L"漢字か" && !s.context->composition->ended, "late close damaged new composition");
        }
        for (const bool preserved : {false, true}) {
            Session s; s.type("/draft"); s.key(VK_SPACE); s.type("la");
            check(s.host->open->value == 1, "inner Latin closed the registration editor");
            s.host->open->set(1); check(s.host->ui != nullptr, "unchanged Windows mode canceled registration");
            if (preserved) {
                BOOL eaten = FALSE; s.keys->OnPreservedKey(s.context.Get(), toggle_mode_key, &eaten);
                check(eaten != FALSE, "registration toggle was not consumed");
            } else s.key(VK_SPACE, true);
            s.context->pump();
            check(s.context->doc->text == L"draft" && !s.host->ui && s.label() == L"A", "mode switch leaked registration draft");
        }
        {
            Session s(false, true); s.type("a"); check(s.context->doc->text == L"あ", "bar failure broke input");
        }
        check(module_objects == 0, "mode objects retained service");
        Sleep(600);
        check(!std::filesystem::exists(file) || read_user_dictionary(file).revision() == 0, "mode switch learned unconfirmed text");
        std::cout << "PASS mode labels, Windows state, menu, key toggles, composition lifetime and teardown\n"; result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    SetKeyboardState(previous); CoUninitialize();
    Sleep(12000);
    for (const auto& path : {file, std::filesystem::path(file.native() + L".lock")}) { std::error_code ignored; std::filesystem::remove(path, ignored); }
    std::error_code ignored; std::filesystem::remove(root, ignored); return result;
}
