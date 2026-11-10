#pragma once
#include "app_settings.h"
#include <optional>

namespace nskkzime::windows {
struct SettingsConfiguration {
    AppSettings settings;
    std::filesystem::path file;
    bool demo = false;
};
std::optional<std::wstring> environment_value(const wchar_t* name);
std::filesystem::path settings_file_path();
AppSettings parse_settings(std::string_view bytes);
AppSettings read_settings(const std::filesystem::path&);
std::string serialize_settings(const AppSettings&);
void save_settings(const std::filesystem::path&, const AppSettings&);
// Explicit inputs keep tests independent of the user's saved configuration.
SettingsConfiguration resolve_settings_configuration(const std::filesystem::path& file, bool missing_is_demo);
SettingsConfiguration current_settings_configuration();
} // namespace nskkzime::windows
