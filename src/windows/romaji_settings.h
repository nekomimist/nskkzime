#pragma once
#include "dictionary_settings.h"
namespace nskkzime::windows {
struct RuntimeConfiguration;
struct LoadedRomajiRules {
    std::shared_ptr<const RomajiRules> rules;
    std::wstring status;
    bool failed = false;
    std::string bytes; // The exact validated file; empty selects built-in rules.
    InputProfile profile;
};
InputProfile resolve_input_profile(const DictionarySettings&, std::string_view overlay = {});
std::wstring input_profile_status(const DictionarySettings&);
InputProfile runtime_input_profile(const RuntimeConfiguration&);
LoadedRomajiRules load_romaji_rules(const DictionaryConfiguration&);
std::shared_ptr<const RomajiRules> runtime_romaji_rules(); // Activation only, never in key handling.
std::shared_ptr<const RomajiRules> runtime_romaji_rules(const RuntimeConfiguration&);
std::wstring runtime_romaji_status();
}
