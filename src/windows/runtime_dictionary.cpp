#include "dictionary_loader.h"
#include "runtime_dictionary.h"
#include "runtime_configuration.h"
#include "container_runtime.h"
#include "settings_file.h"
#include "learning_client.h"
#include "learning_ipc.h"
#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>

namespace nskkzime::windows {
namespace {
std::mutex cache_mutex;
std::map<std::wstring, std::weak_ptr<const RuntimeDictionary>> cache;
std::weak_ptr<const RuntimeDictionary> latest;
bool requested = false;
int module_anchor = 0;
RuntimeDictionarySnapshot failed_snapshot(bool broker = false) {
    return {std::make_shared<MemoryDictionary>(),
        broker ? L"入力設定の取得待ちです。準備ができるまでキーをアプリへ渡します。" :
        L"辞書設定を読み込めません。nskkzime_config --check で設定を確認してください。かな入力は利用できます。",false,{}};
}
void append_key(std::wstring& key, std::wstring_view value) {
    key += std::to_wstring(value.size()) + L":"; key += value;
}
}
#if defined(_MSC_VER) && defined(_M_ARM64)
#pragma warning(push)
// MSVC's ARM64 atomic<shared_ptr> requires alignment padding in this private,
// process-local state. Its layout is neither persisted nor shared through IPC.
#pragma warning(disable: 4324)
#endif
struct RuntimeDictionary::State {
    explicit State(bool broker = false) : failure(std::make_shared<const RuntimeDictionarySnapshot>(failed_snapshot(broker))) {}
    const std::shared_ptr<const RuntimeDictionarySnapshot> failure;
    std::atomic<std::shared_ptr<const RuntimeDictionarySnapshot>> snapshot;
    std::atomic<bool> abandoned{false};
    struct Work { std::shared_ptr<State> state; std::function<RuntimeDictionarySnapshot()> loader; HMODULE module; bool retry; };
    static void CALLBACK callback(PTP_CALLBACK_INSTANCE instance, void* raw) {
        std::unique_ptr<Work> work(static_cast<Work*>(raw));
        FreeLibraryWhenCallbackReturns(instance, work->module);
        CallbackMayRunLong(instance);
        try {
            do {
                if (work->state->abandoned.load()) return;
                RuntimeDictionarySnapshot result; bool succeeded = false;
                try { result = work->loader(); succeeded = result.dictionary != nullptr; if (!succeeded) result = failed_snapshot(); }
                catch (...) { result = failed_snapshot(); }
                if ((succeeded || !work->retry) && !work->state->abandoned.load())
                    work->state->snapshot.store(std::make_shared<const RuntimeDictionarySnapshot>(std::move(result)));
                if (succeeded || !work->retry) return;
                // Startup can precede the desktop broker. Recover without
                // requiring the input application (e.g. SearchHost) to restart.
                for (int i = 0; i < 20 && !work->state->abandoned.load(); ++i) Sleep(50);
            } while (!work->state->abandoned.load());
        } catch (...) {
            if (!work->state->abandoned.load()) work->state->snapshot.store(work->state->failure);
        }
    }
};
#if defined(_MSC_VER) && defined(_M_ARM64)
#pragma warning(pop)
#endif
RuntimeDictionary::RuntimeDictionary(std::function<RuntimeDictionarySnapshot()> loader, bool retry) : state_(std::make_shared<State>(retry)) {
    auto work = std::make_unique<State::Work>(); work->state = state_; work->loader = std::move(loader); work->retry = retry;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&module_anchor),&work->module))
        throw std::runtime_error("cannot pin dictionary loader module");
    if (!TrySubmitThreadpoolCallback(State::callback,work.get(),nullptr)) {
        FreeLibrary(work->module); throw std::runtime_error("cannot start dictionary loader");
    }
    work.release();
}
RuntimeDictionary::RuntimeDictionary(RuntimeDictionarySnapshot immediate) : state_(std::make_shared<State>()) {
    if (!immediate.dictionary) immediate = failed_snapshot();
    state_->snapshot.store(std::make_shared<const RuntimeDictionarySnapshot>(std::move(immediate)));
}
RuntimeDictionary::~RuntimeDictionary() { state_->abandoned.store(true); }
bool RuntimeDictionary::ready() const { return state_->snapshot.load() != nullptr; }
bool RuntimeDictionary::registration_allowed() const {
    const auto snapshot = state_->snapshot.load(); return snapshot && snapshot->registration;
}
std::vector<Candidate> RuntimeDictionary::lookup(const LookupKey& key, std::u32string_view okuri) const {
    const auto snapshot = state_->snapshot.load();
    return snapshot ? snapshot->dictionary->lookup(key,okuri) : std::vector<Candidate>{};
}
bool RuntimeDictionary::registration_pending(const LearningEvent& event) const {
    const auto snapshot = state_->snapshot.load();
    return !snapshot || snapshot->dictionary->registration_pending(event);
}
std::vector<LookupCandidate> RuntimeDictionary::lookup_many(std::span<const LookupKey> keys, std::u32string_view okuri) const {
    const auto snapshot = state_->snapshot.load();
    return snapshot ? snapshot->dictionary->lookup_many(keys, okuri) : std::vector<LookupCandidate>{};
}
void RuntimeDictionary::learn(const LearningEvent& event) const noexcept {
    const auto snapshot = state_->snapshot.load();
    if (snapshot && snapshot->registration && snapshot->learning) snapshot->learning->learn(event);
}
void RuntimeDictionary::learn_rerank(const LearningEvent& event) const noexcept {
    const auto snapshot = state_->snapshot.load();
    if (snapshot && snapshot->registration && snapshot->learning) snapshot->learning->learn_rerank(event);
}
std::optional<RerankBoost> RuntimeDictionary::rerank_boost(const LookupKey& key) const {
    const auto snapshot = state_->snapshot.load();
    return snapshot ? snapshot->dictionary->rerank_boost(key) : std::nullopt;
}
std::optional<LearningEvent> RuntimeDictionary::forget_target(const LookupKey& key, std::u32string_view okuri, std::u32string_view text) const {
    const auto snapshot = state_->snapshot.load();
    return snapshot && snapshot->registration ? snapshot->dictionary->forget_target(key, okuri, text) : std::nullopt;
}
bool RuntimeDictionary::forget(const LearningEvent& event) const noexcept {
    const auto snapshot = state_->snapshot.load();
    if (snapshot && snapshot->registration && snapshot->learning) return snapshot->learning->forget(event);
    return false;
}
void RuntimeDictionary::skipped(std::wstring reason) const noexcept {
    const auto snapshot = state_->snapshot.load();
    if (snapshot && snapshot->learning) snapshot->learning->skipped(std::move(reason));
}
std::wstring RuntimeDictionary::status() const {
    const auto snapshot = state_->snapshot.load();
    if (!snapshot) return L"辞書: 読み込み中。かな入力は利用できます。変換待ちの読みは、完了後にSpaceで再試行できます。";
    if (snapshot->learning) return snapshot->status + L"\r\n" + snapshot->learning->status();
    return snapshot->status;
}
std::shared_ptr<const RuntimeDictionary> runtime_dictionary() {
    return runtime_dictionary(capture_runtime_configuration());
}
std::shared_ptr<const ContainerConfiguration> RuntimeDictionary::container_configuration() const {
    const auto snapshot = state_->snapshot.load();
    return snapshot ? snapshot->container : nullptr;
}
std::shared_ptr<const RuntimeDictionary> runtime_dictionary(const RuntimeConfiguration& settings) {
    if (settings.broker) {
        auto result = container_runtime_dictionary();
        const std::lock_guard lock(cache_mutex); latest = result; requested = true; return result;
    }
    // Capture configuration/environment before queueing. Later activations must
    // never redirect an in-flight load or its learning writer.
    std::shared_ptr<const RuntimeDictionary> result;
    try {
        if (settings.configuration_failed) throw std::runtime_error("invalid captured settings");
        const auto& configuration = settings.dictionaries;
        const auto& cache_root = settings.cache_root;
        const bool learning = settings.learning;
        const auto user_file = learning ? settings.user_file : std::filesystem::path{};
        const bool learning_paths_failed = learning && settings.learning_paths_failed;
        std::wstring key = configuration.demo ? L"demo" : L"file";
        append_key(key,configuration.file.native());
        for (const auto& path : configuration.settings.dictionaries) append_key(key,path.native());
        append_key(key,learning ? L"learn" : L"no-learn");
        append_key(key,user_file.native());
        append_key(key,learning_paths_failed ? L"invalid" : L"valid");
        append_key(key,cache_root ? cache_root->native() : L"cache-off");
        const std::lock_guard lock(cache_mutex);
        std::erase_if(cache,[](const auto& entry) { return entry.second.expired(); });
        if (const auto found = cache.find(key); found != cache.end()) result = found->second.lock();
        if (!result) {
            auto load = [configuration,learning,user_file,learning_paths_failed,cache_root] {
                auto loaded = load_dictionaries(configuration,cache_root);
                if (loaded.failed) { OutputDebugStringW(loaded.status.c_str()); OutputDebugStringW(L"\n"); }
                std::shared_ptr<const LearningDictionary> learner;
                if (learning && !loaded.failed) {
                    try {
                        if (learning_paths_failed) throw std::runtime_error("invalid learning path");
                        learner = std::make_shared<LearningDictionary>(loaded.dictionary,user_file);
                        loaded.dictionary = learner;
                    } catch (...) { loaded.status += L"\r\n学習: 初期化に失敗（通常の変換は利用できます）"; }
                } else loaded.status += L"\r\n学習: 無効、または辞書未設定・読み込み失敗のため停止";
                return RuntimeDictionarySnapshot{std::move(loaded.dictionary),std::move(loaded.status),learner != nullptr,{},std::move(learner)};
            };
            // Demo/empty configurations have no dictionary file to open.
            result = configuration.demo || configuration.settings.dictionaries.empty() ?
                std::make_shared<RuntimeDictionary>(load()) : std::make_shared<RuntimeDictionary>(std::move(load));
            if (cache.size() >= 32) cache.erase(cache.begin()); // Only drops a weak reference.
            cache[key] = result;
        }
        latest = result; requested = true;
        return result;
    } catch (...) {
        result = std::make_shared<RuntimeDictionary>(failed_snapshot());
        OutputDebugStringW(result->status().c_str()); OutputDebugStringW(L"\n");
        const std::lock_guard lock(cache_mutex); latest = result; requested = true;
        return result;
    }
}
std::wstring runtime_dictionary_status() {
    std::shared_ptr<const RuntimeDictionary> dictionary; bool used = false;
    { const std::lock_guard lock(cache_mutex); dictionary = latest.lock(); used = requested; }
    return dictionary ? dictionary->status() : used ? L"辞書: 有効な入力サービスはありません。" : L"辞書: まだ読み込んでいません。";
}
bool runtime_dictionary_ready() {
    std::shared_ptr<const RuntimeDictionary> dictionary;
    { const std::lock_guard lock(cache_mutex); dictionary = latest.lock(); }
    return dictionary && dictionary->ready();
}
} // namespace nskkzime::windows
