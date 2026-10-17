#pragma once
#include "inference_ipc.h"
#include "input_log.h"
#include "nskkzime/inference_policy.h"
#include <functional>
#include <memory>
#include <optional>
namespace nskkzime::windows {
struct RuntimeConfiguration;
struct ContainerConfiguration;
struct InferenceConfig {
    std::wstring token;
    std::filesystem::path server; // Empty: connect only. Always an explicit executable path.
    DWORD timeout_ms = 1000;
    std::filesystem::path model;
    unsigned int threads = 4;
    DWORD idle_ms = 10000; // Server lifetime without requests; saved zenz settings use a longer default.
    InferencePolicy policy;
    bool log_input = false;
    std::filesystem::path log_directory;
    bool allow_unknown_context = false;
    bool container = false;
    std::string container_generation;
};
struct InferenceCompletion {
    InferenceStatus status = InferenceStatus::Unavailable;
    std::uint64_t id = 0;
    ULONGLONG deadline = 0;
    InferenceResponse response;
};
// UI thread owns this object. Only immutable strings/numbers cross to the
// worker. Completion runs on the creating thread's message pump (or poll()).
class InferenceClient {
public:
    using Callback = std::function<void(InferenceCompletion)>;
    InferenceClient(InferenceConfig, Callback);
    ~InferenceClient();
    InferenceClient(const InferenceClient&) = delete;
    InferenceClient& operator=(const InferenceClient&) = delete;
    ULONGLONG submit(InferenceRequest, DWORD timeout_ms = 0); // Zero uses the generation timeout.
    // Same owning UI thread as submit()/destruction. Waits at most the remaining
    // wait_ms budget; startup and I/O run on a separate, lifetime-pinned callback.
    // Timeout cancels that request and yields nullopt; the caller can use submit().
    std::optional<InferenceResponse> exchange_now(const InferenceRequest&, DWORD wait_ms) const noexcept;
    void cancel() noexcept;
    void poll();
    // Called only after TSF has revalidated the current ordinary input field.
    // Queue-only; neither disk I/O nor JSON encoding runs on the input thread.
    void record_input(const InferenceRequest&, const InferenceResponse*, InferenceStatus) noexcept;
    bool input_log_enabled() const noexcept;
    std::wstring input_log_status() const;
private:
    struct State;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    std::shared_ptr<State> state_;
    Callback callback_;
    HWND window_ = nullptr;
    HINSTANCE owner_ = nullptr;
    std::wstring class_name_;
};
// Off by default; enabled by the saved zenz settings.
std::optional<InferenceConfig> current_inference_config();
std::optional<InferenceConfig> inference_config(const RuntimeConfiguration&);
std::optional<InferenceConfig> inference_config(const ContainerConfiguration&);
void set_inference_status(const void* owner, const wchar_t*) noexcept;
void clear_inference_status(const void* owner) noexcept;
std::wstring inference_status();
}
