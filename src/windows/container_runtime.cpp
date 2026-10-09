#include "container_runtime.h"
#include "app_container.h"
#include "learning_client.h"
#include "nskkzime/compiled_dictionary.h"
#include <nlohmann/json.hpp>
#include <mutex>

namespace nskkzime::windows {
namespace {
std::mutex mutex;
std::weak_ptr<const RuntimeDictionary> dictionary;
}
std::shared_ptr<const ContainerConfiguration> container_configuration() {
    const std::lock_guard lock(mutex);
    const auto current = dictionary.lock();
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
    const std::lock_guard lock(mutex);
    if (auto existing = dictionary.lock()) return existing;
    auto result = std::make_shared<RuntimeDictionary>([] {
        NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        const auto bytes = container_exchange("INI2", GetTickCount64() + 30000, cancel.get());
        const auto response = nlohmann::json::from_cbor(bytes);
        if (!response.is_object() || response.size() != 9 || response.at("version") != 2 ||
            !response.at("images").is_array() || response.at("images").size() > 32)
            throw std::runtime_error("invalid AppContainer snapshot");
        auto config = std::make_shared<ContainerConfiguration>();
        config->configuration.settings = parse_dictionary_settings(response.at("settings").get<std::string>());
        config->configuration.file = wide_message(response.at("file").get<std::string>());
        config->configuration.demo = response.at("demo").get<bool>();
        config->generation = response.at("generation").get<std::string>();
        if (config->generation.size() != 64 || config->generation.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid AppContainer generation");
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
    }, true);
    dictionary = result; return result;
}
} // namespace nskkzime::windows
