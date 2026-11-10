#pragma once
#include "nskkzime/dictionary.h"
#include "settings_file.h"

namespace nskkzime::windows {
struct LoadedDictionaries {
    std::shared_ptr<const Dictionary> dictionary;
    std::wstring status;
    bool failed = false;
    std::vector<std::shared_ptr<const Dictionary>> sources; // Successful sources, in priority order.
};
LoadedDictionaries load_dictionaries(const SettingsConfiguration&, const std::optional<std::filesystem::path>& cache_root = {});
std::optional<std::filesystem::path> dictionary_cache_path(const SettingsConfiguration&);
} // namespace nskkzime::windows
