#include "module.h"
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
bool key(ITfKeyEventSink* sink, ITfContext* context, WPARAM code, bool shift = false) {
    BYTE state[256]{};
    if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
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
void adapter_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
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
    check(context->doc->text == L"▽かんじ", "reading was not composed");
    key(keys, context.Get(), VK_SPACE);
    check(context->doc->text == L"▼漢字", "candidate not displayed");
    type(keys, context.Get(), "desu");
    check(context->doc->text == L"漢字です", "committed prefix lost while continuing input");
    check(context->composition->ended, "direct input left a composition open");
    type(keys, context.Get(), "KaKu");
    check(context->doc->text == L"漢字です▼書く", "okuri composition incorrect");
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
    check(context->doc->text == L"漢字です描くあ▼漢字", "failed SetText destroyed the old state");
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
    check(next->doc->text == L"▼書く", "old cleanup modified the new field");
    check(context->doc->text == L"漢字です描くあ漢字dかんじ", "old composition retained a marker");
    next->composition->terminate();
    check(next->doc->text == L"書く", "host termination did not remove markers");
    type(keys, next.Get(), "a");
    check(next->doc->text == L"書くあ", "terminated composition reused old state");
    next->readonly = true;
    check(!key(keys, next.Get(), 'A'), "read-only context consumed input");
    succeeded(service->Deactivate(), "deactivation failed");
    context->pump(); next->pump();
    check(!host->keys && !host->events, "adapter test leaked a sink");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
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
            adapter_tests(service.Get(), keys.Get());
        }
        check(can_unload() == S_OK, "COM object or subscription leaked");
        std::cout << "PASS COM exports/identity/unload, real TSF manager startup, test-host activation/composition/cancel/failure/focus isolation\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; result = 1; }
    // Unload only after the tested COM objects and sinks have been released.
    FreeLibrary(library);
    CoUninitialize();
    return result;
}
