#pragma once
#include <windows.h>
#include <msctf.h>
#include <atomic>

namespace nskkzime::windows {
inline constexpr CLSID service_clsid = {0x6ef45a57, 0xe12e, 0x49f1, {0x8a, 0x4e, 0x25, 0x69, 0xd8, 0x36, 0x1a, 0x50}};
inline constexpr GUID profile_guid = {0x05a0316a, 0xdd2c, 0x44b3, {0x9c, 0xd7, 0x43, 0xe5, 0x54, 0xfb, 0x24, 0xc2}};
inline constexpr LANGID language_id = MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT);
extern HINSTANCE module_instance;
extern std::atomic<long> module_objects;
extern std::atomic<long> server_locks;
HRESULT create_text_service(REFIID iid, void** output) noexcept;
HRESULT register_server() noexcept;
HRESULT unregister_server() noexcept;
}
