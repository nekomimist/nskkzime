#include "dictionary_settings.h"
#include "local_storage.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
constexpr std::size_t max_settings_bytes = 65536, max_dictionaries = 32;
std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
void validate_path(const std::filesystem::path& path) {
    const auto& value = path.native();
    if (!path.is_absolute() || std::any_of(value.begin(), value.end(), [](wchar_t ch) { return ch < 0x20 || ch == 0x7f; }) ||
        value.back() == L' ') throw std::invalid_argument("path must be absolute, without control characters or trailing spaces");
}
std::string serialize(const DictionarySettings& settings) {
    if (settings.dictionaries.size() > max_dictionaries) throw std::invalid_argument("at most 32 dictionaries are supported");
    std::string bytes = "# nskkzime settings (UTF-8); dictionary order is priority order.\nversion=1\n";
    bytes += settings.learning ? "learning=on\n" : "learning=off\n";
    for (const auto& path : settings.dictionaries) {
        validate_path(path);
        bytes += "dictionary=" + utf8_message(path.native()) + "\n";
    }
    if (bytes.size() > max_settings_bytes) throw std::invalid_argument("settings exceed 64 KiB");
    return bytes;
}
struct TemporaryFile {
    std::filesystem::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool created = false;
    ~TemporaryFile() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (created) DeleteFileW(path.c_str()); // Only the file created by this save.
    }
};
}
std::wstring wide_message(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::invalid_argument("invalid UTF-8 text");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), count) != count)
        throw std::runtime_error("UTF-8 conversion failed");
    return output;
}
std::string utf8_message(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::invalid_argument("invalid UTF-16 text");
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("UTF-16 conversion failed");
    return output;
}
std::optional<std::wstring> environment_value(const wchar_t* name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const auto copied = GetEnvironmentVariableW(name, value.data(), size);
    if (!copied || copied >= size) throw std::runtime_error("environment changed while reading configuration");
    value.resize(copied); return value;
}
std::filesystem::path dictionary_settings_path() {
    if (const auto override_path = environment_value(L"NSKKZIME_CONFIG")) {
        std::filesystem::path path(*override_path); validate_path(path); return path;
    }
    PWSTR raw = nullptr;
    const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    if (FAILED(hr)) { CoTaskMemFree(raw); throw std::runtime_error("cannot locate LocalAppData"); }
    std::filesystem::path path;
    try { path = raw; } catch (...) { CoTaskMemFree(raw); throw; }
    CoTaskMemFree(raw);
    return path / L"nskkzime" / L"settings.ini";
}
DictionarySettings parse_dictionary_settings(std::string_view bytes) {
    if (bytes.size() > max_settings_bytes) throw std::invalid_argument("settings exceed 64 KiB");
    from_utf8(bytes); // Validate before interpreting keys or paths.
    if (bytes.find('\0') != bytes.npos) throw std::invalid_argument("settings contain a null character");
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
    DictionarySettings settings; bool version = false, learning = false;
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
        } else if (key == "dictionary") {
            auto path = std::filesystem::path(wide_message(value)); validate_path(path);
            settings.dictionaries.push_back(std::move(path));
            if (settings.dictionaries.size() > max_dictionaries) throw std::invalid_argument("at most 32 dictionaries are supported");
        } else throw std::invalid_argument("unknown settings key");
    }
    if (!version) throw std::invalid_argument("settings version is missing");
    return settings;
}
DictionarySettings read_dictionary_settings(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open settings file");
    const auto length = stream.tellg();
    if (length < 0 || length > static_cast<std::streamoff>(max_settings_bytes)) throw std::runtime_error("settings exceed 64 KiB or cannot be read");
    std::string bytes(static_cast<std::size_t>(length), '\0'); stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("cannot read settings file");
    return parse_dictionary_settings(bytes);
}
void save_dictionary_settings(const std::filesystem::path& path, const DictionarySettings& settings) {
    write_file_atomic(path, serialize(settings));
}
void write_file_atomic(const std::filesystem::path& path, std::string_view bytes) {
    validate_path(path);
    if (bytes.size() > 64 * 1024 * 1024) throw std::invalid_argument("file exceeds 64 MiB");
    std::filesystem::create_directories(path.parent_path());
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("cannot allocate temporary settings name");
    wchar_t suffix[40]{}; StringFromGUID2(guid, suffix, ARRAYSIZE(suffix));
    TemporaryFile temporary;
    temporary.path = path.native() + L"." + suffix + L".tmp";
    temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporary.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create temporary settings file");
    temporary.created = true;
    DWORD written = 0;
    if (!WriteFile(temporary.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
        !FlushFileBuffers(temporary.handle)) throw std::runtime_error("cannot write settings; previous file was kept");
    CloseHandle(temporary.handle); temporary.handle = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot replace settings; previous file was kept");
    temporary.created = false;
}
DictionaryConfiguration resolve_dictionary_configuration(const std::filesystem::path& file,
    const std::optional<std::wstring>& dictionary_override, bool missing_is_demo) {
    DictionaryConfiguration configuration; configuration.file = file;
    if (dictionary_override) {
        auto path = std::filesystem::path(*dictionary_override); validate_path(path);
        configuration.settings.dictionaries.push_back(std::move(path)); configuration.environment = true;
        configuration.settings.learning = false; // Development override is isolated unless explicitly enabled.
    } else if (missing_is_demo && !std::filesystem::exists(file)) configuration.demo = true;
    else configuration.settings = read_dictionary_settings(file);
    return configuration;
}
DictionaryConfiguration current_dictionary_configuration() {
    // An explicit dictionary works even when the saved configuration is broken.
    const auto dictionary = environment_value(L"NSKKZIME_DICTIONARY");
    if (dictionary) return resolve_dictionary_configuration({}, dictionary, false);
    return resolve_dictionary_configuration(dictionary_settings_path(), {}, !environment_value(L"NSKKZIME_CONFIG").has_value());
}
LoadedDictionaries load_dictionaries(const DictionaryConfiguration& configuration) {
    LoadedDictionaries loaded;
    if (configuration.demo) {
        loaded.dictionary = demo_dictionary();
        loaded.status = L"辞書: 内蔵テスト用（5見出し）。通常の辞書は未設定です。\r\n設定: " + configuration.file.native();
        return loaded;
    }
    loaded.status = configuration.environment ? L"辞書設定: 環境変数 NSKKZIME_DICTIONARY（保存設定より優先）" : L"辞書設定: " + configuration.file.native();
    std::vector<std::shared_ptr<const Dictionary>> dictionaries;
    std::size_t index = 0;
    for (const auto& path : configuration.settings.dictionaries) {
        loaded.status += L"\r\n" + std::to_wstring(++index) + L". " + path.native();
        try {
            auto dictionary = SkkDictionary::load(path);
            const auto& report = dictionary->report();
            loaded.status += L"\r\n   読み込み済み: " + std::to_wstring(report.entries) + L"見出し / " +
                std::to_wstring(report.candidates) + L"候補（" + wide_message(encoding_name(report.encoding)) + L"）";
            if (report.malformed_lines || report.unsupported_candidates)
                loaded.status += L" / 不正行 " + std::to_wstring(report.malformed_lines) + L" / 未対応候補 " + std::to_wstring(report.unsupported_candidates);
            for (const auto& diagnostic : report.diagnostics) loaded.status += L"\r\n   " + wide_message(diagnostic);
            dictionaries.push_back(std::move(dictionary));
        } catch (const std::exception& error) {
            loaded.failed = true;
            loaded.status += L"\r\n   読み込み失敗: " + wide_message(error.what());
        }
    }
    if (dictionaries.empty()) loaded.status += L"\r\n使用できる辞書は0件です。かな入力は利用できます。";
    loaded.dictionary = std::make_shared<DictionarySet>(std::move(dictionaries));
    return loaded;
}
} // namespace nskkzime::windows
