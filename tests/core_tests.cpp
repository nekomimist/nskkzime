#include "nskkzime/engine.h"
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nskkzime;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void equal(std::u32string_view actual, std::u32string_view expected) {
    if (actual != expected) throw std::runtime_error("expected " + utf8(expected) + ", got " + utf8(actual));
}
struct Session {
    Engine engine{demo_dictionary()};
    std::u32string committed;
    Result key(Key key) { auto result = engine.handle(key); committed += result.committed; return result; }
    void type(std::string_view text) { for (char ch : text) key({KeyCode::Character, ch}); }
};
std::u32string roman(std::string_view input) {
    Romaji parser; std::u32string result;
    for (char ch : input) result += parser.push(ch);
    return result + parser.flush();
}
}
int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"romaji/basic", [] { equal(roman("nihongo"), U"にほんご"); }},
        {"romaji/digraph", [] { equal(roman("kyoushitsu"), U"きょうしつ"); }},
        {"romaji/sokuon", [] { equal(roman("gakkou"), U"がっこう"); }},
        {"romaji/n-apostrophe", [] { equal(roman("kan'i"), U"かんい"); }},
        {"romaji/double-n", [] { equal(roman("konnichiha"), U"こんにちは"); equal(roman("nn"), U"ん"); }},
        {"romaji/n-before-consonant", [] { equal(roman("konnbanha"), U"こんばんは"); }},
        {"romaji/n-before-y", [] { equal(roman("nyanko"), U"にゃんこ"); }},
        {"romaji/punctuation", [] { equal(roman("[a-i]."), U"「あーい」。"); }},
        {"romaji/incomplete", [] { equal(roman("k"), U"k"); equal(roman("n"), U"ん"); }},
        {"romaji/unsupported-preserved", [] { equal(roman("zka"), U"zか"); }},
        {"input/direct-commit", [] { Session s; s.type("kana"); equal(s.committed, U"かな"); equal(s.engine.preedit(), U""); }},
        {"input/pending-backspace", [] { Session s; s.type("ky"); s.key({KeyCode::Backspace}); equal(s.engine.preedit(), U"k"); s.type("a"); equal(s.committed, U"か"); }},
        {"input/idle-special-keys-pass-through", [] {
            Session s; for (auto key : {KeyCode::Enter, KeyCode::Backspace, KeyCode::Cancel, KeyCode::Space})
                require(!s.key({key}).consumed, "idle key must pass through");
        }},
        {"input/terminal-n", [] { Session s; s.type("n"); s.key({KeyCode::Enter}); equal(s.committed, U"ん"); }},
        {"input/native-composition-does-not-flush-pending-romaji", [] {
            Session s; s.type("Nihon"); equal(s.engine.composition_text(), U"にほn");
            equal(s.engine.literal_text(), U"にほん"); s.key({KeyCode::Space}); equal(s.engine.composition_text(), U"日本");
        }},
        {"input/native-composition-omits-okuri-marker", [] {
            Session s; s.type("KaK"); equal(s.engine.composition_text(), U"かk");
            equal(s.engine.preedit(), U"▽か*k"); s.type("u"); equal(s.engine.composition_text(), U"書く");
        }},
        {"input/katakana-mode", [] { Session s; s.type("qkanaqkana"); equal(s.committed, U"カナかな"); }},
        {"input/latin-mode", [] { Session s; s.type("l"); require(!s.key({KeyCode::Character, 'A'}).consumed, "Latin must pass through"); s.key({KeyCode::Hiragana}); s.type("a"); equal(s.committed, U"あ"); }},
        {"conversion/reading-and-first-candidate", [] { Session s; s.type("Kanji"); equal(s.engine.preedit(), U"▽かんじ"); s.key({KeyCode::Space}); equal(s.engine.preedit(), U"▼漢字"); s.key({KeyCode::Enter}); equal(s.committed, U"漢字"); equal(s.engine.preedit(), U""); }},
        {"conversion/next-previous", [] { Session s; s.type("Kanji"); s.key({KeyCode::Space}); s.key({KeyCode::Space}); equal(s.engine.preedit(), U"▼感じ"); s.type("x"); equal(s.engine.preedit(), U"▼漢字"); s.type("x"); equal(s.engine.preedit(), U"▽かんじ"); }},
        {"conversion/exhaustion-retains-selection", [] { Session s; s.type("Kanji"); s.key({KeyCode::Space}); s.key({KeyCode::Space}); require(s.key({KeyCode::Space}).candidates_exhausted, "exhaustion must be signalled"); equal(s.engine.preedit(), U"▼感じ"); }},
        {"conversion/cancel-twice", [] { Session s; s.type("Kanji"); s.key({KeyCode::Space}); s.key({KeyCode::Cancel}); equal(s.engine.preedit(), U"▽かんじ"); s.key({KeyCode::Cancel}); equal(s.engine.preedit(), U""); equal(s.committed, U""); }},
        {"conversion/continue-typing", [] { Session s; s.type("Kanji"); s.key({KeyCode::Space}); s.type("desu"); equal(s.committed, U"漢字です"); }},
        {"conversion/okuri", [] { Session s; s.type("KaKu"); equal(s.engine.preedit(), U"▼書く"); s.key({KeyCode::Space}); equal(s.engine.preedit(), U"▼描く"); s.key({KeyCode::Enter}); equal(s.committed, U"描く"); }},
        {"conversion/okuri-continued", [] { Session s; s.type("TaBeru"); equal(s.committed, U"食べる"); }},
        {"conversion/okuri-cancel", [] { Session s; s.type("KaKu"); s.key({KeyCode::Cancel}); equal(s.engine.preedit(), U"▽か*く"); s.key({KeyCode::Backspace}); equal(s.engine.preedit(), U"▽か*"); s.key({KeyCode::Backspace}); equal(s.engine.preedit(), U"▽か"); }},
        {"conversion/missing-word-preserves-reading", [] { Session s; s.type("Michi"); require(s.key({KeyCode::Space}).dictionary_miss, "missing lookup must be signalled"); equal(s.engine.preedit(), U"▽みち"); s.key({KeyCode::Enter}); equal(s.committed, U"みち"); }},
        {"conversion/kana-confirmation", [] { Session s; s.type("Kanjiq"); equal(s.committed, U"カンジ"); }},
        {"conversion/terminal-n-lookup", [] { Session s; s.type("Nihon"); s.key({KeyCode::Space}); equal(s.engine.preedit(), U"▼日本"); }},
        {"conversion/reset-does-not-reuse-candidates", [] { Session s; s.type("Kanji"); s.key({KeyCode::Space}); s.engine.reset(); s.type("a"); equal(s.committed, U"あ"); require(s.engine.candidates().empty(), "candidates survived reset"); }},
        {"conversion/probe-is-pure", [] { Session s; s.type("Kanji"); const auto revision = s.engine.revision(); for (int i = 0; i < 5; ++i) require(s.engine.would_handle({KeyCode::Space}), "expected handled key"); require(s.engine.revision() == revision, "probe changed revision"); equal(s.engine.preedit(), U"▽かんじ"); }},
        {"conversion/copy-is-independent", [] { Session s; s.type("Kanji"); auto trial = s.engine; trial.handle({KeyCode::Space}); equal(s.engine.preedit(), U"▽かんじ"); equal(trial.preedit(), U"▼漢字"); }},
        {"unicode/non-bmp", [] { require(utf8(U"𠮷") == "\xf0\xa0\xae\xb7", "non-BMP UTF-8 failed"); equal(katakana(U"かな𠮷"), U"カナ𠮷"); }},
        {"state/random-key-invariants", [] {
            Session s; std::mt19937 random(12345);
            const std::string alphabet = "KanjiTaBeruqxl'-,";
            for (int i = 0; i < 10000; ++i) {
                const auto choice = random() % 24;
                const Key key = choice < alphabet.size() ? Key{KeyCode::Character, alphabet[choice]} : Key{static_cast<KeyCode>(1 + random() % 6)};
                const bool expected = s.engine.would_handle(key);
                const auto result = s.key(key);
                require(result.consumed == expected, "probe disagrees with handler");
                require(result.committed.find(U'▽') == std::u32string::npos && result.committed.find(U'▼') == std::u32string::npos, "marker committed");
                if (s.engine.stage() == Stage::Selecting) require(s.engine.candidate_index() < s.engine.candidates().size(), "invalid candidate index");
                if (i % 50 == 0) s.engine.reset();
            }
        }}
    };
    int failed = 0;
    for (const auto& [name, run] : cases) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << cases.size() - failed << '/' << cases.size() << " passed\n";
    return failed ? 1 : 0;
}
