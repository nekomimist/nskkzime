#include "learning_ipc.h"
#include "dictionary_settings.h"
#include "local_storage.h"
#include <bcrypt.h>
#include <combaseapi.h>
#include <sddl.h>
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
std::wstring user_sid() {
    HANDLE raw = nullptr; need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != 0, "cannot open user token"); NativeHandle token(raw);
    DWORD size = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    need(GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size) != 0, "cannot read user token");
    LPWSTR text = nullptr; need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text) != 0, "cannot read user SID");
    std::wstring result;
    try { result = text; } catch (...) { LocalFree(text); throw; }
    LocalFree(text); return result;
}
struct Descriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    explicit Descriptor(const std::wstring& sid) {
        const auto sddl = L"D:P(A;;GA;;;" + sid + L")";
        need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &value, nullptr) != 0, "cannot create pipe ACL");
    }
    ~Descriptor() { LocalFree(value); }
};
DWORD finish_io(HANDLE handle, OVERLAPPED& operation, BOOL immediate, DWORD timeout) {
    const auto error = immediate ? ERROR_SUCCESS : GetLastError();
    if (!immediate && error != ERROR_IO_PENDING) throw std::runtime_error("dictionary IPC operation failed");
    DWORD bytes = 0;
    if (!GetOverlappedResultEx(handle, &operation, &bytes, timeout, FALSE)) {
        CancelIoEx(handle, &operation);
        // Keep the OVERLAPPED and buffer alive until cancellation completes.
        GetOverlappedResult(handle, &operation, &bytes, TRUE);
        throw std::runtime_error("dictionary IPC timed out or disconnected");
    }
    return bytes;
}
void transfer(HANDLE handle, char* data, std::size_t size, bool write, ULONGLONG deadline) {
    while (size) {
        NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); need(event.valid(), "cannot allocate IPC event");
        OVERLAPPED operation{}; operation.hEvent = event.get();
        const auto now = GetTickCount64(); need(now < deadline, "dictionary IPC deadline exceeded");
        const auto amount = static_cast<DWORD>(size);
        const auto started = write ? WriteFile(handle, data, amount, nullptr, &operation) : ReadFile(handle, data, amount, nullptr, &operation);
        const auto done = finish_io(handle, operation, started, static_cast<DWORD>(deadline - now));
        need(done != 0 && done <= size, "dictionary IPC closed"); data += done; size -= done;
    }
}
void send_frame(HANDLE pipe, std::string_view bytes, ULONGLONG deadline) {
    need(bytes.size() <= learning_max_bytes + 16, "dictionary IPC frame too large");
    std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
    transfer(pipe, reinterpret_cast<char*>(&size), sizeof(size), true, deadline);
    transfer(pipe, const_cast<char*>(bytes.data()), bytes.size(), true, deadline);
}
std::string receive_frame(HANDLE pipe, ULONGLONG deadline, std::size_t maximum) {
    std::uint32_t size = 0; transfer(pipe, reinterpret_cast<char*>(&size), sizeof(size), false, deadline);
    need(size && size <= maximum, "invalid dictionary IPC frame size");
    std::string bytes(size, '\0'); transfer(pipe, bytes.data(), bytes.size(), false, deadline); return bytes;
}
}
std::filesystem::path learning_file_path() {
    if (const auto file = environment_value(L"NSKKZIME_USER_DICTIONARY")) return checked_path(*file);
    return checked_path(dictionary_settings_path().parent_path() / L"user-dictionary.nskkdb");
}
std::wstring learning_pipe_name(const std::filesystem::path& file) {
    auto name = checked_path(file).native();
    need(name.size() <= 32767, "user dictionary path too long");
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    unsigned char hash[32]{};
    need(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(name.data()),
        static_cast<ULONG>(name.size() * sizeof(wchar_t)), hash, sizeof(hash)) >= 0, "cannot hash dictionary path");
    std::wstring result = L"\\\\.\\pipe\\nskkzime-dictionary-v1-" + user_sid() + L"-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto byte : hash) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
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
std::string snapshot_request(std::uint64_t revision) {
    std::string bytes = "GET1";
    for (int i = 0; i < 8; ++i) { bytes += static_cast<char>(revision & 255); revision >>= 8; }
    return bytes;
}
std::string learning_exchange(const std::filesystem::path& file, std::string_view request, DWORD timeout_ms) {
    const auto name = learning_pipe_name(file);
    const auto deadline = GetTickCount64() + timeout_ms;
    HANDLE raw = INVALID_HANDLE_VALUE;
    do {
        raw = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
        if (raw != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        need(error == ERROR_PIPE_BUSY || error == ERROR_FILE_NOT_FOUND, "cannot connect to dictionary server");
        if (error == ERROR_FILE_NOT_FOUND) throw std::runtime_error("dictionary server is unavailable");
        const auto now = GetTickCount64(); need(now < deadline, "dictionary server is busy");
        WaitNamedPipeW(name.c_str(), static_cast<DWORD>(deadline - now));
    } while (GetTickCount64() < deadline);
    NativeHandle pipe(raw); need(pipe.valid(), "cannot connect to dictionary server");
    send_frame(pipe.get(), request, deadline);
    auto response = receive_frame(pipe.get(), deadline, learning_max_bytes + 16);
    char ack = 1; transfer(pipe.get(), &ack, 1, true, deadline); return response;
}
NativeHandle launch_dictionary_server(const std::filesystem::path& executable, const std::filesystem::path& file, DWORD idle_ms) {
    const auto target = checked_path(file); const auto program = checked_path(executable);
    std::wstring command = L"\"" + program.native() + L"\" --file \"" + target.native() + L"\" --idle-ms " + std::to_wstring(idle_ms);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    need(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        program.parent_path().c_str(), &startup, &process) != 0, "cannot start dictionary server");
    CloseHandle(process.hThread); return NativeHandle(process.hProcess);
}
int serve_dictionary(const std::filesystem::path& path, DWORD idle_ms) {
    const auto file = checked_path(path); std::filesystem::create_directories(file.parent_path());
    // An exclusive handle also prevents writers reached through path aliases.
    NativeHandle writer(CreateFileW((file.native() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!writer.valid()) return 3;
    auto dictionary = read_user_dictionary(file); // Corruption never becomes an empty, writable database.
    const auto name = learning_pipe_name(file); Descriptor descriptor(user_sid());
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
    NativeHandle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes));
    need(pipe.valid(), "cannot create dictionary server pipe");
    for (;;) {
        NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); need(event.valid(), "cannot allocate server event");
        OVERLAPPED operation{}; operation.hEvent = event.get();
        const auto connected = ConnectNamedPipe(pipe.get(), &operation);
        const auto error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error != ERROR_PIPE_CONNECTED) {
            if (error != ERROR_IO_PENDING) throw std::runtime_error("dictionary server accept failed");
            DWORD count = 0;
            if (!GetOverlappedResultEx(pipe.get(), &operation, &count, idle_ms, FALSE)) {
                CancelIoEx(pipe.get(), &operation); GetOverlappedResult(pipe.get(), &operation, &count, TRUE); return 0;
            }
        }
        try {
            const auto request = receive_frame(pipe.get(), GetTickCount64() + 1500, 20000);
            std::string response;
            try {
                if (request.starts_with("NKL1") || request.starts_with("NKF1")) {
                    const auto update = decode_learning(request); auto next = dictionary;
                    if (next.apply(update)) { write_file_atomic(file, next.serialize()); dictionary = std::move(next); }
                    response = "OKAY";
                } else if (request.size() == 12 && request.starts_with("GET1")) {
                    if (request == snapshot_request(dictionary.revision())) response = "SAME";
                    else response = "DATA" + dictionary.serialize();
                } else response = "FAIL";
            } catch (const std::exception&) { response = "FAIL"; }
            send_frame(pipe.get(), response, GetTickCount64() + 1500);
            // Client acknowledges receipt before disconnect, without an unbounded FlushFileBuffers on a pipe.
            char ack = 0; transfer(pipe.get(), &ack, 1, false, GetTickCount64() + 1500);
        } catch (const std::exception&) { /* Isolate malformed or disconnected clients. No text logging. */ }
        DisconnectNamedPipe(pipe.get());
    }
}
} // namespace nskkzime::windows
