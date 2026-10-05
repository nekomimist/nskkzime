#include "learning_client.h"
#include "learning_ipc.h"
#include "dictionary_settings.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
int module_anchor = 0;
HMODULE pin_module() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&module_anchor), &module))
        throw std::runtime_error("cannot pin learning worker module");
    return module;
}
std::filesystem::path helper_path() {
    const auto module = pin_module(); wchar_t path[32768]{};
    const auto size = GetModuleFileNameW(module, path, ARRAYSIZE(path)); FreeLibrary(module);
    if (!size || size == ARRAYSIZE(path)) throw std::runtime_error("cannot locate dictionary helper");
    return std::filesystem::path(path).parent_path() / L"nskkzime_dictionary_server.exe";
}
}
struct LearningDictionary::State {
    std::shared_ptr<const Dictionary> base;
    std::atomic<std::shared_ptr<const UserDictionary>> snapshot{std::make_shared<UserDictionary>()};
    std::filesystem::path file, server;
    std::string client = learning_client_id();
    std::uint64_t sequence = 0;
    std::atomic<std::uint64_t> dropped{0};
    NativeHandle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex;
    std::deque<LearningRequest> queue;
    bool stop = false;
    std::wstring report = L"学習: 接続待ち";
    std::wstring skipped;
    void update_status(std::wstring value) { const std::lock_guard lock(mutex); report = std::move(value); }
    void run() noexcept {
        try {
            std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
            ULONGLONG last_launch = 0, stop_deadline = 0;
            for (;;) {
                std::optional<LearningRequest> request;
                {
                    const std::lock_guard lock(mutex);
                    if (stop && !stop_deadline) stop_deadline = GetTickCount64() + 5000;
                    if (stop && (queue.empty() || GetTickCount64() >= stop_deadline)) break;
                    if (!queue.empty()) request = queue.front();
                }
                try {
                    if (request) {
                        const auto response = learning_exchange(file, encode_learning(*request));
                        if (response != "OKAY") throw std::runtime_error("dictionary update rejected");
                    }
                    const auto response = learning_exchange(file, snapshot_request(revision));
                    if (response.starts_with("DATA")) {
                        auto next = std::make_shared<UserDictionary>(UserDictionary::parse(std::string_view(response).substr(4)));
                        revision = next->revision(); snapshot.store(std::move(next));
                    } else if (response != "SAME") throw std::runtime_error("dictionary snapshot rejected");
                    // Keep the event pending until its acknowledged generation
                    // has been published. Engine caches can then safely retire,
                    // even if the word was deleted before the next lookup.
                    if (request) { const std::lock_guard lock(mutex); queue.pop_front(); }
                    update_status(L"学習: 保存済み・同期済み（世代 " + std::to_wstring(revision) + L"）");
                } catch (const std::exception&) {
                    update_status(L"学習: 接続または保存に失敗。未保存分を再試行中（通常の変換は利用できます）");
                    const auto now = GetTickCount64();
                    if (now - last_launch > 5000) {
                        last_launch = now;
                        try { const auto process = launch_dictionary_server(server, file); } catch (...) {}
                    }
                    WaitForSingleObject(wake.get(), 250);
                    continue;
                }
                {
                    const std::lock_guard lock(mutex);
                    if (!queue.empty()) continue;
                }
                WaitForSingleObject(wake.get(), 1000);
            }
        } catch (...) { /* Background failure must never unwind through Windows. */ }
    }
    struct Work { std::shared_ptr<State> state; HMODULE module; };
    static void CALLBACK callback(PTP_CALLBACK_INSTANCE instance, void* raw) {
        std::unique_ptr<Work> work(static_cast<Work*>(raw));
        FreeLibraryWhenCallbackReturns(instance, work->module);
        CallbackMayRunLong(instance);
        work->state->run();
    }
};
LearningDictionary::LearningDictionary(std::shared_ptr<const Dictionary> base, std::filesystem::path file, std::filesystem::path server)
    : state_(std::make_shared<State>()) {
    if (!state_->wake.valid()) throw std::runtime_error("cannot allocate learning worker event");
    state_->base = std::move(base); state_->file = std::move(file); state_->server = std::move(server);
    auto work = std::make_unique<State::Work>(); work->state = state_; work->module = pin_module();
    if (!TrySubmitThreadpoolCallback(State::callback, work.get(), nullptr)) {
        FreeLibrary(work->module); throw std::runtime_error("cannot start learning worker");
    }
    work.release();
}
LearningDictionary::~LearningDictionary() {
    { const std::lock_guard lock(state_->mutex); state_->stop = true; }
    SetEvent(state_->wake.get()); // No join in a TSF callback or DLL unload.
}
std::vector<Candidate> LearningDictionary::lookup(const LookupKey& key, std::u32string_view okuri) const {
    const auto snapshot = state_->snapshot.load();
    return DictionarySet({snapshot, state_->base}).lookup(key, okuri);
}
bool LearningDictionary::registration_pending(const LearningEvent& event) const {
    const std::lock_guard lock(state_->mutex);
    return std::any_of(state_->queue.begin(), state_->queue.end(), [&](const LearningRequest& request) {
        return request.event.key == event.key && request.event.okuri == event.okuri && request.event.candidate.text == event.candidate.text;
    });
}
void LearningDictionary::learn(const LearningEvent& event) const noexcept {
    try {
        const std::lock_guard lock(state_->mutex);
        if (state_->queue.size() >= 128) { ++state_->dropped; return; }
        LearningRequest request{state_->client, state_->sequence + 1, event}; validate_learning(request);
        state_->queue.push_back(std::move(request)); ++state_->sequence;
        state_->skipped.clear();
        SetEvent(state_->wake.get());
    } catch (...) { ++state_->dropped; /* Never turn an applied key into a failed key. */ }
}
void LearningDictionary::skipped(std::wstring reason) const noexcept {
    try { const std::lock_guard lock(state_->mutex); state_->skipped = std::move(reason); } catch (...) {}
}
std::wstring LearningDictionary::status() const {
    const std::lock_guard lock(state_->mutex);
    return state_->report + L" / 未保存 " + std::to_wstring(state_->queue.size()) + L"件 / 受付不可 " +
        std::to_wstring(state_->dropped.load()) + L"件\r\nユーザー辞書: " + state_->file.native() +
        (state_->skipped.empty() ? L"" : L"\r\n学習省略: " + state_->skipped);
}
std::shared_ptr<const Dictionary> with_learning(std::shared_ptr<const Dictionary> base) {
    if (const auto setting = environment_value(L"NSKKZIME_LEARNING"); setting && *setting == L"0") return base;
    return std::make_shared<LearningDictionary>(std::move(base), learning_file_path(), helper_path());
}
void publish_learning(const std::shared_ptr<const Dictionary>& dictionary, const LearningEvent& event) noexcept {
    if (const auto* learning = dynamic_cast<const LearningDictionary*>(dictionary.get())) learning->learn(event);
}
std::filesystem::path learning_helper_path() { return helper_path(); }
} // namespace nskkzime::windows
