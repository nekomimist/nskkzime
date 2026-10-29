#pragma once
#include <string>

namespace nskkzime::windows {
inline constexpr wchar_t default_ui_font[] = L"Yu Gothic UI";
struct UiSettings {
    std::wstring font = default_ui_font;
};
} // namespace nskkzime::windows
