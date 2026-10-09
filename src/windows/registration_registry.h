#pragma once
#include <windows.h>
#include <string>

namespace nskkzime::windows::registration {
// Only the TSF calls are substituted in tests. Registry operations run against
// a private key, using the same code as DllRegisterServer/DllUnregisterServer.
class TsfRegistration {
public:
    virtual ~TsfRegistration() = default;
    virtual HRESULT initialize() noexcept = 0;
    virtual HRESULT register_processor() noexcept = 0;
    virtual HRESULT add_profile(const std::wstring& dll) noexcept = 0;
    virtual HRESULT enable_profile() noexcept = 0;
    virtual HRESULT register_category(REFGUID category) noexcept = 0;
    virtual HRESULT unregister_category(REFGUID category) noexcept = 0;
    virtual HRESULT unregister_processor() noexcept = 0;
};
// root is HKLM in production; the caller serializes registration operations.
HRESULT register_in(HKEY root, const std::wstring& dll, TsfRegistration& tsf, bool shared_profile = true) noexcept;
HRESULT unregister_in(HKEY root, const std::wstring& dll, TsfRegistration& tsf, bool shared_profile = true) noexcept;
}
