#pragma once
#include <windows.h>
#include <stdexcept>

// Tests of ready-state behavior explicitly wait for background initialization.
// Dedicated runtime tests below exercise input while loading without this wait.
template<class Ready> void wait_for_dictionary(Ready ready, DWORD timeout_ms = 5000) {
    const auto deadline = GetTickCount64() + timeout_ms;
    while (!ready()) {
        if (GetTickCount64() >= deadline) throw std::runtime_error("dictionary load did not complete");
        Sleep(1);
    }
}
inline void wait_for_dictionary() {
    using Ready = HRESULT(WINAPI*)(BOOL*);
#pragma warning(push)
#pragma warning(disable:4191)
    const auto ready = reinterpret_cast<Ready>(GetProcAddress(GetModuleHandleW(L"nskkzime_tsf.dll"),"NskkzimeIsDictionaryReady"));
#pragma warning(pop)
    if (!ready) throw std::runtime_error("missing dictionary readiness export");
    wait_for_dictionary([&] { BOOL value = FALSE; if (FAILED(ready(&value))) throw std::runtime_error("dictionary readiness failed"); return value != FALSE; });
}
