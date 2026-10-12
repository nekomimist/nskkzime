#include "learning_ipc.h"
#include "dictionary_settings.h"
#include "local_storage.h"
#include <bcrypt.h>
#include <combaseapi.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
void need(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::filesystem::path checked_path(const std::filesystem::path& path) {
    need(path.is_absolute() && !path.filename().empty() && path.native().find_first_of(L"\r\n\t\"") == std::wstring::npos,
        "user dictionary requires an absolute file path");
    // Stable even before the data file exists or while another process replaces
    // it. Resolving the live file here would make IPC depend on file sharing.
    return path.lexically_normal();
}
}
std::filesystem::path learning_file_path() { return learning_file_path(dictionary_settings_path()); }
std::filesystem::path learning_file_path(const std::filesystem::path& settings_file) {
    return checked_path(settings_file.parent_path() / L"user-dictionary.nskkdb");
}
void validate_learning_file(const std::filesystem::path& file) { (void)checked_path(file); }
namespace {
// Names the change events: one endpoint per user and dictionary path. Earlier
// builds also named their helper's pipe with it; readers of any build share it.
std::wstring endpoint_name(const std::filesystem::path& file) {
    auto name = checked_path(file).native();
    need(name.size() <= 32767, "user dictionary path too long");
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    unsigned char hash[32]{};
    need(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(name.data()),
        static_cast<ULONG>(name.size() * sizeof(wchar_t)), hash, sizeof(hash)) >= 0, "cannot hash dictionary path");
    std::wstring result = L"nskkzime-dictionary-v1-" + user_sid() + L"-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto byte : hash) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
// A manual-reset event per published revision. The server sets the event of
// the revision it replaces; a waiter opens it before re-reading the file, so a
// change can never fall between its read and its wait.
NativeHandle change_event(const std::filesystem::path& file, std::uint64_t revision, DWORD access) {
    const auto name = L"Local\\" + endpoint_name(file) + L"-r" + std::to_wstring(revision);
    SecurityDescriptor descriptor(owner_only_sddl(user_sid()));
    return NativeHandle(CreateEventExW(descriptor.attributes(), name.c_str(), CREATE_EVENT_MANUAL_RESET, access));
}
void signal_change(const std::filesystem::path& file, std::uint64_t replaced) noexcept {
    try { const auto event = change_event(file, replaced, EVENT_MODIFY_STATE); if (event.valid()) SetEvent(event.get()); }
    catch (...) { /* Readers also re-check on a long timeout. */ }
}
NativeHandle lock_writer(const std::filesystem::path& file, DWORD timeout_ms) {
    std::filesystem::create_directories(file.parent_path());
    // An exclusive handle also excludes writers reached through path aliases.
    const auto name = file.native() + L".lock";
    const auto deadline = GetTickCount64() + timeout_ms;
    for (DWORD delay = 5;; delay = (std::min)(delay * 2, DWORD{50})) {
        NativeHandle lock(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.valid()) return lock;
        need(GetLastError() == ERROR_SHARING_VIOLATION, "cannot open user dictionary lock");
        const auto now = GetTickCount64(); need(now < deadline, "user dictionary is busy");
        Sleep((std::min)(delay, static_cast<DWORD>(deadline - now)));
    }
}
template<class Request> UserDictionary commit(const std::filesystem::path& path, const Request& request, DWORD timeout_ms) {
    const auto file = checked_path(path);
    const auto lock = lock_writer(file, timeout_ms);
    auto dictionary = read_user_dictionary(file); // Corruption never becomes an empty, writable database.
    const auto replaced = dictionary.revision();
    if (dictionary.apply(request)) {
        write_file_atomic(file, dictionary.serialize());
        signal_change(file, replaced);
    }
    return dictionary;
}
}
UserDictionary commit_learning(const std::filesystem::path& file, const LearningRequest& request, DWORD timeout_ms) {
    return commit(file, request, timeout_ms);
}
UserDictionary commit_import(const std::filesystem::path& file, const ImportRequest& request, DWORD timeout_ms) {
    return commit(file, request, timeout_ms);
}
NativeHandle learning_change_event(const std::filesystem::path& file, std::uint64_t revision) {
    return change_event(file, revision, SYNCHRONIZE);
}
std::string learning_client_id() {
    GUID guid{}; need(SUCCEEDED(CoCreateGuid(&guid)), "cannot create learning client ID");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&guid); std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < sizeof(guid); ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}
UserDictionary read_user_dictionary(const std::filesystem::path& file) {
    // Keep a handle to one complete generation while allowing the server to
    // atomically replace the path. Listing/exporting must not block its saves.
    NativeHandle stream(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!stream.valid()) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        throw std::runtime_error("cannot open user dictionary");
    }
    LARGE_INTEGER size{};
    need(GetFileSizeEx(stream.get(), &size) && size.QuadPart >= 0 && size.QuadPart <= learning_max_bytes, "invalid user dictionary size");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0'); DWORD count = 0;
    need(ReadFile(stream.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size(), "cannot read user dictionary");
    return UserDictionary::parse(bytes);
}
std::uint64_t user_dictionary_revision(const std::filesystem::path& file) {
    NativeHandle stream(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!stream.valid()) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return UserDictionary{}.revision();
        throw std::runtime_error("cannot open user dictionary");
    }
    // Magic (5 bytes) followed by the little-endian revision. The full parse
    // still validates the checksum whenever this differs from a known value.
    unsigned char header[13]{}; DWORD count = 0;
    need(ReadFile(stream.get(), header, sizeof(header), &count, nullptr) && count == sizeof(header) &&
        (!std::memcmp(header, "NKUD1", 5) || !std::memcmp(header, "NKUD2", 5)), "invalid user dictionary header");
    std::uint64_t revision = 0;
    for (int i = 7; i >= 0; --i) revision = (revision << 8) | header[5 + i];
    return revision;
}
std::string learning_snapshot_response(const std::filesystem::path& file, std::string_view request) {
    need(request.size() == 12 && request.starts_with("GET1"), "invalid snapshot request");
    if (request == snapshot_request(user_dictionary_revision(file))) return "SAME";
    return "DATA" + read_user_dictionary(file).serialize();
}
std::string snapshot_request(std::uint64_t revision) {
    std::string bytes = "GET1";
    for (int i = 0; i < 8; ++i) { bytes += static_cast<char>(revision & 255); revision >>= 8; }
    return bytes;
}
std::string learning_wait_request(std::uint64_t revision) { return "WAT1" + snapshot_request(revision).substr(4); }
std::optional<std::string> learning_wait_response(const std::filesystem::path& file, std::string_view request,
    const std::function<bool()>& cancelled, DWORD timeout_ms) {
    need(request.size() == 12 && request.starts_with("WAT1"), "invalid wait request");
    const auto snapshot = "GET1" + std::string(request.substr(4));
    std::uint64_t revision = 0;
    for (int i = 7; i >= 0; --i) revision = (revision << 8) | static_cast<unsigned char>(request[4 + i]);
    const auto deadline = GetTickCount64() + timeout_ms;
    bool signaled = false;
    for (;;) {
        // Listen before reading the file, as desktop readers do. Saves change the
        // header before signaling, so a signal that still reads SAME (e.g. a
        // restored file) falls back to polling instead of a busy loop.
        NativeHandle changed(signaled ? NativeHandle{} : learning_change_event(file, revision));
        auto response = learning_snapshot_response(file, snapshot);
        // A client closes its wait before it writes, so the save it signals
        // finds the wait already cancelled here instead of failing the reply.
        if (cancelled()) return std::nullopt;
        if (response != "SAME") return response;
        const auto recheck = changed.valid() ? deadline : GetTickCount64() + 1000;
        for (;;) {
            if (cancelled()) return std::nullopt;
            const auto now = GetTickCount64();
            if (now >= deadline) return response;
            if (now >= recheck) break;
            const auto slice = static_cast<DWORD>((std::min)((std::min)(deadline, recheck) - now, ULONGLONG{100}));
            if (!changed.valid()) Sleep(slice);
            else if (WaitForSingleObject(changed.get(), slice) == WAIT_OBJECT_0) { signaled = true; break; }
        }
    }
}
} // namespace nskkzime::windows
