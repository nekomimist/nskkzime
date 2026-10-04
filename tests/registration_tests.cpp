#include "nskkzime/engine.h"
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace nskkzime;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void equal(std::u32string_view actual, std::u32string_view expected) {
    if (actual != expected) throw std::runtime_error("expected " + utf8(expected) + ", got " + utf8(actual));
}
struct Session {
    Engine engine{demo_dictionary()};
    std::u32string document;
    std::vector<LearningEvent> saved;
    Result key(Key k) {
        const auto result = engine.handle(k); document += result.committed;
        if (result.learning) saved.push_back(*result.learning);
        if (result.registration && result.learning) engine.remember_registration(*result.learning);
        return result;
    }
    void type(std::string_view keys) { for (const auto ch : keys) key({KeyCode::Character, ch}); }
    void space() { key({KeyCode::Space}); }
    void enter() { key({KeyCode::Enter}); }
    void cancel() { key({KeyCode::Cancel}); }
    void start(std::string_view reading = "Michi") { type(reading); space(); check(engine.registering(), "registration did not start"); }
};
}
int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"miss-draft-conversion-and-single-enter", [] {
            Session s; s.start(); equal(s.engine.composition_text(), U"みち");
            s.type("Nihon"); s.space(); s.type("Kanji"); s.space();
            equal(s.engine.registration_text(), U"日本▼漢字"); equal(s.document, U"");
            equal(s.engine.composition_text(), U"みち"); check(s.saved.empty(), "tentative inner conversion learned");
            const auto result = s.key({KeyCode::Enter});
            equal(s.document, U"日本漢字"); check(result.registration && result.learning.has_value(), "registration event missing");
            equal(result.learning->key.reading, U"みち"); equal(result.learning->candidate.text, U"日本漢字");
            check(!s.engine.registering() && s.saved.size() == 1, "inner conversion leaked a learning event");
            s.type("Michi"); s.space(); equal(s.engine.preedit(), U"▼日本漢字");
        }},
        {"exhaustion-and-cancel-back-to-reading", [] {
            Session s; s.type("Kanji"); s.space(); s.space();
            check(s.key({KeyCode::Space}).candidates_exhausted && s.engine.registering(), "exhaustion did not register");
            equal(s.engine.composition_text(), U"かんじ"); s.type("kana"); s.cancel();
            equal(s.engine.preedit(), U"▽かんじ"); check(s.saved.empty(), "canceled draft learned");
            s.enter(); equal(s.document, U"かんじ");
        }},
        {"page-keys-only-change-inner-candidate", [] {
            Session s; s.start();
            check(!s.key({KeyCode::PageDown}).consumed && !s.key({KeyCode::PageUp}).consumed, "empty registration claimed page");
            s.type("Kanji"); s.space(); s.key({KeyCode::PageDown});
            equal(s.engine.registration_text(), U"▼感じ"); equal(s.engine.composition_text(), U"みち");
            check(s.saved.empty(), "paging in a registration learned tentative text");
            s.key({KeyCode::PageUp}); equal(s.engine.registration_text(), U"▼漢字");
            s.key({KeyCode::PageDown}); s.enter(); equal(s.document, U"感じ");
            check(s.saved.size() == 1 && s.saved[0].key.reading == U"みち", "inner paging changed registration key");
        }},
        {"empty-enter-and-reading-cancel", [] {
            Session s; s.start(); s.enter(); equal(s.engine.preedit(), U"▽みち");
            s.space(); s.type("Kanji"); s.space(); s.cancel();
            check(s.engine.registering(), "candidate cancel abandoned whole registration");
            equal(s.engine.registration_text(), U"▽かんじ"); s.cancel(); equal(s.engine.registration_text(), U"");
            s.cancel(); equal(s.engine.preedit(), U"▽みち"); check(s.saved.empty(), "cancel emitted learning");
        }},
        {"okuri-register-stem-only", [] {
            Session s; s.type("MiRu"); check(s.engine.registering(), "unknown okuri did not register");
            equal(s.engine.registration_reading(), U"み*る"); s.type("Kanji"); s.space(); s.enter();
            equal(s.document, U"漢字る"); check(s.saved[0].key == LookupKey{U"み", 'r'}, "wrong okuri key");
            equal(s.saved[0].okuri, U"る"); equal(s.saved[0].candidate.text, U"漢字");
            s.type("MiRu"); equal(s.engine.preedit(), U"▼漢字る");
            s.cancel(); s.cancel(); s.type("MiRe"); check(s.engine.registering(), "okuri preference crossed to another suffix");
        }},
        {"abbrev-latin-spaces-and-mode-return", [] {
            Session s; s.start("q/SKK"); equal(s.engine.registration_reading(), U"SKK");
            s.type("lAb/C;"); s.space(); s.type("X"); s.key({KeyCode::Hiragana}); s.type("kana"); s.enter();
            equal(s.document, U"Ab/C; Xかな"); check(s.saved[0].key == LookupKey{U"SKK"}, "abbrev case lost");
            check(s.engine.mode() == Mode::Katakana, "original input mode not restored");
            s.type("a"); equal(s.document, U"Ab/C; Xかなア");
        }},
        {"backspace-pending-and-terminal-n", [] {
            Session s; s.start(); s.type("kanak"); s.key({KeyCode::Backspace}); s.key({KeyCode::Backspace});
            equal(s.engine.registration_text(), U"か"); s.type("n"); s.enter(); equal(s.document, U"かん");
            equal(s.saved[0].candidate.text, U"かん");
        }},
        {"recursive-registration-and-immediate-reuse", [] {
            Session s; s.start("/outer"); s.start("/inner"); check(s.engine.registration_depth() == 2, "wrong recursive depth");
            s.type("Nihon"); s.space(); const auto inner = s.key({KeyCode::Enter});
            check(inner.registration && inner.committed.empty() && s.saved.size() == 1, "inner registration did not remain in draft");
            equal(s.engine.registration_text(), U"日本"); equal(s.engine.composition_text(), U"outer");
            s.type("/inner"); s.space(); equal(s.engine.registration_text(), U"日本▼日本"); s.enter();
            equal(s.document, U"日本日本"); check(s.saved.size() == 2, "recursive events missing or duplicated");
            equal(s.saved[0].key.reading, U"inner"); equal(s.saved[1].key.reading, U"outer");
        }},
        {"recursive-cancel-keeps-only-already-registered-inner", [] {
            Session s; s.start("/outer"); s.start("/inner"); s.type("kana"); s.enter(); s.cancel();
            check(s.saved.size() == 1 && s.engine.stage() == Stage::Reading, "outer cancel undid or added registration");
            equal(s.document, U""); equal(s.saved[0].key.reading, U"inner");
        }},
        {"temporary-cache-yields-to-shared-ranking", [] {
            auto dictionary = std::make_shared<MemoryDictionary>(); Session s{Engine(dictionary)};
            s.start(); s.type("kana"); s.enter();
            dictionary->add({U"みち"}, {{"newer", U"新候補", {}}, {"registered", U"かな", {}}});
            s.type("Michi"); s.space(); equal(s.engine.preedit(), U"▼新候補");
            s.cancel(); s.cancel(); dictionary->add({U"みち"}, {{"other", U"別候補", {}}});
            s.type("Michi"); s.space(); equal(s.engine.preedit(), U"▼別候補");
            check(s.engine.candidates().size() == 1, "retired temporary cache resurrected a spelling");
        }},
        {"trial-copy-pure-probe-and-reset", [] {
            Session s; s.start(); s.type("kana"); const auto revision = s.engine.revision();
            for (int i = 0; i < 3; ++i) check(s.engine.would_handle({KeyCode::Enter}), "register key unclaimed");
            check(s.engine.revision() == revision, "probe mutated registration");
            auto trial = s.engine; const auto proposed = trial.handle({KeyCode::Enter}); check(proposed.registration, "trial did not produce event");
            equal(s.engine.registration_text(), U"かな"); equal(s.engine.composition_text(), U"みち");
            check(demo_dictionary()->lookup({U"みち"}).empty(), "trial mutated dictionary");
            s.engine.reset(); equal(s.engine.preedit(), U""); s.type("a"); equal(s.document, U"あ"); check(s.saved.empty(), "reset saved draft");
        }},
        {"draft-and-recursion-limits-are-transactional", [] {
            Session s; s.start(); s.type("l" + std::string(1024, 'a'));
            check(s.key({KeyCode::Character, 'b'}).registration_limit, "oversized draft accepted");
            equal(s.engine.registration_text(), std::u32string(1024, U'a')); s.enter(); check(s.saved.size() == 1, "bounded draft not accepted");
            Session nested; for (int i = 0; i < 8; ++i) nested.start("/x");
            nested.type("/y"); check(nested.key({KeyCode::Space}).registration_limit && nested.engine.registration_depth() == 8, "recursion limit ignored");
            nested.engine.reset(); check(!nested.engine.registering(), "reset retained nested state");
        }},
        {"oversized-nested-accept-does-not-learn", [] {
            Session s; s.start("/outer"); s.type("l" + std::string(1020, 'a')); s.key({KeyCode::Hiragana}); s.start("/inner");
            s.type("l12345"); const auto result = s.key({KeyCode::Enter});
            check(result.registration_limit && !result.learning && !result.registration && s.saved.empty(), "failed nested accept learned");
            check(s.engine.registration_depth() == 2, "failed nested accept lost draft");
        }},
        {"random-registration-copy-and-output-invariants", [] {
            Session s; std::mt19937 random(54321); const std::string keys = "KanjiMichi/qlxa";
            for (int i = 0; i < 10000; ++i) {
                if (i % 100 == 0) { s.engine.reset(); s.key({KeyCode::Hiragana}); s.start(); }
                const auto choice = random() % 20;
                const Key key = choice < keys.size() ? Key{KeyCode::Character, keys[choice]} : Key{static_cast<KeyCode>(1 + random() % 7)};
                const bool handled = s.engine.would_handle(key); const auto result = s.key(key);
                check(result.consumed == handled, "probe and handle disagree");
                check(s.engine.registration_depth() <= 8, "unbounded registration depth");
                check(!result.registration || result.learning.has_value(), "registered without an event");
                if (s.engine.registering()) check(result.committed.empty(), "tentative draft reached host");
                if (s.engine.input_stage() == Stage::Selecting) check(s.engine.candidate_index() < s.engine.candidates().size(), "invalid nested selection");
            }
        }}
    };
    unsigned passed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << '/' << tests.size() << " passed\n"; return passed == tests.size() ? 0 : 1;
}
