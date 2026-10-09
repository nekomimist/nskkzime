#pragma once
#include "dictionary_settings.h"

namespace nskkzime::windows {
struct BrokerSnapshot {
    std::string bytes;
    bool learning_allowed = false;
    int sources = 0, failed_sources = 0;
    bool romaji_fallback = false;
};
std::string dictionary_configuration_id(const DictionaryConfiguration&);
// Worker-only: uses the same source loading, fallback and diagnostics as desktop.
BrokerSnapshot build_broker_snapshot(const DictionaryConfiguration&, std::string_view generation,
    const std::optional<std::filesystem::path>& cache_root = {}, bool legacy = false);
} // namespace nskkzime::windows
