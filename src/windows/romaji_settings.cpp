#include "romaji_settings.h"
#include <mutex>
namespace nskkzime::windows {
namespace {
std::mutex mutex;
std::wstring status = L"ローマ字: 内蔵ルール";
}
LoadedRomajiRules load_romaji_rules(const DictionaryConfiguration& configuration, const std::optional<std::wstring>& override_path) {
    LoadedRomajiRules loaded{RomajiRules::defaults(),L"ローマ字: 内蔵ルール"};
    if (override_path && *override_path == L"builtin") return loaded;
    const auto path = override_path ? std::filesystem::path(*override_path) : configuration.settings.romaji_rules;
    if (path.empty()) return loaded;
    loaded.status = L"ローマ字: " + path.native();
    if (override_path) loaded.status += L"（NSKKZIME_ROMAJI）";
    try {
        if (!path.is_absolute()) throw std::invalid_argument("romaji path must be absolute");
        loaded.rules = RomajiRules::load(path);
        loaded.status += L" / 追加・上書き " + std::to_wstring(loaded.rules->custom_count()) + L"件";
    } catch (const std::exception& error) {
        loaded.failed = true; loaded.status += L" / 読み込み失敗。内蔵ルールで継続: " + wide_message(error.what());
    }
    return loaded;
}
std::shared_ptr<const RomajiRules> runtime_romaji_rules() {
    const std::lock_guard lock(mutex);
    try {
        const auto override_path = environment_value(L"NSKKZIME_ROMAJI");
        auto loaded = load_romaji_rules(override_path ? DictionaryConfiguration{} : current_dictionary_configuration(),override_path);
        status = std::move(loaded.status); return loaded.rules;
    } catch (...) {
        status = L"ローマ字設定を読み込めません。内蔵ルールで継続します。"; return RomajiRules::defaults();
    }
}
std::wstring runtime_romaji_status() { const std::lock_guard lock(mutex); return status; }
}
