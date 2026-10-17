#include "settings_editor.h"
#include "romaji_settings.h"
#include "local_storage.h"
#include <fstream>
namespace nskkzime::windows {
namespace {
std::optional<std::string> snapshot(const std::filesystem::path& file) {
    if (!file.is_absolute())
        throw std::invalid_argument("settings path must be absolute");
    if (!std::filesystem::exists(file))
        return {};
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot read settings file");
    const auto size = input.tellg();
    if (size < 0 || size > 65536)
        throw std::runtime_error("settings exceed 64 KiB");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read settings file");
    return bytes;
}
} // namespace
SettingsDocument open_settings_document(const std::filesystem::path& file) {
    SettingsDocument result;
    result.file = file;
    result.original = snapshot(file);
    if (result.original)
        result.settings = parse_dictionary_settings(*result.original);
    return result;
}
SettingsCheck check_settings_draft(const DictionarySettings& settings) {
    SettingsCheck result;
    try {
        serialize_dictionary_settings(settings); // Validate even disabled options and every path.
        DictionaryConfiguration configuration;
        configuration.settings = settings;
        const auto dictionaries = load_dictionaries(configuration);
        const auto romaji = load_romaji_rules(configuration);
        result.report = dictionaries.status + L"\r\n" + romaji.status + L"\r\n" + zenz_settings_status(settings.zenz) +
            L"\r\n" + candidate_settings_status(settings.candidates) + L"\r\n" + tsf_log_settings_status(settings.tsf_log);
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
void save_settings_document(SettingsDocument& document, const DictionarySettings& settings) {
    // Prepare every allocation before replacement so a successful write cannot
    // leave this editor with an old revision if an allocation subsequently fails.
    SettingsDocument next{document.file, settings, serialize_dictionary_settings(settings)};
    if (snapshot(document.file) != document.original)
        throw std::runtime_error("settings changed outside this window; reload before saving");
    write_file_atomic(document.file, *next.original);
    document = std::move(next);
}
} // namespace nskkzime::windows
