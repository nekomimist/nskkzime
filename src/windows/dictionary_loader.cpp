#include "dictionary_loader.h"
#include "dictionary_cache.h"
#include "windows_text.h"

namespace nskkzime::windows {
std::optional<std::filesystem::path> dictionary_cache_path(const SettingsConfiguration& configuration) {
    if (configuration.demo || configuration.file.empty()) return {};
    return configuration.file.parent_path() / L"dictionary-cache-v1";
}
LoadedDictionaries load_dictionaries(const SettingsConfiguration& configuration, const std::optional<std::filesystem::path>& cache_root) {
    LoadedDictionaries loaded;
    if (configuration.demo) {
        loaded.dictionary = demo_dictionary();
        loaded.status = L"辞書: 内蔵テスト用（5見出し）。通常の辞書は未設定です。\r\n設定: " + configuration.file.native();
        return loaded;
    }
    loaded.status = L"辞書設定: " + configuration.file.native();
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
