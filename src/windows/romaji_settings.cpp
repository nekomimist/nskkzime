#include "romaji_settings.h"
#include "runtime_configuration.h"
#include <fstream>
#include <mutex>
namespace nskkzime::windows {
namespace {
std::mutex mutex;
std::wstring status = L"ローマ字: 内蔵ルール";
}
InputProfile resolve_input_profile(const DictionarySettings& settings, std::string_view overlay) {
    auto profile = InputProfile::built_in(settings.input_method, settings.azik_keyboard);
    return overlay.empty() ? profile : profile.overlay(overlay);
}
std::wstring input_profile_status(const DictionarySettings& settings) {
    if (settings.input_method == InputMethod::Skk) return L"入力方式: SKK標準";
    return settings.azik_keyboard == AzikKeyboard::Jp106 ? L"入力方式: AZIK（DDSKK方式・日本語JIS）" : L"入力方式: AZIK（DDSKK方式・英語US）";
}
LoadedRomajiRules load_romaji_rules(const DictionaryConfiguration& configuration) {
    const auto profile = resolve_input_profile(configuration.settings);
    LoadedRomajiRules loaded{profile.rules(),input_profile_status(configuration.settings) + L"\r\nローマ字: 内蔵ルール",false,{},profile};
    const auto& path = configuration.settings.romaji_rules;
    if (path.empty()) return loaded;
    loaded.status = input_profile_status(configuration.settings) + L"\r\nローマ字: " + path.native();
    try {
        if (!path.is_absolute()) throw std::invalid_argument("romaji path must be absolute");
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) throw std::runtime_error("cannot open romaji rules");
        const auto size = stream.tellg();
        if (size < 0 || size > 262144) throw std::runtime_error("romaji rules exceed 256 KiB or cannot be read");
        std::string bytes(static_cast<std::size_t>(size), '\0');
        stream.seekg(0);
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("cannot read romaji rules");
        loaded.profile = profile.overlay(bytes);
        loaded.rules = loaded.profile.rules();
        loaded.bytes = std::move(bytes);
        loaded.status += L" / 追加・上書き " + std::to_wstring(loaded.rules->custom_count()) + L"件";
    } catch (const std::exception& error) {
        loaded.failed = true; loaded.status += L" / 読み込み失敗。内蔵ルールで継続: " + wide_message(error.what());
    }
    return loaded;
}
std::shared_ptr<const RomajiRules> runtime_romaji_rules() {
    return runtime_romaji_rules(capture_runtime_configuration());
}
std::shared_ptr<const RomajiRules> runtime_romaji_rules(const RuntimeConfiguration& settings) {
    return runtime_input_profile(settings).rules();
}
InputProfile runtime_input_profile(const RuntimeConfiguration& settings) {
    const std::lock_guard lock(mutex);
    try {
        if (settings.configuration_failed) throw std::runtime_error("invalid captured settings");
        auto loaded = load_romaji_rules(settings.dictionaries);
        status = std::move(loaded.status); return loaded.profile;
    } catch (...) {
        status = L"ローマ字設定を読み込めません。内蔵ルールで継続します。"; return InputProfile{};
    }
}
std::wstring runtime_romaji_status() { const std::lock_guard lock(mutex); return status; }
}
