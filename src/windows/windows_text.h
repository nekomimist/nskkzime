#pragma once
#include <string>
#include <string_view>

namespace nskkzime::windows {
std::wstring wide_message(std::string_view utf8_text);
std::string utf8_message(std::wstring_view wide_text);
} // namespace nskkzime::windows
