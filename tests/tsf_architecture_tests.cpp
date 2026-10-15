#include "module.h"
#include "app_icon.h"
#include "isolated_settings.h"
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <stdexcept>

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
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::current_path() / ("arm64x-probe-" + std::to_string(GetCurrentProcessId()));
    use_isolated_settings(root, "learning=off\n");
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    const auto library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    int result = 1;
    try {
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
        check(GetModuleFileNameW(library, path, ARRAYSIZE(path)) != 0 && std::filesystem::equivalent(path, argv[1]), "wrong loaded module path");
        check(FindResourceW(library, MAKEINTRESOURCEW(IDI_NSKKZIME), RT_GROUP_ICON) != nullptr, "missing shared icon resource");
        check(can_unload() == S_OK, "initial module busy");
        {
            Microsoft::WRL::ComPtr<IClassFactory> factory;
            check(SUCCEEDED(factory_entry(nskkzime::windows::service_clsid, IID_PPV_ARGS(&factory))), "class factory export failed");
            Microsoft::WRL::ComPtr<ITfTextInputProcessorEx> service;
            check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service))), "TSF object creation failed");
            check(can_unload() == S_FALSE, "live objects not counted");
        }
        check(can_unload() == S_OK, "released objects retained");
        const auto utf8_path = std::filesystem::path(path).generic_u8string();
        nlohmann::json report{{"host", host}, {"variant", identity.at("variant")},
            {"source_sha256", identity.at("source_sha256")}, {"source_revision", identity.at("source_revision")},
            {"module_path", std::string(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size())},
            {"function_exports", 11}, {"metadata_data_export", true}, {"icon_resource", true}, {"class_factory", true}};
        std::cout << report.dump() << '\n'; result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << " (Win32 " << GetLastError() << ")\n"; }
    if (library) FreeLibrary(library);
    CoUninitialize();
    std::error_code ignored; std::filesystem::remove_all(root, ignored);
    return result;
}
