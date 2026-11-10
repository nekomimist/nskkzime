#include "settings_file.h"
#include "local_storage.h"
#include "windows_text.h"
#include <windows.h>
#include <shlobj.h>
#include <charconv>
#include <fstream>
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
    AppSettings settings; bool version = false, learning = false, romaji = false, startup_ime_off = false, ui_font = false;
    std::set<std::string_view> inference_keys, candidate_options, diagnostic_options, mode_options, input_options;
    while (!bytes.empty()) {
        const auto newline = bytes.find('\n');
        auto line = trim(bytes.substr(0, newline));
        bytes = newline == bytes.npos ? std::string_view{} : bytes.substr(newline + 1);
        if (line.empty() || line.starts_with('#') || line.starts_with(';')) continue;
        const auto separator = line.find('=');
        if (separator == line.npos) throw std::invalid_argument("expected key=value in settings");
        const auto key = trim(line.substr(0, separator)), value = trim(line.substr(separator + 1));
        if (key == "version") {
            if (version || value != "1") throw std::invalid_argument("settings version must appear once and equal 1");
            version = true;
        } else if (key == "learning") {
            if (learning || (value != "on" && value != "off")) throw std::invalid_argument("learning must appear at most once and equal on or off");
            learning = true; settings.learning = value == "on";
        } else if (key == "startup-ime-off") {
            if (startup_ime_off || (value != "on" && value != "off"))
                throw std::invalid_argument("startup-ime-off must appear at most once and equal on or off");
            startup_ime_off = true; settings.startup_ime_off = value == "on";
        } else if (key == "ui-font") {
            if (ui_font) throw std::invalid_argument("ui-font must appear at most once");
            ui_font = true; settings.ui.font = wide_message(value);
        } else if (key == "show-input-mode" || key == "input-mode-duration-ms") {
            if (!mode_options.insert(key).second) throw std::invalid_argument("duplicate input mode setting");
            if (key == "show-input-mode") {
                if (value != "on" && value != "off") throw std::invalid_argument("show-input-mode must equal on or off");
                settings.ui.show_input_mode = value == "on";
            } else {
                unsigned int number = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                    throw std::invalid_argument("invalid input-mode-duration-ms");
                settings.ui.input_mode_duration_ms = number;
            }
        } else if (key == "off-key-exclude") {
            settings.off_key_exclusions.push_back(wide_message(value));
            validate_off_key_exclusions(settings.off_key_exclusions);
        } else if (key == "candidate-show-after" || key == "candidate-keys") {
            if (!candidate_options.insert(key).second) throw std::invalid_argument("duplicate candidate setting");
            if (key == "candidate-keys") {
                if (value != "numbers" && value != "asdfjkl") throw std::invalid_argument("candidate-keys must equal numbers or asdfjkl");
                settings.candidates.keys = value == "asdfjkl" ? CandidateKeys::Asdfjkl : CandidateKeys::Numbers;
            } else {
                unsigned int number = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) throw std::invalid_argument("invalid candidate-show-after");
                settings.candidates.show_after = number;
            }
        } else if (key == "tsf-log" || key == "tsf-log-directory") {
            if (!diagnostic_options.insert(key).second) throw std::invalid_argument("duplicate TSF log setting");
            if (key == "tsf-log") {
                if (value != "on" && value != "off") throw std::invalid_argument("tsf-log must equal on or off");
                settings.tsf_log.enabled = value == "on";
            } else settings.tsf_log.directory = std::filesystem::path(wide_message(value));
        } else if (key == "input-method" || key == "azik-keyboard") {
            if (!input_options.insert(key).second) throw std::invalid_argument("duplicate input profile setting");
            if (key == "input-method") {
                if (value != "skk" && value != "azik") throw std::invalid_argument("input-method must equal skk or azik");
                settings.input_method = value == "azik" ? InputMethod::Azik : InputMethod::Skk;
            } else {
                if (value != "jp106" && value != "us101") throw std::invalid_argument("azik-keyboard must equal jp106 or us101");
                settings.azik_keyboard = value == "us101" ? AzikKeyboard::Us101 : AzikKeyboard::Jp106;
            }
        } else if (key == "romaji") {
            if (romaji) throw std::invalid_argument("romaji must appear at most once");
            romaji = true; settings.romaji_rules = std::filesystem::path(wide_message(value)); validate_storage_path(settings.romaji_rules);
        } else if (key == "inference" || key == "zenz-model" || key == "zenz-server" || key == "inference-threads" || key == "inference-timeout-ms" ||
            key == "zenz-min-length" || key == "zenz-rerank" || key == "zenz-rerank-learn" || key == "zenz-learn-fallback" || key == "zenz-rerank-limit" || key == "zenz-score-timeout-ms" ||
            key == "zenz-rerank-wait-ms" || key == "zenz-idle-seconds" || key == "zenz-promote-threshold" || key == "zenz-max-score-gap" || key == "input-log" || key == "input-log-directory" ||
            key == "zenz-unknown-context") {
            if (!inference_keys.insert(key).second) throw std::invalid_argument("duplicate inference setting");
            if (key == "inference") {
                if (value != "zenz" && value != "off") throw std::invalid_argument("inference must equal zenz or off");
                settings.zenz.enabled = value == "zenz";
            } else if (key == "zenz-unknown-context") {
                if (value != "on" && value != "off") throw std::invalid_argument("zenz-unknown-context must equal on or off");
                settings.zenz.allow_unknown_context = value == "on";
            } else if (key == "input-log") {
                if (value != "on" && value != "off") throw std::invalid_argument("input-log must equal on or off");
                settings.zenz.log_input = value == "on";
            } else if (key == "input-log-directory") {
                settings.zenz.log_directory = std::filesystem::path(wide_message(value));
                validate_storage_path(settings.zenz.log_directory);
            } else if (key == "zenz-model" || key == "zenz-server") {
                auto& path = key == "zenz-model" ? settings.zenz.model : settings.zenz.server;
                path = std::filesystem::path(wide_message(value)); validate_storage_path(path);
            } else if (key == "zenz-rerank" || key == "zenz-rerank-learn" || key == "zenz-learn-fallback") {
                if (value != "on" && value != "off") throw std::invalid_argument("zenz toggle must equal on or off");
                (key == "zenz-rerank" ? settings.zenz.policy.rerank : key == "zenz-rerank-learn" ? settings.zenz.policy.rerank_learn : settings.zenz.policy.learn_fallback) = value == "on";
            } else if (key == "zenz-promote-threshold" || key == "zenz-max-score-gap") {
                float number = 0;
                const auto parsed = std::from_chars(value.data(),value.data()+value.size(),number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size()) throw std::invalid_argument("invalid zenz score limit");
                (key == "zenz-promote-threshold" ? settings.zenz.policy.promote_threshold : settings.zenz.policy.max_score_gap) = number;
            } else {
                unsigned int number = 0;
                const auto parsed = std::from_chars(value.data(),value.data()+value.size(),number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size()) throw std::invalid_argument("invalid inference number");
                if (key == "inference-threads") settings.zenz.threads = number;
                else if (key == "inference-timeout-ms") settings.zenz.timeout_ms = number;
                else if (key == "zenz-min-length") settings.zenz.policy.long_min_length = number;
                else if (key == "zenz-rerank-limit") settings.zenz.policy.rerank_limit = number;
                else if (key == "zenz-rerank-wait-ms") settings.zenz.policy.rerank_wait_ms = number;
                else if (key == "zenz-idle-seconds") settings.zenz.idle_seconds = number;
                else settings.zenz.policy.score_timeout_ms = number;
            }
        } else if (key == "dictionary") {
            auto path = std::filesystem::path(wide_message(value)); validate_storage_path(path);
            settings.dictionaries.push_back(std::move(path));
            if (settings.dictionaries.size() > max_dictionaries) throw std::invalid_argument("at most 32 dictionaries are supported");
        } else throw std::invalid_argument("unknown settings key");
    }
    if (!version) throw std::invalid_argument("settings version is missing");
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
