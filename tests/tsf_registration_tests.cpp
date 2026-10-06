#include "registration_registry.h"
#include "module.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace nskkzime::windows;
using namespace nskkzime::windows::registration;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
constexpr wchar_t class_key[] = L"Software\\Classes\\CLSID\\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}";
const std::wstring dll = L"C:\\nskkzime test 日本語\\version-new\\nskkzime_tsf.dll";
const std::wstring other = L"C:\\nskkzime test 日本語\\version-old\\nskkzime_tsf.dll";
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
struct Fixture {
    std::wstring path;
    Key root;
    Fixture() {
        GUID id{};
        check(SUCCEEDED(CoCreateGuid(&id)), "guid");
        wchar_t guid[39]{};
        StringFromGUID2(id, guid, 39);
        path = std::wstring(L"Software\\nskkzime-registration-tests-") + guid;
        DWORD disposition = 0;
        check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
            KEY_ALL_ACCESS, nullptr, &root.value, &disposition) == ERROR_SUCCESS &&
            disposition == REG_CREATED_NEW_KEY, "private registry fixture");
    }
    ~Fixture() { RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    bool exists() const {
        Key key;
        return RegOpenKeyExW(root.value, class_key, 0, KEY_READ, &key.value) == ERROR_SUCCESS;
    }
    void clear() { RegDeleteTreeW(root.value, class_key); }
    void raw_owner(DWORD type, const void* value, DWORD size) {
        Key key;
        const auto path_inproc = std::wstring(class_key) + L"\\InprocServer32";
        check(RegCreateKeyExW(root.value, path_inproc.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                              &key.value, nullptr) == ERROR_SUCCESS, "write fixture owner");
        check(RegSetValueExW(key.value, nullptr, 0, type, static_cast<const BYTE*>(value), size) == ERROR_SUCCESS,
              "write fixture value");
    }
    void owner(const std::wstring& value) {
        raw_owner(REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    std::wstring read(const wchar_t* name = nullptr) const {
        Key key;
        const auto path_inproc = std::wstring(class_key) + L"\\InprocServer32";
        check(RegOpenKeyExW(root.value, path_inproc.c_str(), 0, KEY_READ, &key.value) == ERROR_SUCCESS, "read fixture");
        std::array<wchar_t, 32768> value{};
        DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t)), type = 0;
        check(RegQueryValueExW(key.value, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()),
                               &size) == ERROR_SUCCESS && type == REG_SZ, "read owner");
        return value.data();
    }
};
const std::array<const GUID*, 4> ids{
    &GUID_TFCAT_TIP_KEYBOARD, &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT};
struct FakeTsf : TsfRegistration {
    std::array<bool, 4> categories{};
    bool processor = false, profile = false, enabled = false;
    int fail_at = -1, cleanup_fail_at = -1;
    std::vector<int> calls;
    std::wstring profile_dll;
    Fixture* replace_owner_on_remove = nullptr;
    HRESULT result(int step) noexcept {
        calls.push_back(step);
        return step == fail_at || step == cleanup_fail_at ? E_ACCESSDENIED : S_OK;
    }
    HRESULT initialize() noexcept override { return result(0); }
    HRESULT register_processor() noexcept override { processor = true; return result(1); }
    HRESULT add_profile(const std::wstring& path) noexcept override { profile = true; profile_dll = path; return result(2); }
    HRESULT enable_profile() noexcept override { enabled = true; return result(7); }
    size_t index(REFGUID id) noexcept {
        for (size_t i = 0; i < ids.size(); ++i) if (*ids[i] == id) return i;
        std::terminate();
    }
    HRESULT register_category(REFGUID id) noexcept override {
        const auto i = index(id); categories[i] = true;
        return result(3 + static_cast<int>(i));
    }
    HRESULT unregister_category(REFGUID id) noexcept override {
        const auto i = index(id);
        const auto hr = result(10 + static_cast<int>(i));
        if (FAILED(hr)) return hr;
        const bool existed = categories[i]; categories[i] = false;
        return existed ? S_OK : S_FALSE;
    }
    HRESULT unregister_processor() noexcept override {
        const auto hr = result(14);
        if (FAILED(hr)) return hr;
        if (replace_owner_on_remove) replace_owner_on_remove->owner(other);
        const bool existed = processor;
        processor = profile = enabled = false;
        return existed ? S_OK : S_FALSE;
    }
    bool empty() const { return !processor && !profile && !enabled && std::none_of(categories.begin(), categories.end(), [](bool x) { return x; }); }
};
void successful_lifecycle(Fixture& f) {
    FakeTsf tsf;
    check(register_in(f.root.value, dll, tsf) == S_OK, "register");
    check(tsf.processor && tsf.profile && tsf.enabled && tsf.profile_dll == dll &&
          std::all_of(tsf.categories.begin(), tsf.categories.end(), [](bool x) { return x; }), "incomplete TSF registration");
    check(f.read() == dll && f.read(L"ThreadingModel") == L"Apartment", "COM values");
    tsf.calls.clear();
    check(register_in(f.root.value, other, tsf) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) && tsf.calls.empty(), "overwrite existing version");
    check(unregister_in(f.root.value, other, tsf) == HRESULT_FROM_WIN32(ERROR_NOT_OWNER) && tsf.calls.empty(), "old DLL removed new version");
    check(f.read() == dll && tsf.enabled, "foreign unregister changed state");
    auto different_case = dll;
    different_case[0] = L'c';
    check(unregister_in(f.root.value, different_case, tsf) == S_OK, "case-insensitive owner check");
    check(!f.exists() && tsf.empty(), "unregister incomplete");
    tsf.calls.clear();
    check(unregister_in(f.root.value, dll, tsf) == S_FALSE && tsf.calls.empty(), "repeat unregister touched TSF");
}
void registration_failures(Fixture& f) {
    for (int step = 0; step <= 7; ++step) {
        FakeTsf tsf; tsf.fail_at = step;
        check(register_in(f.root.value, dll, tsf) == E_ACCESSDENIED, "registration failure not returned");
        check(!f.exists() && tsf.empty(), "registration failure leaked state");
        for (int i = 0; i < 4; ++i) {
            const bool attempted = std::find(tsf.calls.begin(), tsf.calls.end(), 3 + i) != tsf.calls.end();
            const bool removed = std::find(tsf.calls.begin(), tsf.calls.end(), 10 + i) != tsf.calls.end();
            check(attempted == removed, "rollback cleaned an unattempted category or missed attempted one");
        }
        tsf.fail_at = -1;
        check(register_in(f.root.value, dll, tsf) == S_OK && unregister_in(f.root.value, dll, tsf) == S_OK,
              "registration retry failed");
    }
    for (int cleanup_step = 10; cleanup_step <= 14; ++cleanup_step) {
        FakeTsf tsf; tsf.fail_at = 7; tsf.cleanup_fail_at = cleanup_step;
        check(register_in(f.root.value, dll, tsf) == E_ACCESSDENIED && f.read() == dll,
              "failed rollback lost owner path");
        tsf.fail_at = tsf.cleanup_fail_at = -1;
        check(unregister_in(f.root.value, dll, tsf) == S_OK && !f.exists() && tsf.empty(), "failed rollback not recoverable");
    }
}
void unregister_failures(Fixture& f) {
    for (const int step : {0, 10, 11, 12, 13, 14}) {
        FakeTsf tsf;
        check(register_in(f.root.value, dll, tsf) == S_OK, "setup unregister failure");
        tsf.fail_at = step;
        check(unregister_in(f.root.value, dll, tsf) == E_ACCESSDENIED && f.read() == dll,
              "failed unregister removed owner or reported success");
        tsf.fail_at = -1;
        check(unregister_in(f.root.value, dll, tsf) == S_OK && !f.exists() && tsf.empty(), "unregister retry failed");
    }
    FakeTsf tsf;
    check(register_in(f.root.value, dll, tsf) == S_OK, "setup owner change");
    tsf.replace_owner_on_remove = &f;
    check(unregister_in(f.root.value, dll, tsf) == HRESULT_FROM_WIN32(ERROR_NOT_OWNER) && f.read() == other,
          "owner changed during unregister but COM key was deleted");
    f.clear();
}
void invalid_owners(Fixture& f) {
    FakeTsf tsf;
    int case_index = 0;
    auto rejected = [&] {
        ++case_index;
        const auto hr = unregister_in(f.root.value, dll, tsf);
        if (SUCCEEDED(hr) || !tsf.calls.empty() || !f.exists())
            throw std::runtime_error("invalid owner case " + std::to_string(case_index) +
                ": hr=" + std::to_string(hr) + ", calls=" + std::to_string(tsf.calls.size()));
        f.clear();
    };
    f.raw_owner(REG_EXPAND_SZ, dll.c_str(), static_cast<DWORD>((dll.size() + 1) * sizeof(wchar_t))); rejected();
    const DWORD number = 42;
    f.raw_owner(REG_DWORD, &number, sizeof(number)); rejected();
    // Some Windows versions append a NUL just beyond cbData if one is already
    // present in the supplied buffer. Keep a nonzero sentinel there instead.
    const auto unterminated = dll + L"x";
    f.raw_owner(REG_SZ, unterminated.c_str(), static_cast<DWORD>(dll.size() * sizeof(wchar_t))); rejected();
    f.raw_owner(REG_SZ, unterminated.c_str(), static_cast<DWORD>(dll.size() * sizeof(wchar_t) - 1)); rejected();
    f.owner(L""); rejected();
    auto embedded = dll + std::wstring(1, L'\0') + L"hidden";
    f.owner(embedded); rejected();
    f.owner(std::wstring(32768, L'a')); rejected();
    // An incomplete class key must not authorize deleting another TSF profile.
    {
        Key key;
        check(RegCreateKeyExW(f.root.value, class_key, 0, nullptr, 0, KEY_WRITE, nullptr, &key.value, nullptr) == ERROR_SUCCESS,
              "incomplete fixture");
        check(register_in(f.root.value, dll, tsf) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "partial registration overwritten");
    }
    rejected();
    check(register_in(f.root.value, L"", tsf) == E_INVALIDARG && !f.exists(), "empty DLL path");
    check(register_in(f.root.value, embedded, tsf) == E_INVALIDARG && !f.exists(), "embedded null path");
    check(register_in(f.root.value, std::wstring(32768, L'a'), tsf) == E_INVALIDARG && !f.exists(), "oversized path");
}
}
int main() {
    try {
        Fixture fixture;
        successful_lifecycle(fixture);
        registration_failures(fixture);
        unregister_failures(fixture);
        invalid_owners(fixture);
        std::cout << "PASS isolated COM registration, ownership, rollback, failed cleanup, retry and malformed registry values\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
