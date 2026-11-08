#include "nskkzime/romaji.h"
#include <array>

namespace nskkzime {
namespace {
struct Row {
    std::string_view prefix;
    std::u32string_view a, i, u, e, o;
    std::string_view extensions;
};
// AZIK extensions: z/n = an, k = in, j = un, d = en, l = on,
// q = ai, h = uu, w = ei, p = ou. Omitted spellings stay available
// for the word shortcuts and the original romaji below.
constexpr auto rows = std::to_array<Row>({
    {"b", U"ば", U"び", U"ぶ", U"べ", U"ぼ", "znkjdlqhpw"},
    {"c", U"ちゃ", U"ち", U"ちゅ", U"ちぇ", U"ちょ", "znkjdlqhpw"},
    {"d", U"だ", U"ぢ", U"づ", U"で", U"ど", "znkjdlqhpw"},
    {"f", U"ふぁ", U"ふぃ", U"ふ", U"ふぇ", U"ふぉ", "znkjdlqhpw"},
    {"g", U"が", U"ぎ", U"ぐ", U"げ", U"ご", "znkjdlqhpw"},
    {"h", U"は", U"ひ", U"ふ", U"へ", U"ほ", "znkjdlqhpw"},
    {"j", U"じゃ", U"じ", U"じゅ", U"じぇ", U"じょ", "znkjdlqhpw"},
    {"k", U"か", U"き", U"く", U"け", U"こ", "znkjdlqhpw"},
    {"m", U"ま", U"み", U"む", U"め", U"も", "zkjdlqhpw"},
    {"n", U"な", U"に", U"ぬ", U"ね", U"の", "zkjdlqhpw"},
    {"p", U"ぱ", U"ぴ", U"ぷ", U"ぺ", U"ぽ", "znkjdlqhpw"},
    {"r", U"ら", U"り", U"る", U"れ", U"ろ", "znkjdlqhpw"},
    {"s", U"さ", U"し", U"す", U"せ", U"そ", "znkjdlqhpw"},
    {"t", U"た", U"ち", U"つ", U"て", U"と", "znkjdlqhpw"},
    {"v", U"う゛ぁ", U"う゛ぃ", U"う゛", U"う゛ぇ", U"う゛ぉ", "znkdlqpw"},
    {"w", U"わ", U"うぃ", U"う", U"うぇ", U"うぉ", "znkdlqp"},
    {"x", U"しゃ", U"し", U"しゅ", U"しぇ", U"しょ", "znkjdlqhpw"},
    {"y", U"や", U"", U"ゆ", U"", U"よ", "znjlqhp"},
    {"z", U"ざ", U"じ", U"ず", U"ぜ", U"ぞ", "znkjdlqhpw"},
    {"by", U"びゃ", U"", U"びゅ", U"びぇ", U"びょ", "znjdlqhpw"},
    {"gy", U"ぎゃ", U"", U"ぎゅ", U"ぎぇ", U"ぎょ", "znjdlqhpw"},
    {"hy", U"ひゃ", U"", U"ひゅ", U"ひぇ", U"ひょ", "zdlqhpw"},
    {"ky", U"きゃ", U"", U"きゅ", U"きぇ", U"きょ", "znjdlqhpw"},
    {"my", U"みゃ", U"", U"みゅ", U"みぇ", U"みょ", "znjdlqhpw"},
    {"ny", U"にゃ", U"", U"にゅ", U"にぇ", U"にょ", "znjdlqhpw"},
    {"py", U"ぴゃ", U"", U"ぴゅ", U"ぴぇ", U"ぴょ", "znjdlqhpw"},
    {"ry", U"りゃ", U"", U"りゅ", U"りぇ", U"りょ", "znjdlqhpw"},
    {"sy", U"しゃ", U"", U"しゅ", U"しぇ", U"しょ", "zjdlqhpw"},
    {"ty", U"ちゃ", U"", U"ちゅ", U"ちぇ", U"ちょ", "znjdlqhpw"},
    {"zy", U"じゃ", U"", U"じゅ", U"じぇ", U"じょ", "znjdlqhpw"},
    {"hg", U"ひゃ", U"", U"ひゅ", U"ひぇ", U"ひょ", "znjdlqhpw"},
    {"kg", U"きゃ", U"", U"きゅ", U"きぇ", U"きょ", "zndlqhpw"},
    {"mg", U"みゃ", U"", U"みゅ", U"みぇ", U"みょ", "znjdlqhpw"},
    {"ng", U"にゃ", U"", U"にゅ", U"にぇ", U"にょ", "znjdlqhpw"},
    {"pg", U"ぴゃ", U"", U"ぴゅ", U"ぴぇ", U"ぴょ", "znjdlqhpw"}
});
using Literal = std::pair<std::string_view, std::u32string_view>;
constexpr auto literals = std::to_array<Literal>({
    {";", U"っ"}, {"x;", U";"}, {"q", U"ん"}, {"nn", U"ん"},
    {"ca", U"ちゃ"}, {"ci", U"ち"}, {"cu", U"ちゅ"}, {"ce", U"ちぇ"}, {"co", U"ちょ"},
    {"xa", U"しゃ"}, {"xi", U"し"}, {"xu", U"しゅ"}, {"xe", U"しぇ"}, {"xo", U"しょ"},
    {"va", U"う゛ぁ"}, {"vi", U"う゛ぃ"}, {"vu", U"う゛"}, {"ve", U"う゛ぇ"}, {"vo", U"う゛ぉ"},
    {"vya", U"う゛ゃ"}, {"vye", U"う゛ぇ"}, {"vyo", U"う゛ょ"}, {"vyu", U"う゛ゅ"},
    {"br", U"ばら"}, {"bt", U"びと"}, {"bx", U"べい"},
    {"cc", U"ちゃ"}, {"cf", U"ちぇ"}, {"cv", U"ちゃい"}, {"cx", U"ちぇい"},
    {"dch", U"でゅー"}, {"dci", U"でぃ"}, {"dck", U"でぃん"}, {"dcp", U"どぅー"}, {"dcu", U"でゅ"},
    {"dd", U"でん"}, {"df", U"で"}, {"dg", U"だが"}, {"dm", U"でも"}, {"dr", U"である"},
    {"ds", U"です"}, {"dt", U"だち"}, {"dv", U"でん"}, {"dy", U"でぃ"},
    {"fm", U"ふむ"}, {"fp", U"ふぉー"}, {"fr", U"ふる"}, {"fs", U"ふぁい"},
    {"gr", U"がら"}, {"gt", U"ごと"}, {"hf", U"ふ"}, {"hh", U"ふう"}, {"ht", U"ひと"},
    {"jf", U"じゅ"}, {"jj", U"じゅん"}, {"jv", U"じゅう"},
    {"kA", U"ヵ"}, {"kE", U"ヶ"}, {"kf", U"き"}, {"kk", U"きん"}, {"km", U"かも"},
    {"kr", U"から"}, {"kt", U"こと"}, {"kv", U"きん"},
    {"mf", U"む"}, {"mn", U"もの"}, {"mr", U"まる"}, {"ms", U"ます"}, {"mt", U"また"}, {"mv", U"むん"},
    {"nb", U"ねば"}, {"nf", U"ぬ"}, {"nr", U"なる"}, {"nt", U"にち"}, {"nv", U"ぬん"},
    {"pf", U"ぽん"}, {"pp", U"ぽう"}, {"pv", U"ぽう"}, {"rr", U"られ"}, {"ryk", U"りょく"},
    {"sf", U"さい"}, {"sm", U"しも"}, {"sr", U"する"}, {"ss", U"せい"}, {"st", U"した"}, {"sv", U"さい"},
    {"tb", U"たび"}, {"tm", U"ため"}, {"tr", U"たら"}, {"tt", U"たち"},
    {"tgh", U"てゅー"}, {"tgi", U"てぃ"}, {"tgk", U"てぃん"}, {"tgp", U"とぅー"}, {"tgu", U"てゅ"},
    {"vp", U"う゛ぉー"}, {"wA", U"ゎ"}, {"wf", U"わい"}, {"wp", U"うぉー"}, {"wr", U"われ"}, {"wt", U"わた"},
    {"wha", U"うぁ"}, {"whe", U"うぇ"}, {"whi", U"うぃ"}, {"who", U"うぉ"}, {"whu", U"う"}, {"wso", U"うぉ"},
    {"xc", U"しゃ"}, {"xf", U"しぇい"}, {"xt", U"しゅつ"}, {"xv", U"しゃい"},
    {"xxa", U"ぁ"}, {"xxi", U"ぃ"}, {"xxu", U"ぅ"}, {"xxe", U"ぇ"}, {"xxo", U"ぉ"},
    {"xxh", U"←"}, {"xxj", U"↓"}, {"xxk", U"↑"}, {"xxl", U"→"},
    {"y<", U"←"}, {"y>", U"→"}, {"y^", U"↑"}, {"yf", U"ゆ"}, {"yi", U"ゐ"}, {"yr", U"よる"}, {"yv", U"ゆう"},
    {"zc", U"ざ"}, {"zf", U"ぜ"}, {"zr", U"ざる"}, {"zv", U"ざい"}, {"zx", U"ぜい"}, {"zz", U"ざん"}
});
}

std::shared_ptr<const RomajiRules> RomajiRules::azik(bool japanese_keyboard) {
    const auto build = [](bool japanese) {
        auto result = std::make_shared<RomajiRules>(*defaults());
        const auto add = [&](std::string key, std::u32string output) {
            // Only built-in AZIK replaces conflicting standard spellings.
            // User overlays still reject terminal/prefix ambiguity.
            auto it = result->rules_.lower_bound(key);
            while (it != result->rules_.end() && it->first.starts_with(key)) it = result->rules_.erase(it);
            result->rules_.emplace(std::move(key), RomajiRule{std::move(output), {}});
        };
        for (const auto& row : rows) {
            if (row.prefix.size() == 2 && row.prefix.back() == 'g') {
                for (const auto& [vowel, kana] : std::array<std::pair<char, std::u32string_view>, 4>{{
                    {'a', row.a}, {'u', row.u}, {'e', row.e}, {'o', row.o}}})
                    add(std::string(row.prefix) + vowel, std::u32string(kana));
            }
            for (const auto extension : row.extensions) {
                std::u32string_view kana;
                char32_t tail = U'ん';
                switch (extension) {
                case 'z': case 'n': kana = row.a; break;
                case 'k': kana = row.i; break;
                case 'j': kana = row.u; break;
                case 'd': kana = row.e; break;
                case 'l': kana = row.o; break;
                case 'q': kana = row.a; tail = U'い'; break;
                case 'h': kana = row.u; tail = U'う'; break;
                case 'p': kana = row.o; tail = U'う'; break;
                case 'w': kana = row.e; tail = U'い'; break;
                }
                add(std::string(row.prefix) + extension, std::u32string(kana) + tail);
            }
        }
        for (const auto& [key, text] : literals) add(std::string(key), std::u32string(text));
        if (japanese) { add(":", U"ー"); add("x@", U"@"); }
        else { add("'", U"ー"); add("x'", U"'"); add("x[", U"「"); }
        return result;
    };
    static const auto jis = build(true), us = build(false);
    return japanese_keyboard ? jis : us;
}
} // namespace nskkzime
