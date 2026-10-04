#include "module.h"
#include "learning_ipc.h"
#include "tsf_test_host.h"
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
    std::filesystem::create_directory(root); const auto file = root / L"user.nskkdb";
    SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", argv[2]);
    SetEnvironmentVariableW(L"NSKKZIME_LEARNING", L"1"); SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", file.c_str());
    BYTE previous[256]{}; GetKeyboardState(previous);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto library = LoadLibraryW(argv[1]); int result = 1;
    try {
        check(library != nullptr, "DLL load failed");
        using Factory = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto factory_export = reinterpret_cast<Factory>(GetProcAddress(library, "DllGetClassObject"));
#pragma warning(pop)
        ComPtr<IClassFactory> factory; check(factory_export && SUCCEEDED(factory_export(service_clsid, IID_PPV_ARGS(&factory))), "factory failed");
        ComPtr<ITfTextInputProcessorEx> service; check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service))), "service failed");
        ComPtr<ITfKeyEventSink> keys; service.As(&keys);
        ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, 0)), "activation failed");
        ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
        context->scope_requires_text_range = true; // Inspect the whole conversion, not a caret outside it.
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_SPACE);
        context->doc->reject_write = true;
        check(!key(keys.Get(), context.Get(), VK_RETURN), "failed edit consumed key");
        context->doc->reject_write = false;
        check(key(keys.Get(), context.Get(), VK_RETURN), "retry failed");
        until([&] { return read_user_dictionary(file).revision() == 1; });
        check(read_user_dictionary(file).lookup({U"かんじ"})[0].text == U"感じ", "wrong confirmed candidate saved");
        // Each successful commit below must preserve the one permitted event.
        for (const auto scope : {IS_PASSWORD, IS_PRIVATE, IS_NUMERIC_PASSWORD, IS_NUMERIC_PIN, IS_ALPHANUMERIC_PIN, IS_ALPHANUMERIC_PIN_SET}) {
            context.Attach(new test_host::Context); context->input_scope = scope;
            type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_RETURN);
        }
        context.Attach(new test_host::Context); context->unknown_scope = true;
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_RETURN);
        for (const auto status : {E_FAIL, S_FALSE}) { // Failure, absent scope, or mixed values must never permit persistence.
            context.Attach(new test_host::Context); context->scope_value_status = status;
            type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_RETURN);
        }
        context.Attach(new test_host::Context);
        type(keys.Get(), context.Get(), "Kanji"); key(keys.Get(), context.Get(), VK_SPACE); key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        Sleep(1200);
        check(read_user_dictionary(file).revision() == 1, "failed/private/unknown/canceled edit leaked learning");
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
        context->doc->reject_write = true;
        check(!key(keys.Get(), context.Get(), VK_RETURN), "failed registration consumed Enter");
        context->doc->reject_write = false;
        check(key(keys.Get(), context.Get(), VK_RETURN), "registration retry failed");
        check(context->doc->text == L"日本", "registered text not applied");
        // Reuse immediately, before waiting for IPC or a snapshot refresh.
        type(keys.Get(), context.Get(), "Michi"); key(keys.Get(), context.Get(), VK_SPACE);
        check(context->doc->text == L"日本日本" && host->ui && SUCCEEDED(host->ui.As(&list)), "new registration not immediately available");
        list.Reset(); key(keys.Get(), context.Get(), VK_ESCAPE); key(keys.Get(), context.Get(), VK_ESCAPE);
        until([&] { return read_user_dictionary(file).revision() == 2; });
        check(read_user_dictionary(file).lookup({U"みち"})[0].text == U"日本", "registered word not saved");

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
        for (const bool unknown : {false, true}) {
            context.Attach(new test_host::Context); context->input_scope = IS_PRIVATE; context->unknown_scope = unknown;
            type(keys.Get(), context.Get(), "/secret"); key(keys.Get(), context.Get(), VK_SPACE);
            check(context->doc->text == L"secret" && !host->ui, "private/unknown field started registration");
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
        service->Deactivate(); keys.Reset(); service.Reset(); factory.Reset(); host.Reset(); context.Reset();
        std::cout << "PASS TSF learning, transactional registration, immediate reuse, recursive cancel, private scopes, focus/UI failure\n";
        result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    if (library && !result) FreeLibrary(library); // Worker owns a separate reference until its callback returns.
    SetKeyboardState(previous); CoUninitialize();
    Sleep(12000); // Allow worker drain and helper idle exit before removing owned test files.
    for (const auto& path : {file, std::filesystem::path(file.native() + L".lock")}) { std::error_code ignored; std::filesystem::remove(path, ignored); }
    std::error_code ignored; std::filesystem::remove(root, ignored);
    return result;
}
