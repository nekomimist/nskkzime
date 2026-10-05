#include "prompt.h"

namespace zenz {
namespace {

constexpr char32_t kInputTag = 0xEE00;
constexpr char32_t kOutputTag = 0xEE01;
constexpr char32_t kContextTag = 0xEE02;
constexpr char32_t kRightContextTag = 0xEE07;
constexpr char32_t kReplacement = 0xFFFD;

// Decodes UTF-8, replacing malformed sequences with U+FFFD.
std::u32string decode_utf8(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        std::size_t len;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c >> 5) == 0x6) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c >> 4) == 0xE) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            cp = c & 0x07;
            len = 4;
        } else {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        bool ok = i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            ok = (cc >> 6) == 0x2;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string encode_utf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (char32_t cp : s) {
        append_utf8(out, cp);
    }
    return out;
}

}  // namespace

std::string to_katakana(std::string_view utf8) {
    std::u32string s = decode_utf8(utf8);
    for (char32_t& cp : s) {
        // ぁ..ゖ and ゝゞ have katakana counterparts 0x60 code points later.
        if ((cp >= 0x3041 && cp <= 0x3096) || cp == 0x309D || cp == 0x309E) {
            cp += 0x60;
        }
    }
    return encode_utf8(s);
}

std::string normalize_for_model(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (char c : utf8) {
        if (c == ' ' || c == '\t') {
            append_utf8(out, 0x3000);
        } else if (c != '\n' && c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

std::string utf8_suffix(std::string_view utf8, std::size_t n_chars) {
    std::u32string s = decode_utf8(utf8);
    if (s.size() <= n_chars) {
        return encode_utf8(s);
    }
    return encode_utf8(std::u32string_view(s).substr(s.size() - n_chars));
}

std::string utf8_prefix(std::string_view utf8, std::size_t n_chars) {
    std::u32string s = decode_utf8(utf8);
    return encode_utf8(std::u32string_view(s).substr(0, n_chars));
}

std::size_t utf8_length(std::string_view utf8) {
    return decode_utf8(utf8).size();
}

std::string build_prompt(const PromptInput& input, const PromptOptions& options) {
    const std::string left = utf8_suffix(normalize_for_model(input.left), options.max_left_chars);
    const std::string right = utf8_prefix(normalize_for_model(input.right), options.max_right_chars);
    std::string prompt;
    if (!left.empty()) {
        append_utf8(prompt, kContextTag);
        prompt += left;
    }
    if (!right.empty()) {
        append_utf8(prompt, kRightContextTag);
        prompt += right;
    }
    append_utf8(prompt, kInputTag);
    prompt += to_katakana(normalize_for_model(input.kana));
    append_utf8(prompt, kOutputTag);
    return prompt;
}

}  // namespace zenz
