#pragma once
#include "nskkzime/learning.h"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <optional>
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
std::filesystem::path learning_file_path(const std::filesystem::path& settings_file);
std::wstring learning_pipe_name(const std::filesystem::path&);
std::string learning_client_id();
UserDictionary read_user_dictionary(const std::filesystem::path&);
// Header-only read: lets readers skip unchanged generations without the server.
std::uint64_t user_dictionary_revision(const std::filesystem::path&);
// SAME/DATA answer for a GET1 request, read directly from the saved file.
std::string learning_snapshot_response(const std::filesystem::path&, std::string_view request);
// WAT1 is GET1 that waits up to learning_wait_ms for a newer revision, for
// readers that cannot open the change events. nullopt once cancelled() is true.
inline constexpr DWORD learning_wait_ms = 20000;
std::string learning_wait_request(std::uint64_t revision);
std::optional<std::string> learning_wait_response(const std::filesystem::path&, std::string_view request,
    const std::function<bool()>& cancelled, DWORD timeout_ms = learning_wait_ms);
// Signaled once the server replaces this revision. Open it before re-reading the file.
NativeHandle learning_change_event(const std::filesystem::path&, std::uint64_t revision);
// All synchronous functions below run off the input thread. Transport failures
// may follow a committed write; retry using the identical client/sequence.
std::string learning_exchange(const std::filesystem::path&, std::string_view request, DWORD timeout_ms = 1500);
NativeHandle launch_dictionary_server(const std::filesystem::path& executable, const std::filesystem::path& file, DWORD idle_ms = 10000);
int serve_dictionary(const std::filesystem::path& file, DWORD idle_ms);
std::string snapshot_request(std::uint64_t revision);
} // namespace nskkzime::windows
