#include "nskkzime/input_profile.h"
#include <stdexcept>
#include <utility>

namespace nskkzime {
InputProfile::InputProfile() : rules_(RomajiRules::defaults()) {}
InputProfile::InputProfile(std::shared_ptr<const RomajiRules> standard_rules) : rules_(std::move(standard_rules)) {
    if (!rules_) throw std::invalid_argument("romaji rules must not be null");
}
InputProfile InputProfile::built_in(InputMethod method, AzikKeyboard keyboard) {
    if (method != InputMethod::Skk && method != InputMethod::Azik)
        throw std::invalid_argument("invalid input method");
    if (keyboard != AzikKeyboard::Jp106 && keyboard != AzikKeyboard::Us101)
        throw std::invalid_argument("invalid AZIK keyboard");
    InputProfile result;
    result.method_ = method;
    result.keyboard_ = keyboard;
    if (method == InputMethod::Azik) result.rules_ = RomajiRules::azik(keyboard == AzikKeyboard::Jp106);
    return result;
}
InputProfile InputProfile::overlay(std::string_view bytes) const {
    auto result = *this;
    const std::string reserved = method_ == InputMethod::Azik ? std::string{kana_key(), reading_key(), shifted_sokuon_key()} : std::string{};
    result.rules_ = RomajiRules::parse(bytes, rules_, reserved);
    return result;
}
char InputProfile::kana_key() const {
    return method_ == InputMethod::Skk ? 'q' : keyboard_ == AzikKeyboard::Jp106 ? '@' : '[';
}
InputProfile InputProfile::load_overlay(const std::filesystem::path& path) const {
    auto result = *this;
    const std::string reserved = method_ == InputMethod::Azik ? std::string{kana_key(), reading_key(), shifted_sokuon_key()} : std::string{};
    result.rules_ = RomajiRules::load(path, rules_, reserved);
    return result;
}
char InputProfile::reading_key() const { return keyboard_ == AzikKeyboard::Jp106 ? '`' : '{'; }
char InputProfile::shifted_sokuon_key() const { return keyboard_ == AzikKeyboard::Jp106 ? '+' : ':'; }
} // namespace nskkzime
