#include "dictionary_wait.h"
#include "runtime_dictionary.h"
#include "dictionary_settings.h"
#include "tsf_mode_host.h"
#include "learning_ipc.h"
#include "candidate_ui.h"
#include "isolated_settings.h"
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
    Session(bool closed = false, bool reject_bar = false, bool opened = false) {
        host.Attach(new test_host::ModeManager); context.Attach(new test_host::Context);
        if (closed) host->open->value = 0;
        else if (opened) {
            host->open->value = 1;
            host->conversion->value = TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_KATAKANA;
        }
        host->reject_bar = reject_bar;
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "create service"); service.As(&keys);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, 0)), "activate service"); wait_for_dictionary(nskkzime::windows::runtime_dictionary_ready);
    }
    ~Session() { service->Deactivate(); context->pump(); }
    bool down(WPARAM code, bool control = false, bool shift = false, bool repeat = false) {
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
        BOOL again = FALSE; keys->OnTestKeyDown(context.Get(), code, parameter, &again);
        check(eaten == again, "repeated preflight changed key routing");
        if (eaten) keys->OnKeyDown(context.Get(), code, parameter, &eaten);
        return eaten != FALSE;
    }
    bool up(WPARAM code) {
        const auto parameter = (static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16) |
            (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
        BOOL eaten = FALSE; keys->OnTestKeyUp(context.Get(), code, parameter, &eaten);
        BOOL again = FALSE; keys->OnTestKeyUp(context.Get(), code, parameter, &again);
        check(eaten == again, "repeated key-up preflight changed routing");
        if (eaten) keys->OnKeyUp(context.Get(), code, parameter, &eaten);
        return eaten != FALSE;
    }
    bool key(WPARAM code, bool control = false, bool shift = false, bool repeat = false) {
        const auto eaten = down(code, control, shift, repeat);
        check(up(code) == eaten, "physical key down/up routing disagrees");
        return eaten;
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
    use_isolated_settings(root, "learning=on\ndictionary=" + settings_path(argv[1]) + "\n");
    const auto file = root / L"user-dictionary.nskkdb";
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
            auto actions = std::make_shared<ModeActions>();
            unsigned mode_changes = 0, menus = 0;
            std::vector<ImeTool> opened;
            actions->change = [&](Mode) { ++mode_changes; };
            actions->tools_available = {true, false};
            actions->open_tool = [&](ImeTool tool) { opened.push_back(tool); return S_OK; };
            UINT chosen = settings_menu_id;
            bool retire = false, hide = false;
            ComPtr<ModeButton> button;
            HMENU last_menu = nullptr;
            button.Attach(new ModeButton(actions, [&](HMENU menu, POINT point, const RECT* area) -> UINT {
                ++menus; last_menu = menu;
                check(point.x == -20 && point.y == 30 && area && area->left == -25, "menu anchor lost");
                check(GetMenuItemCount(menu) == 6 && GetMenuItemID(menu, 4) == settings_menu_id &&
                    GetMenuItemID(menu, 5) == dictionary_menu_id, "native menu commands missing");
                check((GetMenuState(menu, 1, MF_BYCOMMAND) & MF_CHECKED) &&
                    (GetMenuState(menu, dictionary_menu_id, MF_BYCOMMAND) & MF_GRAYED), "native menu state wrong");
                wchar_t label[32]{}; GetMenuStringW(menu, settings_menu_id, label, ARRAYSIZE(label), MF_BYCOMMAND);
                check(std::wstring_view(label) == L"設定…", "native settings label wrong");
                check(button->OnClick(TF_LBI_CLK_RIGHT, point, area) == S_FALSE, "recursive menu opened");
                if (retire) button->close();
                if (hide) button->Show(FALSE);
                return chosen;
            }));
            TF_LANGBARITEMINFO info{}; button->GetInfo(&info);
            check((info.dwStyle & TF_LBI_STYLE_BTN_BUTTON) && !(info.dwStyle & TF_LBI_STYLE_BTN_MENU), "click notification style missing");
            const POINT point{-20, 30}; const RECT area{-25, 20, 0, 40};
            check(button->OnClick(TF_LBI_CLK_RIGHT, point, &area) == S_OK && menus == 1 &&
                opened == std::vector<ImeTool>{ImeTool::Settings} && !mode_changes, "right click did not open settings or toggled mode");
            check(!IsMenu(last_menu), "popup menu handle leaked");
            chosen = 0;
            check(button->OnClick(TF_LBI_CLK_RIGHT, point, &area) == S_OK && opened.size() == 1 && !mode_changes, "cancel activated a command");
            chosen = dictionary_menu_id;
            check(button->OnClick(TF_LBI_CLK_RIGHT, point, &area) == S_FALSE && opened.size() == 1, "disabled popup command executed");
            chosen = 2;
            check(button->OnClick(TF_LBI_CLK_RIGHT, point, &area) == S_OK && mode_changes == 1, "popup mode selection lost");
            hide = true; chosen = settings_menu_id;
            check(button->OnClick(TF_LBI_CLK_RIGHT, point, &area) == S_FALSE && opened.size() == 1, "hidden popup dispatched stale selection");
            hide = false; button->Show(TRUE); retire = true;
            check(button->OnClick(TF_LBI_CLK_RIGHT, point, &area) == S_FALSE && opened.size() == 1, "retired popup dispatched stale selection");
            const auto count = menus;
            check(button->OnClick(TF_LBI_CLK_RIGHT, point, &area) == S_FALSE && menus == count, "retired button displayed popup");
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
            Session s(false, false, true);
            check(s.label() == L"ア" && s.host->open->value == 1, "default startup lost Windows open mode");
        }
        {
            auto settings = read_dictionary_settings(root / L"settings.ini");
            settings.startup_ime_off = true; save_dictionary_settings(root / L"settings.ini", settings);
            for (const auto initial : {0, 1, 2}) {
                Session s(initial == 1, false, initial == 2);
                check(s.label() == L"A" && s.host->open->value == 0 && s.host->conversion->value == 0,
                    "startup did not force engine and Windows OFF");
                check(!s.key('A'), "startup OFF captured ordinary typing");
                check(s.key(VK_IME_ON) && s.label() == L"あ", "startup OFF prevented explicit ON");
                s.keys->OnSetFocus(FALSE); s.keys->OnSetFocus(TRUE);
                check(s.label() == L"あ", "focus reapplied startup OFF");
                s.service->Deactivate();
                check(SUCCEEDED(s.service->ActivateEx(s.host.Get(), 1, 0)) && s.label() == L"あ",
                    "reactivation reapplied startup OFF");
                s.type("a"); check(s.context->doc->text == L"あ", "manual ON did not restore kana input");
                s.host->open->set(0); s.host->open->set(1);
                check(s.label() == L"あ", "startup policy overrode Windows ON");
                s.button()->OnMenuSelect(3); s.button()->OnMenuSelect(2);
                check(s.label() == L"ア", "startup policy overrode mode menu");
                s.key(VK_SPACE, true); s.key(VK_SPACE, true);
                check(s.label() == L"あ", "startup policy overrode toggle shortcut");
            }
            settings.startup_ime_off = false; save_dictionary_settings(root / L"settings.ini", settings);
            Session s(false, false, true);
            check(s.label() == L"ア" && s.host->open->value == 1, "disabling startup policy did not restore Windows mode");
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
        {
            Session s; s.type("d"); check(s.context->doc->text == L"d", "pending prefix missing");
            s.context->doc->reject_write = true;
            check(!s.key('L') && s.label() == L"あ" && s.host->open->value == 1 && s.context->doc->text == L"d",
                "failed prefix edit changed mode or text");
            s.context->doc->reject_write = false; s.type("l");
            check(s.label() == L"A" && s.host->open->value == 0 && s.context->doc->text.empty() && !s.key('S'),
                "failed prefix did not discard d and switch Latin");
        }
        {
            Session s; s.type("dqkana"); check(s.label() == L"ア" && s.context->doc->text == L"カナ", "prefix retry did not sync katakana");
            s.key('J', true); s.type("zlz/");
            check(s.label() == L"あ" && s.context->doc->text == L"カナ→・", "valid continuation became a mode command");
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
        {
            Session s(true);
            check(!s.key('Q') && !s.key('Q', true, true), "non-prefix Q captured");
            check(s.key('Q', true) && !s.key('J', true) && s.label() == L"A", "quoted C-j did not pass through");
            check(s.key('J', true) && s.label() == L"あ", "quote did not expire after C-j");
            s.key(VK_IME_OFF);
            check(s.down('Q', true) && s.down('Q', true, false, true) && s.up('Q'), "prefix Q repeats escaped");
            check(!s.down('Q', true) && !s.down('Q', true, false, true) && !s.up('Q'), "quoted Q down/repeat/up captured");
            check(s.key('J', true), "quoted Q repeat armed another quote");
            s.key(VK_IME_OFF); s.key('Q', true);
            check(!s.down('J', true) && !s.down('J', true, false, true) && !s.up('J') && s.label() == L"A", "quoted J repeat reopened IME");
            check(s.key('J', true), "quoted J release retained bypass");
            s.key(VK_IME_OFF); s.key('Q', true);
            check(!s.key(VK_CONTROL) && !s.key(VK_CONTROL, true) && !s.key('J', true), "Ctrl release/repress consumed quote");
            check(s.key('J', true), "modifier sequence left quote armed");
            for (const auto other : std::array<WPARAM, 2>{{'A', VK_ESCAPE}}) {
                s.key(VK_IME_OFF); s.key('Q', true);
                check(!s.key(other) && s.key('J', true), "other key did not cancel quote normally");
            }
            s.key(VK_IME_OFF); s.key('Q', true); s.keys->OnSetFocus(FALSE);
            check(s.key('J', true), "quote survived focus loss");
            s.key(VK_IME_OFF); s.key('Q', true);
            ComPtr<ITfThreadMgrEventSink> events; s.service.As(&events);
            events->OnSetFocus(nullptr, nullptr);
            check(s.key('J', true), "quote survived document focus loss");
            s.key(VK_IME_OFF); s.key('Q', true);
            s.context.Attach(new test_host::Context);
            check(s.key('J', true), "quote leaked into another context");
            s.key(VK_IME_OFF); s.key('Q', true); s.key(VK_IME_ON); s.key(VK_IME_OFF);
            check(s.key('J', true), "quote survived mode changes");
            s.key(VK_IME_OFF); s.key('Q', true);
            s.service->Deactivate(); s.service->ActivateEx(s.host.Get(), 1, 0);
            check(s.key('J', true), "quote survived reactivation");
        }
        {
            Session s; s.type("/draft"); s.key(VK_SPACE); s.type("la");
            check(s.key('J', true) && s.host->open->value == 1 && s.host->ui, "registration Latin used OFF key policy");
        }
        {
            auto settings = read_dictionary_settings(root / L"settings.ini");
            wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
            auto name = std::filesystem::path(executable).filename().native();
            for (auto& ch : name) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
            settings.off_key_exclusions = {name}; save_dictionary_settings(root / L"settings.ini", settings);
            {
                Session s(true);
                check(!s.key('J', true) && !s.key('Q', true) && !s.key('J', true) && s.label() == L"A", "excluded OFF keys captured");
                check(s.key(VK_IME_ON) && s.key('J', true), "exclusion disabled ON-mode C-j");
                s.type("l"); check(!s.key('A') && !s.key('J', true), "l bypassed app exclusion");
                check(s.key(VK_SPACE, true) && s.label() == L"あ", "exclusion disabled explicit ON/OFF shortcut");
            }
            settings.off_key_exclusions.clear(); save_dictionary_settings(root / L"settings.ini", settings);
        }
        check(module_objects == 0, "mode objects retained service");
        Sleep(600);
        check(!std::filesystem::exists(file) || read_user_dictionary(file).revision() == 0, "mode switch learned unconfirmed text");
        std::cout << "PASS mode labels, Windows state, quote keys, app exclusions, composition lifetime and teardown\n"; result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    SetKeyboardState(previous); CoUninitialize();
    Sleep(300); // Let background saves release the writer lock.
    std::error_code ignored; std::filesystem::remove_all(root, ignored); return result;
}
