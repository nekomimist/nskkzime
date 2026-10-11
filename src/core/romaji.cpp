#include "nskkzime/romaji.h"
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace nskkzime {
namespace {
using Rule = std::pair<std::string_view, std::u32string_view>;
constexpr auto base_rules = std::to_array<Rule>({
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
    {"-", U"ー"}, {",", U"、"}, {".", U"。"}, {"[", U"「"}, {"]", U"」"},
    {"she", U"しぇ"}, {"je", U"じぇ"}, {"che", U"ちぇ"}, {"ye", U"いぇ"},
    {"kye", U"きぇ"}, {"gye", U"ぎぇ"}, {"sye", U"しぇ"}, {"zye", U"じぇ"},
    {"tye", U"ちぇ"}, {"nye", U"にぇ"}, {"hye", U"ひぇ"}, {"bye", U"びぇ"},
    {"pye", U"ぴぇ"}, {"mye", U"みぇ"}, {"rye", U"りぇ"}, {"jye", U"じぇ"},
    {"thi", U"てぃ"}, {"thu", U"てゅ"}, {"dhi", U"でぃ"}, {"dhu", U"でゅ"},
    {"tsa", U"つぁ"}, {"tsi", U"つぃ"}, {"tse", U"つぇ"}, {"tso", U"つぉ"},
    {"xwa", U"ゎ"}, {"xka", U"ゕ"}, {"xke", U"ゖ"}, {"xn", U"ん"},
    {"z ", U"　"}, {"z*", U"※"}, {"z,", U"‥"}, {"z-", U"〜"}, {"z.", U"…"},
    {"z/", U"・"}, {"z0", U"○"}, {"z@", U"◎"}, {"z[", U"『"}, {"z]", U"』"},
    {"z{", U"〖"}, {"z}", U"〗"}, {"z(", U"（"}, {"z)", U"）"},
    {"zh", U"←"}, {"zj", U"↓"}, {"zk", U"↑"}, {"zl", U"→"}
});
bool consonant(char ch) { return ch >= 'a' && ch <= 'z' && std::string_view("aiueon").find(ch) == std::string_view::npos; }
}

std::shared_ptr<const RomajiRules> RomajiRules::defaults() {
    static const auto table = [] {
        auto result = std::make_shared<RomajiRules>();
        for (const auto& [key, value] : base_rules) result->rules_.emplace(key, RomajiRule{std::u32string(value),{}});
        return result;
    }();
    return table;
}
const RomajiRule* RomajiRules::find(std::string_view key) const {
    const auto it = rules_.find(key); return it == rules_.end() ? nullptr : &it->second;
}
bool RomajiRules::prefix(std::string_view key) const {
    const auto it = rules_.lower_bound(key); return it != rules_.end() && it->first.starts_with(key);
}
std::shared_ptr<const RomajiRules> RomajiRules::parse(std::string_view bytes) {
    if (bytes.size() > 262144) throw std::invalid_argument("romaji rules exceed 256 KiB");
    from_utf8(bytes);
    if (bytes.find('\0') != bytes.npos) throw std::invalid_argument("romaji rules contain a null character");
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
    auto result = std::make_shared<RomajiRules>(*defaults()); std::map<std::string,std::size_t> seen;
    std::size_t line_number = 0; bool version = false;
    auto fail = [&](const char* reason) { throw std::invalid_argument("romaji line " + std::to_string(line_number) + ": " + reason); };
    auto key_valid = [](std::string_view key) {
        if (key.empty() || key.size() > 16) return false;
        for (const auto ch : key) if (ch < 0x20 || ch > 0x7e || (ch >= 'A' && ch <= 'Z')) return false;
        return key.front() != ' ' && key.front() != 'q' && key.front() != 'l' && key.front() != '/';
    };
    while (!bytes.empty()) {
        ++line_number;
        const auto end = bytes.find('\n'); auto line = bytes.substr(0,end);
        bytes = end == bytes.npos ? std::string_view{} : bytes.substr(end+1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || (line.starts_with('#') && line.find('\t') == line.npos)) continue;
        if (!version) { if (line != "version=1") fail("expected version=1"); version = true; continue; }
        const auto tab = line.find('\t'); if (tab == line.npos) fail("expected input, output and optional carry separated by tabs");
        const auto key = line.substr(0,tab); line.remove_prefix(tab+1);
        const auto next = line.find('\t'); const auto text = line.substr(0,next);
        const auto carry = next == line.npos ? std::string_view{} : line.substr(next+1);
        if (!key_valid(key) || (!carry.empty() && !key_valid(carry))) fail("invalid or reserved key/carry");
        auto output = from_utf8(text); if (output.empty() || output.size() > 64) fail("output must contain 1 to 64 Unicode characters");
        for (const auto ch : output) if (ch < 0x20 || ch == 0x7f || ch == 0xfffc) fail("output contains a control or object character");
        if (!seen.emplace(key,line_number).second) fail("duplicate input");
        if (seen.size() > 2048) fail("at most 2048 custom rules are supported");
        result->rules_.insert_or_assign(std::string(key),RomajiRule{std::move(output),std::string(carry)});
    }
    if (!version) fail("expected version=1");
    // Immediate matches must not hide longer rules. Carries remain unfinished
    // input until the next key, rather than recursively feeding themselves.
    for (auto it = result->rules_.begin(); it != result->rules_.end(); ++it) {
        const auto next = std::next(it);
        if (next != result->rules_.end() && next->first.starts_with(it->first)) {
            line_number = seen.contains(it->first) ? seen.at(it->first) : seen.at(next->first);
            fail("a complete key is a prefix of another rule");
        }
        const auto& carry = it->second.carry;
        if (!carry.empty() && (result->find(carry) || (!result->prefix(carry) && carry != "n"))) {
            line_number = seen.at(it->first); fail("carry must be an unfinished rule prefix");
        }
    }
    result->custom_count_ = seen.size(); return result;
}
std::shared_ptr<const RomajiRules> RomajiRules::load(const std::filesystem::path& file) {
    std::ifstream stream(file,std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open romaji rules");
    const auto size = stream.tellg();
    if (size < 0 || size > 262144) throw std::runtime_error("romaji rules exceed 256 KiB or cannot be read");
    std::string bytes(static_cast<std::size_t>(size),'\0'); stream.seekg(0);
    if (!stream.read(bytes.data(),static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("cannot read romaji rules");
    return parse(bytes);
}
Romaji::Romaji(std::shared_ptr<const RomajiRules> rules) : rules_(std::move(rules)) {
    if (!rules_) throw std::invalid_argument("romaji rules must not be null");
}
bool Romaji::accepts(char ch) const { return rules_->prefix(pending_ + ch); }
std::u32string Romaji::resolve_pending(char ch) {
    if (pending_.empty() || accepts(ch) || (pending_ == "n" && (ch == 'n' || ch == '\'')) ||
        (pending_.size() == 1 && pending_[0] == ch && consonant(ch))) return {};
    auto output = pending_ == "n" && !carried_n_ ? std::u32string{U"ん"} : std::u32string{};
    // DDSKK retries only the new key from the root, discarding the whole prefix.
    clear();
    return output;
}
std::u32string Romaji::push(char ch) {
    auto output = resolve_pending(ch);
    const auto combined = pending_ + ch;
    if (const auto* rule = rules_->find(combined)) {
        pending_ = rule->carry; carried_n_ = false; return output + rule->output;
    }
    if (rules_->prefix(combined)) { pending_ = combined; carried_n_ = false; return output; }
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
    }
    carried_n_ = false;
    if (pending_.size() == 1 && pending_[0] == ch && consonant(ch)) {
        return output + U"っ";
    }
    if (ch == 'n') pending_ = "n";
    else output += static_cast<unsigned char>(ch); // Unsupported keys at the root remain literal.
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
