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
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) NativeHandle old(std::exchange(value_, std::exchange(other.value_, INVALID_HANDLE_VALUE)));
        return *this;
    }
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
std::filesystem::path learning_file_path();
std::filesystem::path learning_file_path(const std::filesystem::path& settings_file);
// Throws unless the path can name a user dictionary (absolute, no quotes or controls).
void validate_learning_file(const std::filesystem::path&);
std::string learning_client_id();
UserDictionary read_user_dictionary(const std::filesystem::path&);
// Header-only read: lets readers skip unchanged generations.
std::uint64_t user_dictionary_revision(const std::filesystem::path&);
// Writers of every version share these rules, so processes running different
// versions can update one file:
// - Hold "<file>.lock" open without sharing while reading, changing and
//   replacing the file. Earlier builds wrote through a helper process that
//   kept it open for its whole lifetime; later writers wait for it to exit.
// - Re-read the saved file under the lock. A file that does not parse (corrupt
//   or written by a newer version) is never replaced.
// - Replace the file atomically, then signal the replaced revision's event.
// These run off the input thread. Each throws without writing when the lock
// stays busy past timeout_ms or the save fails; retry with the same
// client/sequence. The result is the saved dictionary.
UserDictionary commit_learning(const std::filesystem::path&, const LearningRequest&, DWORD timeout_ms = 1500);
UserDictionary commit_import(const std::filesystem::path&, const ImportRequest&, DWORD timeout_ms = 1500);
// SAME/DATA answer for a GET1 request, read directly from the saved file. The
// broker answers these for AppContainers, which cannot read the file.
std::string learning_snapshot_response(const std::filesystem::path&, std::string_view request);
std::string snapshot_request(std::uint64_t revision);
// WAT1 is GET1 that waits up to learning_wait_ms for a newer revision, for
// readers that cannot open the change events. nullopt once cancelled() is true.
inline constexpr DWORD learning_wait_ms = 20000;
std::string learning_wait_request(std::uint64_t revision);
std::optional<std::string> learning_wait_response(const std::filesystem::path&, std::string_view request,
    const std::function<bool()>& cancelled, DWORD timeout_ms = learning_wait_ms);
// Signaled once a writer replaces this revision. Open it before re-reading the file.
NativeHandle learning_change_event(const std::filesystem::path&, std::uint64_t revision);
} // namespace nskkzime::windows
