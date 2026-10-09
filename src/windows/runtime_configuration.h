#pragma once
#include "dictionary_settings.h"
#include "tool_launcher.h"

namespace nskkzime::windows {
struct InferenceOverrides {
    std::optional<std::wstring> dummy, delay_ms, timeout_ms, token;
    std::filesystem::path default_server;
};
// Owned by one activation. The dictionary may be shared, but these settings are
// never fetched back from another activation's shared dictionary.
struct RuntimeConfiguration {
    DictionaryConfiguration dictionaries;
    bool broker = false;
    bool configuration_failed = false;
    std::optional<std::filesystem::path> cache_root;
    std::optional<std::wstring> romaji_override;
    bool learning = false, learning_paths_failed = false;
    std::filesystem::path user_file, learning_helper;
    InferenceOverrides inference;
    std::array<ToolCommand, 2> tools;
};
RuntimeConfiguration capture_runtime_configuration(); // Activation/worker only.
// Broker requests already resolved the file once; derive all remaining inputs
// from that value, including inference configuration and learning destinations.
RuntimeConfiguration capture_runtime_configuration(DictionaryConfiguration);
} // namespace nskkzime::windows
