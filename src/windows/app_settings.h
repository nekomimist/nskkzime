#pragma once
#include "nskkzime/input_profile.h"
#include "nskkzime/inference_policy.h"
#include "candidate_settings.h"
#include "ui_settings.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
struct AppSettings {
    std::vector<std::filesystem::path> dictionaries; // Highest priority first.
    bool learning = true;
    std::filesystem::path romaji_rules; // Empty selects the built-in rules.
    ZenzSettings zenz;
    CandidateSettings candidates;
    struct TsfLogSettings {
        bool enabled = false;
        std::filesystem::path directory;
    } tsf_log;
    std::vector<std::wstring> off_key_exclusions;
    bool startup_ime_off = false; // Initial TSF activation only; later mode choices remain available.
    UiSettings ui;
    InputMethod input_method = InputMethod::Skk;
    AzikKeyboard azik_keyboard = AzikKeyboard::Jp106;
};
void validate_zenz_settings(const ZenzSettings&, bool check_files = false);
void validate_tsf_log_settings(const AppSettings::TsfLogSettings&, bool check_files = false);
std::wstring tsf_log_settings_status(const AppSettings::TsfLogSettings&);
void validate_off_key_exclusions(const std::vector<std::wstring>&);
bool off_keys_excluded(const AppSettings&, std::wstring_view executable);
void validate_candidate_settings(const CandidateSettings&);
void validate_ui_settings(const UiSettings&);
std::wstring ui_settings_status(const UiSettings&);
std::wstring candidate_settings_status(const CandidateSettings&);
std::wstring zenz_settings_status(const ZenzSettings&);
} // namespace nskkzime::windows
