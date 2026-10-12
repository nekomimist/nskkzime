#include "learning_client.h"
#include "app_container.h"
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
}
struct LearningDictionary::State {
    std::shared_ptr<const Dictionary> base;
    std::atomic<std::shared_ptr<const UserDictionary>> snapshot{std::make_shared<UserDictionary>()};
    std::filesystem::path file;
    std::string container_generation;
    // Readers outside an AppContainer open the change events directly. An
    // AppContainer cannot, so the broker waits for the change on its behalf.
    bool remote_wait = false;
    std::string client = learning_client_id();
    std::uint64_t sequence = 0;
    std::atomic<std::uint64_t> dropped{0};
    NativeHandle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    NativeHandle interrupt{CreateEventW(nullptr, TRUE, FALSE, nullptr)}; // Ends a remote wait for a write or stop.
    std::mutex mutex;
    std::deque<LearningRequest> queue;
    bool stop = false;
    std::wstring report = L"学習: 接続待ち";
    std::wstring skipped;
    // Call with mutex held. Replay only this small scope, never copy the whole
    // user dictionary on the key path. Pending forgets must not resurrect words.
    std::vector<Candidate> scope(const LookupKey& key, std::u32string_view okuri) const {
        auto result = snapshot.load()->lookup_scope(key, okuri);
        for (const auto& request : queue) {
            if (request.event.key != key || request.event.okuri != okuri) continue;
            std::erase_if(result, [&](const Candidate& item) { return item.text == request.event.candidate.text; });
            if (request.action == DictionaryAction::Learn) {
                result.insert(result.begin(), request.event.candidate);
                if (result.size() > 32) result.resize(32);
            }
        }
        return result;
    }
    void update_status(std::wstring value) { const std::lock_guard lock(mutex); report = std::move(value); }
    struct StaleSettings : std::runtime_error { StaleSettings() : std::runtime_error("stale settings") {} };
    // Broker-backed workers only; others read and write the file themselves.
    std::string exchange(std::string_view body, DWORD timeout = 1500, HANDLE cancel = nullptr) {
        NativeHandle local(cancel ? INVALID_HANDLE_VALUE : CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!cancel) cancel = local.get();
        auto response = container_exchange("LRN1" + container_generation + std::string(body), GetTickCount64() + timeout, cancel);
        if (response == "STAL") throw StaleSettings();
        return response;
    }
    // Desktop readers use the saved file directly. Other readers ask the
    // broker; with wait, it holds the request until a newer revision is saved.
    std::shared_ptr<UserDictionary> refresh(std::uint64_t revision, bool wait) {
        if (container_generation.empty()) {
            if (user_dictionary_revision(file) == revision) return nullptr;
            return std::make_shared<UserDictionary>(read_user_dictionary(file));
        }
        const auto response = wait ? exchange(learning_wait_request(revision), learning_wait_ms + 5000, interrupt.get()) :
            exchange(snapshot_request(revision));
        if (response == "SAME") return nullptr;
        if (!response.starts_with("DATA")) throw std::runtime_error("dictionary snapshot rejected");
        return std::make_shared<UserDictionary>(UserDictionary::parse(std::string_view(response).substr(4)));
    }
    NativeHandle listen(std::uint64_t revision) const noexcept {
        try { return learning_change_event(file, revision); } catch (...) { return NativeHandle{}; }
    }
    void run() noexcept {
        try {
            constexpr std::uint64_t none = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t revision = none;
            ULONGLONG stop_deadline = 0; DWORD retry_delay = 250;
            for (;;) {
                // Reset before reading the queue, so a later enqueue() or stop ends the wait at once.
                if (remote_wait) ResetEvent(interrupt.get());
                std::optional<LearningRequest> request;
                {
                    const std::lock_guard lock(mutex);
                    if (stop && !stop_deadline) stop_deadline = GetTickCount64() + 5000;
                    if (stop && (queue.empty() || GetTickCount64() >= stop_deadline)) break;
                    if (!queue.empty()) request = queue.front();
                }
                // Listen before reading the file, so a later change cannot be missed.
                NativeHandle changed(!remote_wait && revision != none ? listen(revision) : NativeHandle{});
                const bool wait = remote_wait && !request && revision != none;
                try {
                    std::shared_ptr<UserDictionary> next;
                    if (request && container_generation.empty()) {
                        // The saved result is the next snapshot; no second read.
                        auto saved = std::make_shared<UserDictionary>(commit_learning(file, *request));
                        if (saved->revision() != revision) next = std::move(saved);
                    } else {
                        if (request && exchange(encode_learning(*request)) != "OKAY") throw std::runtime_error("dictionary update rejected");
                        next = refresh(revision, wait);
                    }
                    const bool published = next != nullptr;
                    // Keep the event pending until its acknowledged generation
                    // has been published. Engine caches can then safely retire,
                    // even if the word was deleted before the next lookup.
                    {
                        const std::lock_guard lock(mutex);
                        if (next) { revision = next->revision(); snapshot.store(std::move(next)); }
                        if (request) queue.pop_front();
                    }
                    update_status(L"学習: 保存済み・同期済み（世代 " + std::to_wstring(revision) + L"）");
                    retry_delay = 250;
                    if (published && !remote_wait) continue; // Listen on the new revision's event.
                } catch (const StaleSettings&) {
                    // This generation can no longer save. The next activation
                    // bootstraps the new settings and replaces this worker.
                    update_status(L"学習: 設定が変わったため保存を停止中（次に入力を始めると新しい設定を読み込みます）");
                    { const std::lock_guard lock(mutex); if (stop) break; }
                    WaitForSingleObject(wake.get(), 60000);
                    continue;
                } catch (const std::exception&) {
                    // A write or stop closed the remote wait; not a failure.
                    if (wait && WaitForSingleObject(interrupt.get(), 0) == WAIT_OBJECT_0) continue;
                    // A busy lock, a corrupt or newer file, or a failed save.
                    update_status(request ? L"学習: 保存に失敗。未保存分を再試行中（通常の変換は利用できます）"
                                          : L"学習: ユーザー辞書を読めません。再試行中（通常の変換は利用できます）");
                    WaitForSingleObject(wake.get(), retry_delay);
                    retry_delay = (std::min)(retry_delay * 2, request ? DWORD{5000} : DWORD{60000});
                    continue;
                }
                {
                    const std::lock_guard lock(mutex);
                    if (!queue.empty()) continue;
                }
                if (remote_wait) continue; // The broker waits in the next request.
                if (changed.valid()) {
                    // A long timeout only guards against a missed signal; it reads a file header, not the helper.
                    const HANDLE events[]{wake.get(), changed.get()};
                    WaitForMultipleObjects(2, events, FALSE, 60000);
                } else WaitForSingleObject(wake.get(), container_generation.empty() ? 5000 : 1000);
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
LearningDictionary::LearningDictionary(std::shared_ptr<const Dictionary> base, std::filesystem::path file, std::string container_generation)
    : state_(std::make_shared<State>()) {
    if (!state_->wake.valid() || !state_->interrupt.valid()) throw std::runtime_error("cannot allocate learning worker event");
    state_->remote_wait = !container_generation.empty() && in_app_container();
    state_->container_generation = std::move(container_generation);
    state_->base = std::move(base); state_->file = std::move(file);
    auto work = std::make_unique<State::Work>(); work->state = state_; work->module = pin_module();
    if (!TrySubmitThreadpoolCallback(State::callback, work.get(), nullptr)) {
        FreeLibrary(work->module); throw std::runtime_error("cannot start learning worker");
    }
    work.release();
}
LearningDictionary::~LearningDictionary() {
    { const std::lock_guard lock(state_->mutex); state_->stop = true; }
    SetEvent(state_->wake.get()); SetEvent(state_->interrupt.get()); // No join in a TSF callback or DLL unload.
}
std::vector<Candidate> LearningDictionary::lookup(const LookupKey& key, std::u32string_view okuri) const {
    std::vector<Candidate> result;
    const auto append = [&](const std::vector<Candidate>& items) {
        for (const auto& candidate : items)
            if (std::none_of(result.begin(), result.end(), [&](const Candidate& old) { return old.text == candidate.text; }))
                result.push_back(candidate);
    };
    {
        const std::lock_guard lock(state_->mutex);
        result = state_->scope(key, okuri);
        if (key.okuri && !okuri.empty()) append(state_->scope(key, {}));
    }
    append(state_->base->lookup(key, okuri));
    return result;
}
std::optional<LearningEvent> LearningDictionary::forget_target(const LookupKey& key, std::u32string_view okuri, std::u32string_view text) const {
    const std::lock_guard lock(state_->mutex);
    for (const auto& candidate : state_->scope(key, okuri))
        if (candidate.text == text) return LearningEvent{key, std::u32string(okuri), candidate};
    if (key.okuri && !okuri.empty())
        for (const auto& candidate : state_->scope(key, {}))
            if (candidate.text == text) return LearningEvent{key, {}, candidate};
    return {};
}
std::vector<LookupCandidate> LearningDictionary::lookup_many(std::span<const LookupKey> keys, std::u32string_view okuri) const {
    std::vector<LookupCandidate> result;
    {
        const std::lock_guard lock(state_->mutex);
        for (const auto& key : keys) {
            for (auto& candidate : state_->scope(key, okuri)) result.push_back({key, std::move(candidate)});
            if (key.okuri && !okuri.empty())
                for (auto& candidate : state_->scope(key, {})) result.push_back({key, std::move(candidate)});
        }
    }
    for (auto& candidate : state_->base->lookup_many(keys, okuri)) result.push_back(std::move(candidate));
    return result;
}
bool LearningDictionary::registration_pending(const LearningEvent& event) const {
    const std::lock_guard lock(state_->mutex);
    for (auto it = state_->queue.rbegin(); it != state_->queue.rend(); ++it)
        if (it->event.key == event.key && it->event.okuri == event.okuri && it->event.candidate.text == event.candidate.text)
            return it->action == DictionaryAction::Learn;
    return false;
}
bool LearningDictionary::enqueue(const LearningEvent& event, bool forget) const noexcept {
    try {
        const std::lock_guard lock(state_->mutex);
        if (state_->queue.size() >= 128) { ++state_->dropped; return false; }
        LearningRequest request{state_->client, state_->sequence + 1, event, forget ? DictionaryAction::Forget : DictionaryAction::Learn}; validate_learning(request);
        state_->queue.push_back(std::move(request)); ++state_->sequence;
        state_->skipped.clear();
        SetEvent(state_->wake.get()); SetEvent(state_->interrupt.get());
        return true;
    } catch (...) { ++state_->dropped; /* Never turn an applied key into a failed key. */ }
    return false;
}
void LearningDictionary::learn(const LearningEvent& event) const noexcept { enqueue(event, false); }
bool LearningDictionary::forget(const LearningEvent& event) const noexcept { return enqueue(event, true); }
void LearningDictionary::skipped(std::wstring reason) const noexcept {
    try { const std::lock_guard lock(state_->mutex); state_->skipped = std::move(reason); } catch (...) {}
}
std::wstring LearningDictionary::status() const {
    const std::lock_guard lock(state_->mutex);
    return state_->report + L" / 未保存 " + std::to_wstring(state_->queue.size()) + L"件 / 受付不可 " +
        std::to_wstring(state_->dropped.load()) + L"件\r\nユーザー辞書: " + state_->file.native() +
        (state_->skipped.empty() ? L"" : L"\r\n学習省略: " + state_->skipped);
}
void publish_learning(const std::shared_ptr<const Dictionary>& dictionary, const LearningEvent& event) noexcept {
    if (const auto* learning = dynamic_cast<const LearningDictionary*>(dictionary.get())) learning->learn(event);
}
} // namespace nskkzime::windows
