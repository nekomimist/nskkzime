#include "app_settings.h"
#include "local_storage.h"
#include "windows_text.h"
#include "diagnostic_file.h"
#include <windows.h>
#include <algorithm>
#include <stdexcept>

namespace nskkzime::windows {
void validate_zenz_settings(const ZenzSettings& settings, bool check_files) {
    // Settings may be edited by a different architecture from the server.
    // The launcher checks the actual executable's CPU contract before startup.
    validate_inference_policy(settings.policy);
    if (settings.log_input || !settings.log_directory.empty()) {
        validate_storage_path(settings.log_directory);
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
    if (settings.idle_seconds < 1 || settings.idle_seconds > 86400) throw std::invalid_argument("zenz idle time must be 1..86400 seconds");
    const auto check_path = [&](const std::filesystem::path& path) {
        validate_storage_path(path);
        if (path.native().find(L'\"') != std::wstring::npos || path.filename().empty()) throw std::invalid_argument("invalid inference file path");
        if (check_files && settings.enabled && !std::filesystem::is_regular_file(path)) throw std::invalid_argument("inference model/server file is missing");
    };
    if (settings.enabled || !settings.model.empty()) check_path(settings.model);
    // An empty server selects the one bundled with the IME; a path is a development override.
    if (!settings.server.empty()) check_path(settings.server);
}
void validate_tsf_log_settings(const AppSettings::TsfLogSettings& settings, bool check_files) {
    if (settings.enabled || !settings.directory.empty()) validate_diagnostic_directory(settings.directory, check_files && settings.enabled);
}
std::wstring tsf_log_settings_status(const AppSettings::TsfLogSettings& settings) {
    return settings.enabled ? L"TSF診断ログ: オン（本文・打鍵内容なし） / 保存先: " + settings.directory.native() : L"TSF診断ログ: オフ";
}
void validate_off_key_exclusions(const std::vector<std::wstring>& executables) {
    if (executables.size() > 32) throw std::invalid_argument("at most 32 off-key exclusions are supported");
    for (std::size_t i = 0; i < executables.size(); ++i) {
        const auto& name = executables[i];
        if (name.size() <= 4 || name.size() > 255 || name.front() == L' ' ||
            name.find_first_of(L"<>:\"/\\|?*") != name.npos ||
            std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 0x20 || ch == 0x7f; }) ||
            CompareStringOrdinal(name.data() + name.size() - 4, 4, L".exe", 4, TRUE) != CSTR_EQUAL)
            throw std::invalid_argument("off-key exclusion must be an executable filename ending in .exe, without paths or wildcards");
        for (std::size_t j = 0; j < i; ++j)
            if (CompareStringOrdinal(name.c_str(), -1, executables[j].c_str(), -1, TRUE) == CSTR_EQUAL)
                throw std::invalid_argument("duplicate off-key exclusion");
    }
}
bool off_keys_excluded(const AppSettings& settings, std::wstring_view executable) {
    return std::any_of(settings.off_key_exclusions.begin(), settings.off_key_exclusions.end(), [&](const auto& name) {
        return CompareStringOrdinal(name.data(), static_cast<int>(name.size()), executable.data(),
            static_cast<int>(executable.size()), TRUE) == CSTR_EQUAL;
    });
}
std::wstring zenz_settings_status(const ZenzSettings& settings) {
    const auto context = std::wstring(L"\r\n属性不明の周辺本文: ") + (settings.allow_unknown_context ? L"ローカル推論に利用（学習・入力ログの許可とは独立）" : L"取得しない");
    const auto logging = settings.log_input ? L"\r\n入力ログ: オン（読み・周辺本文・候補を保存） / 保存先: " + settings.log_directory.native() : std::wstring(L"\r\n入力ログ: オフ");
    if (!settings.enabled) return L"推論設定: オフ（入力ログも動作しません）" + context + logging;
    return L"推論設定: zenz / モデル: " + settings.model.native() + L"\r\nサーバー: " +
        (settings.server.empty() ? std::wstring(L"IMEと同じ版を使用") : L"開発用の上書き " + settings.server.native()) +
        L"\r\nスレッド: " + std::to_wstring(settings.threads) + L" / 期限: " + std::to_wstring(settings.timeout_ms) + L" ms" +
        L" / サーバー待機: " + std::to_wstring(settings.idle_seconds) + L" 秒"+
        L"\r\n長い読みの生成: "+std::to_wstring(settings.policy.long_min_length)+L"文字（0は無効） / 候補採点: "+(settings.policy.rerank ? L"on" : L"off")+
        L" / 採点への選び直しの学習: "+(settings.policy.rerank_learn ? L"on" : L"off")+
        L" / 短い生成候補の学習: "+(settings.policy.learn_fallback ? L"on" : L"off") + context + logging;
}
void validate_ui_settings(const UiSettings& settings) {
    const auto& font = settings.font;
    if (font.empty() || font.size() >= LF_FACESIZE || font.front() == L' ' || font.back() == L' ' ||
        std::any_of(font.begin(), font.end(), [](wchar_t ch) { return ch < 0x20 || ch == 0x7f; }))
        throw std::invalid_argument("ui-font must contain 1..31 UTF-16 code units, without control characters or surrounding spaces");
    utf8_message(font); // Reject malformed UTF-16 before saving or passing it to GDI.
    if (settings.input_mode_duration_ms < 1 || settings.input_mode_duration_ms > 60000)
        throw std::invalid_argument("input-mode-duration-ms must be 1..60000");
}
std::wstring ui_settings_status(const UiSettings& settings) {
    return L"入力UIのフォント: " + settings.font + L"\r\n入力モード表示: " +
        (settings.show_input_mode ? std::to_wstring(settings.input_mode_duration_ms) + L"ms / 単語登録中は常時表示" : L"オフ");
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
} // namespace nskkzime::windows
