#pragma once
#include <string>

namespace nskkzime::windows {
inline constexpr wchar_t default_ui_font[] = L"Yu Gothic UI";
inline constexpr unsigned int default_input_mode_duration_ms = 3000;
struct UiSettings {
    std::wstring font = default_ui_font;
    bool show_input_mode = false;
    unsigned int input_mode_duration_ms = default_input_mode_duration_ms;
};
} // namespace nskkzime::windows
