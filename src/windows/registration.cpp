#include "module.h"
#include "registration_registry.h"
#include <wrl/client.h>
#include <string>
#include <vector>

namespace nskkzime::windows {
namespace {
using Microsoft::WRL::ComPtr;
HRESULT contains_guid(IEnumGUID* items, REFGUID wanted) noexcept {
    if (!items) return E_UNEXPECTED;
    for (;;) {
        GUID id{};
        ULONG fetched = 0;
        const auto hr = items->Next(1, &id, &fetched);
        if (FAILED(hr)) return hr;
        if (!fetched) return hr == S_FALSE ? S_FALSE : E_UNEXPECTED;
        if (id == wanted) return S_OK;
    }
}
struct ComScope {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
};
struct RegistrationLock {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\nskkzime-registration-{6EF45A57-E12E-49F1-8A4E-2569D8361A50}");
    HRESULT status = E_FAIL;
    bool acquired = false;
    RegistrationLock() noexcept {
        if (!mutex) { status = HRESULT_FROM_WIN32(GetLastError()); return; }
        const auto result = WaitForSingleObject(mutex, 0);
        acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        status = acquired ? S_OK : HRESULT_FROM_WIN32(result == WAIT_TIMEOUT ? ERROR_BUSY : GetLastError());
    }
    ~RegistrationLock() { if (acquired) ReleaseMutex(mutex); if (mutex) CloseHandle(mutex); }
};
class SystemTsfRegistration final : public registration::TsfRegistration {
    ComPtr<ITfInputProcessorProfiles> profiles;
    ComPtr<ITfCategoryMgr> categories;
public:
    HRESULT initialize() noexcept override {
        auto hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles));
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories));
        return hr;
    }
    HRESULT register_processor() noexcept override { return profiles->Register(service_clsid); }
    HRESULT add_profile(const std::wstring& dll) noexcept override {
        constexpr wchar_t description[] = L"nskkzime (development)";
        return profiles->AddLanguageProfile(service_clsid, language_id, profile_guid, description,
            ARRAYSIZE(description) - 1, dll.c_str(), static_cast<ULONG>(dll.size()), 0);
    }
    HRESULT enable_profile() noexcept override {
        return profiles->EnableLanguageProfile(service_clsid, language_id, profile_guid, TRUE);
    }
    HRESULT register_category(REFGUID category) noexcept override {
        return categories->RegisterCategory(service_clsid, category, service_clsid);
    }
    HRESULT unregister_category(REFGUID category) noexcept override {
        ComPtr<IEnumGUID> items;
        auto hr = categories->EnumCategoriesInItem(service_clsid, &items);
        if (FAILED(hr)) return hr;
        hr = contains_guid(items.Get(), category);
        if (hr != S_OK) return hr;
        return categories->UnregisterCategory(service_clsid, category, service_clsid);
    }
    HRESULT unregister_processor() noexcept override {
        ComPtr<IEnumGUID> items;
        auto hr = profiles->EnumInputProcessorInfo(&items);
        if (FAILED(hr)) return hr;
        hr = contains_guid(items.Get(), service_clsid);
        return hr == S_OK ? profiles->Unregister(service_clsid) : hr;
    }
};
HRESULT change_registration(bool remove) noexcept {
    try {
        RegistrationLock lock;
        if (FAILED(lock.status)) return lock.status;
        ComScope com;
        if (FAILED(com.status) && com.status != RPC_E_CHANGED_MODE) return com.status;
        std::vector<wchar_t> filename(32768);
        const auto length = GetModuleFileNameW(module_instance, filename.data(), static_cast<DWORD>(filename.size()));
        if (!length) return HRESULT_FROM_WIN32(GetLastError());
        if (length == filename.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        const std::wstring path(filename.data(), length);
        SystemTsfRegistration tsf;
        return remove ? registration::unregister_in(HKEY_LOCAL_MACHINE, path, tsf)
                      : registration::register_in(HKEY_LOCAL_MACHINE, path, tsf);
    } catch (...) { return E_OUTOFMEMORY; }
}
}
HRESULT register_server() noexcept { return change_registration(false); }
HRESULT unregister_server() noexcept { return change_registration(true); }
}
