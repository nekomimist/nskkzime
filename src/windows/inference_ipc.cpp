#include "inference_ipc.h"
#include <sddl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>

namespace nskkzime::windows {
namespace {
void need(bool value, InferenceStatus status = InferenceStatus::Unavailable) { if (!value) throw InferenceFailure(status); }
std::wstring sid(HANDLE process) {
    HANDLE raw = nullptr; need(OpenProcessToken(process, TOKEN_QUERY, &raw) != 0); NativeHandle token(raw);
    DWORD length = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> bytes(length); need(GetTokenInformation(token.get(), TokenUser, bytes.data(), length, &length) != 0);
    LPWSTR text = nullptr; need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text) != 0);
    std::wstring result; try { result = text; } catch (...) { LocalFree(text); throw; } LocalFree(text); return result;
}
void check_peer(HANDLE pipe, bool server) {
    ULONG pid = 0;
    need((server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)) != 0);
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); need(process.valid());
    need(sid(process.get()) == sid(GetCurrentProcess()));
}
DWORD remaining(ULONGLONG deadline) { const auto now = GetTickCount64(); need(now < deadline, InferenceStatus::Timeout); return static_cast<DWORD>(deadline - now); }
void check_cancel(HANDLE cancel) { need(WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0, InferenceStatus::Cancelled); }
DWORD finish(HANDLE pipe, OVERLAPPED& operation, BOOL started, ULONGLONG deadline, HANDLE cancel) {
    const auto error = started ? ERROR_SUCCESS : GetLastError();
    need(started || error == ERROR_IO_PENDING);
    DWORD count = 0;
    if (!started) {
        const HANDLE events[]{cancel, operation.hEvent};
        const auto now = GetTickCount64();
        const auto wait = WaitForMultipleObjects(2, events, FALSE, now < deadline ? static_cast<DWORD>(deadline - now) : 0);
        if (wait != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE);
            throw InferenceFailure(wait == WAIT_OBJECT_0 ? InferenceStatus::Cancelled : InferenceStatus::Timeout);
        }
    }
    need(GetOverlappedResult(pipe, &operation, &count, FALSE) != 0);
    check_cancel(cancel); remaining(deadline); return count;
}
DWORD io(HANDLE pipe, char* buffer, DWORD size, bool write, ULONGLONG deadline, HANDLE cancel) {
    check_cancel(cancel); remaining(deadline);
    NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); need(event.valid());
    OVERLAPPED operation{}; operation.hEvent = event.get();
    return finish(pipe, operation, write ? WriteFile(pipe, buffer, size, nullptr, &operation) : ReadFile(pipe, buffer, size, nullptr, &operation), deadline, cancel);
}
void send_line(HANDLE pipe, std::string line, ULONGLONG deadline, HANDLE cancel) {
    need(line.size() <= inference_max_line, InferenceStatus::InvalidResponse); line += '\n';
    std::size_t offset = 0;
    while (offset < line.size()) {
        const auto count = io(pipe, line.data() + offset, static_cast<DWORD>(line.size() - offset), true, deadline, cancel);
        need(count != 0); offset += count;
    }
}
std::string receive_line(HANDLE pipe, ULONGLONG deadline, HANDLE cancel) {
    std::string result; std::array<char, 2048> buffer{};
    for (;;) {
        const auto count = io(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), false, deadline, cancel); need(count != 0);
        result.append(buffer.data(), count); const auto end = result.find('\n');
        need(result.size() <= inference_max_line + 1, InferenceStatus::InvalidResponse);
        if (end != std::string::npos) {
            need(end + 1 == result.size(), InferenceStatus::InvalidResponse); result.pop_back();
            if (!result.empty() && result.back() == '\r') result.pop_back(); return result;
        }
    }
}
struct Descriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    Descriptor() { const auto text = L"D:P(A;;GA;;;" + sid(GetCurrentProcess()) + L")";
        need(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &value, nullptr) != 0); }
    ~Descriptor() { LocalFree(value); }
};
}
std::wstring inference_pipe_name(std::wstring_view token) {
    need(!token.empty() && token.size() <= 64);
    for (const auto ch : token) need((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-');
    return L"\\\\.\\pipe\\nkkz-ime-v1-" + sid(GetCurrentProcess()) + L"-" + std::wstring(token);
}
InferenceResponse inference_exchange(std::wstring_view token, const InferenceRequest& request, ULONGLONG deadline, HANDLE cancel) {
    const auto name = inference_pipe_name(token);
    // Move-only handles intentionally have no assignment; keep the raw value
    // local until the cancellable connection loop has finished.
    HANDLE raw = INVALID_HANDLE_VALUE;
    for (;;) {
        check_cancel(cancel); remaining(deadline);
        raw = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
        if (raw != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError(); need(error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY);
        if (error == ERROR_FILE_NOT_FOUND) throw InferenceFailure(InferenceStatus::Unavailable);
        WaitForSingleObject(cancel, (std::min)(remaining(deadline), DWORD(10)));
    }
    NativeHandle connected(raw); check_peer(connected.get(), false);
    try {
        validate_inference_hello(receive_line(connected.get(), deadline, cancel));
        send_line(connected.get(), encode_request(request), deadline, cancel);
        return decode_response(receive_line(connected.get(), deadline, cancel), request.id, request.count);
    } catch (const InferenceFailure&) { throw; }
    catch (...) { throw InferenceFailure(InferenceStatus::InvalidResponse); }
}
NativeHandle launch_dummy_server(const std::filesystem::path& program, std::wstring_view token, DWORD delay_ms, DWORD idle_ms) {
    inference_pipe_name(token); need(program.is_absolute() && program.native().find(L'"') == std::wstring::npos && delay_ms <= 10000 && idle_ms <= 60000);
    auto command = L"\"" + program.native() + L"\" --dummy --pipe-token " + std::wstring(token) + L" --delay-ms " + std::to_wstring(delay_ms) + L" --idle-ms " + std::to_wstring(idle_ms);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    need(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, program.parent_path().c_str(), &startup, &process) != 0);
    CloseHandle(process.hThread); return NativeHandle(process.hProcess);
}
int serve_dummy_inference(std::wstring_view token, DWORD delay_ms, DWORD idle_ms) {
    const auto name = inference_pipe_name(token); Descriptor descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
    NativeHandle first(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4, 16384, 16384, 0, &attributes));
    need(first.valid());
    // Create all instances before serving so another process cannot take over
    // the name while all listeners are busy.
    std::vector<NativeHandle> extra;
    for (int i = 0; i < 3; ++i) {
        extra.emplace_back(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4, 16384, 16384, 0, &attributes)); need(extra.back().valid());
    }
    NativeHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)); need(stop.valid());
    std::atomic<ULONGLONG> activity{GetTickCount64()};
    auto serve = [&](HANDLE pipe) {
        while (GetTickCount64() - activity.load() < idle_ms) {
            NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event.valid()) break;
            OVERLAPPED operation{}; operation.hEvent = event.get();
            const auto connected = ConnectNamedPipe(pipe, &operation); const auto error = connected ? ERROR_SUCCESS : GetLastError();
            try {
                if (!connected && error != ERROR_PIPE_CONNECTED) {
                    need(error == ERROR_IO_PENDING);
                    // An idle check must not cancel a live accept every 100 ms:
                    // a connection completing at that boundary could be dropped.
                    // Keep this operation pending until it connects or the whole
                    // server is idle. Always drain before its stack frame ends.
                    for (;;) {
                        const auto waited = WaitForSingleObject(event.get(),100);
                        if (waited == WAIT_OBJECT_0) {
                            DWORD count = 0; need(GetOverlappedResult(pipe,&operation,&count,FALSE) != 0); break;
                        }
                        if (waited == WAIT_TIMEOUT && GetTickCount64() - activity.load() < idle_ms) continue;
                        CancelIoEx(pipe,&operation);
                        DWORD count = 0;
                        const auto completed = GetOverlappedResult(pipe,&operation,&count,TRUE);
                        // Cancellation may lose a race with a successful accept.
                        // In that case the connection is still ours to serve.
                        need(completed != 0,InferenceStatus::Timeout); break;
                    }
                }
                check_peer(pipe, true); activity = GetTickCount64();
                send_line(pipe, inference_hello(), GetTickCount64() + 1000, stop.get());
                const auto request = decode_request(receive_line(pipe, GetTickCount64() + 1000, stop.get()));
                const auto ready = GetTickCount64() + delay_ms;
                while (GetTickCount64() < ready) {
                    need(PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr) != 0);
                    Sleep(5); activity = GetTickCount64();
                }
                InferenceResponse response{request.id, {U"試験〈" + request.reading + U"〉"}};
                if (request.count > 1) response.candidates.push_back(U"別候補〈" + request.reading + U"〉");
                send_line(pipe, encode_response(response), GetTickCount64() + 1000, stop.get());
                // Wait for the client to close after reading; never FlushFileBuffers.
                for (int i = 0; i < 200 && PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr); ++i) Sleep(5);
                activity = GetTickCount64();
            } catch (...) { /* Drop malformed/disconnected clients without logging their text. */ }
            DisconnectNamedPipe(pipe);
        }
    };
    std::vector<std::jthread> workers;
    workers.emplace_back(serve, first.get()); for (const auto& pipe : extra) workers.emplace_back(serve, pipe.get());
    return 0; // jthread destructors join before the handles and ACL are released.
}
}
