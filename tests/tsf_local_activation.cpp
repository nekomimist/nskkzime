#include "module.h"
#include <wrl/client.h>
#include <filesystem>
#include <iostream>

// Exercise the same process-private COM mapping as ime_probe_local. This test
// creates no windows, activates no keyboard profile, and writes no registry.
int wmain() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int result = 1;
    {
        Microsoft::WRL::ComPtr<ITfTextInputProcessorEx> service;
        const auto hr = CoCreateInstance(nskkzime::windows::service_clsid, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
        if (FAILED(hr)) std::cerr << "CoCreateInstance failed: 0x" << std::hex << static_cast<unsigned long>(hr) << '\n';
        else {
            const auto module = GetModuleHandleW(L"nskkzime_tsf.dll");
            wchar_t executable[32768]{}, library[32768]{};
            const auto executable_size = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
            const auto library_size = module ? GetModuleFileNameW(module, library, ARRAYSIZE(library)) : 0;
            if (executable_size && executable_size < ARRAYSIZE(executable) && library_size && library_size < ARRAYSIZE(library)) {
                const auto expected = std::filesystem::path(executable).parent_path() / L"nskkzime_tsf.dll";
                if (_wcsicmp(expected.c_str(), library) == 0) {
                    std::wcout << L"Process-private COM activation: " << library << L'\n';
                    result = 0;
                } else std::wcerr << L"Unexpected COM DLL: " << library << L'\n';
            }
        }
    }
    CoFreeUnusedLibraries();
    CoUninitialize();
    return result;
}
