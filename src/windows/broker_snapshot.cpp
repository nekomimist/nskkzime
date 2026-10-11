#include "broker_snapshot.h"
#include "app_container.h"
#include "romaji_settings.h"
#include "nskkzime/compiled_dictionary.h"
#include <bcrypt.h>
#include <nlohmann/json.hpp>

namespace nskkzime::windows {
std::string dictionary_configuration_id(const DictionaryConfiguration& config) {
    auto bytes = serialize_dictionary_settings(config.settings) + utf8_message(config.file.native());
    bytes += config.demo ? "\ndemo" : "\nconfigured";
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) < 0)
        throw std::runtime_error("cannot identify settings");
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
BrokerSnapshot build_broker_snapshot(const DictionaryConfiguration& config, std::string_view generation,
    const std::optional<std::filesystem::path>& cache_root, bool legacy) {
    const auto dictionaries = load_dictionaries(config, cache_root);
    const auto romaji = load_romaji_rules(config);
    // Old clients cannot represent degraded initialization or learning denial.
    if (legacy && (dictionaries.failed || romaji.failed)) throw std::runtime_error("legacy bootstrap requires valid sources");
    const bool learning = config.settings.learning && !config.demo &&
        !config.settings.dictionaries.empty() && !dictionaries.failed;
    nlohmann::json response{{"version", 2}, {"settings", serialize_dictionary_settings(config.settings)},
        {"file", utf8_message(config.file.native())}, {"demo", config.demo}, {"generation", generation},
        {"romaji", romaji.bytes}, {"images", nlohmann::json::array()},
        {"status", utf8_message(dictionaries.status + L"\r\n" + romaji.status)}, {"learning", learning},
        {"complete", !dictionaries.failed && !romaji.failed}};
    if (legacy) { response["version"] = 1; response.erase("status"); response.erase("learning"); response.erase("complete"); }
    std::size_t total = 0;
    for (const auto& source : dictionaries.sources) {
        std::string packed;
        std::string_view image;
        if (const auto* compiled = dynamic_cast<const CompiledDictionary*>(source.get())) image = compiled->image();
        else if (const auto* parsed = dynamic_cast<const SkkDictionary*>(source.get())) {
            packed = CompiledDictionary::compile(*parsed); image = packed;
        } else throw std::runtime_error("unsupported broker dictionary source");
        if (image.size() > container_response_limit - 1024 * 1024 - total) throw std::runtime_error("snapshot too large");
        total += image.size();
        response["images"].push_back(nlohmann::json::binary(std::vector<std::uint8_t>(image.begin(), image.end())));
    }
    const auto encoded = nlohmann::json::to_cbor(response);
    if (encoded.size() > container_response_limit) throw std::runtime_error("snapshot too large");
    return {std::string(encoded.begin(), encoded.end()), learning, static_cast<int>(dictionaries.sources.size()),
        static_cast<int>(config.demo ? 0 : config.settings.dictionaries.size() - dictionaries.sources.size()), romaji.failed};
}
} // namespace nskkzime::windows
