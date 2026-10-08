#include "runtime_dictionary.h"
#include "container_runtime.h"
#include "dictionary_settings.h"
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
RuntimeDictionarySnapshot failed_snapshot() {
    return {std::make_shared<MemoryDictionary>(),
        L"辞書設定を読み込めません。nskkzime_config --check で設定を確認してください。かな入力は利用できます。",false};
}
void append_key(std::wstring& key, std::wstring_view value) {
    key += std::to_wstring(value.size()) + L":"; key += value;
}
}
struct RuntimeDictionary::State {
    const std::shared_ptr<const RuntimeDictionarySnapshot> failure = std::make_shared<const RuntimeDictionarySnapshot>(failed_snapshot());
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
RuntimeDictionary::RuntimeDictionary(std::function<RuntimeDictionarySnapshot()> loader, bool retry) : state_(std::make_shared<State>()) {
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
void RuntimeDictionary::learn(const LearningEvent& event) const noexcept {
    const auto snapshot = state_->snapshot.load();
    if (snapshot && snapshot->registration) publish_learning(snapshot->dictionary,event);
}
std::optional<LearningEvent> RuntimeDictionary::forget_target(const LookupKey& key, std::u32string_view okuri, std::u32string_view text) const {
    const auto snapshot = state_->snapshot.load();
    return snapshot && snapshot->registration ? snapshot->dictionary->forget_target(key, okuri, text) : std::nullopt;
}
bool RuntimeDictionary::forget(const LearningEvent& event) const noexcept {
    const auto snapshot = state_->snapshot.load();
    if (snapshot && snapshot->registration)
        if (const auto* learning = dynamic_cast<const LearningDictionary*>(snapshot->dictionary.get())) return learning->forget(event);
    return false;
}
void RuntimeDictionary::skipped(std::wstring reason) const noexcept {
    const auto snapshot = state_->snapshot.load();
    if (snapshot) if (const auto* learning = dynamic_cast<const LearningDictionary*>(snapshot->dictionary.get())) learning->skipped(std::move(reason));
}
std::wstring RuntimeDictionary::status() const {
    const auto snapshot = state_->snapshot.load();
    if (!snapshot) return L"辞書: 読み込み中。かな入力は利用できます。変換待ちの読みは、完了後にSpaceで再試行できます。";
    if (const auto* learning = dynamic_cast<const LearningDictionary*>(snapshot->dictionary.get())) return snapshot->status + L"\r\n" + learning->status();
    return snapshot->status;
}
std::shared_ptr<const Dictionary> runtime_dictionary() {
    if (uses_data_broker()) {
        auto result = std::dynamic_pointer_cast<const RuntimeDictionary>(container_runtime_dictionary());
        const std::lock_guard lock(cache_mutex); latest = result; requested = true; return result;
    }
    // Capture configuration/environment before queueing. Later activations must
    // never redirect an in-flight load or its learning writer.
    std::shared_ptr<const RuntimeDictionary> result;
    try {
        const auto configuration = current_dictionary_configuration();
        const auto cache_root = dictionary_cache_path(configuration);
        const auto override_learning = environment_value(L"NSKKZIME_LEARNING");
        const bool learning = !configuration.demo && !configuration.settings.dictionaries.empty() &&
            (override_learning ? *override_learning == L"1" : configuration.settings.learning);
        std::filesystem::path user_file, helper;
        bool learning_paths_failed = false;
        if (learning) try { user_file = learning_file_path(); helper = learning_helper_path(); } catch (...) { learning_paths_failed = true; }
        std::wstring key = configuration.demo ? L"demo" : configuration.environment ? L"env" : L"file";
        append_key(key,configuration.file.native());
        for (const auto& path : configuration.settings.dictionaries) append_key(key,path.native());
        append_key(key,learning ? L"learn" : L"no-learn");
        append_key(key,user_file.native()); append_key(key,helper.native());
        append_key(key,learning_paths_failed ? L"invalid" : L"valid");
        append_key(key,cache_root ? cache_root->native() : L"cache-off");
        const std::lock_guard lock(cache_mutex);
        std::erase_if(cache,[](const auto& entry) { return entry.second.expired(); });
        if (const auto found = cache.find(key); found != cache.end()) result = found->second.lock();
        if (!result) {
            auto load = [configuration,learning,user_file,helper,learning_paths_failed,cache_root] {
                auto loaded = load_dictionaries(configuration,cache_root);
                if (loaded.failed) { OutputDebugStringW(loaded.status.c_str()); OutputDebugStringW(L"\n"); }
                bool registration = false;
                if (learning && !loaded.failed) {
                    try {
                        if (learning_paths_failed) throw std::runtime_error("invalid learning path");
                        loaded.dictionary = std::make_shared<LearningDictionary>(loaded.dictionary,user_file,helper);
                        registration = true;
                    } catch (...) { loaded.status += L"\r\n学習: 初期化に失敗（通常の変換は利用できます）"; }
                } else loaded.status += L"\r\n学習: 無効、または辞書未設定・読み込み失敗のため停止";
                return RuntimeDictionarySnapshot{std::move(loaded.dictionary),std::move(loaded.status),registration};
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
void publish_runtime_learning(const std::shared_ptr<const Dictionary>& dictionary, const LearningEvent& event) noexcept {
    if (const auto* runtime = dynamic_cast<const RuntimeDictionary*>(dictionary.get())) runtime->learn(event);
}
void skip_runtime_learning(const std::shared_ptr<const Dictionary>& dictionary, std::wstring reason) noexcept {
    if (const auto* runtime = dynamic_cast<const RuntimeDictionary*>(dictionary.get())) runtime->skipped(std::move(reason));
}
bool publish_runtime_forgetting(const std::shared_ptr<const Dictionary>& dictionary, const LearningEvent& event) noexcept {
    if (const auto* runtime = dynamic_cast<const RuntimeDictionary*>(dictionary.get())) return runtime->forget(event);
    return false;
}
}
