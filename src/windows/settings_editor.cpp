#include "dictionary_loader.h"
#include "windows_text.h"
#include "settings_editor.h"
#include "romaji_settings.h"
namespace nskkzime::windows {
SettingsCheck check_settings_draft(const AppSettings& settings) {
    SettingsCheck result;
    try {
        serialize_settings(settings); // Validate even disabled options and every path.
        SettingsConfiguration configuration;
        configuration.settings = settings;
        const auto dictionaries = load_dictionaries(configuration);
        const auto romaji = load_romaji_rules(configuration);
        result.report = dictionaries.status + L"\r\n" + romaji.status + L"\r\n" + zenz_settings_status(settings.zenz) +
            L"\r\n" + candidate_settings_status(settings.candidates) + L"\r\n" + ui_settings_status(settings.ui) +
            L"\r\n" + tsf_log_settings_status(settings.tsf_log);
        validate_zenz_settings(settings.zenz, true);
        validate_tsf_log_settings(settings.tsf_log, true);
        result.valid = !dictionaries.failed && !romaji.failed;
        if (settings.zenz.enabled)
            result.report += L"\r\nモデル・サーバーの存在を確認しました。CPU/OSとの互換性は推論サーバーの起動時に検査します。推論動作の検証は含みません。";
        result.report = (result.valid ? L"検査が完了しました。\r\n"
                                      : L"設定を保存できません。以下のエラーを確認してください。\r\n") +
                        result.report;
    } catch (const std::exception& error) {
        result.report = L"検査エラー: " + wide_message(error.what()) + L"\r\n" + result.report;
    }
    return result;
}
} // namespace nskkzime::windows
