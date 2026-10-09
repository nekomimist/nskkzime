#include "dictionary_settings.h"
#include "cpu_requirements.h"
#include "local_storage.h"
#include "dictionary_cache.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

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
    validate_candidate_settings(settings.candidates);
    // Old/default settings retain their representation and behavior.
    if (settings.candidates.show_after != 1)
        bytes += "candidate-show-after=" + std::to_string(settings.candidates.show_after) + "\n";
    if (settings.candidates.keys != CandidateKeys::Numbers) bytes += "candidate-keys=asdfjkl\n";
    if (!settings.romaji_rules.empty()) {
        validate_path(settings.romaji_rules);
        bytes += "romaji=" + utf8_message(settings.romaji_rules.native()) + "\n";
    }
    validate_zenz_settings(settings.zenz);
    if (settings.zenz.allow_unknown_context) bytes += "zenz-unknown-context=on\n";
    if (settings.zenz.log_input || !settings.zenz.log_directory.empty()) {
        bytes += settings.zenz.log_input ? "input-log=on\n" : "input-log=off\n";
        if (!settings.zenz.log_directory.empty()) bytes += "input-log-directory=" + utf8_message(settings.zenz.log_directory.native()) + "\n";
    }
    const auto& policy = settings.zenz.policy;
    if (settings.zenz.enabled || !settings.zenz.model.empty() || !settings.zenz.server.empty() || settings.zenz.threads != 4 || settings.zenz.timeout_ms != 1000 ||
        policy.long_min_length != 10 || policy.rerank || policy.learn_fallback || policy.rerank_limit != 20 || policy.score_timeout_ms != 300 ||
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
    }
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
void validate_zenz_settings(const ZenzSettings& settings, bool check_files) {
    validate_inference_policy(settings.policy);
    if (check_files && settings.enabled) require_zenz_cpu(current_cpu_features());
    if (settings.log_input || !settings.log_directory.empty()) {
        validate_path(settings.log_directory);
        if (settings.log_directory.root_name().native().size() != 2 || settings.log_directory.native().find(L'"') != std::wstring::npos)
            throw std::invalid_argument("input log requires an absolute local directory path");
        if (check_files && settings.log_input) {
            const auto drive = GetDriveTypeW(settings.log_directory.root_path().c_str());
            if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE) throw std::invalid_argument("input log requires a local drive");
            for (auto part = settings.log_directory; !part.empty();) {
                const auto attributes = GetFileAttributesW(part.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    throw std::invalid_argument("input log directory must exist and have no reparse points");
                const auto parent = part.parent_path();
                if (parent == part) break;
                part = parent;
            }
        }
    }
    if (settings.threads < 1 || settings.threads > 64 || settings.timeout_ms < 1 || settings.timeout_ms > 10000)
        throw std::invalid_argument("inference threads must be 1..64; timeout must be 1..10000 ms");
    for (const auto& path : {settings.model,settings.server}) {
        if (path.empty() && !settings.enabled) continue;
        validate_path(path);
        if (path.native().find(L'\"') != std::wstring::npos || path.filename().empty()) throw std::invalid_argument("invalid inference file path");
        if (check_files && settings.enabled && !std::filesystem::is_regular_file(path)) throw std::invalid_argument("inference model/server file is missing");
    }
}
std::wstring zenz_settings_status(const ZenzSettings& settings) {
    const auto context = std::wstring(L"\r\n属性不明の周辺本文: ") + (settings.allow_unknown_context ? L"ローカル推論に利用（学習・入力ログの許可とは独立）" : L"取得しない");
    const auto logging = settings.log_input ? L"\r\n入力ログ: オン（読み・周辺本文・候補を保存） / 保存先: " + settings.log_directory.native() : std::wstring(L"\r\n入力ログ: オフ");
    if (!settings.enabled) return L"推論設定: オフ（入力ログも動作しません）" + context + logging;
    return L"推論設定: zenz / モデル: " + settings.model.native() + L"\r\nサーバー: " + settings.server.native() +
        L"\r\nスレッド: " + std::to_wstring(settings.threads) + L" / 期限: " + std::to_wstring(settings.timeout_ms) + L" ms"+
        L"\r\n長い読みの生成: "+std::to_wstring(settings.policy.long_min_length)+L"文字（0は無効） / 候補採点: "+(settings.policy.rerank ? L"on" : L"off")+
        L" / 短い生成候補の学習: "+(settings.policy.learn_fallback ? L"on" : L"off") + context + logging;
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
void validate_candidate_settings(const CandidateSettings& settings) {
    if (settings.show_after < 1 || settings.show_after > 99)
        throw std::invalid_argument("candidate-show-after must be 1..99");
    if (settings.keys != CandidateKeys::Numbers && settings.keys != CandidateKeys::Asdfjkl)
        throw std::invalid_argument("candidate-keys must equal numbers or asdfjkl");
}
std::wstring candidate_settings_status(const CandidateSettings& settings) {
    return L"候補一覧: " + std::to_wstring(settings.show_after) + L"回目から / 選択キー: " +
        std::wstring(candidate_labels(settings.keys));
}
DictionarySettings parse_dictionary_settings(std::string_view bytes) {
    if (bytes.size() > max_settings_bytes) throw std::invalid_argument("settings exceed 64 KiB");
    from_utf8(bytes); // Validate before interpreting keys or paths.
    if (bytes.find('\0') != bytes.npos) throw std::invalid_argument("settings contain a null character");
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
    DictionarySettings settings; bool version = false, learning = false, romaji = false;
    std::set<std::string_view> inference_keys, candidate_options;
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
        } else if (key == "romaji") {
            if (romaji) throw std::invalid_argument("romaji must appear at most once");
            romaji = true; settings.romaji_rules = std::filesystem::path(wide_message(value)); validate_path(settings.romaji_rules);
        } else if (key == "inference" || key == "zenz-model" || key == "zenz-server" || key == "inference-threads" || key == "inference-timeout-ms" ||
            key == "zenz-min-length" || key == "zenz-rerank" || key == "zenz-learn-fallback" || key == "zenz-rerank-limit" || key == "zenz-score-timeout-ms" ||
            key == "zenz-rerank-wait-ms" || key == "zenz-promote-threshold" || key == "zenz-max-score-gap" || key == "input-log" || key == "input-log-directory" ||
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
                validate_path(settings.zenz.log_directory);
            } else if (key == "zenz-model" || key == "zenz-server") {
                auto& path = key == "zenz-model" ? settings.zenz.model : settings.zenz.server;
                path = std::filesystem::path(wide_message(value)); validate_path(path);
            } else if (key == "zenz-rerank" || key == "zenz-learn-fallback") {
                if (value != "on" && value != "off") throw std::invalid_argument("zenz toggle must equal on or off");
                (key == "zenz-rerank" ? settings.zenz.policy.rerank : settings.zenz.policy.learn_fallback) = value == "on";
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
                else settings.zenz.policy.score_timeout_ms = number;
            }
        } else if (key == "dictionary") {
            auto path = std::filesystem::path(wide_message(value)); validate_path(path);
            settings.dictionaries.push_back(std::move(path));
            if (settings.dictionaries.size() > max_dictionaries) throw std::invalid_argument("at most 32 dictionaries are supported");
        } else throw std::invalid_argument("unknown settings key");
    }
    if (!version) throw std::invalid_argument("settings version is missing");
    validate_candidate_settings(settings.candidates);
    validate_zenz_settings(settings.zenz);
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
std::string serialize_dictionary_settings(const DictionarySettings& settings) { return serialize(settings); }
void write_file_atomic(const std::filesystem::path& path, std::string_view bytes, bool replace_existing) {
    validate_path(path);
    if (bytes.size() > 64 * 1024 * 1024) throw std::invalid_argument("file exceeds 64 MiB");
    std::filesystem::create_directories(path.parent_path());
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("cannot allocate temporary settings name");
    wchar_t suffix[40]{}; StringFromGUID2(guid, suffix, ARRAYSIZE(suffix));
    TemporaryFile temporary;
    temporary.path = path.native() + L"." + suffix + L".tmp";
    // Readers may open the published name while this handle still flushes the rename.
    temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporary.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create temporary settings file");
    temporary.created = true;
    DWORD written = 0;
    if (!WriteFile(temporary.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
        !FlushFileBuffers(temporary.handle)) throw std::runtime_error("cannot write settings; previous file was kept");
    // POSIX semantics replace the name even while a reader holds the old file
    // with FILE_SHARE_DELETE (e.g. IMEs reading the user dictionary); the reader
    // keeps its complete old generation. Other file systems use MoveFileEx.
    bool renamed = false;
    if (replace_existing) {
        const auto& target = path.native();
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, target.c_str(), info->FileNameLength);
        renamed = SetFileInformationByHandle(temporary.handle, FileRenameInfoEx, info, static_cast<DWORD>(buffer.size())) != FALSE;
        if (renamed) { temporary.created = false; FlushFileBuffers(temporary.handle); }
    }
    CloseHandle(temporary.handle); temporary.handle = INVALID_HANDLE_VALUE;
    if (!renamed && !MoveFileExW(temporary.path.c_str(), path.c_str(), (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
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
std::optional<std::filesystem::path> dictionary_cache_path(const DictionaryConfiguration& configuration) {
    if (const auto value = environment_value(L"NSKKZIME_DICTIONARY_CACHE")) {
        if (*value == L"off") return {};
        return std::filesystem::path(*value);
    }
    if (configuration.demo || configuration.environment || configuration.file.empty()) return {};
    return configuration.file.parent_path() / L"dictionary-cache-v1";
}
LoadedDictionaries load_dictionaries(const DictionaryConfiguration& configuration, const std::optional<std::filesystem::path>& cache_root) {
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
            std::shared_ptr<const Dictionary> dictionary; DictionaryReport report; std::wstring cache_status;
            if (cache_root) {
                auto cached = load_cached_dictionary(path,*cache_root); dictionary=std::move(cached.dictionary); report=std::move(cached.report);
                switch (cached.use) {
                case DictionaryCacheUse::Shared: cache_status=L" / 共有キャッシュ"; break;
                case DictionaryCacheUse::Disk: cache_status=L" / 永続キャッシュ"; break;
                case DictionaryCacheUse::Created: cache_status=L" / キャッシュ作成"; break;
                case DictionaryCacheUse::Fallback: cache_status=L" / キャッシュ利用不可・通常読み込み"; break;
                }
            } else { auto parsed=SkkDictionary::load(path); report=parsed->report(); dictionary=std::move(parsed); }
            loaded.status += L"\r\n   読み込み済み: " + std::to_wstring(report.entries) + L"見出し / " +
                std::to_wstring(report.candidates) + L"候補（" + wide_message(encoding_name(report.encoding)) + L"）" + cache_status;
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
    loaded.sources = std::move(dictionaries);
    loaded.dictionary = std::make_shared<DictionarySet>(loaded.sources);
    return loaded;
}
} // namespace nskkzime::windows
