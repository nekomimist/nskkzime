#include "registration_registry.h"
#include "module.h"
#include <algorithm>
#include <array>
#include <vector>

namespace nskkzime::windows::registration {
namespace {
struct KeyHandle {
    HKEY key = nullptr;
    ~KeyHandle() { if (key) RegCloseKey(key); }
    void close() noexcept { if (key) RegCloseKey(key); key = nullptr; }
};
std::wstring class_path() {
    wchar_t guid[39]{};
    StringFromGUID2(service_clsid, guid, 39);
    return std::wstring(L"Software\\Classes\\CLSID\\") + guid;
}
const std::array<const GUID*, 4> categories{
    &GUID_TFCAT_TIP_KEYBOARD, &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT};
LSTATUS set_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}
HRESULT check_owner(HKEY key, const std::wstring& expected) {
    KeyHandle inproc;
    auto error = RegOpenKeyExW(key, L"InprocServer32", 0, KEY_QUERY_VALUE, &inproc.key);
    if (error == ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    DWORD type = 0, size = 0;
    error = RegQueryValueExW(inproc.key, nullptr, nullptr, &type, nullptr, &size);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    if (type != REG_SZ || size < 2 * sizeof(wchar_t) || size > 32768 * sizeof(wchar_t) || size % sizeof(wchar_t))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::vector<wchar_t> value(size / sizeof(wchar_t));
    const auto original_size = size;
    error = RegQueryValueExW(inproc.key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size);
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    // RegQueryValueEx does not guarantee a terminator. Reject malformed values,
    // including embedded NULs and changes between the two queries.
    if (type != REG_SZ || size != original_size || value.back() != L'\0' ||
        std::find(value.begin(), value.end() - 1, L'\0') != value.end() - 1)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (CompareStringOrdinal(value.data(), static_cast<int>(value.size() - 1), expected.c_str(),
                             static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL)
        return HRESULT_FROM_WIN32(ERROR_NOT_OWNER);
    return S_OK;
}
HRESULT delete_class(HKEY root, const std::wstring& path) noexcept {
    const auto error = RegDeleteTreeW(root, path.c_str());
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
}
}

HRESULT register_in(HKEY root, const std::wstring& dll, TsfRegistration& tsf) noexcept {
    try {
        // Allocate strings before creating the key so an allocation failure
        // cannot leave a registration with no owner path.
        const auto path = class_path();
        const std::wstring description = L"nskkzime development prototype";
        const std::wstring threading = L"Apartment";
        if (dll.empty() || dll.size() >= 32768 || dll.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
        KeyHandle key;
        DWORD disposition = 0;
        auto error = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE,
                                    nullptr, &key.key, &disposition);
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        if (disposition != REG_CREATED_NEW_KEY) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        error = set_string(key.key, nullptr, description);
        KeyHandle inproc;
        if (!error) error = RegCreateKeyExW(key.key, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc.key, nullptr);
        if (!error) error = set_string(inproc.key, nullptr, dll);
        if (!error) error = set_string(inproc.key, L"ThreadingModel", threading);
        inproc.close();
        HRESULT hr = HRESULT_FROM_WIN32(error);
        bool processor_attempted = false;
        size_t categories_attempted = 0;
        if (SUCCEEDED(hr)) hr = tsf.initialize();
        if (SUCCEEDED(hr)) { processor_attempted = true; hr = tsf.register_processor(); }
        if (SUCCEEDED(hr)) hr = tsf.add_profile(dll);
        for (const auto category : categories) {
            if (FAILED(hr)) break;
            ++categories_attempted;
            hr = tsf.register_category(*category);
        }
        if (SUCCEEDED(hr)) hr = tsf.enable_profile();
        if (FAILED(hr)) {
            // A failed call may have made a partial change. Undo attempted
            // steps too, but never touch categories we have not reached.
            bool cleaned = true;
            while (categories_attempted) {
                if (FAILED(tsf.unregister_category(*categories[--categories_attempted]))) cleaned = false;
            }
            if (processor_attempted && FAILED(tsf.unregister_processor())) cleaned = false;
            key.close();
            // If TSF cleanup failed, retain the owner path so the same DLL can
            // retry unregistering. Do not report a successful installation.
            if (cleaned) (void)delete_class(root, path);
        }
        return hr;
    } catch (...) { return E_OUTOFMEMORY; }
}

HRESULT unregister_in(HKEY root, const std::wstring& dll, TsfRegistration& tsf) noexcept {
    try {
        if (dll.empty() || dll.size() >= 32768 || dll.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
        const auto path = class_path();
        KeyHandle key;
        const auto error = RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WRITE, &key.key);
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_FALSE;
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        auto hr = check_owner(key.key, dll);
        if (FAILED(hr)) return hr;
        hr = tsf.initialize();
        if (FAILED(hr)) return hr;
        // Preserve the COM owner on any failure, allowing the caller to retry.
        // Removing a category may already have succeeded; retries are safe.
        for (const auto category : categories) {
            hr = tsf.unregister_category(*category);
            if (FAILED(hr)) return hr;
        }
        hr = tsf.unregister_processor();
        if (FAILED(hr)) return hr;
        hr = check_owner(key.key, dll);
        if (FAILED(hr)) return hr;
        key.close();
        return delete_class(root, path);
    } catch (...) { return E_OUTOFMEMORY; }
}
}
