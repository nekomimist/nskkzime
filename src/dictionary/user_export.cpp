#include "nskkzime/learning.h"
#include <stdexcept>

namespace nskkzime {
namespace {
void representable(bool ok) {
    if (!ok) throw std::invalid_argument("user dictionary contains a key, character or line not representable in SKK format");
}
std::u32string literal(std::u32string_view value) {
    representable(value.find(U'\ufffd') == std::u32string_view::npos);
    if (value.find_first_of(U"/;()[]\\\"") == std::u32string_view::npos) return std::u32string(value);
    std::u32string result = U"(concat \"";
    for (const auto ch : value) {
        switch (ch) {
        case U'/': result += U"\\057"; break;
        case U';': result += U"\\073"; break;
        case U'[': result += U"\\133"; break;
        case U']': result += U"\\135"; break;
        case U'\\': result += U"\\\\"; break;
        case U'"': result += U"\\\""; break;
        default: result += ch; break;
        }
    }
    return result + U"\")";
}
}
std::string UserDictionary::export_skk() const {
    // Separate sections and sort the complete headword, including its okuri key.
    std::map<std::u32string, std::u32string> ari, nasi;
    for (const auto& [key, candidates] : entries_) {
        const auto& reading = key.lookup.reading;
        representable(!reading.empty() && reading.front() != U';' &&
            reading.find_first_of(U" \t\r\n\ufffd") == std::u32string::npos);
        representable(key.okuri.find_first_of(U"/\ufffd") == std::u32string::npos);
        auto headword = reading;
        if (key.lookup.okuri) headword += static_cast<char32_t>(key.lookup.okuri);
        auto& line = (key.lookup.okuri ? ari : nasi)[headword];
        if (line.empty()) line = headword + U" /";
        // Block-only entries preserve the exact okuri scope of learned ranks;
        // never promote these candidates to every inflection of the headword.
        if (key.lookup.okuri) line += U"[" + key.okuri + U"/";
        for (const auto& candidate : candidates) {
            line += literal(candidate.text);
            if (!candidate.annotation.empty()) line += U";" + literal(candidate.annotation);
            line += U'/';
            representable(line.size() <= 65536);
        }
        if (key.lookup.okuri) line += U"]/";
        representable(line.size() <= 65536);
    }
    std::string result = ";; -*- coding: utf-8 -*-\n;; nskkzime user dictionary; exact okuri blocks\n;; okuri-ari entries.\n";
    auto append = [&](const std::u32string& line) {
        result += utf8(line); result += '\n';
        representable(result.size() <= 64 * 1024 * 1024);
    };
    for (auto it = ari.rbegin(); it != ari.rend(); ++it) append(it->second);
    result += ";; okuri-nasi entries.\n";
    for (const auto& [key, line] : nasi) append(line);
    return result;
}
} // namespace nskkzime
