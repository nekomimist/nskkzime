#include "inference_ipc.h"
#include "cpu_requirements.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>

namespace nskkzime::windows {
namespace {
void need(bool value, InferenceStatus status = InferenceStatus::Unavailable) { if (!value) throw InferenceFailure(status); }
InferenceStatus status(const PipeFailure& failure) {
    if (failure.reason == PipeError::Timeout) return InferenceStatus::Timeout;
    if (failure.reason == PipeError::Cancelled) return InferenceStatus::Cancelled;
    return InferenceStatus::Unavailable;
}
void check_peer(HANDLE pipe, bool server) {
    ULONG pid = 0;
    need((server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)) != 0);
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); need(process.valid());
    need(user_sid(process.get()) == user_sid());
}
void check_cancel(HANDLE cancel) { need(WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0, InferenceStatus::Cancelled); }
NativeHandle launch_hidden(const std::filesystem::path& program, std::wstring command) {
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    need(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, program.parent_path().c_str(), &startup, &process) != 0);
    CloseHandle(process.hThread); return NativeHandle(process.hProcess);
}
}
std::wstring inference_pipe_name(std::wstring_view token) {
    need(!token.empty() && token.size() <= 64);
    for (const auto ch : token) need((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-');
    return L"\\\\.\\pipe\\nkkz-ime-v2-" + user_sid() + L"-" + std::wstring(token);
}
NativeHandle claim_inference_server(std::wstring_view token) {
    (void)inference_pipe_name(token);
    SecurityDescriptor descriptor(owner_only_sddl(user_sid()));
    const auto name = L"Local\\nkkz-ime-start-v2-" + user_sid() + L"-" + std::wstring(token);
    NativeHandle claim(CreateEventW(descriptor.attributes(), TRUE, FALSE, name.c_str()));
    const auto error = GetLastError(); need(claim.valid());
    if (error == ERROR_ALREADY_EXISTS) return NativeHandle{};
    return claim; // A named event is just a lifetime marker; no thread-owned lock.
}
InferenceResponse inference_exchange(std::wstring_view token, const InferenceRequest& request, ULONGLONG deadline, HANDLE cancel) {
    const auto name = inference_pipe_name(token);
    try {
        const auto pipe = open_pipe(name, deadline, cancel);
        check_peer(pipe.get(), false);
        send_frame(pipe.get(), encode_request(request), inference_max_line, deadline, cancel);
        const auto response = receive_frame(pipe.get(), inference_max_line, deadline, cancel);
        send_receipt(pipe.get(), deadline, cancel);
        return decode_response(response, request);
    } catch (const InferenceFailure&) { throw; }
    catch (const PipeFailure& failure) { throw InferenceFailure(status(failure)); }
    catch (...) { throw InferenceFailure(InferenceStatus::InvalidResponse); }
}
InferenceResponse inference_exchange_launching(std::wstring_view token, const InferenceRequest& request, ULONGLONG deadline, HANDLE cancel,
    const InferenceLauncher& launch, DWORD idle_ms) {
    std::optional<NativeHandle> process;
    for (;;) {
        try { return inference_exchange(token, request, deadline, cancel); }
        catch (const InferenceFailure& failure) { if (failure.status != InferenceStatus::Unavailable || !launch) throw; }
        const auto now = GetTickCount64(); need(now < deadline, InferenceStatus::Timeout);
        if (!process) { check_cancel(cancel); process.emplace(launch(idle_ms)); }
        if (WaitForSingleObject(cancel, static_cast<DWORD>((std::min)(deadline - now, ULONGLONG{10}))) == WAIT_OBJECT_0)
            throw InferenceFailure(InferenceStatus::Cancelled);
    }
}
NativeHandle launch_dummy_server(const std::filesystem::path& program, std::wstring_view token, DWORD delay_ms, DWORD idle_ms) {
    inference_pipe_name(token); need(program.is_absolute() && program.native().find(L'"') == std::wstring::npos && delay_ms <= 10000 && idle_ms <= inference_max_idle_ms);
    return launch_hidden(program, L"\"" + program.native() + L"\" --dummy --pipe-token " + std::wstring(token) +
        L" --delay-ms " + std::to_wstring(delay_ms) + L" --idle-ms " + std::to_wstring(idle_ms));
}
int serve_dummy_inference(std::wstring_view token, DWORD delay_ms, DWORD idle_ms) {
    return serve_inference(token,[delay_ms](const InferenceRequest& request,const std::function<bool()>& cancelled) {
        const auto ready = GetTickCount64()+delay_ms;
        while (GetTickCount64() < ready) { need(!cancelled(),InferenceStatus::Cancelled); Sleep(5); }
        need(request.operation == InferenceOperation::Convert,InferenceStatus::InvalidResponse);
        InferenceResponse response{request.id,{U"試験〈"+request.reading+U"〉"}};
        if (request.count > 1) response.candidates.push_back(U"別候補〈"+request.reading+U"〉");
        return response;
    },idle_ms,delay_ms+1500);
}
NativeHandle launch_zenz_server(const std::filesystem::path& program, const std::filesystem::path& model,
    std::wstring_view token, unsigned threads, DWORD timeout_ms, DWORD idle_ms, ULONGLONG deadline, HANDLE cancel) {
    const auto check_launch = [&] {
        if (cancel) check_cancel(cancel);
        if (deadline) need(GetTickCount64() < deadline, InferenceStatus::Timeout);
    };
    check_launch();
    inference_pipe_name(token);
    for (const auto& path : {program,model}) need(path.is_absolute() && !path.filename().empty() && path.native().find_first_of(L"\"\r\n") == std::wstring::npos);
    need(threads >= 1 && threads <= 64 && timeout_ms >= 1 && timeout_ms <= 10000 && idle_ms >= 100 && idle_ms <= inference_max_idle_ms);
    // This worker/broker owns the resolved executable. A caller's architecture
    // (e.g. an x86 TSF companion) does not identify the server's ISA contract.
    require_zenz_cpu(current_cpu_features(), zenz_server_cpu_contract(program));
    check_launch();
    return launch_hidden(program, L"\""+program.native()+L"\" --model \""+model.native()+L"\" --pipe-token "+std::wstring(token)+
        L" --threads "+std::to_wstring(threads)+L" --timeout-ms "+std::to_wstring(timeout_ms)+L" --idle-ms "+std::to_wstring(idle_ms));
}
int serve_inference(std::wstring_view token, const InferenceHandler& handler, DWORD idle_ms, DWORD timeout_ms) {
    need(idle_ms >= 100 && idle_ms <= inference_max_idle_ms && timeout_ms >= 1 && timeout_ms <= 15000);
    const auto name = inference_pipe_name(token);
    SecurityDescriptor descriptor(owner_only_sddl(user_sid()));
    // Create all instances before serving so another process cannot take over
    // the name while all listeners are busy.
    std::vector<NativeHandle> pipes;
    for (int i = 0; i < 4; ++i) {
        pipes.push_back(create_pipe_instance(name, i == 0, 4, 16384, descriptor.attributes())); need(pipes.back().valid());
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
                const auto request = decode_request(receive_frame(pipe, inference_max_line, GetTickCount64() + 1000, stop.get()));
                const auto deadline = GetTickCount64()+timeout_ms;
                auto cancelled = [&] {
                    const auto now = GetTickCount64(); activity = now;
                    return now >= deadline || !pipe_peer_open(pipe);
                };
                const auto response = handler(request,cancelled);
                need(!cancelled(),InferenceStatus::Cancelled);
                send_frame(pipe, encode_response(response), inference_max_line, GetTickCount64() + 1000, stop.get());
                receive_receipt(pipe, GetTickCount64() + 1000, stop.get());
                activity = GetTickCount64();
            } catch (...) { /* Drop malformed/disconnected clients without logging their text. */ }
            DisconnectNamedPipe(pipe);
        }
    };
    std::vector<std::jthread> workers;
    for (const auto& pipe : pipes) workers.emplace_back(serve, pipe.get());
    return 0; // jthread destructors join before the handles and ACL are released.
}
}
