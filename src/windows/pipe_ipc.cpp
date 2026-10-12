#include "pipe_ipc.h"
#include <sddl.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace nskkzime::windows {
namespace {
void native_need(bool ok) { if (!ok) throw PipeFailure(PipeError::Win32, GetLastError()); }
void check(HANDLE cancel, ULONGLONG deadline) {
    const auto waited = WaitForSingleObject(cancel, 0);
    if (waited == WAIT_FAILED) throw PipeFailure(PipeError::Win32, GetLastError());
    if (waited == WAIT_OBJECT_0) throw PipeFailure(PipeError::Cancelled);
    if (GetTickCount64() >= deadline) throw PipeFailure(PipeError::Timeout);
}
DWORD finish(HANDLE pipe, OVERLAPPED& operation, BOOL started, ULONGLONG deadline, HANDLE cancel) {
    const auto error = started ? ERROR_SUCCESS : GetLastError();
    if (!started && error != ERROR_IO_PENDING) throw PipeFailure(PipeError::Win32, error);
    DWORD count = 0;
    if (!started) {
        const HANDLE events[]{cancel, operation.hEvent}; const auto now = GetTickCount64();
        const auto waited = WaitForMultipleObjects(2, events, FALSE, now < deadline ? static_cast<DWORD>(deadline - now) : 0);
        if (waited != WAIT_OBJECT_0 + 1) {
            const auto wait_error = waited == WAIT_FAILED ? GetLastError() : 0;
            // Keep the OVERLAPPED and buffer alive until cancellation completes.
            CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE);
            if (waited == WAIT_TIMEOUT) throw PipeFailure(PipeError::Timeout);
            if (waited == WAIT_OBJECT_0) throw PipeFailure(PipeError::Cancelled);
            throw PipeFailure(PipeError::Win32, wait_error);
        }
    }
    native_need(GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE);
    check(cancel, deadline); return count;
}
void transfer(HANDLE pipe, char* bytes, std::size_t size, bool write, ULONGLONG deadline, HANDLE cancel) {
    while (size) {
        check(cancel, deadline);
        NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); native_need(event.valid());
        OVERLAPPED operation{}; operation.hEvent = event.get();
        const auto amount = static_cast<DWORD>((std::min)(size, std::size_t(65536)));
        const auto started = write ? WriteFile(pipe, bytes, amount, nullptr, &operation) : ReadFile(pipe, bytes, amount, nullptr, &operation);
        const auto count = finish(pipe, operation, started, deadline, cancel);
        if (!count || count > amount) throw PipeFailure(PipeError::Invalid);
        bytes += count; size -= count;
    }
}
}
std::wstring user_sid(HANDLE process) {
    HANDLE raw = nullptr; native_need(OpenProcessToken(process, TOKEN_QUERY, &raw) != FALSE); NativeHandle token(raw);
    DWORD size = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    native_need(GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size) != FALSE);
    LPWSTR text = nullptr; native_need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text) != FALSE);
    std::wstring result;
    try { result = text; } catch (...) { LocalFree(text); throw; }
    LocalFree(text); return result;
}
SecurityDescriptor::SecurityDescriptor(const std::wstring& sddl) {
    native_need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &value_, nullptr) != FALSE);
    attributes_ = {sizeof(attributes_), value_, FALSE};
}
SecurityDescriptor::~SecurityDescriptor() { LocalFree(value_); }
std::wstring owner_only_sddl(const std::wstring& sid) { return L"D:P(A;;GA;;;" + sid + L")"; }
NativeHandle create_pipe_instance(const std::wstring& name, bool first, DWORD instances, DWORD buffer, SECURITY_ATTRIBUTES* attributes) {
    return NativeHandle(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, instances, buffer, buffer, 0, attributes));
}
void accept_pipe(HANDLE pipe, ULONGLONG deadline, HANDLE cancel) {
    NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); native_need(event.valid());
    OVERLAPPED operation{}; operation.hEvent = event.get();
    const auto connected = ConnectNamedPipe(pipe, &operation);
    if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) return;
    finish(pipe, operation, connected, deadline, cancel);
}
NativeHandle open_pipe(const std::wstring& name, ULONGLONG deadline, HANDLE cancel, bool wait_for_server) {
    for (;;) {
        check(cancel, deadline);
        // AppContainer clients may not request FILE_CREATE_PIPE_INSTANCE.
        NativeHandle pipe(CreateFileW(name.c_str(), (FILE_GENERIC_READ | FILE_GENERIC_WRITE) & ~FILE_CREATE_PIPE_INSTANCE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
        if (pipe.valid()) return pipe;
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && (error != ERROR_FILE_NOT_FOUND || !wait_for_server)) throw PipeFailure(PipeError::Win32, error);
        const auto now = GetTickCount64();
        WaitForSingleObject(cancel, static_cast<DWORD>((std::min)(deadline > now ? deadline - now : 0, ULONGLONG{10})));
    }
}
void send_frame(HANDLE pipe, std::string_view bytes, std::size_t limit, ULONGLONG deadline, HANDLE cancel) {
    if (bytes.empty() || bytes.size() > limit || bytes.size() > UINT32_MAX) throw PipeFailure(PipeError::Invalid);
    auto size = static_cast<std::uint32_t>(bytes.size());
    transfer(pipe, reinterpret_cast<char*>(&size), sizeof(size), true, deadline, cancel);
    transfer(pipe, const_cast<char*>(bytes.data()), bytes.size(), true, deadline, cancel);
}
std::string receive_frame(HANDLE pipe, std::size_t limit, ULONGLONG deadline, HANDLE cancel) {
    std::uint32_t size = 0; transfer(pipe, reinterpret_cast<char*>(&size), sizeof(size), false, deadline, cancel);
    if (!size || size > limit) throw PipeFailure(PipeError::Invalid);
    std::string bytes(size, '\0'); transfer(pipe, bytes.data(), bytes.size(), false, deadline, cancel); return bytes;
}
// The receipt lets the server disconnect after the client has read everything,
// without an unbounded FlushFileBuffers on the pipe.
void send_receipt(HANDLE pipe, ULONGLONG deadline, HANDLE cancel) { char receipt = 1; transfer(pipe, &receipt, 1, true, deadline, cancel); }
void receive_receipt(HANDLE pipe, ULONGLONG deadline, HANDLE cancel) { char receipt = 0; transfer(pipe, &receipt, 1, false, deadline, cancel); }
bool pipe_peer_open(HANDLE pipe) { return PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr) != FALSE; }
} // namespace nskkzime::windows
