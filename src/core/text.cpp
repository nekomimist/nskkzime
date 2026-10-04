#include "nskkzime/romaji.h"
#include <stdexcept>
namespace nskkzime {
std::u32string from_utf8(std::string_view text) {
    std::u32string result;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto begin = offset;
        const auto first = static_cast<unsigned char>(text[offset++]);
        char32_t value;
        std::size_t trailing;
        char32_t minimum;
        if (first < 0x80) { value = first; trailing = 0; minimum = 0; }
        else if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1f; trailing = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0f; trailing = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; trailing = 3; minimum = 0x10000; }
        else throw std::invalid_argument("invalid UTF-8 at byte " + std::to_string(begin));
        if (trailing > text.size() - offset) throw std::invalid_argument("truncated UTF-8 at byte " + std::to_string(begin));
        while (trailing--) {
            const auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xc0) != 0x80) throw std::invalid_argument("invalid UTF-8 continuation at byte " + std::to_string(offset - 1));
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            throw std::invalid_argument("invalid UTF-8 code point at byte " + std::to_string(begin));
        result += value;
    }
    return result;
}
std::u32string katakana(std::u32string_view text) {
    std::u32string result(text);
    for (auto& ch : result) if (ch >= U'ぁ' && ch <= U'ゖ') ch += 0x60;
    return result;
}
std::u32string ascii(std::string_view text) {
    std::u32string result;
    for (unsigned char ch : text) result += ch;
    return result;
}
std::string utf8(std::u32string_view text) {
    std::string result;
    for (char32_t ch : text) {
        if (ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) ch = 0xfffd;
        if (ch < 0x80) result += static_cast<char>(ch);
        else if (ch < 0x800) {
            result += static_cast<char>(0xc0 | (ch >> 6));
            result += static_cast<char>(0x80 | (ch & 0x3f));
        } else if (ch < 0x10000) {
            result += static_cast<char>(0xe0 | (ch >> 12));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (ch & 0x3f));
        } else {
            result += static_cast<char>(0xf0 | (ch >> 18));
            result += static_cast<char>(0x80 | ((ch >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (ch & 0x3f));
        }
    }
    return result;
}
} // namespace nskkzime
