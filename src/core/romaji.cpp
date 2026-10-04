#include "nskkzime/romaji.h"
#include <array>
#include <utility>

namespace nskkzime {
namespace {
using Rule = std::pair<std::string_view, std::u32string_view>;
constexpr auto rules = std::to_array<Rule>({
    {"a", U"あ"}, {"i", U"い"}, {"u", U"う"}, {"e", U"え"}, {"o", U"お"},
    {"ka", U"か"}, {"ki", U"き"}, {"ku", U"く"}, {"ke", U"け"}, {"ko", U"こ"},
    {"ga", U"が"}, {"gi", U"ぎ"}, {"gu", U"ぐ"}, {"ge", U"げ"}, {"go", U"ご"},
    {"sa", U"さ"}, {"si", U"し"}, {"shi", U"し"}, {"su", U"す"}, {"se", U"せ"}, {"so", U"そ"},
    {"za", U"ざ"}, {"zi", U"じ"}, {"ji", U"じ"}, {"zu", U"ず"}, {"ze", U"ぜ"}, {"zo", U"ぞ"},
    {"ta", U"た"}, {"ti", U"ち"}, {"chi", U"ち"}, {"tu", U"つ"}, {"tsu", U"つ"}, {"te", U"て"}, {"to", U"と"},
    {"da", U"だ"}, {"di", U"ぢ"}, {"du", U"づ"}, {"de", U"で"}, {"do", U"ど"},
    {"na", U"な"}, {"ni", U"に"}, {"nu", U"ぬ"}, {"ne", U"ね"}, {"no", U"の"},
    {"ha", U"は"}, {"hi", U"ひ"}, {"hu", U"ふ"}, {"fu", U"ふ"}, {"he", U"へ"}, {"ho", U"ほ"},
    {"ba", U"ば"}, {"bi", U"び"}, {"bu", U"ぶ"}, {"be", U"べ"}, {"bo", U"ぼ"},
    {"pa", U"ぱ"}, {"pi", U"ぴ"}, {"pu", U"ぷ"}, {"pe", U"ぺ"}, {"po", U"ぽ"},
    {"ma", U"ま"}, {"mi", U"み"}, {"mu", U"む"}, {"me", U"め"}, {"mo", U"も"},
    {"ya", U"や"}, {"yu", U"ゆ"}, {"yo", U"よ"},
    {"ra", U"ら"}, {"ri", U"り"}, {"ru", U"る"}, {"re", U"れ"}, {"ro", U"ろ"},
    {"wa", U"わ"}, {"wi", U"うぃ"}, {"we", U"うぇ"}, {"wo", U"を"},
    {"kya", U"きゃ"}, {"kyu", U"きゅ"}, {"kyo", U"きょ"},
    {"gya", U"ぎゃ"}, {"gyu", U"ぎゅ"}, {"gyo", U"ぎょ"},
    {"sha", U"しゃ"}, {"shu", U"しゅ"}, {"sho", U"しょ"},
    {"sya", U"しゃ"}, {"syu", U"しゅ"}, {"syo", U"しょ"},
    {"ja", U"じゃ"}, {"ju", U"じゅ"}, {"jo", U"じょ"},
    {"jya", U"じゃ"}, {"jyu", U"じゅ"}, {"jyo", U"じょ"},
    {"zya", U"じゃ"}, {"zyu", U"じゅ"}, {"zyo", U"じょ"},
    {"cha", U"ちゃ"}, {"chu", U"ちゅ"}, {"cho", U"ちょ"},
    {"tya", U"ちゃ"}, {"tyu", U"ちゅ"}, {"tyo", U"ちょ"},
    {"nya", U"にゃ"}, {"nyu", U"にゅ"}, {"nyo", U"にょ"},
    {"hya", U"ひゃ"}, {"hyu", U"ひゅ"}, {"hyo", U"ひょ"},
    {"bya", U"びゃ"}, {"byu", U"びゅ"}, {"byo", U"びょ"},
    {"pya", U"ぴゃ"}, {"pyu", U"ぴゅ"}, {"pyo", U"ぴょ"},
    {"mya", U"みゃ"}, {"myu", U"みゅ"}, {"myo", U"みょ"},
    {"rya", U"りゃ"}, {"ryu", U"りゅ"}, {"ryo", U"りょ"},
    {"xa", U"ぁ"}, {"xi", U"ぃ"}, {"xu", U"ぅ"}, {"xe", U"ぇ"}, {"xo", U"ぉ"},
    {"xya", U"ゃ"}, {"xyu", U"ゅ"}, {"xyo", U"ょ"}, {"xtu", U"っ"}, {"xtsu", U"っ"},
    {"fa", U"ふぁ"}, {"fi", U"ふぃ"}, {"fe", U"ふぇ"}, {"fo", U"ふぉ"},
    {"va", U"ゔぁ"}, {"vi", U"ゔぃ"}, {"vu", U"ゔ"}, {"ve", U"ゔぇ"}, {"vo", U"ゔぉ"},
    {"-", U"ー"}, {",", U"、"}, {".", U"。"}, {"[", U"「"}, {"]", U"」"}
});
bool consonant(char ch) { return ch >= 'a' && ch <= 'z' && std::string_view("aiueon").find(ch) == std::string_view::npos; }
}

std::u32string Romaji::push(char ch) {
    std::u32string output;
    if (pending_ == "n") {
        if (ch == '\'') {
            if (!carried_n_) output += U"ん";
            clear();
            return output;
        }
        if (ch == 'n') {
            output += U"ん";
            carried_n_ = true;
            return output;
        }
        if (std::string_view("aiueoy").find(ch) == std::string_view::npos) {
            if (!carried_n_) output += U"ん";
            clear();
        }
    }
    carried_n_ = false;
    if (pending_.size() == 1 && pending_[0] == ch && consonant(ch)) {
        return output + U"っ";
    }
    pending_ += ch;
    while (!pending_.empty()) {
        bool prefix = pending_ == "n";
        for (const auto& [roman, kana] : rules) {
            if (roman == pending_) {
                output += kana;
                clear();
                return output;
            }
            prefix = prefix || roman.starts_with(pending_);
        }
        if (prefix) break;
        // Preserve unsupported input literally; never silently lose a keystroke.
        output += static_cast<unsigned char>(pending_.front());
        pending_.erase(0, 1);
    }
    return output;
}
std::u32string Romaji::flush() {
    auto output = pending_ == "n" ? (carried_n_ ? U"" : U"ん") : ascii(pending_);
    clear();
    return output;
}
bool Romaji::backspace() {
    if (pending_.empty()) return false;
    pending_.pop_back();
    carried_n_ = false;
    return true;
}
void Romaji::clear() { pending_.clear(); carried_n_ = false; }
} // namespace nskkzime
