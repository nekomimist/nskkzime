#include "module.h"
#include <wrl/client.h>
#include <string>
#include <vector>

namespace nskkzime::windows {
namespace {
using Microsoft::WRL::ComPtr;
struct ComScope {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
};
struct KeyHandle {
    HKEY key = nullptr;
    ~KeyHandle() { if (key) RegCloseKey(key); }
};
std::wstring class_path() {
    wchar_t guid[39]{};
    StringFromGUID2(service_clsid, guid, 39);
    return std::wstring(L"Software\\Classes\\CLSID\\") + guid;
}
LSTATUS set_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}
}
HRESULT register_server() noexcept {
    try {
        ComScope com;
        if (FAILED(com.status) && com.status != RPC_E_CHANGED_MODE) return com.status;
        std::vector<wchar_t> filename(32768);
        const auto length = GetModuleFileNameW(module_instance, filename.data(), static_cast<DWORD>(filename.size()));
        if (!length) return HRESULT_FROM_WIN32(GetLastError());
        if (length == filename.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        const std::wstring path(filename.data(), length);
        const auto registry_path = class_path();
        KeyHandle key;
        DWORD disposition = 0;
        auto error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, registry_path.c_str(), 0, nullptr, 0,
                                    KEY_WRITE, nullptr, &key.key, &disposition);
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        // Development registration: never overwrite another build's registration.
        if (disposition != REG_CREATED_NEW_KEY) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        error = set_string(key.key, nullptr, L"nskkzime development prototype");
        KeyHandle inproc;
        if (!error) error = RegCreateKeyExW(key.key, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc.key, nullptr);
        if (!error) error = set_string(inproc.key, nullptr, path);
        if (!error) error = set_string(inproc.key, L"ThreadingModel", L"Apartment");
        HRESULT hr = HRESULT_FROM_WIN32(error);
        ComPtr<ITfInputProcessorProfiles> profiles;
        ComPtr<ITfCategoryMgr> categories;
        bool registered = false;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles));
        if (SUCCEEDED(hr)) { hr = profiles->Register(service_clsid); registered = SUCCEEDED(hr); }
        constexpr wchar_t description[] = L"nskkzime (development)";
        if (SUCCEEDED(hr)) hr = profiles->AddLanguageProfile(service_clsid, language_id, profile_guid,
            description, ARRAYSIZE(description) - 1, path.c_str(), static_cast<ULONG>(path.size()), 0);
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories));
        if (SUCCEEDED(hr)) hr = categories->RegisterCategory(service_clsid, GUID_TFCAT_TIP_KEYBOARD, service_clsid);
        if (SUCCEEDED(hr)) hr = categories->RegisterCategory(service_clsid, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, service_clsid);
        if (SUCCEEDED(hr)) hr = profiles->EnableLanguageProfile(service_clsid, language_id, profile_guid, TRUE);
        if (FAILED(hr)) {
            if (categories) categories->UnregisterCategory(service_clsid, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, service_clsid);
            if (categories) categories->UnregisterCategory(service_clsid, GUID_TFCAT_TIP_KEYBOARD, service_clsid);
            if (registered) profiles->Unregister(service_clsid);
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, registry_path.c_str());
        }
        return hr;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT unregister_server() noexcept {
    try {
        ComScope com;
        if (FAILED(com.status) && com.status != RPC_E_CHANGED_MODE) return com.status;
        ComPtr<ITfCategoryMgr> categories;
        auto hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&categories));
        if (FAILED(hr)) return hr;
        categories->UnregisterCategory(service_clsid, GUID_TFCAT_TIP_KEYBOARD, service_clsid);
        categories->UnregisterCategory(service_clsid, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, service_clsid);
        ComPtr<ITfInputProcessorProfiles> profiles;
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles));
        if (FAILED(hr)) return hr;
        hr = profiles->Unregister(service_clsid);
        if (FAILED(hr)) return hr;
        const auto error = RegDeleteTreeW(HKEY_LOCAL_MACHINE, class_path().c_str());
        return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    } catch (...) { return E_OUTOFMEMORY; }
}
} // namespace nskkzime::windows
