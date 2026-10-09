#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>

// Selects a private settings file through NSKKZIME_CONFIG. Its folder also holds
// the user dictionary and dictionary cache, so tests never touch the user's data.
inline std::string settings_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
inline std::filesystem::path use_isolated_settings(const std::filesystem::path& directory, const std::string& lines) {
    std::filesystem::create_directories(directory);
    const auto file = directory / L"settings.ini";
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "version=1\n" << lines;
    SetEnvironmentVariableW(L"NSKKZIME_CONFIG", file.c_str());
    return file;
}
