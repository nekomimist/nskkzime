#include "module.h"
#include "app_icon.h"
#include "isolated_settings.h"
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
#if defined(_M_ARM64EC)
constexpr auto host = "arm64ec", expected_variant = "arm64x-ec";
#elif defined(_M_ARM64)
constexpr auto host = "arm64", expected_variant = "arm64x-native";
#elif defined(_M_IX86)
constexpr auto host = "x86", expected_variant = "x86-companion";
#else
constexpr auto host = "x64", expected_variant = "arm64x-ec";
#endif
int registration_state() {
    Microsoft::WRL::ComPtr<ITfInputProcessorProfiles> profiles;
    Microsoft::WRL::ComPtr<ITfCategoryMgr> categories;
    check(SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&profiles))), "cannot inspect TSF processors");
    check(SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&categories))), "cannot inspect TSF categories");
    const auto count = [](IEnumGUID* items, bool ours_only) {
        unsigned total = 0;
        if (!items) return total;
        for (;;) {
            GUID id{}; ULONG fetched = 0;
            const auto hr = items->Next(1, &id, &fetched);
            check(SUCCEEDED(hr), "cannot enumerate TSF registration");
            if (!fetched) return total;
            if (!ours_only || id == nskkzime::windows::service_clsid) ++total;
        }
    };
    Microsoft::WRL::ComPtr<IEnumGUID> processors, items;
    check(SUCCEEDED(profiles->EnumInputProcessorInfo(&processors)), "cannot enumerate processors");
    check(SUCCEEDED(categories->EnumCategoriesInItem(nskkzime::windows::service_clsid, &items)), "cannot enumerate categories");
    const bool registered = count(processors.Get(), true) != 0;
    BSTR description = nullptr;
    const auto profile_hr = profiles->GetLanguageProfileDescription(nskkzime::windows::service_clsid,
        nskkzime::windows::language_id, nskkzime::windows::profile_guid, &description);
    SysFreeString(description);
    BOOL enabled = FALSE;
    const auto enabled_hr = profiles->IsEnabledLanguageProfile(nskkzime::windows::service_clsid,
        nskkzime::windows::language_id, nskkzime::windows::profile_guid, &enabled);
    nlohmann::json report{{"host", host}, {"processor_registered", registered},
        {"category_count", count(items.Get(), false)}, {"profile_exists", profile_hr == S_OK},
        {"enabled_for_user", enabled_hr == S_OK && enabled != FALSE}};
    std::cout << report.dump() << '\n';
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--registration-state") {
        const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) return 1;
        int result = 1;
        try { result = registration_state(); }
        catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
        CoUninitialize(); return result;
    }
    const bool registered = argc == 3 && std::wstring_view(argv[1]) == L"--registered";
    if (argc != 2 && !registered) return 2;
    const auto* expected_dll = argv[registered ? 2 : 1];
    const auto root = std::filesystem::current_path() / ("arm64x-probe-" + std::to_string(GetCurrentProcessId()));
    use_isolated_settings(root, "learning=off\n");
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    HMODULE library = nullptr;
    int result = 1;
    try {
        if (registered) {
            // Resolve through the real COM registry before looking up the DLL.
            // Preloading the expected path would mask a broken registration.
            Microsoft::WRL::ComPtr<ITfTextInputProcessorEx> service;
            const auto hr = CoCreateInstance(nskkzime::windows::service_clsid, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
            if (FAILED(hr)) std::cerr << "CoCreateInstance HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << '\n';
            check(SUCCEEDED(hr), "registered COM activation failed");
            check(GetModuleHandleExW(0, L"nskkzime_tsf.dll", &library) != 0, "registered module missing");
        } else {
            library = LoadLibraryExW(expected_dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        check(library != nullptr, "ARM64X module load failed");
        for (const auto* name : {"DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer", "DllUnregisterServer",
            "NskkzimeGetDictionaryStatus", "NskkzimeIsDictionaryReady", "NskkzimeEnableContextPreview",
            "NskkzimeGetContextPreview", "NskkzimeGetInferenceStatus", "NskkzimeEnableInputTiming", "NskkzimeGetInputTiming"})
            check(GetProcAddress(library, name) != nullptr, "missing function export");
        using Factory = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        using CanUnload = HRESULT(WINAPI*)();
#pragma warning(push)
#pragma warning(disable:4191)
        const auto factory_entry = reinterpret_cast<Factory>(GetProcAddress(library, "DllGetClassObject"));
        const auto can_unload = reinterpret_cast<CanUnload>(GetProcAddress(library, "DllCanUnloadNow"));
        const auto* metadata = reinterpret_cast<const char*>(GetProcAddress(library, "nskkzime_build_metadata"));
#pragma warning(pop)
        check(metadata != nullptr, "missing metadata data export");
        const auto length = strnlen_s(metadata, 3001);
        check(length < 3001, "invalid metadata data export");
        const auto identity = nlohmann::json::parse(std::string_view(metadata, length));
        check(identity.at("variant") == expected_variant && identity.at("binary") == "nskkzime_tsf.dll", "wrong architecture view metadata");
        wchar_t path[32768]{};
        check(GetModuleFileNameW(library, path, ARRAYSIZE(path)) != 0 && std::filesystem::equivalent(path, expected_dll), "wrong loaded module path");
        check(FindResourceW(library, MAKEINTRESOURCEW(IDI_NSKKZIME), RT_GROUP_ICON) != nullptr, "missing shared icon resource");
        const auto initial_unload = can_unload();
        check(initial_unload == S_OK || (registered && initial_unload == S_FALSE), "unexpected initial unload status");
        {
            Microsoft::WRL::ComPtr<IClassFactory> factory;
            check(SUCCEEDED(factory_entry(nskkzime::windows::service_clsid, IID_PPV_ARGS(&factory))), "class factory export failed");
            Microsoft::WRL::ComPtr<ITfTextInputProcessorEx> service;
            check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service))), "TSF object creation failed");
            check(can_unload() == S_FALSE, "live objects not counted");
        }
        check(can_unload() == initial_unload, "released objects retained");
        const auto utf8_path = std::filesystem::path(path).generic_u8string();
        nlohmann::json report{{"host", host}, {"variant", identity.at("variant")},
            {"source_sha256", identity.at("source_sha256")}, {"source_revision", identity.at("source_revision")},
            {"module_path", std::string(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size())},
            {"function_exports", 11}, {"metadata_data_export", true}, {"icon_resource", true}, {"class_factory", true},
            {"registered_com", registered}};
        std::cout << report.dump() << '\n'; result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << " (Win32 " << GetLastError() << ")\n"; }
    if (library) FreeLibrary(library);
    CoUninitialize();
    std::error_code ignored; std::filesystem::remove_all(root, ignored);
    return result;
}
