#include "container_runtime.h"
#include "app_container.h"
#include "learning_client.h"
#include "nskkzime/compiled_dictionary.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>

namespace nskkzime::windows {
namespace {
std::atomic<std::shared_ptr<const ContainerConfiguration>> configuration;
std::mutex mutex;
std::weak_ptr<const RuntimeDictionary> dictionary;
std::uint64_t load_sequence = 0;
}
std::shared_ptr<const ContainerConfiguration> container_configuration() { return configuration.load(); }
std::string container_operation(std::string_view operation, std::string_view body, ULONGLONG deadline, HANDLE cancel) {
    const auto config = container_configuration();
    if (!config || operation.size() != 4) throw std::runtime_error("AppContainer initialization pending");
    return container_exchange(std::string(operation) + config->generation + std::string(body), deadline, cancel);
}
std::shared_ptr<const Dictionary> container_runtime_dictionary() {
    const std::lock_guard lock(mutex);
    if (auto existing = dictionary.lock()) return existing;
    configuration.store(nullptr);
    const auto sequence = ++load_sequence;
    auto result = std::make_shared<RuntimeDictionary>([sequence] {
        NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        const auto bytes = container_exchange("INIT", GetTickCount64() + 30000, cancel.get());
        const auto response = nlohmann::json::from_cbor(bytes);
        if (!response.is_object() || response.size() != 7 || response.at("version") != 1 ||
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
            std::shared_ptr<const Dictionary>(std::make_shared<MemoryDictionary>()) : std::make_shared<DictionarySet>(std::move(bases));
        const bool learning = config->configuration.settings.learning && !config->configuration.demo &&
            !config->configuration.settings.dictionaries.empty();
        // Publish before the learning worker makes its first broker request.
        {
            const std::lock_guard lock(mutex);
            if (load_sequence != sequence) throw std::runtime_error("superseded container initialization");
            configuration.store(config);
        }
        if (learning) base = std::make_shared<LearningDictionary>(base,
            config->configuration.file.parent_path() / L"user-dictionary.nskkdb", std::filesystem::path{}, config->generation);
        return RuntimeDictionarySnapshot{base, L"辞書: 補助プロセスから読み込み済み", learning};
    }, true);
    dictionary = result; return result;
}
} // namespace nskkzime::windows
