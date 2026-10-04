#include "nskkzime/romaji.h"
namespace nskkzime {
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
