#pragma once
#include "romaji.h"

namespace nskkzime {
enum class InputMethod { Skk, Azik };
enum class AzikKeyboard { Jp106, Us101 };

// Resolved once per activation, then copied with the transactional Engine.
class InputProfile {
public:
    InputProfile();
    explicit InputProfile(std::shared_ptr<const RomajiRules> standard_rules);
    static InputProfile built_in(InputMethod, AzikKeyboard = AzikKeyboard::Jp106);
    InputProfile overlay(std::string_view bytes) const;
    InputProfile load_overlay(const std::filesystem::path&) const;
    InputMethod method() const { return method_; }
    AzikKeyboard keyboard() const { return keyboard_; }
    const std::shared_ptr<const RomajiRules>& rules() const { return rules_; }
    char kana_key() const;
    char reading_key() const;
    char shifted_sokuon_key() const;
private:
    InputMethod method_ = InputMethod::Skk;
    AzikKeyboard keyboard_ = AzikKeyboard::Jp106;
    std::shared_ptr<const RomajiRules> rules_;
};
} // namespace nskkzime
