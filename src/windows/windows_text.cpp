#include "windows_text.h"
#include <windows.h>
#include <stdexcept>

namespace nskkzime::windows {
std::wstring wide_message(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::invalid_argument("invalid UTF-8 text");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), count) != count)
        throw std::runtime_error("UTF-8 conversion failed");
    return output;
}
std::string utf8_message(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::invalid_argument("invalid UTF-16 text");
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("UTF-16 conversion failed");
    return output;
}
} // namespace nskkzime::windows
