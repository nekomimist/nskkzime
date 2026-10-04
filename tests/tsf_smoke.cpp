#include "module.h"
#include "display_attributes.h"
#include "tsf_test_host.h"
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::cerr << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << '\n'; throw std::runtime_error(message); }
}
struct KeyboardState {
    BYTE saved[256]{};
    KeyboardState() { GetKeyboardState(saved); }
    ~KeyboardState() { SetKeyboardState(saved); }
};
bool key(ITfKeyEventSink* sink, ITfContext* context, WPARAM code, bool shift = false, bool control = false) {
    BYTE state[256]{};
    if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (control) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    SetKeyboardState(state); // Thread-local state only; no SendInput/global keystrokes.
    const auto parameter = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
    BOOL probe = FALSE;
    succeeded(sink->OnTestKeyDown(context, code, parameter, &probe), "key probe failed");
    BOOL eaten = FALSE;
    succeeded(sink->OnKeyDown(context, code, parameter, &eaten), "key handling failed");
    BOOL up = FALSE;
    succeeded(sink->OnTestKeyUp(context, code, parameter, &up), "key-up probe failed");
    check(up == eaten, "key-up disagrees with key-down");
    succeeded(sink->OnKeyUp(context, code, parameter, &up), "key-up failed");
    return eaten != FALSE;
}
void type(ITfKeyEventSink* sink, ITfContext* context, std::string_view text) {
    for (const char ch : text) {
        const auto translated = VkKeyScanExW(static_cast<wchar_t>(ch), GetKeyboardLayout(0));
        check(translated != -1 && !(HIBYTE(translated) & 6), "test character unavailable on this keyboard layout");
        check(key(sink, context, LOBYTE(translated), (HIBYTE(translated) & 1) != 0), "character not consumed");
    }
}
void adapter_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys, bool file_dictionary) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    host->reject_sink = true;
    check(FAILED(service->ActivateEx(host.Get(), 1, 0)), "failed sink subscription accepted");
    check(!host->keys && !host->events, "partial activation leaked a sink");
    host->reject_sink = false;
    for (int i = 0; i < 3; ++i) {
        succeeded(service->ActivateEx(host.Get(), 1, 0), "test-host activate failed");
        check(service->ActivateEx(host.Get(), 1, 0) == E_UNEXPECTED, "double activation accepted");
        succeeded(service->Deactivate(), "test-host deactivate failed");
        check(!host->keys && !host->events, "deactivation leaked a sink");
    }
    succeeded(service->ActivateEx(host.Get(), 1, 0), "test-host activate failed");
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    type(keys, context.Get(), "Kanji");
    check(context->doc->text == L"かんじ", "reading was not composed");
    key(keys, context.Get(), VK_SPACE);
    check(context->doc->text == L"漢字", "candidate not displayed");
    type(keys, context.Get(), "desu");
    check(context->doc->text == L"漢字です", "committed prefix lost while continuing input");
    check(context->composition->ended, "direct input left a composition open");
    type(keys, context.Get(), "KaKu");
    check(context->doc->text == L"漢字です書く", "okuri composition incorrect");
    key(keys, context.Get(), VK_SPACE);
    key(keys, context.Get(), VK_RETURN);
    check(context->doc->text == L"漢字です描く", "okuri commit incorrect");
    type(keys, context.Get(), "Kanji");
    key(keys, context.Get(), VK_SPACE);
    key(keys, context.Get(), VK_ESCAPE);
    key(keys, context.Get(), VK_ESCAPE);
    check(context->doc->text == L"漢字です描く", "cancel changed committed prefix");
    context->reject_session = true;
    check(!key(keys, context.Get(), 'K', true), "refused edit ate a key");
    context->reject_session = false;
    context->reject_composition = true;
    check(!key(keys, context.Get(), 'K', true), "null composition was accepted");
    context->reject_composition = false;
    type(keys, context.Get(), "a");
    check(context->doc->text == L"漢字です描くあ", "rejected key mutated the engine");
    type(keys, context.Get(), "Kanji");
    context->doc->reject_write = true;
    check(!key(keys, context.Get(), VK_SPACE), "failed SetText ate a key");
    context->doc->reject_write = false;
    key(keys, context.Get(), VK_SPACE);
    check(context->doc->text == L"漢字です描くあ漢字", "failed SetText destroyed the old state");
    context->doc->reject_shift = true;
    type(keys, context.Get(), "d");
    check(context->doc->text == L"漢字です描くあ漢字d", "range failure lost committed prefix or left markers");
    context->doc->reject_shift = false;
    // Delayed cleanup for the previous field must not modify the new field.
    type(keys, context.Get(), "Kanji");
    keys->OnSetFocus(FALSE);
    check(context->queued.size() == 1, "focus cleanup was not queued");
    ComPtr<test_host::Context> next; next.Attach(new test_host::Context);
    type(keys, next.Get(), "KaKu");
    context->pump();
    check(next->doc->text == L"書く", "old cleanup modified the new field");
    check(context->doc->text == L"漢字です描くあ漢字dかんじ", "old composition retained a marker");
    next->composition->terminate();
    check(next->doc->text == L"書く", "host termination did not remove markers");
    type(keys, next.Get(), "a");
    check(next->doc->text == L"書くあ", "terminated composition reused old state");
    next->readonly = true;
    check(!key(keys, next.Get(), 'A'), "read-only context consumed input");
    if (file_dictionary) {
        ComPtr<test_host::Context> dictionary_context; dictionary_context.Attach(new test_host::Context);
        type(keys, dictionary_context.Get(), "Jisho");
        key(keys, dictionary_context.Get(), VK_SPACE);
        key(keys, dictionary_context.Get(), VK_RETURN);
        check(dictionary_context->doc->text == L"辞書", "TSF did not use the configured dictionary file");
    }
    succeeded(service->Deactivate(), "deactivation failed");
    context->pump(); next->pump();
    check(!host->keys && !host->events, "adapter test leaked a sink");
}
void selection_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    succeeded(service->ActivateEx(host.Get(), 1, 0), "selection-test activate failed");
    auto fresh = [] { ComPtr<test_host::Context> value; value.Attach(new test_host::Context); return value; };
    {
        auto context = fresh(); context->doc->text = L"前後"; context->doc->caret = 1;
        type(keys, context.Get(), "Kanji");
        context->move_selection(0, 0);
        check(context->queued.size() == 1 && !context->edits, "selection change did not detach and schedule completion");
        type(keys, context.Get(), "a"); // A key can arrive before the queued completion.
        context->pump();
        check(context->doc->text == L"あ前かんじ後" && context->doc->caret == 1, "delayed completion changed text or moved the new caret");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->move_selection(1, 2); context->pump();
        type(keys, context.Get(), "a");
        check(context->doc->text == L"かあじ", "new input ignored the user's range selection");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        auto old = context->composition;
        context->move_selection(0, 0);
        type(keys, context.Get(), "KaKu");
        check(old->ended && !context->composition->ended, "new composition raced with pending completion in the same field");
        context->pump();
        check(context->doc->text == L"書くかんじ" && !context->composition->ended, "queued old completion ended new composition");
        key(keys, context.Get(), VK_RETURN);
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->host_replace(1, 2, L"改");
        check(!key(keys, context.Get(), VK_SPACE), "external edit retained the old conversion state");
        context->pump();
        check(context->doc->text == L"か改じ", "completion reverted an application's edit");
        type(keys, context.Get(), "a");
        check(context->doc->text == L"か改じあ", "input after external edit reused stale reading");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->doc->caret = context->doc->selection_end = 0; // Missing/late notification.
        type(keys, context.Get(), "a");
        check(context->doc->text == L"あかんじ" && context->composition->ended, "write-time selection validation failed");
    }
    {
        auto context = fresh(); context->doc->text = L"前"; context->doc->caret = 1;
        type(keys, context.Get(), "Kanji");
        context->host_replace(0, 1, L"接頭");
        check(context->queued.empty(), "edit outside the composition cancelled input");
        key(keys, context.Get(), VK_SPACE); key(keys, context.Get(), VK_RETURN);
        check(context->doc->text == L"接頭漢字", "range tracking lost the composition after a prefix edit");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        check(!key(keys, context.Get(), VK_LEFT), "navigation key was consumed");
        check(context->composition->ended && !context->edits && context->doc->text == L"かんじ", "navigation did not finish before passing through");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        check(!key(keys, context.Get(), 'V', false, true), "paste shortcut was consumed");
        check(context->composition->ended, "paste shortcut retained a live composition");
        context->host_replace(0, 3, L"貼り付け");
        type(keys, context.Get(), "a");
        check(context->doc->text == L"貼り付けあ", "input after paste restored stale text");
    }
    {
        auto context = fresh(); context->reject_edit_sink = true;
        check(!key(keys, context.Get(), 'K', true), "unmonitored composition consumed input");
        check(context->doc->text.empty() && !context->composition, "failed edit sink changed the document");
        context->reject_edit_sink = false; type(keys, context.Get(), "a");
        check(context->doc->text == L"あ", "failed edit sink changed the engine");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->doc->replace(0, 3, L"置換済み");
        context->composition->terminate();
        check(context->doc->text == L"置換済み" && !context->edits, "host termination overwrote the current document");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->doc->reject_read = true; type(keys, context.Get(), "a");
        context->doc->reject_read = false;
        check(context->doc->text == L"かんじあ" && context->composition->ended, "failed validation read overwrote unverified text");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "K" + std::string(300, 'a'));
        check(context->queued.empty() && context->edits, "long composition failed chunked content validation");
        key(keys, context.Get(), VK_RETURN);
        check(context->doc->text.size() == 300 && context->composition->ended, "long composition did not commit");
    }
    succeeded(service->Deactivate(), "selection-test deactivation failed");
    check(!host->keys && !host->events, "selection test leaked a sink");
}
void attribute_tests(ITfTextInputProcessorEx* service) {
    ComPtr<ITfDisplayAttributeProvider> provider;
    succeeded(service->QueryInterface(IID_PPV_ARGS(&provider)), "display attribute provider missing");
    ComPtr<IEnumTfDisplayAttributeInfo> items;
    succeeded(provider->EnumDisplayAttributeInfo(&items), "attribute enumeration failed");
    ComPtr<ITfDisplayAttributeInfo> reading;
    check(items->Next(1, &reading, nullptr) == S_OK, "reading attribute missing");
    TF_DISPLAYATTRIBUTE attribute{};
    succeeded(reading->GetAttributeInfo(&attribute), "reading attribute not readable");
    check(attribute.bAttr == TF_ATTR_INPUT && attribute.lsStyle == TF_LS_DOT, "reading underline incorrect");
    ComPtr<IEnumTfDisplayAttributeInfo> copy;
    succeeded(items->Clone(&copy), "attribute enumeration clone failed");
    ComPtr<ITfDisplayAttributeInfo> converted;
    check(copy->Next(1, &converted, nullptr) == S_OK, "cloned enumeration lost its position");
    succeeded(converted->GetAttributeInfo(&attribute), "converted attribute not readable");
    check(attribute.bAttr == TF_ATTR_TARGET_CONVERTED && attribute.fBoldLine, "candidate underline incorrect");
    GUID id{}; succeeded(converted->GetGUID(&id), "attribute GUID missing");
    check(id == nskkzime::windows::converted_attribute_guid, "wrong stable attribute GUID");
    ComPtr<ITfDisplayAttributeInfo> by_guid;
    succeeded(provider->GetDisplayAttributeInfo(id, &by_guid), "attribute lookup failed");
    attribute.lsStyle = TF_LS_NONE;
    succeeded(by_guid->SetAttributeInfo(&attribute), "attribute customization failed");
    succeeded(by_guid->Reset(), "attribute reset failed");
    succeeded(by_guid->GetAttributeInfo(&attribute), "reset attribute not readable");
    check(attribute.lsStyle == TF_LS_SOLID, "reset did not restore defaults");
    ComPtr<ITfDisplayAttributeInfo> end;
    check(copy->Next(1, &end, nullptr) == S_FALSE, "attribute enumeration did not end");
    check(provider->GetDisplayAttributeInfo(GUID_NULL, &end) == E_INVALIDARG, "unknown attribute GUID accepted");
    check(items->Next(2, &end, nullptr) == E_POINTER, "enumerator accepted missing count");
}
void missing_dictionary_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    succeeded(service->ActivateEx(host.Get(), 1, 0), "test-host activate failed");
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    type(keys, context.Get(), "Kanji");
    key(keys, context.Get(), VK_SPACE);
    check(context->doc->text == L"かんじ", "missing dictionary used demo candidates or lost the reading");
    key(keys, context.Get(), VK_RETURN);
    type(keys, context.Get(), "desu");
    check(context->doc->text == L"かんじです", "kana input failed without a dictionary");
    succeeded(service->Deactivate(), "deactivation failed");
    context->pump();
    check(!host->keys && !host->events, "missing-dictionary test leaked a sink");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4 || (argc == 4 && std::wstring_view(argv[3]) != L"--expect-empty")) return 2;
    // Test process only. Do not inherit a user's configured dictionary into fixtures.
    if (!SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", argc >= 3 ? argv[2] : nullptr)) return 2;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    const auto library = LoadLibraryW(argv[1]);
    if (!library) { CoUninitialize(); return 1; }
    int result = 0;
    try {
        using GetFactory = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        using CanUnload = HRESULT(WINAPI*)();
        // GetProcAddress is the documented Windows pattern for these COM exports.
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto get_factory = reinterpret_cast<GetFactory>(GetProcAddress(library, "DllGetClassObject"));
        const auto can_unload = reinterpret_cast<CanUnload>(GetProcAddress(library, "DllCanUnloadNow"));
#pragma warning(pop)
        check(get_factory && can_unload, "missing COM exports");
        check(GetProcAddress(library, "DllRegisterServer") && GetProcAddress(library, "DllUnregisterServer"), "missing registration exports");
        check(can_unload() == S_OK, "fresh DLL cannot unload");
        {
            ComPtr<IClassFactory> factory;
            check(get_factory(CLSID_NULL, IID_PPV_ARGS(&factory)) == CLASS_E_CLASSNOTAVAILABLE, "unknown CLSID accepted");
            succeeded(get_factory(nskkzime::windows::service_clsid, IID_PPV_ARGS(&factory)), "factory failed");
            check(can_unload() == S_FALSE, "factory not counted");
            succeeded(factory->LockServer(TRUE), "lock failed");
            succeeded(factory->LockServer(FALSE), "unlock failed");
            check(factory->LockServer(FALSE) == E_UNEXPECTED, "unbalanced unlock accepted");
            ComPtr<ITfTextInputProcessorEx> service;
            succeeded(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service)), "create service failed");
            check(service->ActivateEx(nullptr, TF_CLIENTID_NULL, 0) == E_INVALIDARG, "null manager accepted");
            ComPtr<IUnknown> identity;
            succeeded(service.As(&identity), "IUnknown missing");
            ComPtr<ITfKeyEventSink> keys;
            succeeded(service.As(&keys), "key sink missing");
            ComPtr<IUnknown> key_identity;
            succeeded(keys.As(&key_identity), "key sink identity missing");
            check(identity.Get() == key_identity.Get(), "COM identity differs across interfaces");
            BOOL eaten = TRUE;
            succeeded(keys->OnTestKeyDown(nullptr, 'A', 0, &eaten), "null-context key probe failed");
            check(!eaten, "null-context key consumed");
            check(keys->OnTestKeyDown(nullptr, 'A', 0, nullptr) == E_POINTER, "null output accepted");
            ComPtr<ITfThreadMgr> manager;
            succeeded(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager)), "real TSF manager unavailable");
            TfClientId client = TF_CLIENTID_NULL;
            succeeded(manager->Activate(&client), "TSF manager activate failed");
            succeeded(manager->Deactivate(), "TSF manager deactivate failed");
            attribute_tests(service.Get());
            if (argc == 4) missing_dictionary_tests(service.Get(), keys.Get());
            else { adapter_tests(service.Get(), keys.Get(), argc == 3); selection_tests(service.Get(), keys.Get()); }
        }
        check(can_unload() == S_OK, "COM object or subscription leaked");
        std::cout << "PASS COM exports/identity/unload, real TSF manager startup, "
                  << (argc == 4 ? "kana fallback without dictionary" : "test-host activation/composition/cancel/failure/focus isolation") << '\n';
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; result = 1; }
    // Unload only after the tested COM objects and sinks have been released.
    if (!result) FreeLibrary(library); // A failed assertion may leave a subscription alive until process exit.
    CoUninitialize();
    return result;
}
