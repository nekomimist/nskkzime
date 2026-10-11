#include "nskkzime/numeric.h"
#include <algorithm>

namespace nskkzime {
namespace {
constexpr std::size_t max_text = 1024;
constexpr std::u32string_view kanji = U"〇一二三四五六七八九";
bool digit(char32_t ch) { return ch >= U'0' && ch <= U'9'; }
std::u32string unit_kanji(std::u32string_view number) {
    constexpr std::u32string_view small[] = {U"", U"十", U"百", U"千"};
    constexpr std::u32string_view large[] = {U"", U"万", U"億", U"兆", U"京"};
    std::u32string result;
    for (std::size_t start = 0; start < number.size();) {
        const auto count = start == 0 ? (number.size() - 1) % 4 + 1 : 4;
        const auto group = (number.size() - start - count) / 4;
        std::u32string part;
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = number[start + i] - U'0';
            const auto place = count - i - 1;
            if (!value) continue;
            if (value != 1 || place == 0) part += kanji[value];
            part += small[place];
        }
        // DDSKK writes 一千万, but 千億 and 千兆.
        if (group == 1 && part == U"千") part.insert(part.begin(), U'一');
        if (!part.empty()) { result += part; result += large[group]; }
        start += count;
    }
    return result.empty() ? U"〇" : result;
}
std::optional<std::u32string> format_number(std::u32string_view number, char32_t type) {
    if (number.empty() || number.size() > max_text || !std::all_of(number.begin(), number.end(), digit)) return {};
    if (type == U'0') return std::u32string(number);
    if (type == U'3') return number.size() <= 20 ? std::optional{unit_kanji(number)} : std::nullopt;
    std::u32string result;
    if (type == U'1' || type == U'2') {
        for (const auto ch : number) result += type == U'1' ? U'０' + (ch - U'0') : kanji[ch - U'0'];
    } else if (type == U'8') {
        // Group without an integer conversion, including numbers above uint64.
        const auto first = number.find_first_not_of(U'0');
        number = first == std::u32string_view::npos ? number.substr(number.size() - 1) : number.substr(first);
        for (std::size_t i = 0; i < number.size(); ++i) {
            if (i && (number.size() - i) % 3 == 0) result += U',';
            result += number[i];
        }
    } else return {};
    return result.size() <= max_text ? std::optional{std::move(result)} : std::nullopt;
}
}
NumericReading numeric_reading(const LookupKey& key) {
    auto text = key.reading;
    for (auto& ch : text) if (ch >= U'０' && ch <= U'９') ch = U'0' + (ch - U'０');
    NumericReading result{{{}, key.okuri}, {}};
    for (std::size_t i = 0; i < text.size();) {
        if (!digit(text[i])) { result.key.reading += text[i++]; continue; }
        std::u32string number;
        do {
            number += text[i++];
            if (i + 1 < text.size() && text[i] == U',' && digit(text[i + 1])) ++i;
        } while (i < text.size() && digit(text[i]));
        result.numbers.push_back(std::move(number));
        result.key.reading += U'#';
    }
    return result;
}
std::optional<NumericExpansion> expand_numeric(std::u32string_view text, std::span<const std::u32string> numbers) {
    if (numbers.empty()) return NumericExpansion{std::u32string(text), false};
    NumericExpansion result;
    std::size_t number = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == U'#' && i + 1 < text.size() && digit(text[i + 1])) {
            if (number >= numbers.size() || (i + 2 < text.size() && digit(text[i + 2]))) return {};
            auto formatted = format_number(numbers[number++], text[++i]);
            if (!formatted) return {};
            result.text += *formatted;
            result.converted = true;
        } else result.text += text[i];
        if (result.text.size() > max_text) return {};
    }
    return result;
}
}
