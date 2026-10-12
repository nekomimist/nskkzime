#include "dictionary_wait.h"
#include "module.h"
#include "learning_ipc.h"
#include "local_storage.h"
#include "tsf_test_host.h"
#include "isolated_settings.h"
#include "helper_cleanup.h"
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace nskkzime::windows;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool key(ITfKeyEventSink* sink, ITfContext* context, WPARAM code, bool shift = false) {
    BYTE state[256]{}; if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80; SetKeyboardState(state);
    const auto parameter = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
    BOOL eaten = FALSE;
    check(SUCCEEDED(sink->OnTestKeyDown(context, code, parameter, &eaten)), "key probe failed");
    if (eaten) check(SUCCEEDED(sink->OnKeyDown(context, code, parameter, &eaten)), "key handling failed");
    BOOL up = FALSE; sink->OnKeyUp(context, code, parameter, &up); return eaten != FALSE;
}
void type(ITfKeyEventSink* sink, ITfContext* context, std::string_view text) {
    for (const char ch : text) {
        const auto translated = VkKeyScanExW(static_cast<wchar_t>(ch), GetKeyboardLayout(0));
        check(translated != -1 && !(HIBYTE(translated) & 6), "test key unavailable");
        check(key(sink, context, LOBYTE(translated), (HIBYTE(translated) & 1) != 0), "key not consumed");
    }
}
template<class F> void until(F test) {
    const auto limit = GetTickCount64() + 7000;
    do { try { if (test()) return; } catch (...) {} Sleep(25); } while (GetTickCount64() < limit);
    throw std::runtime_error("learning did not reach the server");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const auto root = std::filesystem::current_path() / ("tsf-learning-" + learning_client_id());
    // Exercise the default user dictionary and persistent cache beside an isolated config.
    const auto numeric_file = root / L"numeric.skk";
    use_isolated_settings(root, "dictionary=" + settings_path(argv[2]) + "\ndictionary=" + settings_path(numeric_file) + "\n");
    std::ofstream(numeric_file, std::ios::binary) << "# /#1/#3/#2/\n#えん /#3円/\n";
    const auto file = root / L"user-dictionary.nskkdb";
    BYTE previous[256]{}; GetKeyboardState(previous);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto library = LoadLibraryW(argv[1]); int result = 1;
    try {
        check(library != nullptr, "DLL load failed");
        using Factory = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        using Status = HRESULT(WINAPI*)(BSTR*);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto factory_export = reinterpret_cast<Factory>(GetProcAddress(library, "DllGetClassObject"));
        const auto status_export = reinterpret_cast<Status>(GetProcAddress(library, "NskkzimeGetDictionaryStatus"));
#pragma warning(pop)
        ComPtr<IClassFactory> factory; check(factory_export && SUCCEEDED(factory_export(service_clsid, IID_PPV_ARGS(&factory))), "factory failed");
        ComPtr<ITfTextInputProcessorEx> service; check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service))), "service failed");
        ComPtr<ITfKeyEventSink> keys; service.As(&keys);
        ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, 0)), "activation failed"); wait_for_dictionary();
        BSTR dictionary_status = nullptr;
        check(status_export && SUCCEEDED(status_export(&dictionary_status)), "dictionary status unavailable");
        const bool cached = dictionary_status && std::wstring_view(dictionary_status).find(L"キャッシュ作成") != std::wstring_view::npos;
        SysFreeString(dictionary_status); check(cached, "saved configuration did not use the default dictionary cache");
        ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
        context->scope_requires_text_range = true; // Inspect the whole conversion, not a caret outside it.
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE);
        context->doc->reject_write = true;
        check(!key(keys.Get(), context.Get(), '2') && context->doc->text == L"漢字", "failed numeric edit consumed key or changed text");
        context->doc->reject_write = false;
        check(key(keys.Get(), context.Get(), '2') && context->doc->text == L"感じ", "numeric retry failed");
        until([&] { return read_user_dictionary(file).revision() == 1; });
        check(read_user_dictionary(file).lookup({U"かんじ"})[0].text == U"感じ", "wrong confirmed candidate saved");
        // Each successful commit below must preserve the one permitted event.
        for (const auto scope : {IS_PASSWORD, IS_PRIVATE, IS_NUMERIC_PASSWORD, IS_NUMERIC_PIN, IS_ALPHANUMERIC_PIN, IS_ALPHANUMERIC_PIN_SET}) {
            context.Attach(new test_host::Context); context->input_scope = scope;
            type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_RETURN);
        }
        {
            context.Attach(new test_host::Context); context->input_scope = IS_PRIVATE;
            const auto origin = test_host::transitory_context(context.Get());
            type(keys.Get(), origin.Get(), "Kanji"); key(keys.Get(), origin.Get(), VK_SPACE); key(keys.Get(), origin.Get(), VK_RETURN);
            check(context->composition->ended && origin->doc->text.empty(), "parent learning check did not edit the parent");
        }
        context.Attach(new test_host::Context);
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        Sleep(1200);
        check(read_user_dictionary(file).revision() == 1, "failed/private/canceled edit leaked learning");
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"感じ", "updated snapshot not used by next conversion");
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        context.Attach(new test_host::Context);
        type(keys.Get(), context.Get(), "Michi"); key(keys.Get(), context.Get(), VK_SPACE);
        check(host->ui != nullptr && context->doc->text == L"みち", "registration did not retain host reading");
        ComPtr<ITfCandidateListUIElement> list;
        check(host->ui.As(&list) == E_NOINTERFACE, "registration exposed draft as candidates");
        type(keys.Get(), context.Get(), "Nihon"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"みち", "registration draft leaked into document");
        context->reject_session = true;
        check(!key(keys.Get(), context.Get(), '1'), "refused inner selection consumed shortcut");
        context->reject_session = false;
        const auto draft_writes = context->doc->writes;
        check(key(keys.Get(), context.Get(), '1') && host->ui && context->doc->text == L"みち", "inner selection registered or changed host text");
        check(context->doc->writes == draft_writes, "inner selection unnecessarily rewrote the host reading");
        BSTR description = nullptr; host->ui->GetDescription(&description);
        const bool in_draft = description && std::wstring_view(description).find(L"日本") != std::wstring_view::npos &&
            std::wstring_view(description).find(L"▼") == std::wstring_view::npos;
        SysFreeString(description); check(in_draft, "inner selection did not settle in draft");
        Sleep(300); check(read_user_dictionary(file).revision() == 1, "inner selection learned before registration");
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_NEXT);
        context->reject_session = true;
        check(!key(keys.Get(), context.Get(), VK_RETURN), "refused inner candidate Enter consumed key");
        context->reject_session = false;
        check(key(keys.Get(), context.Get(), VK_RETURN) && host->ui && context->doc->text == L"みち", "inner candidate Enter registered or changed host text");
        check(context->doc->writes == draft_writes && !context->composition->ended, "inner candidate Enter rewrote or ended host composition");
        host->ui->GetDescription(&description);
        const bool entered_draft = description && std::wstring_view(description).find(L"日本漢字") != std::wstring_view::npos &&
            std::wstring_view(description).find(L"▼") == std::wstring_view::npos;
        SysFreeString(description); check(entered_draft, "paged candidate Enter did not append to existing draft");
        Sleep(300); check(read_user_dictionary(file).revision() == 1, "inner candidate Enter learned before registration");
        context->doc->reject_write = true;
        check(!key(keys.Get(), context.Get(), VK_RETURN), "failed registration consumed Enter");
        context->doc->reject_write = false;
        check(key(keys.Get(), context.Get(), VK_RETURN), "registration retry failed");
        check(context->doc->text == L"日本漢字", "registered text not applied");
        // Reuse immediately, before waiting for IPC or a snapshot refresh.
        type(keys.Get(), context.Get(), "Michi"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"日本漢字日本漢字" && host->ui && SUCCEEDED(host->ui.As(&list)), "new registration not immediately available");
        list.Reset(); key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        until([&] { return read_user_dictionary(file).revision() == 2; });
        check(read_user_dictionary(file).lookup({U"みち"})[0].text == U"日本漢字", "registered word not saved");

        context.Attach(new test_host::Context);
        type(keys.Get(), context.Get(), "/outer"); key(keys.Get(), context.Get(), VK_SPACE);
        type(keys.Get(), context.Get(), "/inner"); key(keys.Get(), context.Get(), VK_SPACE);
        type(keys.Get(), context.Get(), "kana"); key(keys.Get(), context.Get(), VK_RETURN);
        check(context->doc->text == L"outer", "nested registration leaked into host");
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        until([&] { return read_user_dictionary(file).revision() == 3; });
        check(read_user_dictionary(file).lookup({U"inner"})[0].text == U"かな" &&
            read_user_dictionary(file).lookup({U"outer"}).empty(), "nested registration/cancel persistence incorrect");

        context.Attach(new test_host::Context);
        type(keys.Get(), context.Get(), "/aborted"); key(keys.Get(), context.Get(), VK_SPACE);
        type(keys.Get(), context.Get(), "Nihon"); key(keys.Get(), context.Get(), VK_SPACE);
        host->events->OnSetFocus(nullptr, nullptr); context->pump();
        check(context->doc->text == L"aborted" && !host->ui, "focus loss inserted registration draft or retained UI");
        context.Attach(new test_host::Context); type(keys.Get(), context.Get(), "a");
        check(context->doc->text == L"あ", "registration followed focus into another field");
        {
            context.Attach(new test_host::Context); context->input_scope = IS_PRIVATE;
            type(keys.Get(), context.Get(), "/secret"); key(keys.Get(), context.Get(), VK_SPACE);
            check(context->doc->text == L"secret" && !host->ui, "private field started registration");
            key(keys.Get(), context.Get(), VK_RETURN); type(keys.Get(), context.Get(), "kana");
            check(context->doc->text == L"secretかな", "private field lost normal input");
        }
        // A scope can change while the editor is open; recheck before saving.
        context.Attach(new test_host::Context); type(keys.Get(), context.Get(), "/changed"); key(keys.Get(), context.Get(), VK_SPACE);
        type(keys.Get(), context.Get(), "kana"); context->input_scope = IS_PRIVATE; key(keys.Get(), context.Get(), VK_RETURN);
        check(context->doc->text == L"かな", "scope change lost confirmed text");
        Sleep(1200);
        check(read_user_dictionary(file).revision() == 3, "canceled/private registration persisted");
        context.Attach(new test_host::Context); host->reject_ui = true;
        type(keys.Get(), context.Get(), "/no-ui"); key(keys.Get(), context.Get(), VK_SPACE);
        check(!host->ui && context->doc->text == L"no-ui", "unavailable editor lost reading");
        key(keys.Get(), context.Get(), VK_RETURN); type(keys.Get(), context.Get(), "a");
        check(context->doc->text == L"no-uiあ", "invisible registration consumed more input");
        host->reject_ui = false;
        const auto open_forget = [&] {
            context.Attach(new test_host::Context);
            type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE);
            check(context->doc->text == L"感じ", "learned deletion target not selected");
            type(keys.Get(), context.Get(), "X");
            check(host->ui && host->ui.As(&list) == E_NOINTERFACE, "deletion prompt exposed candidate shortcuts");
            BSTR prompt = nullptr; host->ui->GetDescription(&prompt);
            const auto shown = prompt && std::wstring_view(prompt).find(L"学習語の削除") != std::wstring_view::npos &&
                std::wstring_view(prompt).find(L"感じ") != std::wstring_view::npos;
            SysFreeString(prompt); check(shown && context->doc->text == L"感じ", "deletion prompt missing or leaked into document");
        };
        open_forget(); type(keys.Get(), context.Get(), "no"); key(keys.Get(), context.Get(), VK_RETURN);
        check(context->doc->text == L"感じ" && host->ui && SUCCEEDED(host->ui.As(&list)), "cancel lost selected candidate");
        list.Reset(); key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        open_forget(); type(keys.Get(), context.Get(), "yes"); context->input_scope = IS_PRIVATE;
        key(keys.Get(), context.Get(), VK_RETURN); key(keys.Get(), context.Get(), VK_ESCAPE);
        open_forget(); type(keys.Get(), context.Get(), "yes"); host->events->OnSetFocus(nullptr, nullptr); context->pump();
        check(!host->ui && context->doc->text == L"感じ", "focus loss retained prompt or inserted answer");
        context.Attach(new test_host::Context); type(keys.Get(), context.Get(), "a");
        check(context->doc->text == L"あ", "deletion confirmation followed input focus");
        {
            context.Attach(new test_host::Context); context->input_scope = IS_PRIVATE;
            type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); type(keys.Get(), context.Get(), "X");
            check(host->ui && SUCCEEDED(host->ui.As(&list)), "private field opened deletion prompt");
            list.Reset(); key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        }
        context.Attach(new test_host::Context); type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE);
        host->reject_ui = true; type(keys.Get(), context.Get(), "X"); host->reject_ui = false;
        check(!host->ui && context->doc->text == L"感じ", "unavailable deletion UI changed text");
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        open_forget(); type(keys.Get(), context.Get(), "yes"); context->doc->reject_write = true;
        check(!key(keys.Get(), context.Get(), VK_RETURN) && context->doc->text == L"感じ", "failed deletion edit consumed Enter");
        Sleep(300); check(read_user_dictionary(file).revision() == 3, "canceled, private, focus-lost or failed deletion persisted");
        context->doc->reject_write = false; check(key(keys.Get(), context.Get(), VK_RETURN), "deletion retry failed");
        check(context->doc->text == L"かんじ", "approved deletion did not restore reading");
        key(keys.Get(), context.Get(), VK_SPACE); check(context->doc->text == L"漢字", "pending deletion kept learned priority");
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        until([&] { return read_user_dictionary(file).revision() == 4; });
        check(read_user_dictionary(file).lookup({U"かんじ"}).empty(), "deletion did not reach durable store");
        context.Attach(new test_host::Context); type(keys.Get(), context.Get(), "Michi"); key(keys.Get(), context.Get(), VK_SPACE);
        type(keys.Get(), context.Get(), "Xyes"); key(keys.Get(), context.Get(), VK_RETURN); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"みち" && host->ui && host->ui.As(&list) == E_NOINTERFACE, "deleted registration resurrected from cache");
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        until([&] { return read_user_dictionary(file).revision() == 5; });
        check(read_user_dictionary(file).lookup({U"みち"}).empty(), "registered word deletion not durable");

        // Hosts without a usable input scope (Office etc.) learn, register and delete like ordinary fields.
        context.Attach(new test_host::Context); context->unknown_scope = true;
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"漢字", "deleted learning still ranked first");
        key(keys.Get(), context.Get(), VK_RETURN);
        until([&] { return read_user_dictionary(file).revision() == 6; });
        check(read_user_dictionary(file).lookup({U"かんじ"})[0].text == U"漢字", "unknown-scope commit not learned");
        context.Attach(new test_host::Context); context->unknown_scope = true;
        type(keys.Get(), context.Get(), "/unscoped"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"unscoped" && host->ui, "unknown-scope field did not open registration");
        type(keys.Get(), context.Get(), "kana"); key(keys.Get(), context.Get(), VK_RETURN);
        check(context->doc->text == L"かな", "unknown-scope registration not applied");
        until([&] { return read_user_dictionary(file).revision() == 7; });
        check(read_user_dictionary(file).lookup({U"unscoped"})[0].text == U"かな", "unknown-scope registration not saved");
        context.Attach(new test_host::Context); context->unknown_scope = true;
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); type(keys.Get(), context.Get(), "X");
        check(host->ui && host->ui.As(&list) == E_NOINTERFACE, "unknown-scope field did not open deletion prompt");
        type(keys.Get(), context.Get(), "yes"); key(keys.Get(), context.Get(), VK_RETURN);
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        until([&] { return read_user_dictionary(file).revision() == 8; });
        check(read_user_dictionary(file).lookup({U"かんじ"}).empty(), "unknown-scope deletion not saved");
        for (const auto status : {E_FAIL, S_FALSE}) { // Failed or mixed scope values are also not a protected field.
            context.Attach(new test_host::Context); context->scope_value_status = status;
            type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_RETURN);
        }
        {
            context.Attach(new test_host::Context); context->unknown_scope = true;
            const auto origin = test_host::transitory_context(context.Get());
            type(keys.Get(), origin.Get(), "Kanji"); key(keys.Get(), origin.Get(), VK_SPACE); key(keys.Get(), origin.Get(), VK_RETURN);
            check(context->composition->ended && origin->doc->text.empty(), "unknown parent learning did not edit the parent");
        }
        until([&] { return read_user_dictionary(file).revision() == 11; });
        context.Attach(new test_host::Context);
        type(keys.Get(), context.Get(), "/99"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"９９", "numeric display leaked a template");
        context->doc->reject_write = true;
        check(!key(keys.Get(), context.Get(), '3'), "failed numeric conversion consumed selection");
        Sleep(300); check(read_user_dictionary(file).revision() == 11, "failed numeric conversion learned");
        context->doc->reject_write = false; check(key(keys.Get(), context.Get(), '3'), "numeric conversion retry failed");
        check(context->doc->text == L"九九", "numeric candidate did not commit expanded text");
        until([&] { return read_user_dictionary(file).revision() == 12; });
        check(read_user_dictionary(file).lookup({U"#"})[0].text == U"#2" && read_user_dictionary(file).lookup({U"99"}).empty(),
            "numeric TSF learning saved a concrete number");
        context.Attach(new test_host::Context); type(keys.Get(), context.Get(), "/98"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"九八", "numeric preference did not transfer to another number");
        type(keys.Get(), context.Get(), "Xyes"); context->doc->reject_write = true;
        check(!key(keys.Get(), context.Get(), VK_RETURN), "failed numeric delete consumed Enter");
        Sleep(300); check(read_user_dictionary(file).revision() == 12, "failed numeric delete persisted");
        context->doc->reject_write = false; check(key(keys.Get(), context.Get(), VK_RETURN), "numeric delete retry failed");
        check(context->doc->text == L"98", "numeric delete did not restore original reading");
        key(keys.Get(), context.Get(), VK_SPACE); check(context->doc->text == L"９８", "numeric delete retained learned format");
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        until([&] { return read_user_dictionary(file).revision() == 13; });
        check(read_user_dictionary(file).lookup({U"#"}).empty(), "numeric template was not deleted");
        context.Attach(new test_host::Context); context->input_scope = IS_PASSWORD;
        type(keys.Get(), context.Get(), "/97"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), '3');
        Sleep(300); check(read_user_dictionary(file).revision() == 13, "private numeric selection learned");
        context.Attach(new test_host::Context); type(keys.Get(), context.Get(), "Q99en"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"九十九円", "Q numeric kana input failed in TSF");
        key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        service->Deactivate(); keys.Reset(); service.Reset(); factory.Reset(); host.Reset(); context.Reset();
        std::cout << "PASS cached TSF learning, transactional registration, immediate reuse, recursive cancel, private and unknown scopes, focus/UI failure\n";
        result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    if (library && !result) FreeLibrary(library); // Worker owns a separate reference until its callback returns.
    SetKeyboardState(previous); CoUninitialize();
    Sleep(300); stop_dictionary_helper(file); // Release the helper's lock before removing owned test files.
    std::error_code ignored; std::filesystem::remove_all(root, ignored);
    return result;
}
