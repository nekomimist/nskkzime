#include "nskkzime/engine.h"
#include <iostream>
#include <stdexcept>

using namespace nskkzime;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void equal(std::u32string_view actual, std::u32string_view expected) {
    if (actual != expected) throw std::runtime_error("expected " + utf8(expected) + ", got " + utf8(actual));
}
struct Session {
    Engine engine;
    std::u32string output;
    explicit Session(AzikKeyboard keyboard = AzikKeyboard::Jp106, std::shared_ptr<const Dictionary> dictionary = demo_dictionary())
        : engine(std::move(dictionary), true, InputProfile::built_in(InputMethod::Azik, keyboard)) {}
    Result key(KeyCode code) { auto result = engine.handle({code}); output += result.committed; return result; }
    void type(std::string_view text) { for (const auto ch : text) output += engine.handle({KeyCode::Character, ch}).committed; }
};
}
int main() {
    try {
        for (const auto keyboard : {AzikKeyboard::Jp106, AzikKeyboard::Us101}) {
            const auto profile = InputProfile::built_in(InputMethod::Azik, keyboard).overlay("version=1\n");
            Session input(keyboard);
            input.type("kzkqkkkgpxtxxaxxixxuxxexxoxxhxxjxxkxxlnnq");
            equal(input.output, U"かんかいきんきょうしゅつぁぃぅぇぉ←↓↑→んん");
            input.type("y<y>y^"); equal(input.output, U"かんかいきんきょうしゅつぁぃぅぇぉ←↓↑→んん←→↑");
            Session punctuation(keyboard);
            punctuation.type(keyboard == AzikKeyboard::Jp106 ? "@vkvl@" : "[vkvl[");
            equal(punctuation.output, U"ヴィンヴォン");
            punctuation.type(keyboard == AzikKeyboard::Jp106 ? ":x;x@" : "'x'x[");
            equal(punctuation.output, keyboard == AzikKeyboard::Jp106 ? U"ヴィンヴォンー;@" : U"ヴィンヴォンー'「");
            check(profile.method() == InputMethod::Azik && profile.rules()->find("q"), "overlay lost AZIK");
            for (const auto reserved : {profile.kana_key(), profile.reading_key(), profile.shifted_sokuon_key()}) {
                bool rejected = false;
                try { profile.overlay("version=1\n" + std::string(1, reserved) + "\tあ\n"); }
                catch (const std::invalid_argument&) { rejected = true; }
                check(rejected, "custom rule hid a profile command");
            }
        }
        Session commands;
        commands.type("Q"); equal(commands.engine.preedit(), U"▽ん");
        commands.key(KeyCode::Cancel); commands.type("kQ"); equal(commands.engine.preedit(), U"▽かい");
        commands.key(KeyCode::Cancel); commands.type("Kzji"); commands.key(KeyCode::Space);
        commands.type("Xa"); equal(commands.output, U"漢字"); equal(commands.engine.preedit(), U"▽しゃ");
        commands.key(KeyCode::Cancel); commands.type("lq");
        check(commands.engine.mode() == Mode::Latin && !commands.engine.would_handle({KeyCode::Character, 'q'}), "Latin interpreted AZIK");
        commands.key(KeyCode::Hiragana); commands.type("Lq/"); equal(commands.output, U"漢字ｑ／");
        commands.key(KeyCode::Hiragana); commands.type("/QxL"); equal(commands.engine.preedit(), U"▽QxL");
        commands.key(KeyCode::Cancel); commands.type("`99eq"); equal(commands.engine.preedit(), U"▽99えん");
        Session us(AzikKeyboard::Us101); us.type("{99eq"); equal(us.engine.preedit(), U"▽99えん");

        auto dictionary = std::make_shared<MemoryDictionary>();
        dictionary->add({U"た", 't'}, {{"stand", U"立", {}}});
        dictionary->add({U"はな", 's'}, {{"speak", U"話", {}}});
        dictionary->add({U"い", 't'}, {{"go", U"行", {}}});
        dictionary->add({U"ろん", 'j'}, {{"discuss", U"論", {}}});
        for (const auto keyboard : {AzikKeyboard::Jp106, AzikKeyboard::Us101}) {
            Session okuri(keyboard, dictionary);
            okuri.type("TaCi"); equal(okuri.engine.composition_text(), U"立ち");
            auto learned = okuri.key(KeyCode::Enter);
            check(learned.learning && learned.learning->key == LookupKey{U"た", 't'} && learned.learning->okuri == U"ち", "alternate c learning key");
            okuri.type("HanaXi"); equal(okuri.engine.composition_text(), U"話し");
            learned = okuri.key(KeyCode::Enter);
            check(learned.learning && learned.learning->key == LookupKey{U"はな", 's'}, "alternate x learning key");
            okuri.type(keyboard == AzikKeyboard::Jp106 ? "I+ta" : "I:ta");
            equal(okuri.output, U"立ち話し行った");
            okuri.type("RonZi"); equal(okuri.engine.composition_text(), U"論じ");
            learned = okuri.key(KeyCode::Enter);
            check(learned.learning && learned.learning->key == LookupKey{U"ろん", 'j'}, "zi did not use DDSKK's ji prefix");
        }
        Session registration;
        registration.type("Mici"); registration.key(KeyCode::Space);
        check(registration.engine.registering(), "missing reading did not register");
        registration.type("xxaq"); equal(registration.engine.registration_text(), U"ぁん");
        registration.key(KeyCode::Enter); equal(registration.output, U"ぁん");
        registration.engine.reset(); registration.type("q"); equal(registration.output, U"ぁんん");

        for (const auto method : {InputMethod::Skk, InputMethod::Azik}) {
            Engine deletion(demo_dictionary(), true, InputProfile::built_in(method));
            check(!deletion.would_handle({KeyCode::ForgetCandidate}), "idle deletion consumed");
            for (const auto ch : method == InputMethod::Azik ? std::string_view("Kzji") : std::string_view("Kanji")) deletion.handle({KeyCode::Character, ch});
            check(!deletion.would_handle({KeyCode::ForgetCandidate}), "reading deletion consumed");
            deletion.handle({KeyCode::Space});
            check(deletion.would_handle({KeyCode::ForgetCandidate}), "selected deletion unavailable");
            deletion.handle({KeyCode::ForgetCandidate}); check(deletion.forgetting(), "missing deletion prompt");
            deletion.handle({KeyCode::ForgetCandidate}); check(deletion.forgetting(), "repeat escaped prompt");
            deletion.handle({KeyCode::Cancel}); check(!deletion.forgetting() && deletion.input_stage() == Stage::Selecting, "cancel lost candidates");
        }
        auto overlay = InputProfile::built_in(InputMethod::Azik).overlay("version=1\nkz\t関\nq\tんん\n");
        Engine custom(demo_dictionary(), true, overlay);
        std::u32string output;
        for (const auto ch : std::string_view("kzq")) output += custom.handle({KeyCode::Character, ch}).committed;
        equal(output, U"関んん");
        std::cout << "PASS AZIK extensions, profiles, commands, JIS/US, canonical okuri, learning, registration and deletion\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
