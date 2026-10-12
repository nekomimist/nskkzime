#include "broker_snapshot.h"
#include "app_container.h"
#include "byte_codec.h"
#include "romaji_settings.h"
#include "nskkzime/compiled_dictionary.h"
#include <bcrypt.h>

namespace nskkzime::windows {
namespace {
constexpr std::string_view bootstrap_magic = "NKB1";
constexpr std::size_t text_limit = 1024 * 1024, image_count_limit = 32;
}
std::string encode_broker_bootstrap(const BrokerBootstrap& value) {
    if (value.images.size() > image_count_limit) throw std::runtime_error("too many dictionaries for a snapshot");
    ByteWriter w(bootstrap_magic);
    w.text(value.settings); w.text(value.file); w.number(value.demo); w.text(value.generation); w.text(value.romaji);
    w.text(value.status); w.number(value.learning); w.number(value.complete);
    w.number(static_cast<std::uint32_t>(value.images.size()));
    for (const auto& image : value.images) w.text(image);
    if (w.bytes.size() > container_response_limit) throw std::runtime_error("snapshot too large");
    return std::move(w.bytes);
}
BrokerBootstrap decode_broker_bootstrap(std::string_view bytes) {
    ByteReader r(bytes, bootstrap_magic); BrokerBootstrap value;
    value.settings = r.text(text_limit); value.file = r.text(text_limit); value.demo = r.flag();
    value.generation = r.text(64); value.romaji = r.text(text_limit); value.status = r.text(text_limit);
    value.learning = r.flag(); value.complete = r.flag();
    const auto count = r.number(); ByteReader::need(count <= image_count_limit);
    for (std::uint32_t i = 0; i < count; ++i) value.images.emplace_back(r.text(container_response_limit));
    r.end(); return value;
}
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
    const std::optional<std::filesystem::path>& cache_root) {
    const auto dictionaries = load_dictionaries(config, cache_root);
    const auto romaji = load_romaji_rules(config);
    BrokerBootstrap value;
    value.settings = serialize_dictionary_settings(config.settings); value.file = utf8_message(config.file.native());
    value.demo = config.demo; value.generation = generation; value.romaji = romaji.bytes;
    value.status = utf8_message(dictionaries.status + L"\r\n" + romaji.status);
    value.learning = config.settings.learning && !config.demo && !config.settings.dictionaries.empty() && !dictionaries.failed;
    value.complete = !dictionaries.failed && !romaji.failed;
    for (const auto& source : dictionaries.sources) {
        if (const auto* compiled = dynamic_cast<const CompiledDictionary*>(source.get())) value.images.emplace_back(compiled->image());
        else if (const auto* parsed = dynamic_cast<const SkkDictionary*>(source.get())) value.images.push_back(CompiledDictionary::compile(*parsed));
        else throw std::runtime_error("unsupported broker dictionary source");
    }
    return {encode_broker_bootstrap(value), value.learning, static_cast<int>(dictionaries.sources.size()),
        static_cast<int>(config.demo ? 0 : config.settings.dictionaries.size() - dictionaries.sources.size()), romaji.failed};
}
} // namespace nskkzime::windows
