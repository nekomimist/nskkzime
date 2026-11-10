#include "settings_file.h"
#include "local_storage.h"
#include "windows_text.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
constexpr std::size_t max_settings_bytes = 65536, max_dictionaries = 32;
std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
template<typename Number>
Number parse_number(std::string_view value, const char* error) {
    Number number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) throw std::invalid_argument(error);
    return number;
}
template<typename Value>
Value parse_choice(std::string_view value, std::string_view first, Value first_value,
        std::string_view second, Value second_value, const char* error) {
    if (value == first) return first_value;
    if (value == second) return second_value;
    throw std::invalid_argument(error);
}
bool parse_toggle(std::string_view value, const char* error) {
    return parse_choice(value, "on", true, "off", false, error);
}
std::filesystem::path parse_storage_path(std::string_view value) {
    std::filesystem::path path(wide_message(value));
    validate_storage_path(path);
    return path;
}
struct SettingField {
    std::string_view key;
    const char* duplicate_error; // Null for repeatable keys.
    void (*assign)(AppSettings&, std::string_view);
};
constexpr const char* learning_error = "learning must appear at most once and equal on or off";
constexpr const char* startup_error = "startup-ime-off must appear at most once and equal on or off";
constexpr const char* version_error = "settings version must appear once and equal 1";
const SettingField setting_fields[] = {
    {"version", version_error, [](AppSettings&, std::string_view value) {
        if (value != "1") throw std::invalid_argument(version_error);
    }},
    {"learning", learning_error, [](AppSettings& s, std::string_view value) {
        s.learning = parse_toggle(value, learning_error);
    }},
    {"startup-ime-off", startup_error, [](AppSettings& s, std::string_view value) {
        s.startup_ime_off = parse_toggle(value, startup_error);
    }},
    {"ui-font", "ui-font must appear at most once", [](AppSettings& s, std::string_view value) {
        s.ui.font = wide_message(value);
    }},
    {"show-input-mode", "duplicate input mode setting", [](AppSettings& s, std::string_view value) {
        s.ui.show_input_mode = parse_toggle(value, "show-input-mode must equal on or off");
    }},
    {"input-mode-duration-ms", "duplicate input mode setting", [](AppSettings& s, std::string_view value) {
        s.ui.input_mode_duration_ms = parse_number<unsigned int>(value, "invalid input-mode-duration-ms");
    }},
    {"off-key-exclude", nullptr, [](AppSettings& s, std::string_view value) {
        s.off_key_exclusions.push_back(wide_message(value));
        validate_off_key_exclusions(s.off_key_exclusions);
    }},
    {"candidate-show-after", "duplicate candidate setting", [](AppSettings& s, std::string_view value) {
        s.candidates.show_after = parse_number<unsigned int>(value, "invalid candidate-show-after");
    }},
    {"candidate-keys", "duplicate candidate setting", [](AppSettings& s, std::string_view value) {
        s.candidates.keys = parse_choice(value, "numbers", CandidateKeys::Numbers, "asdfjkl", CandidateKeys::Asdfjkl,
            "candidate-keys must equal numbers or asdfjkl");
    }},
    {"tsf-log", "duplicate TSF log setting", [](AppSettings& s, std::string_view value) {
        s.tsf_log.enabled = parse_toggle(value, "tsf-log must equal on or off");
    }},
    {"tsf-log-directory", "duplicate TSF log setting", [](AppSettings& s, std::string_view value) {
        s.tsf_log.directory = wide_message(value);
    }},
    {"input-method", "duplicate input profile setting", [](AppSettings& s, std::string_view value) {
        s.input_method = parse_choice(value, "skk", InputMethod::Skk, "azik", InputMethod::Azik,
            "input-method must equal skk or azik");
    }},
    {"azik-keyboard", "duplicate input profile setting", [](AppSettings& s, std::string_view value) {
        s.azik_keyboard = parse_choice(value, "jp106", AzikKeyboard::Jp106, "us101", AzikKeyboard::Us101,
            "azik-keyboard must equal jp106 or us101");
    }},
    {"romaji", "romaji must appear at most once", [](AppSettings& s, std::string_view value) {
        s.romaji_rules = parse_storage_path(value);
    }},
    {"inference", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.enabled = parse_choice(value, "zenz", true, "off", false, "inference must equal zenz or off");
    }},
    {"zenz-unknown-context", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.allow_unknown_context = parse_toggle(value, "zenz-unknown-context must equal on or off");
    }},
    {"input-log", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.log_input = parse_toggle(value, "input-log must equal on or off");
    }},
    {"input-log-directory", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.log_directory = parse_storage_path(value);
    }},
    {"zenz-model", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.model = parse_storage_path(value);
    }},
    {"zenz-server", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.server = parse_storage_path(value);
    }},
    {"zenz-rerank", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.rerank = parse_toggle(value, "zenz toggle must equal on or off");
    }},
    {"zenz-rerank-learn", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.rerank_learn = parse_toggle(value, "zenz toggle must equal on or off");
    }},
    {"zenz-learn-fallback", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.learn_fallback = parse_toggle(value, "zenz toggle must equal on or off");
    }},
    {"zenz-promote-threshold", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.promote_threshold = parse_number<float>(value, "invalid zenz score limit");
    }},
    {"zenz-max-score-gap", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.max_score_gap = parse_number<float>(value, "invalid zenz score limit");
    }},
    {"inference-threads", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.threads = parse_number<unsigned int>(value, "invalid inference number");
    }},
    {"inference-timeout-ms", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.timeout_ms = parse_number<unsigned int>(value, "invalid inference number");
    }},
    {"zenz-min-length", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.long_min_length = parse_number<unsigned int>(value, "invalid inference number");
    }},
    {"zenz-rerank-limit", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.rerank_limit = parse_number<unsigned int>(value, "invalid inference number");
    }},
    {"zenz-rerank-wait-ms", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.rerank_wait_ms = parse_number<unsigned int>(value, "invalid inference number");
    }},
    {"zenz-idle-seconds", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.idle_seconds = parse_number<unsigned int>(value, "invalid inference number");
    }},
    {"zenz-score-timeout-ms", "duplicate inference setting", [](AppSettings& s, std::string_view value) {
        s.zenz.policy.score_timeout_ms = parse_number<unsigned int>(value, "invalid inference number");
    }},
    {"dictionary", nullptr, [](AppSettings& s, std::string_view value) {
        s.dictionaries.push_back(parse_storage_path(value));
        if (s.dictionaries.size() > max_dictionaries) throw std::invalid_argument("at most 32 dictionaries are supported");
    }}
};

std::string serialize(const AppSettings& settings) {
    if (settings.dictionaries.size() > max_dictionaries) throw std::invalid_argument("at most 32 dictionaries are supported");
    std::string bytes = "# nskkzime settings (UTF-8); dictionary order is priority order.\nversion=1\n";
    bytes += settings.learning ? "learning=on\n" : "learning=off\n";
    if (settings.startup_ime_off) bytes += "startup-ime-off=on\n";
    if (settings.input_method != InputMethod::Skk && settings.input_method != InputMethod::Azik)
        throw std::invalid_argument("invalid input method");
    if (settings.azik_keyboard != AzikKeyboard::Jp106 && settings.azik_keyboard != AzikKeyboard::Us101)
        throw std::invalid_argument("invalid AZIK keyboard");
    if (settings.input_method == InputMethod::Azik) bytes += "input-method=azik\n";
    if (settings.azik_keyboard == AzikKeyboard::Us101) bytes += "azik-keyboard=us101\n";
    validate_ui_settings(settings.ui);
    if (settings.ui.font != default_ui_font) bytes += "ui-font=" + utf8_message(settings.ui.font) + "\n";
    if (settings.ui.show_input_mode) bytes += "show-input-mode=on\n";
    if (settings.ui.input_mode_duration_ms != default_input_mode_duration_ms)
        bytes += "input-mode-duration-ms=" + std::to_string(settings.ui.input_mode_duration_ms) + "\n";
    validate_candidate_settings(settings.candidates);
    // Old/default settings retain their representation and behavior.
    if (settings.candidates.show_after != 1)
        bytes += "candidate-show-after=" + std::to_string(settings.candidates.show_after) + "\n";
    if (settings.candidates.keys != CandidateKeys::Numbers) bytes += "candidate-keys=asdfjkl\n";
    validate_off_key_exclusions(settings.off_key_exclusions);
    for (const auto& executable : settings.off_key_exclusions)
        bytes += "off-key-exclude=" + utf8_message(executable) + "\n";
    if (!settings.romaji_rules.empty()) {
        validate_storage_path(settings.romaji_rules);
        bytes += "romaji=" + utf8_message(settings.romaji_rules.native()) + "\n";
    }
    validate_zenz_settings(settings.zenz);
    validate_tsf_log_settings(settings.tsf_log);
    if (settings.tsf_log.enabled || !settings.tsf_log.directory.empty()) {
        bytes += settings.tsf_log.enabled ? "tsf-log=on\n" : "tsf-log=off\n";
        if (!settings.tsf_log.directory.empty()) bytes += "tsf-log-directory=" + utf8_message(settings.tsf_log.directory.native()) + "\n";
    }
    if (settings.zenz.allow_unknown_context) bytes += "zenz-unknown-context=on\n";
    if (settings.zenz.log_input || !settings.zenz.log_directory.empty()) {
        bytes += settings.zenz.log_input ? "input-log=on\n" : "input-log=off\n";
        if (!settings.zenz.log_directory.empty()) bytes += "input-log-directory=" + utf8_message(settings.zenz.log_directory.native()) + "\n";
    }
    const auto& policy = settings.zenz.policy;
    if (settings.zenz.enabled || !settings.zenz.model.empty() || !settings.zenz.server.empty() || settings.zenz.threads != 4 || settings.zenz.timeout_ms != 1000 ||
        settings.zenz.idle_seconds != ZenzSettings{}.idle_seconds ||
        policy.long_min_length != 10 || policy.rerank || !policy.rerank_learn || policy.learn_fallback || policy.rerank_limit != 20 || policy.score_timeout_ms != 300 ||
        policy.rerank_wait_ms != InferencePolicy{}.rerank_wait_ms || policy.promote_threshold != 1.0f || policy.max_score_gap != 8.0f) {
        bytes += settings.zenz.enabled ? "inference=zenz\n" : "inference=off\n";
        if (!settings.zenz.model.empty()) bytes += "zenz-model=" + utf8_message(settings.zenz.model.native()) + "\n";
        if (!settings.zenz.server.empty()) bytes += "zenz-server=" + utf8_message(settings.zenz.server.native()) + "\n";
        bytes += "inference-threads=" + std::to_string(settings.zenz.threads) + "\ninference-timeout-ms=" + std::to_string(settings.zenz.timeout_ms) + "\n";
        bytes += "zenz-min-length="+std::to_string(policy.long_min_length)+"\nzenz-rerank="+(policy.rerank ? "on" : "off")+
            "\nzenz-learn-fallback="+(policy.learn_fallback ? "on" : "off")+"\nzenz-rerank-limit="+std::to_string(policy.rerank_limit)+
            "\nzenz-score-timeout-ms="+std::to_string(policy.score_timeout_ms)+"\nzenz-promote-threshold="+std::to_string(policy.promote_threshold)+
            "\nzenz-max-score-gap="+std::to_string(policy.max_score_gap)+"\n";
        // Written only when changed: older IMEs still running reject unknown keys.
        if (policy.rerank_wait_ms != InferencePolicy{}.rerank_wait_ms) bytes += "zenz-rerank-wait-ms=" + std::to_string(policy.rerank_wait_ms) + "\n";
        if (!policy.rerank_learn) bytes += "zenz-rerank-learn=off\n";
        if (settings.zenz.idle_seconds != ZenzSettings{}.idle_seconds) bytes += "zenz-idle-seconds=" + std::to_string(settings.zenz.idle_seconds) + "\n";
    }
    for (const auto& path : settings.dictionaries) {
        validate_storage_path(path);
        bytes += "dictionary=" + utf8_message(path.native()) + "\n";
    }
    if (bytes.size() > max_settings_bytes) throw std::invalid_argument("settings exceed 64 KiB");
    return bytes;
}
}
std::optional<std::wstring> environment_value(const wchar_t* name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const auto copied = GetEnvironmentVariableW(name, value.data(), size);
    if (!copied || copied >= size) throw std::runtime_error("environment changed while reading configuration");
    value.resize(copied); return value;
}
std::filesystem::path settings_file_path() {
    if (const auto override_path = environment_value(L"NSKKZIME_CONFIG")) {
        std::filesystem::path path(*override_path); validate_storage_path(path); return path;
    }
    PWSTR raw = nullptr;
    const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    if (FAILED(hr)) { CoTaskMemFree(raw); throw std::runtime_error("cannot locate LocalAppData"); }
    std::filesystem::path path;
    try { path = raw; } catch (...) { CoTaskMemFree(raw); throw; }
    CoTaskMemFree(raw);
    return path / L"nskkzime" / L"settings.ini";
}
AppSettings parse_settings(std::string_view bytes) {
    if (bytes.size() > max_settings_bytes) throw std::invalid_argument("settings exceed 64 KiB");
    from_utf8(bytes); // Validate before interpreting keys or paths.
    if (bytes.find('\0') != bytes.npos) throw std::invalid_argument("settings contain a null character");
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
    AppSettings settings;
    std::set<std::string_view> seen;
    while (!bytes.empty()) {
        const auto newline = bytes.find('\n');
        auto line = trim(bytes.substr(0, newline));
        bytes = newline == bytes.npos ? std::string_view{} : bytes.substr(newline + 1);
        if (line.empty() || line.starts_with('#') || line.starts_with(';')) continue;
        const auto separator = line.find('=');
        if (separator == line.npos) throw std::invalid_argument("expected key=value in settings");
        const auto key = trim(line.substr(0, separator)), value = trim(line.substr(separator + 1));
        const auto field = std::find_if(std::begin(setting_fields), std::end(setting_fields),
            [key](const auto& entry) { return entry.key == key; });
        if (field == std::end(setting_fields)) throw std::invalid_argument("unknown settings key");
        if (field->duplicate_error && !seen.insert(key).second) throw std::invalid_argument(field->duplicate_error);
        field->assign(settings, value);
    }
    if (!seen.contains("version")) throw std::invalid_argument("settings version is missing");
    validate_ui_settings(settings.ui);
    validate_candidate_settings(settings.candidates);
    validate_zenz_settings(settings.zenz);
    validate_tsf_log_settings(settings.tsf_log);
    return settings;
}
AppSettings read_settings(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open settings file");
    const auto length = stream.tellg();
    if (length < 0 || length > static_cast<std::streamoff>(max_settings_bytes)) throw std::runtime_error("settings exceed 64 KiB or cannot be read");
    std::string bytes(static_cast<std::size_t>(length), '\0'); stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("cannot read settings file");
    return parse_settings(bytes);
}
void save_settings(const std::filesystem::path& path, const AppSettings& settings) {
    write_file_atomic(path, serialize(settings));
}
std::string serialize_settings(const AppSettings& settings) { return serialize(settings); }
SettingsConfiguration resolve_settings_configuration(const std::filesystem::path& file, bool missing_is_demo) {
    SettingsConfiguration configuration; configuration.file = file;
    if (missing_is_demo && !std::filesystem::exists(file)) configuration.demo = true;
    else configuration.settings = read_settings(file);
    return configuration;
}
SettingsConfiguration current_settings_configuration() {
    // An explicitly selected file (NSKKZIME_CONFIG) must exist; the default may be absent.
    return resolve_settings_configuration(settings_file_path(), !environment_value(L"NSKKZIME_CONFIG").has_value());
}
} // namespace nskkzime::windows
