#pragma once
#include "learning_ipc.h"
#include "nskkzime/inference.h"
#include <stdexcept>
#include <functional>
namespace nskkzime::windows {
enum class InferenceStatus { Ok, Cancelled, Timeout, Unavailable, InvalidResponse };
struct InferenceFailure : std::runtime_error {
    explicit InferenceFailure(InferenceStatus value) : std::runtime_error("inference IPC failed"), status(value) {}
    InferenceStatus status;
};
// All I/O and process operations below are worker-only. One request per
// connection; closing it cancels the server's work. No document text logging.
std::wstring inference_pipe_name(std::wstring_view token);
// Hold through model load and server lifetime; invalid means another starter owns it.
NativeHandle claim_inference_server(std::wstring_view token);
InferenceResponse inference_exchange(std::wstring_view token, const InferenceRequest&, ULONGLONG deadline, HANDLE cancel);
NativeHandle launch_dummy_server(const std::filesystem::path&, std::wstring_view token, DWORD delay_ms = 150, DWORD idle_ms = 10000);
NativeHandle launch_zenz_server(const std::filesystem::path& program, const std::filesystem::path& model,
    std::wstring_view token, unsigned threads = 4, DWORD timeout_ms = 1000, DWORD idle_ms = 10000);
int serve_dummy_inference(std::wstring_view token, DWORD delay_ms, DWORD idle_ms);
// Servers from older installs reject --idle-ms above the legacy limit.
inline constexpr DWORD inference_max_idle_ms = 86'400'000, inference_legacy_idle_ms = 60'000;
using InferenceLauncher = std::function<NativeHandle(DWORD idle_ms)>;
// inference_exchange that launches the server while it is unavailable, until the deadline.
// A server rejecting idle_ms (exit code 2) is relaunched once with the legacy limit.
InferenceResponse inference_exchange_launching(std::wstring_view token, const InferenceRequest&, ULONGLONG deadline, HANDLE cancel,
    const InferenceLauncher&, DWORD idle_ms);
using InferenceHandler = std::function<InferenceResponse(const InferenceRequest&, const std::function<bool()>&)>;
int serve_inference(std::wstring_view token, const InferenceHandler&, std::string_view backend, DWORD idle_ms, DWORD timeout_ms);
}
