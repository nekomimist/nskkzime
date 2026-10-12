#pragma once
#include "dictionary_settings.h"
#include <vector>

namespace nskkzime::windows {
// The broker's answer to an AppContainer bootstrap ("INI3").
struct BrokerBootstrap {
    std::string settings; // serialize_dictionary_settings
    std::string file; // UTF-8 settings path
    bool demo = false;
    std::string generation; // dictionary_configuration_id
    std::string romaji; // Validated rules; empty means the built-in rules.
    std::string status; // UTF-8 load report
    bool learning = false, complete = false;
    std::vector<std::string> images; // Compiled dictionaries, in priority order.
};
std::string encode_broker_bootstrap(const BrokerBootstrap&);
BrokerBootstrap decode_broker_bootstrap(std::string_view);
struct BrokerSnapshot {
    std::string bytes;
    bool learning_allowed = false;
    int sources = 0, failed_sources = 0;
    bool romaji_fallback = false;
};
std::string dictionary_configuration_id(const DictionaryConfiguration&);
// Worker-only: uses the same source loading, fallback and diagnostics as desktop.
BrokerSnapshot build_broker_snapshot(const DictionaryConfiguration&, std::string_view generation,
    const std::optional<std::filesystem::path>& cache_root = {});
} // namespace nskkzime::windows
