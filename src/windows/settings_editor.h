#pragma once
#include "dictionary_settings.h"
namespace nskkzime::windows {
struct SettingsDocument {
    std::filesystem::path file;
    DictionarySettings settings;
    std::optional<std::string> original;
};
struct SettingsCheck {
    bool valid = false;
    std::wstring report;
};
SettingsDocument open_settings_document(const std::filesystem::path&);
SettingsCheck check_settings_draft(const DictionarySettings&);
// Reject a changed destination. On any error the caller keeps its draft.
void save_settings_document(SettingsDocument&, const DictionarySettings&);
} // namespace nskkzime::windows
