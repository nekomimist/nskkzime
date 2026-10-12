#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>
#include <string_view>
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
std::wstring user_sid(HANDLE process = GetCurrentProcess());
class SecurityDescriptor {
public:
    explicit SecurityDescriptor(const std::wstring& sddl);
    ~SecurityDescriptor();
    SecurityDescriptor(const SecurityDescriptor&) = delete;
    SecurityDescriptor& operator=(const SecurityDescriptor&) = delete;
    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }
private:
    PSECURITY_DESCRIPTOR value_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};
// "D:P(A;;GA;;;<sid>)": only this user, for pipes and named objects.
std::wstring owner_only_sddl(const std::wstring& sid);

// The broker and the inference server answer one request per connection over
// a local byte pipe: the client sends a frame (little-endian u32 length, then
// that many bytes, never empty), the server answers with one frame, and the
// client confirms receipt with one byte before closing.
enum class PipeError { Win32, Timeout, Cancelled, Invalid };
struct PipeFailure : std::runtime_error {
    PipeFailure(PipeError why, DWORD code = 0) : std::runtime_error("pipe I/O failed"), reason(why), error(code) {}
    PipeError reason; DWORD error;
};
// Overlapped, same-machine, byte-mode instance for the caller's accept loop.
NativeHandle create_pipe_instance(const std::wstring& name, bool first, DWORD instances, DWORD buffer, SECURITY_ATTRIBUTES*);
// Waits for a client on an instance from create_pipe_instance.
void accept_pipe(HANDLE pipe, ULONGLONG deadline, HANDLE cancel);
// Retries while every instance is busy. An absent server fails at once
// (Win32 ERROR_FILE_NOT_FOUND) unless wait_for_server, which retries until the deadline.
NativeHandle open_pipe(const std::wstring& name, ULONGLONG deadline, HANDLE cancel, bool wait_for_server = false);
void send_frame(HANDLE pipe, std::string_view bytes, std::size_t limit, ULONGLONG deadline, HANDLE cancel);
std::string receive_frame(HANDLE pipe, std::size_t limit, ULONGLONG deadline, HANDLE cancel);
void send_receipt(HANDLE pipe, ULONGLONG deadline, HANDLE cancel);
void receive_receipt(HANDLE pipe, ULONGLONG deadline, HANDLE cancel);
// False once the peer has closed; lets a server abandon work nobody awaits.
bool pipe_peer_open(HANDLE pipe);
} // namespace nskkzime::windows
