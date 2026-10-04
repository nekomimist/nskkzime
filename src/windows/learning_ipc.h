#pragma once
#include "nskkzime/learning.h"
#include <windows.h>
#include <filesystem>
#include <utility>

namespace nskkzime::windows {
class NativeHandle {
public:
    explicit NativeHandle(HANDLE h = INVALID_HANDLE_VALUE) : value_(h) {}
    ~NativeHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle(NativeHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
std::filesystem::path learning_file_path();
std::wstring learning_pipe_name(const std::filesystem::path&);
std::string learning_client_id();
UserDictionary read_user_dictionary(const std::filesystem::path&);
// All synchronous functions below run off the input thread. Transport failures
// may follow a committed write; retry using the identical client/sequence.
std::string learning_exchange(const std::filesystem::path&, std::string_view request, DWORD timeout_ms = 1500);
NativeHandle launch_dictionary_server(const std::filesystem::path& executable, const std::filesystem::path& file, DWORD idle_ms = 10000);
int serve_dictionary(const std::filesystem::path& file, DWORD idle_ms);
std::string snapshot_request(std::uint64_t revision);
} // namespace nskkzime::windows
