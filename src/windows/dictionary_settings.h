#pragma once
#include "nskkzime/dictionary.h"
#include "nskkzime/inference_policy.h"
#include "candidate_settings.h"
#include <optional>

namespace nskkzime::windows {
struct ZenzSettings {
    bool enabled = false;
    std::filesystem::path model, server;
    unsigned int threads = 4, timeout_ms = 1000;
    // The server keeps the model loaded until idle this long; a restart costs a model load.
    unsigned int idle_seconds = 1800;
    InferencePolicy policy;
    bool log_input = false;
    std::filesystem::path log_directory;
    bool allow_unknown_context = false;
};
struct DictionarySettings {
    std::vector<std::filesystem::path> dictionaries; // Highest priority first.
    bool learning = true;
    std::filesystem::path romaji_rules; // Empty selects the built-in rules.
    ZenzSettings zenz;
    CandidateSettings candidates;
};
struct DictionaryConfiguration {
    DictionarySettings settings;
    std::filesystem::path file;
    bool demo = false;
};
struct LoadedDictionaries {
    std::shared_ptr<const Dictionary> dictionary;
    std::wstring status;
    bool failed = false;
    std::vector<std::shared_ptr<const Dictionary>> sources; // Successful sources, in priority order.
};
std::optional<std::wstring> environment_value(const wchar_t* name);
std::filesystem::path dictionary_settings_path();
DictionarySettings parse_dictionary_settings(std::string_view bytes);
DictionarySettings read_dictionary_settings(const std::filesystem::path&);
std::string serialize_dictionary_settings(const DictionarySettings&);
void save_dictionary_settings(const std::filesystem::path&, const DictionarySettings&);
void validate_zenz_settings(const ZenzSettings&, bool check_files = false);
void validate_candidate_settings(const CandidateSettings&);
std::wstring candidate_settings_status(const CandidateSettings&);
std::wstring zenz_settings_status(const ZenzSettings&);
// Explicit inputs keep tests independent of the user's saved configuration.
DictionaryConfiguration resolve_dictionary_configuration(const std::filesystem::path& file, bool missing_is_demo);
DictionaryConfiguration current_dictionary_configuration();
LoadedDictionaries load_dictionaries(const DictionaryConfiguration&, const std::optional<std::filesystem::path>& cache_root = {});
std::optional<std::filesystem::path> dictionary_cache_path(const DictionaryConfiguration&);
std::wstring wide_message(std::string_view utf8_text);
std::string utf8_message(std::wstring_view wide_text);
} // namespace nskkzime::windows
