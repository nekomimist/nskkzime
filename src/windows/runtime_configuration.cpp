#include "runtime_configuration.h"
#include "app_container.h"
#include "learning_client.h"
#include "learning_ipc.h"

namespace nskkzime::windows {
RuntimeConfiguration capture_runtime_configuration(DictionaryConfiguration dictionaries) {
    RuntimeConfiguration result;
    result.dictionaries = std::move(dictionaries);
    try { result.cache_root = dictionary_cache_path(result.dictionaries); }
    catch (...) { /* An invalid optional cache must not disable the source. */ }
    result.learning = !result.dictionaries.demo && !result.dictionaries.settings.dictionaries.empty() &&
        result.dictionaries.settings.learning;
    auto settings_file = result.dictionaries.file;
    if (settings_file.empty()) try { settings_file = dictionary_settings_path(); } catch (...) {}
    try { result.user_file = learning_file_path(settings_file); }
    catch (...) { result.learning_paths_failed = true; }
    try { result.inference_server = product_directory() / L"nkkz-ime-server.exe"; }
    catch (...) { /* Inference reports the missing server when it is enabled. */ }
    result.tools = tool_commands(settings_file, result.user_file);
    return result;
}
RuntimeConfiguration capture_runtime_configuration() {
    if (uses_data_broker()) { RuntimeConfiguration result; result.broker = true; return result; }
    try { return capture_runtime_configuration(current_dictionary_configuration()); }
    catch (...) {
        auto result = capture_runtime_configuration(DictionaryConfiguration{});
        result.configuration_failed = true;
        return result;
    }
}
} // namespace nskkzime::windows
