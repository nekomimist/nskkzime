#pragma once
#include "dictionary_settings.h"
namespace nskkzime::windows {
struct LoadedRomajiRules {
    std::shared_ptr<const RomajiRules> rules;
    std::wstring status;
    bool failed = false;
};
LoadedRomajiRules load_romaji_rules(const DictionaryConfiguration&, const std::optional<std::wstring>& override_path);
std::shared_ptr<const RomajiRules> runtime_romaji_rules(); // Activation only, never in key handling.
std::wstring runtime_romaji_status();
}
