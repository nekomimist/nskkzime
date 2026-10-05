#pragma once
#include "inference_ipc.h"
#include <functional>
#include <memory>
#include <optional>
namespace nskkzime::windows {
struct InferenceConfig {
    std::wstring token;
    std::filesystem::path dummy_server; // Empty: connect only. Never run arbitrary commands.
    DWORD delay_ms = 150;
    DWORD timeout_ms = 1000;
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
    ULONGLONG submit(InferenceRequest);
    void cancel() noexcept;
    void poll();
private:
    struct State;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    std::shared_ptr<State> state_;
    Callback callback_;
    HWND window_ = nullptr;
    HINSTANCE owner_ = nullptr;
    std::wstring class_name_;
};
// Development configuration only. Ordinary activation keeps inference off.
std::optional<InferenceConfig> dummy_inference_config();
void set_inference_status(const void* owner, const wchar_t*) noexcept;
void clear_inference_status(const void* owner) noexcept;
std::wstring inference_status();
}
