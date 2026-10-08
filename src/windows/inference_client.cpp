#include "inference_client.h"
#include "container_runtime.h"
#include <nlohmann/json.hpp>
#include "dictionary_settings.h"
#include "cpu_requirements.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <bcrypt.h>
#include <mutex>
#include <optional>

namespace nskkzime::windows {
namespace {
int anchor;
thread_local const void* status_owner = nullptr;
thread_local const wchar_t* status_text = L"推論: オフ";
thread_local const InferenceClient* logging_client = nullptr;
HMODULE module(bool pin) {
    HMODULE result = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | (pin ? 0 : GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT),
        reinterpret_cast<LPCWSTR>(&anchor), &result)) throw std::runtime_error("cannot locate inference module");
    return result;
}
std::wstring zenz_token(const InferenceConfig& config) {
    // Same settings share one server across activations/processes. A different
    // model/server/limit set cannot accidentally reuse an incompatible model.
    const auto key = L"zenz-v1\n" + config.model.lexically_normal().native() + L"\n" + config.server.lexically_normal().native() +
        L"\n" + std::to_wstring(config.threads) + L"\n" + std::to_wstring((std::max)(config.timeout_ms,static_cast<DWORD>(config.policy.score_timeout_ms)));
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(key.data())),
        static_cast<ULONG>(key.size()*sizeof(wchar_t)),digest,sizeof(digest)) < 0) throw std::runtime_error("cannot name inference server");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring token;
    for (const auto byte : digest) { token += hex[byte >> 4]; token += hex[byte & 15]; }
    return token;
}
}
struct InferenceClient::State {
    struct Job {
        InferenceRequest request;
        ULONGLONG deadline = 0;
        NativeHandle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    };
    InferenceConfig config;
    NativeHandle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex;
    bool stop = false;
    std::shared_ptr<Job> pending;
    std::optional<InferenceCompletion> completion;
    std::unique_ptr<InputLog> logger;
    std::deque<InputLogRecord> records;
    std::atomic<InputLogStatus> log_status{InputLogStatus::Off};
    std::atomic<unsigned> log_dropped{0};
    void run() noexcept {
        for (;;) {
            std::shared_ptr<Job> job;
            std::optional<InputLogRecord> record;
            {
                const std::lock_guard lock(mutex); if (stop) return; job = pending;
                if (!job && !records.empty()) { record = std::move(records.front()); records.pop_front(); }
            }
            if (!job) {
                if (record) {
                    if (config.container) {
                        try {
                            nlohmann::json value{{"request", encode_request(record->request)},
                                {"response", record->response ? nlohmann::json(encode_response(*record->response)) : nlohmann::json(nullptr)},
                                {"status", static_cast<int>(record->status)}};
                            const auto bytes = nlohmann::json::to_cbor(value);
                            NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                            const auto response = container_exchange("LOG1" + config.container_generation + std::string(bytes.begin(), bytes.end()),
                                GetTickCount64() + 1000, cancel.get());
                            const auto status = response == "OFF" ? 0 : std::stoi(response);
                            if (status < 0 || status > static_cast<int>(InputLogStatus::Unavailable)) throw std::runtime_error("invalid log status");
                            log_status = static_cast<InputLogStatus>(status);
                        } catch (...) { log_status = InputLogStatus::Unavailable; }
                    } else log_status = logger->write(*record);
                    continue;
                }
                WaitForSingleObject(wake.get(), INFINITE); continue;
            }
            InferenceCompletion result; result.id = job->request.id; result.deadline = job->deadline;
            try {
                if (config.container) {
                    result.response = decode_response(container_exchange("INF1" + config.container_generation + encode_request(job->request), job->deadline, job->cancel.get()), job->request);
                    result.status = InferenceStatus::Ok;
                } else {
                bool launched = false;
                for (;;) {
                    try {
                        result.response = inference_exchange(config.token, job->request, job->deadline, job->cancel.get());
                        result.status = InferenceStatus::Ok; break;
                    } catch (const InferenceFailure& failure) {
                        if (failure.status != InferenceStatus::Unavailable || config.server.empty()) throw;
                        if (GetTickCount64() >= job->deadline) throw InferenceFailure(InferenceStatus::Timeout);
                        if (!launched) {
                            const auto process = config.model.empty() ? launch_dummy_server(config.server,config.token,config.delay_ms) :
                                launch_zenz_server(config.server,config.model,config.token,config.threads,(std::max)(config.timeout_ms,static_cast<DWORD>(config.policy.score_timeout_ms)));
                            launched = true;
                        }
                        if (WaitForSingleObject(job->cancel.get(), 10) == WAIT_OBJECT_0) throw InferenceFailure(InferenceStatus::Cancelled);
                    }
                }
                }
            } catch (const InferenceFailure& failure) { result.status = failure.status; }
            catch (...) { result.status = GetTickCount64() >= job->deadline ? InferenceStatus::Timeout : InferenceStatus::Unavailable; }
            { const std::lock_guard lock(mutex);
                if (pending == job) { pending.reset(); if (!stop && WaitForSingleObject(job->cancel.get(), 0) != WAIT_OBJECT_0) completion = std::move(result); }
            }
        }
    }
    struct Work { std::shared_ptr<State> state; HMODULE module; };
    static void CALLBACK callback(PTP_CALLBACK_INSTANCE instance, void* raw) {
        std::unique_ptr<Work> work(static_cast<Work*>(raw));
        FreeLibraryWhenCallbackReturns(instance, work->module); CallbackMayRunLong(instance); work->state->run();
    }
};
InferenceClient::InferenceClient(InferenceConfig config, Callback callback) : state_(std::make_shared<State>()), callback_(std::move(callback)) {
    if (!state_->wake.valid() || config.timeout_ms == 0 || config.timeout_ms > 10000) throw std::invalid_argument("invalid inference configuration");
    state_->config = std::move(config); owner_ = module(false);
    if (state_->config.log_input) {
        if (!state_->config.container) {
            if (state_->config.log_directory.empty()) throw std::invalid_argument("input log directory required");
            state_->logger = std::make_unique<InputLog>(state_->config.log_directory);
        }
        state_->log_status = InputLogStatus::Ready;
    }
    wchar_t name[96]{}; swprintf_s(name,L"nskkzime-inference-%lu-%p",GetCurrentThreadId(),this); class_name_ = name;
    WNDCLASSW cls{}; cls.lpfnWndProc = window_proc; cls.hInstance = owner_; cls.lpszClassName = class_name_.c_str();
    if (!RegisterClassW(&cls)) throw std::runtime_error("cannot create inference dispatcher");
    try {
        window_ = CreateWindowExW(0,class_name_.c_str(),L"",0,0,0,0,0,HWND_MESSAGE,nullptr,owner_,this);
        if (!window_ || !SetTimer(window_,1,20,nullptr)) throw std::runtime_error("cannot start inference dispatcher");
        auto work = std::make_unique<State::Work>(); work->state = state_; work->module = module(true);
        if (!TrySubmitThreadpoolCallback(State::callback,work.get(),nullptr)) { FreeLibrary(work->module); throw std::runtime_error("cannot start inference worker"); }
        work.release();
        logging_client = this;
    } catch (...) { if (window_) DestroyWindow(window_); UnregisterClassW(class_name_.c_str(),owner_); throw; }
}
InferenceClient::~InferenceClient() {
    if (logging_client == this) logging_client = nullptr;
    if (window_) DestroyWindow(window_); UnregisterClassW(class_name_.c_str(),owner_);
    { const std::lock_guard lock(state_->mutex); state_->stop = true; if (state_->pending) SetEvent(state_->pending->cancel.get()); state_->completion.reset(); state_->records.clear(); }
    SetEvent(state_->wake.get()); // No join/pipe wait in a TSF callback.
}
ULONGLONG InferenceClient::submit(InferenceRequest request, DWORD timeout_ms) {
    if (timeout_ms > 10000) throw std::invalid_argument("invalid request timeout");
    auto job = std::make_shared<State::Job>(); if (!job->cancel.valid()) throw std::runtime_error("cannot create cancellation event");
    job->request = std::move(request); job->deadline = GetTickCount64() + (timeout_ms ? timeout_ms : state_->config.timeout_ms);
    { const std::lock_guard lock(state_->mutex);
        if (state_->pending) SetEvent(state_->pending->cancel.get());
        state_->pending = job; state_->completion.reset();
    }
    SetEvent(state_->wake.get()); return job->deadline;
}
void InferenceClient::cancel() noexcept {
    const std::lock_guard lock(state_->mutex);
    if (state_->pending) SetEvent(state_->pending->cancel.get());
    state_->pending.reset(); state_->completion.reset();
}
void InferenceClient::record_input(const InferenceRequest& request, const InferenceResponse* response, InferenceStatus status) noexcept {
    if (!state_->config.log_input) return;
    try {
        const std::lock_guard lock(state_->mutex);
        if (state_->stop || state_->log_status == InputLogStatus::Full || state_->log_status == InputLogStatus::Unavailable) return;
        if (state_->records.size() >= 8) { ++state_->log_dropped; return; }
        state_->records.push_back({request, response ? std::optional<InferenceResponse>(*response) : std::nullopt, status});
        SetEvent(state_->wake.get());
    } catch (...) { ++state_->log_dropped; }
}
bool InferenceClient::input_log_enabled() const noexcept { return state_->config.log_input; }
std::wstring InferenceClient::input_log_status() const {
    auto text = std::wstring(input_log_status_text(state_->log_status.load()));
    const auto dropped = state_->log_dropped.load();
    if (dropped) text += L" / 保存待ちから除外: " + std::to_wstring(dropped);
    return text;
}
void InferenceClient::poll() {
    std::optional<InferenceCompletion> result;
    { const std::lock_guard lock(state_->mutex); result = std::move(state_->completion); state_->completion.reset(); }
    if (result) {
        if (result->status == InferenceStatus::Ok && GetTickCount64() >= result->deadline) { result->status = InferenceStatus::Timeout; result->response = {}; }
        const auto callback = callback_; if (callback) callback(std::move(*result)); // May destroy this object.
    }
}
LRESULT CALLBACK InferenceClient::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<InferenceClient*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<InferenceClient*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
    if (message == WM_TIMER && wparam == 1 && self) { try { self->poll(); } catch (...) {} return 0; }
    return DefWindowProcW(window,message,wparam,lparam);
}
std::optional<InferenceConfig> current_inference_config() {
    InferenceConfig config;
    if (in_app_container()) {
        const auto remote = container_configuration();
        if (!remote || !remote->configuration.settings.zenz.enabled) return {};
        const auto& settings = remote->configuration.settings.zenz;
        config.container = true; config.timeout_ms = settings.timeout_ms; config.policy = settings.policy;
        config.container_generation = remote->generation;
        config.log_input = settings.log_input; config.allow_unknown_context = settings.allow_unknown_context;
        return config;
    }
    if (const auto dummy = environment_value(L"NSKKZIME_INFERENCE_DUMMY")) {
        if (*dummy == L"0") return {};
        if (*dummy != L"1") throw std::invalid_argument("invalid inference override");
        wchar_t path[32768]{}; const auto size = GetModuleFileNameW(module(false),path,ARRAYSIZE(path));
        if (!size || size == ARRAYSIZE(path)) throw std::runtime_error("cannot locate inference server");
        config.server = std::filesystem::path(path).parent_path() / L"nkkz-ime-server.exe";
        config.policy.long_min_length = 0; // Existing dummy fixtures exercise fallback only.
    } else {
        const auto settings = current_dictionary_configuration().settings.zenz;
        if (!settings.enabled) return {};
        require_zenz_cpu(current_cpu_features());
        validate_zenz_settings(settings); // Do not open a model or run a process on the TSF thread.
        config.server = settings.server; config.model = settings.model;
        config.threads = settings.threads; config.timeout_ms = settings.timeout_ms;
        config.policy = settings.policy;
        config.log_input = settings.log_input; config.log_directory = settings.log_directory;
        config.allow_unknown_context = settings.allow_unknown_context;
    }
    for (const auto item : {std::pair{L"NSKKZIME_INFERENCE_DELAY_MS",&config.delay_ms},std::pair{L"NSKKZIME_INFERENCE_TIMEOUT_MS",&config.timeout_ms}})
        if (const auto value = environment_value(item.first)) {
            if (value->empty() || !std::all_of(value->begin(),value->end(),[](auto ch) { return ch >= L'0' && ch <= L'9'; })) throw std::invalid_argument("invalid inference limit");
            const auto parsed = std::stoul(*value); if (parsed > 10000) throw std::invalid_argument("inference limit exceeds 10000 ms"); *item.second = parsed;
        }
    if (!config.timeout_ms) throw std::invalid_argument("inference timeout must be positive");
    if (const auto token = environment_value(L"NSKKZIME_INFERENCE_TOKEN")) config.token = *token;
    else if (!config.model.empty()) config.token = zenz_token(config);
    else { const auto id = learning_client_id(); config.token.assign(id.begin(),id.end()); }
    (void)inference_pipe_name(config.token);
    return config;
}
void set_inference_status(const void* owner, const wchar_t* text) noexcept {
    status_owner = owner; status_text = text;
    // Explicit development-only trace. Contains fixed status messages only,
    // never a reading, document text, candidates, or model request/response.
    try {
        const auto path = environment_value(L"NSKKZIME_STATUS_TRACE");
        if (!path || !std::filesystem::path(*path).is_absolute()) return;
        NativeHandle file(CreateFileW(path->c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER length{};
        if (!file.valid() || GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileSizeEx(file.get(), &length) || length.QuadPart >= 65536) return;
        const auto line = utf8_message(std::to_wstring(GetCurrentProcessId()) + L"/" + std::to_wstring(GetCurrentThreadId()) +
            L" " + std::to_wstring(GetTickCount64()) + L" " + text + L"\n");
        DWORD written = 0; WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    } catch (...) { }
}
void clear_inference_status(const void* owner) noexcept { if (status_owner == owner) { status_owner = nullptr; status_text = L"推論: オフ"; } }
std::wstring inference_status() {
    return std::wstring(status_text) + L"\r\n" + (logging_client ? logging_client->input_log_status() : L"入力ログ: オフ");
}
}
