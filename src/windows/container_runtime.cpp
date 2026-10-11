#include "container_runtime.h"
#include "app_container.h"
#include "learning_client.h"
#include "nskkzime/compiled_dictionary.h"
#include <nlohmann/json.hpp>
#include <mutex>

namespace nskkzime::windows {
namespace {
constexpr ULONGLONG retention_ms = 10 * 60 * 1000;
constexpr DWORD retention_check_ms = 30000;
int module_anchor = 0;
struct Retention {
    std::mutex mutex;
    std::weak_ptr<const RuntimeDictionary> live;
    // The last complete bootstrap and its learning worker. It outlives the input
    // services, so reactivation does not copy every dictionary image again.
    std::shared_ptr<const RuntimeDictionarySnapshot> kept;
    bool keeper = false;
};
// Never destroyed: the keeper may still run while statics are torn down at exit.
Retention& retention() { static auto* value = new Retention; return *value; }
void CALLBACK keep(PTP_CALLBACK_INSTANCE instance, void* module) {
    FreeLibraryWhenCallbackReturns(instance, static_cast<HMODULE>(module));
    CallbackMayRunLong(instance);
    std::shared_ptr<const RuntimeDictionarySnapshot> released; // Destroyed unlocked, while the module is pinned.
    try {
        auto& state = retention(); ULONGLONG idle_since = 0;
        for (;;) {
            Sleep(retention_check_ms);
            const std::lock_guard lock(state.mutex);
            const auto now = GetTickCount64();
            if (!state.live.expired()) idle_since = 0;
            else if (!idle_since) idle_since = now;
            if (state.kept && (!idle_since || now - idle_since < retention_ms)) continue;
            released = std::move(state.kept); state.keeper = false; return;
        }
    } catch (...) { /* Background failure must never unwind through Windows. */ }
}
void retain(const RuntimeDictionarySnapshot& snapshot) noexcept {
    try {
        auto value = std::make_shared<const RuntimeDictionarySnapshot>(snapshot);
        auto& state = retention();
        const std::lock_guard lock(state.mutex);
        if (!state.keeper) {
            // The keeper pins the module until it releases the snapshot.
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&module_anchor), &module)) return;
            if (!TrySubmitThreadpoolCallback(keep, module, nullptr)) { FreeLibrary(module); return; }
            state.keeper = true;
        }
        std::swap(state.kept, value); // The replaced snapshot is destroyed after unlocking.
    } catch (...) { /* Without retention the next activation bootstraps again. */ }
}
RuntimeDictionarySnapshot bootstrap() {
    NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto bytes = container_exchange("INI2", GetTickCount64() + 30000, cancel.get());
    const auto response = nlohmann::json::from_cbor(bytes);
    if (!response.is_object() || response.size() != 10 || response.at("version") != 2 ||
        !response.at("images").is_array() || response.at("images").size() > 32)
        throw std::runtime_error("invalid AppContainer snapshot");
    auto config = std::make_shared<ContainerConfiguration>();
    config->configuration.settings = parse_dictionary_settings(response.at("settings").get<std::string>());
    config->configuration.file = wide_message(response.at("file").get<std::string>());
    config->configuration.demo = response.at("demo").get<bool>();
    config->generation = response.at("generation").get<std::string>();
    if (config->generation.size() != 64 || config->generation.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("invalid AppContainer generation");
    config->complete = response.at("complete").get<bool>();
    const auto romaji = response.at("romaji").get<std::string>();
    config->romaji = romaji.empty() ? RomajiRules::defaults() : RomajiRules::parse(romaji);
    std::vector<std::shared_ptr<const Dictionary>> bases;
    for (const auto& image : response.at("images")) {
        const auto& binary = image.get_binary();
        const auto owner = std::make_shared<const std::string>(binary.begin(), binary.end());
        bases.push_back(std::make_shared<CompiledDictionary>(*owner, owner));
    }
    std::shared_ptr<const Dictionary> base = config->configuration.demo ?
        demo_dictionary() : std::make_shared<DictionarySet>(std::move(bases));
    const bool learning = config->configuration.settings.learning && !config->configuration.demo &&
        !config->configuration.settings.dictionaries.empty() && response.at("learning").get<bool>();
    auto status = L"辞書: 補助プロセス経由\r\n" + wide_message(response.at("status").get<std::string>());
    bool registration = false;
    if (learning) {
        try {
            base = std::make_shared<LearningDictionary>(base,
                config->configuration.file.parent_path() / L"user-dictionary.nskkdb", std::filesystem::path{}, config->generation);
            registration = true;
        } catch (...) { status += L"\r\n学習: 初期化に失敗（通常の変換は利用できます）"; }
    } else status += L"\r\n学習: 無効、または辞書未設定・読み込み失敗のため停止";
    // Configuration and dictionary become visible in one atomic publication.
    // Workers already own their generation; they never consult a global config.
    return RuntimeDictionarySnapshot{base, std::move(status), registration, std::move(config)};
}
}
std::shared_ptr<const ContainerConfiguration> container_configuration() {
    auto& state = retention();
    const std::lock_guard lock(state.mutex);
    const auto current = state.live.lock();
    return current ? current->container_configuration() : nullptr;
}
std::shared_ptr<const ContainerConfiguration> container_configuration(const std::shared_ptr<const Dictionary>& dictionary_value) {
    const auto runtime = std::dynamic_pointer_cast<const RuntimeDictionary>(dictionary_value);
    return runtime ? runtime->container_configuration() : nullptr;
}
std::string container_operation(std::string_view generation, std::string_view operation, std::string_view body, ULONGLONG deadline, HANDLE cancel) {
    if (generation.size() != 64 || operation.size() != 4) throw std::runtime_error("AppContainer initialization pending");
    return container_exchange(std::string(operation) + std::string(generation) + std::string(body), deadline, cancel);
}
std::shared_ptr<const Dictionary> container_runtime_dictionary() {
    auto& state = retention();
    const std::lock_guard lock(state.mutex);
    if (auto existing = state.live.lock()) return existing;
    auto result = std::make_shared<RuntimeDictionary>([previous = state.kept] {
        if (previous) {
            // Dictionary files replaced under unchanged settings apply after the
            // application restarts. An unreachable broker cannot confirm the
            // settings; keep input working as an active service would have.
            try {
                NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (container_operation(previous->container->generation, "CHK1", {}, GetTickCount64() + 5000, cancel.get()) == "SAME")
                    return *previous;
            } catch (...) { return *previous; }
        }
        auto snapshot = bootstrap();
        if (snapshot.container->complete) retain(snapshot);
        return snapshot;
    }, true);
    state.live = result; return result;
}
} // namespace nskkzime::windows
