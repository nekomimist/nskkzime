#pragma once
#include "settings_file.h"
namespace nskkzime::windows {
struct SettingsCheck {
    bool valid = false;
    std::wstring report;
};
SettingsCheck check_settings_draft(const AppSettings&);
} // namespace nskkzime::windows
