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
        {"conversion/shift-release-during-first-kana", [] {
            for (const auto input : {"Kanji", "KAnji"}) {
                Session s; s.type(input); equal(s.engine.preedit(), U"▽かんじ");
                s.key({KeyCode::Space}); s.key({KeyCode::Enter}); equal(s.committed, U"漢字");
            }
            for (const auto input : {"Kyou", "KYou", "KYOu"}) {
                Session s; s.type(input); equal(s.engine.preedit(), U"▽きょう");
                require(s.engine.stage() == Stage::Reading, "extra Shift started okuri inside the first kana");
            }
            Session s; s.type("NEko"); equal(s.engine.preedit(), U"▽ねこ");
        }},
        {"conversion/shift-release-with-okuri", [] {
            auto dictionary = std::make_shared<MemoryDictionary>();
            dictionary->add({U"ころ", 'n'}, {{"korobu", U"転", U""}});
            for (const auto input : {"KoroNde", "KOroNde", "KOroNDe", "KOroNDE"}) {
                Session s{Engine(dictionary)}; s.type(input);
                equal(s.engine.preedit(), U"▼転んで");
                s.key({KeyCode::Enter}); equal(s.committed, U"転んで");
            }
            for (const auto input : {"KaKu", "KAKu", "KaKU", "KAKU"}) {
                Session s; s.type(input); equal(s.engine.preedit(), U"▼書く");
            }
        }},
        {"conversion/shift-keeps-okuri-boundary-and-terminal-n", [] {
            Session s; s.type("KAnJ"); equal(s.engine.preedit(), U"▽かん*j");
            s.type("I"); equal(s.engine.preedit(), U"▽かん*じ");
            Session vowel; vowel.type("AI"); equal(vowel.engine.preedit(), U"▽あ*い");
        }},
        {"conversion/shift-release-after-backspace-and-cancel", [] {
            Session s; s.type("KY"); s.key({KeyCode::Backspace}); s.type("Anji");
            equal(s.engine.preedit(), U"▽かんじ");
            s.key({KeyCode::Cancel}); s.type("KAKU"); equal(s.engine.preedit(), U"▼書く");
            s.key({KeyCode::Cancel}); equal(s.engine.preedit(), U"▽か*く");
        }},
        {"conversion/shift-release-in-katakana-mode", [] {
            Session s; s.type("qKAnji"); equal(s.engine.preedit(), U"▽カンジ");
            s.key({KeyCode::Space}); equal(s.engine.preedit(), U"▼漢字");
            s.key({KeyCode::Cancel}); s.key({KeyCode::Cancel}); s.type("KAKU");
            equal(s.engine.preedit(), U"▼書ク");
        }},
        {"conversion/okuri-continued", [] { Session s; s.type("TaBeru"); equal(s.committed, U"食べる"); }},
        {"conversion/okuri-cancel", [] { Session s; s.type("KaKu"); s.key({KeyCode::Cancel}); equal(s.engine.preedit(), U"▽か*く"); s.key({KeyCode::Backspace}); equal(s.engine.preedit(), U"▽か*"); s.key({KeyCode::Backspace}); equal(s.engine.preedit(), U"▽か"); }},
        {"conversion/missing-word-preserves-reading", [] { Session s; s.type("Michi"); require(s.key({KeyCode::Space}).dictionary_miss, "missing lookup must be signalled"); equal(s.engine.preedit(), U"▽みち"); s.key({KeyCode::Enter}); equal(s.committed, U"みち"); }},
        {"conversion/kana-confirmation", [] { Session s; s.type("Kanjiq"); equal(s.committed, U"カンジ"); }},
        {"conversion/terminal-n-lookup", [] { Session s; s.type("Nihon"); s.key({KeyCode::Space}); equal(s.engine.preedit(), U"▼日本"); }},
        {"conversion/reset-does-not-reuse-candidates", [] { Session s; s.type("Kanji"); s.key({KeyCode::Space}); s.engine.reset(); s.type("a"); equal(s.committed, U"あ"); require(s.engine.candidates().empty(), "candidates survived reset"); }},
        {"conversion/probe-is-pure", [] { Session s; s.type("Kanji"); const auto revision = s.engine.revision(); for (int i = 0; i < 5; ++i) require(s.engine.would_handle({KeyCode::Space}), "expected handled key"); require(s.engine.revision() == revision, "probe changed revision"); equal(s.engine.preedit(), U"▽かんじ"); }},
        {"conversion/copy-is-independent", [] { Session s; s.type("Kanji"); auto trial = s.engine; trial.handle({KeyCode::Space}); equal(s.engine.preedit(), U"▽かんじ"); equal(trial.preedit(), U"▼漢字"); }},
        {"abbrev/entry-and-literal-spelling", [] {
            Session s; s.type("/"); require(s.engine.abbrev(), "slash did not enter abbrev");
            equal(s.engine.preedit(), U"▽"); equal(s.engine.composition_text(), U"");
            s.type("SKKqlx-1.,/"); equal(s.engine.preedit(), U"▽SKKqlx-1.,/");
            s.key({KeyCode::Enter}); equal(s.committed, U"SKKqlx-1.,/");
            require(!s.engine.abbrev(), "confirm retained abbrev");
            s.type("a"); equal(s.committed, U"SKKqlx-1.,/あ");
        }},
        {"abbrev/case-sensitive-lookup-and-candidates", [] {
            auto dictionary = std::make_shared<MemoryDictionary>();
            dictionary->add({U"skk"}, {{"lower", U"かな漢字変換", U"注釈"}, {"second", U"日本語入力", U""}});
            dictionary->add({U"SKK"}, {{"upper", U"大文字", U""}});
            Session s{Engine(dictionary)}; s.type("/skk"); s.key({KeyCode::Space});
            equal(s.engine.preedit(), U"▼かな漢字変換"); s.key({KeyCode::Space});
            equal(s.engine.preedit(), U"▼日本語入力"); s.type("x");
            equal(s.engine.preedit(), U"▼かな漢字変換"); s.key({KeyCode::Enter});
            s.type("/SKK"); s.key({KeyCode::Space}); s.type("desu");
            equal(s.committed, U"かな漢字変換大文字です");
        }},
        {"abbrev/cancel-candidate-keeps-ascii-editing", [] {
            auto dictionary = std::make_shared<MemoryDictionary>();
            dictionary->add({U"q"}, {{"q", U"候補", U""}});
            for (const auto cancel : {Key{KeyCode::Cancel}, Key{KeyCode::Backspace}, Key{KeyCode::Character, 'x'}}) {
                Session s{Engine(dictionary)}; s.type("/q"); s.key({KeyCode::Space}); s.key(cancel);
                equal(s.engine.preedit(), U"▽q"); require(s.engine.abbrev(), "candidate cancel lost abbrev");
                s.type("l"); equal(s.engine.preedit(), U"▽ql");
                s.key({KeyCode::Cancel}); s.type("a"); equal(s.committed, U"あ");
            }
        }},
        {"abbrev/backspace-to-empty-and-exit", [] {
            Session s; s.type("/a"); s.key({KeyCode::Backspace});
            equal(s.engine.preedit(), U"▽"); require(s.engine.abbrev(), "last deletion prematurely exited abbrev");
            require(!s.key({KeyCode::Space}).dictionary_miss, "empty abbrev requested a lookup");
            s.key({KeyCode::Backspace}); require(!s.engine.abbrev(), "empty backspace did not exit abbrev");
            s.type("a"); equal(s.committed, U"あ");
        }},
        {"abbrev/missing-word-and-control-j", [] {
            Session s; s.type("/Missing"); require(s.key({KeyCode::Space}).dictionary_miss, "abbrev miss not signalled");
            equal(s.engine.preedit(), U"▽Missing"); s.key({KeyCode::Hiragana});
            require(!s.engine.abbrev(), "C-j retained abbrev"); s.type("a"); equal(s.committed, U"Missingあ");
        }},
        {"abbrev/katakana-return-and-latin-pass-through", [] {
            Session s; s.type("q/Test"); equal(s.engine.preedit(), U"▽Test");
            s.key({KeyCode::Enter}); s.type("a"); equal(s.committed, U"Testア");
            s.type("l"); require(!s.key({KeyCode::Character, '/'}).consumed, "Latin slash entered abbrev");
            require(!s.engine.abbrev(), "Latin slash changed mode");
        }},
        {"abbrev/copy-probe-and-reset", [] {
            Session s; s.type("/SK"); const auto revision = s.engine.revision();
            require(s.engine.would_handle({KeyCode::Character, 'K'}), "abbrev key not claimed");
            require(s.engine.revision() == revision, "abbrev probe mutated state");
            auto copy = s.engine; copy.handle({KeyCode::Character, 'K'});
            equal(s.engine.preedit(), U"▽SK"); equal(copy.preedit(), U"▽SKK");
            s.engine.reset(); s.type("a"); equal(s.committed, U"あ");
        }},
        {"learning/explicit-confirm-once-with-selected-metadata", [] {
            for (const auto confirm : {KeyCode::Enter, KeyCode::Hiragana}) {
                Session s; s.type("Kanji");
                require(!s.key({KeyCode::Space}).learning, "lookup learned before confirmation");
                require(!s.key({KeyCode::Space}).learning, "navigation learned before confirmation");
                require(!s.key({KeyCode::Space}).learning, "candidate exhaustion learned before confirmation");
                const auto result = s.key({confirm});
                require(result.learning.has_value(), "selected candidate was not emitted");
                const auto& event = *result.learning;
                require(event.key == LookupKey{U"かんじ"} && event.okuri.empty(), "wrong learning key");
                equal(event.candidate.text, U"感じ"); equal(event.candidate.annotation, U"感覚");
                require(event.candidate.id == "kanji-2" && event.candidate.source == CandidateSource::Dictionary, "candidate metadata lost");
                require(!s.key({KeyCode::Enter}).learning && !s.key({KeyCode::Hiragana}).learning, "duplicate learning event");
            }
        }},
        {"learning/implicit-confirm-separates-following-input", [] {
            Session s; s.type("Kanji"); s.key({KeyCode::Space});
            const auto result = s.key({KeyCode::Character, 'a'});
            equal(result.committed, U"漢字あ"); require(result.learning.has_value(), "implicit confirmation missing");
            equal(result.learning->candidate.text, U"漢字"); equal(result.learning->key.reading, U"かんじ");
            s.type("Kanji"); s.key({KeyCode::Space});
            const auto next = s.key({KeyCode::Character, 'N'});
            require(next.learning.has_value() && s.engine.stage() == Stage::Reading, "new conversion lost old event");
            s.type("ihon"); s.key({KeyCode::Space});
            const auto japanese = s.key({KeyCode::Enter});
            require(japanese.learning.has_value(), "new conversion missing event"); equal(japanese.learning->key.reading, U"にほん");
            equal(next.learning->candidate.text, U"漢字");
        }},
        {"learning/okuri-stem-and-normalized-reading", [] {
            for (const auto sequence : {"KaKU", "qKaKU"}) {
                Session s; s.type(sequence); s.key({KeyCode::Space});
                const auto result = s.key({KeyCode::Enter}); require(result.learning.has_value(), "okuri event missing");
                require(result.learning->key == LookupKey{U"か", 'k'}, "okuri key changed with display mode");
                equal(result.learning->okuri, U"く"); equal(result.learning->candidate.text, U"描");
            }
        }},
        {"learning/abbrev-preserves-case", [] {
            auto dictionary = std::make_shared<MemoryDictionary>();
            dictionary->add({U"SKK"}, {{"upper", U"日本語入力", U""}});
            Session s{Engine(dictionary)}; s.type("/SKK"); s.key({KeyCode::Space});
            const auto result = s.key({KeyCode::Enter}); require(result.learning.has_value(), "abbrev event missing");
            require(result.learning->key == LookupKey{U"SKK"}, "abbrev learning case changed");
        }},
        {"learning/cancel-edit-and-reset-do-not-learn", [] {
            for (const auto cancel : {Key{KeyCode::Cancel}, Key{KeyCode::Backspace}, Key{KeyCode::Character, 'x'}}) {
                Session s; s.type("Kanji"); s.key({KeyCode::Space});
                require(!s.key(cancel).learning && !s.key({KeyCode::Enter}).learning, "canceled candidate was learned");
            }
            Session s; s.type("Kanji"); s.key({KeyCode::Space}); s.engine.reset();
            require(!s.key({KeyCode::Enter}).learning, "reset retained learning");
        }},
        {"learning/literal-and-missing-words-do-not-learn", [] {
            for (const auto sequence : {"kana", "n", "Kanji", "/literal"}) {
                Session s; s.type(sequence); require(!s.key({KeyCode::Enter}).learning, "literal input learned");
            }
            Session s; s.type("Michi"); require(!s.key({KeyCode::Space}).learning, "dictionary miss learned");
            require(!s.key({KeyCode::Enter}).learning, "missing word learned without registration");
        }},
        {"learning/policy-controls-generated-and-dictionary-candidates", [] {
            for (const auto source : {CandidateSource::Dictionary, CandidateSource::GeneratedLong, CandidateSource::GeneratedFallback}) {
                for (const auto policy : {LearningPolicy::Learn, LearningPolicy::DoNotLearn}) {
                    auto dictionary = std::make_shared<MemoryDictionary>();
                    dictionary->add({U"かな"}, {{"candidate", U"仮名", U"注釈", source, policy}});
                    Session s{Engine(dictionary)}; s.type("Kana"); s.key({KeyCode::Space});
                    const auto result = s.key({KeyCode::Enter}); equal(result.committed, U"仮名");
                    require(result.learning.has_value() == (policy == LearningPolicy::Learn && source != CandidateSource::GeneratedLong),
                        "learning policy or long-generation exclusion ignored");
                    if (result.learning) require(result.learning->candidate.source == source, "candidate source changed");
                }
            }
        }},
        {"learning/trial-does-not-mutate-shared-dictionary", [] {
            auto dictionary = demo_dictionary(); Session s{Engine(dictionary)}; s.type("Kanji"); s.key({KeyCode::Space}); s.key({KeyCode::Space});
            auto trial = s.engine; const auto revision = s.engine.revision();
            const auto proposed = trial.handle({KeyCode::Enter}); require(proposed.learning.has_value(), "trial event missing");
            equal(s.engine.preedit(), U"▼感じ"); require(s.engine.revision() == revision, "trial modified original state");
            equal(dictionary->lookup({U"かんじ"})[0].text, U"漢字");
            const auto retried = s.key({KeyCode::Enter}); require(retried.learning.has_value(), "retry event missing");
            equal(retried.learning->candidate.text, proposed.learning->candidate.text);
            require(!s.key({KeyCode::Enter}).learning, "applied confirmation emitted twice");
        }},
        {"unicode/non-bmp", [] { require(utf8(U"𠮷") == "\xf0\xa0\xae\xb7", "non-BMP UTF-8 failed"); equal(katakana(U"かな𠮷"), U"カナ𠮷"); }},
        {"state/random-key-invariants", [] {
            Session s; std::mt19937 random(12345);
            const std::string alphabet = "KanjiTaBeruqxl'-,/";
            for (int i = 0; i < 10000; ++i) {
                const auto choice = random() % 24;
                const Key key = choice < alphabet.size() ? Key{KeyCode::Character, alphabet[choice]} : Key{static_cast<KeyCode>(1 + random() % 6)};
                const bool expected = s.engine.would_handle(key);
                const auto result = s.key(key);
                require(result.consumed == expected, "probe disagrees with handler");
                if (result.learning) require(result.consumed && !result.committed.empty() && result.learning->candidate.learning == LearningPolicy::Learn &&
                    result.learning->candidate.source != CandidateSource::GeneratedLong,
                    "invalid learning event");
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
