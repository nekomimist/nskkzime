#pragma once
#include "nskkzime/dictionary.h"
#include <optional>

namespace nskkzime::windows {
struct DictionarySettings {
    std::vector<std::filesystem::path> dictionaries; // Highest priority first.
    bool learning = true;
    std::filesystem::path romaji_rules; // Empty selects the built-in rules.
};
struct DictionaryConfiguration {
    DictionarySettings settings;
    std::filesystem::path file;
    bool demo = false, environment = false;
};
struct LoadedDictionaries {
    std::shared_ptr<const Dictionary> dictionary;
    std::wstring status;
    bool failed = false;
};
std::optional<std::wstring> environment_value(const wchar_t* name);
std::filesystem::path dictionary_settings_path();
DictionarySettings parse_dictionary_settings(std::string_view bytes);
DictionarySettings read_dictionary_settings(const std::filesystem::path&);
void save_dictionary_settings(const std::filesystem::path&, const DictionarySettings&);
// Explicit inputs keep tests independent of the user's saved configuration.
DictionaryConfiguration resolve_dictionary_configuration(const std::filesystem::path& file,
    const std::optional<std::wstring>& dictionary_override, bool missing_is_demo);
DictionaryConfiguration current_dictionary_configuration();
LoadedDictionaries load_dictionaries(const DictionaryConfiguration&);
std::wstring wide_message(std::string_view utf8_text);
std::string utf8_message(std::wstring_view wide_text);
} // namespace nskkzime::windows
