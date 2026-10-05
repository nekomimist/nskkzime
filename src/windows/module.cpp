#include "module.h"
#include "runtime_dictionary.h"
#include "context_text.h"
#include "inference_client.h"
#include "romaji_settings.h"
#include <new>

namespace nskkzime::windows {
HINSTANCE module_instance = nullptr;
std::atomic<long> module_objects{0};
std::atomic<long> server_locks{0};
namespace {
class Factory final : public IClassFactory {
public:
    Factory() { ++module_objects; }
    ~Factory() { --module_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *output = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return create_text_service(iid, output);
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++server_locks;
        else {
            auto count = server_locks.load();
            do { if (count == 0) return E_UNEXPECTED; } while (!server_locks.compare_exchange_weak(count, count - 1));
        }
        return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1};
};
}
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        nskkzime::windows::module_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    using namespace nskkzime::windows;
    return module_objects == 0 && server_locks == 0 ? S_OK : S_FALSE;
}
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != nskkzime::windows::service_clsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) nskkzime::windows::Factory;
    if (!factory) return E_OUTOFMEMORY;
    const auto hr = factory->QueryInterface(iid, output); factory->Release(); return hr;
}
extern "C" HRESULT __stdcall DllRegisterServer() { return nskkzime::windows::register_server(); }
extern "C" HRESULT __stdcall DllUnregisterServer() { return nskkzime::windows::unregister_server(); }
extern "C" HRESULT __stdcall NskkzimeEnableContextPreview(BOOL enable) {
    nskkzime::windows::enable_context_preview(enable != FALSE); return S_OK;
}
extern "C" HRESULT __stdcall NskkzimeGetInferenceStatus(BSTR* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    try { const auto text = nskkzime::windows::inference_status(); *output = SysAllocStringLen(text.data(),static_cast<UINT>(text.size())); return *output ? S_OK : E_OUTOFMEMORY; }
    catch (...) { return E_OUTOFMEMORY; }
}
extern "C" HRESULT __stdcall NskkzimeGetContextPreview(BSTR* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    try {
        const auto text = nskkzime::windows::context_preview_status();
        *output = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        return *output ? S_OK : E_OUTOFMEMORY;
    } catch (...) { return E_OUTOFMEMORY; }
}
extern "C" HRESULT __stdcall NskkzimeGetDictionaryStatus(BSTR* output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    try {
        const auto status = nskkzime::windows::runtime_dictionary_status() + L"\r\n" + nskkzime::windows::runtime_romaji_status();
        *output = SysAllocStringLen(status.data(), static_cast<UINT>(status.size()));
        return *output ? S_OK : E_OUTOFMEMORY;
    } catch (...) { return E_OUTOFMEMORY; }
}
