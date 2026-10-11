#include "nskkzime/compiled_dictionary.h"
#include "nskkzime/learning.h"
#include "nskkzime/numeric.h"
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace nskkzime;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void equal(std::u32string_view actual, std::u32string_view expected) {
    if (actual != expected) throw std::runtime_error("expected " + utf8(expected) + ", got " + utf8(actual));
}
void type(Engine& engine, std::string_view text) { for (const auto ch : text) engine.handle({KeyCode::Character, ch}); }
void convert(Engine& engine, std::string_view text) { type(engine, text); engine.handle({KeyCode::Space}); }
Result choose(Engine& engine, std::size_t index) { return engine.handle({KeyCode::SelectCandidate, 0, index}); }
auto fixture() {
    return SkkDictionary::parse(U";; okuri-nasi entries.\n"
        U"# /#1/#3/#2;桁ごと/#0/#8/＃;number/#4/#5/#9/#7/#10/\n"
        U"だい#かい /第#1回/第#2回/第#3回/\n"
        U"#えん /#3円/\n#m#d /#0月#0日/\n#.# /#1．#1/#0月#0日/\n");
}
void expansion(std::u32string_view number, std::u32string_view spec, std::u32string_view expected) {
    const std::vector<std::u32string> numbers{std::u32string(number)};
    const auto result = expand_numeric(spec, numbers);
    check(result && result->converted, "number did not expand"); equal(result->text, expected);
}
}
int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"extract/integer-runs-fullwidth-grouping-and-punctuation", [] {
            const auto value = numeric_reading({U"だい００９かい1,234.56-7", 'k'});
            check(value.key == LookupKey{U"だい#かい#.#-#", 'k'} &&
                value.numbers == std::vector<std::u32string>{U"009", U"1234", U"56", U"7"}, "numeric key or runs changed");
            equal(numeric_reading({U"1,,2"}).key.reading, U"#,,#");
            check(numeric_reading({U"#"}).numbers.empty(), "literal hash became a number");
        }},
        {"format/digits-leading-zero-and-grouping", [] {
            expansion(U"0009", U"#0", U"0009"); expansion(U"0009", U"#1", U"０００９");
            expansion(U"0009", U"#2", U"〇〇〇九"); expansion(U"0009", U"#3", U"九");
            expansion(U"0009", U"#8", U"9"); expansion(U"0000", U"#8", U"0");
            expansion(U"1234567", U"#8", U"1,234,567");
            expansion(U"123456789012345678901234", U"#8", U"123,456,789,012,345,678,901,234");
        }},
        {"format/unit-boundaries-zero-and-twenty-digits", [] {
            for (const auto number : {U"0", U"0000"}) expansion(number, U"#3", U"〇");
            expansion(U"10", U"#3", U"十"); expansion(U"100", U"#3", U"百");
            expansion(U"1000", U"#3", U"千"); expansion(U"1024", U"#3", U"千二十四");
            expansion(U"10000", U"#3", U"一万"); expansion(U"10000000", U"#3", U"一千万");
            expansion(U"100000000000", U"#3", U"千億"); expansion(U"1000000000000000", U"#3", U"千兆");
            expansion(U"10000000000000000", U"#3", U"一京");
            expansion(U"99999999999999999999", U"#3", U"九千九百九十九京九千九百九十九兆九千九百九十九億九千九百九十九万九千九百九十九");
        }},
        {"format/unsupported-malformed-missing-and-bounded", [] {
            const std::vector<std::u32string> numbers{U"99"};
            for (const auto spec : {U"#4", U"#5", U"#9", U"#6", U"#10", U"#0#1", U"#3#"}) {
                // A trailing ordinary hash is valid.
                if (std::u32string_view(spec) == U"#3#") { equal(expand_numeric(spec, numbers)->text, U"九十九#"); continue; }
                check(!expand_numeric(spec, numbers), "unsupported/missing numeric placeholder accepted");
            }
            check(!expand_numeric(U"#3", std::vector<std::u32string>{std::u32string(21, U'9')}), "oversized unit conversion accepted");
            check(!expand_numeric(U"#8", std::vector<std::u32string>{std::u32string(1024, U'9')}), "oversized expanded text accepted");
            check(!expand_numeric(U"prefix#0", std::vector<std::u32string>{std::u32string(1024, U'9')}), "prefix exceeded output bound");
            const auto literal = expand_numeric(U"C# #2", {});
            check(literal && !literal->converted && literal->text == U"C# #2", "literal template changed without numbers");
            const auto fixed = expand_numeric(U"＃", numbers);
            check(fixed && !fixed->converted && fixed->text == U"＃", "fixed numeric-key candidate rejected");
            equal(expand_numeric(U"#0日", std::vector<std::u32string>{U"2", U"25"})->text, U"2日");
        }},
        {"engine/parsed-and-compiled-display-learning-and-return-mode", [] {
            const auto parsed = fixture();
            const auto image = std::make_shared<const std::string>(CompiledDictionary::compile(*parsed));
            for (const auto& dictionary : std::vector<std::shared_ptr<const Dictionary>>{
                parsed, std::make_shared<CompiledDictionary>(*image, image)}) {
                Engine engine(dictionary); engine.set_mode(Mode::Katakana); convert(engine, "/99");
                check(engine.candidates().size() == 5, "unsupported or duplicate candidates shown");
                equal(engine.candidates()[0].text, U"９９"); equal(engine.candidates()[1].text, U"九十九");
                equal(engine.candidates()[2].text, U"九九"); equal(engine.candidates()[3].text, U"99");
                equal(engine.candidates()[4].text, U"＃"); equal(engine.candidates()[2].annotation, U"桁ごと");
                equal(engine.composition_text(), U"９９");
                const auto result = choose(engine, 2); equal(result.committed, U"九九");
                check(result.learning && result.learning->key == LookupKey{U"#"} && result.learning->candidate.text == U"#2" &&
                    !result.learning->candidate.origin && engine.mode() == Mode::Katakana && !engine.abbrev(), "expanded identity leaked to learning");
            }
        }},
        {"engine/q-reading-mixed-kana-multiple-runs-and-dot-date", [] {
            Engine engine(fixture()); convert(engine, "Dai12kai"); equal(choose(engine, 2).committed, U"第十二回");
            convert(engine, "Q99en"); equal(choose(engine, 0).committed, U"九十九円");
            convert(engine, "/2m25d"); equal(choose(engine, 0).committed, U"2月25日");
            convert(engine, "/2.25"); equal(choose(engine, 1).committed, U"2月25日");
            type(engine, "Q"); equal(engine.preedit(), U"▽"); engine.handle({KeyCode::Cancel});
            type(engine, "nQ7gatu"); equal(engine.preedit(), U"▽7がつ");
            engine.reset(); convert(engine, "/#"); equal(engine.candidates()[0].text, U"#1");
        }},
        {"engine/source-priority-before-exact-key-and-display-dedup", [] {
            const auto high = SkkDictionary::parse(U"# /#2;high/\n");
            const auto low = SkkDictionary::parse(U"99 /固定/#2;exact/\n# /#1/\n");
            auto dictionary = std::make_shared<DictionarySet>(std::vector<std::shared_ptr<const Dictionary>>{high, low});
            Engine engine(dictionary); convert(engine, "/99");
            check(engine.candidates().size() == 3, "display candidates not deduplicated");
            equal(engine.candidates()[0].text, U"九九"); equal(engine.candidates()[0].annotation, U"high");
            equal(engine.candidates()[1].text, U"固定"); equal(engine.candidates()[2].text, U"９９");
            const auto result = choose(engine, 0); check(result.learning->key == LookupKey{U"#"}, "first provenance lost");
            Engine same_layer(low); convert(same_layer, "/99"); equal(same_layer.candidates()[0].text, U"固定");
        }},
        {"engine/one-digit-dedup-keeps-first-template", [] {
            Engine engine(fixture()); convert(engine, "/9");
            check(engine.candidates().size() == 4, "one digit unit/digit templates duplicated");
            const auto result = choose(engine, 1); equal(result.committed, U"九"); equal(result.learning->candidate.text, U"#3");
        }},
        {"learning/persist-restart-different-number-and-delete-template", [] {
            Engine first(fixture()); convert(first, "/99"); const auto selected = choose(first, 2);
            UserDictionary store;
            const LearningRequest request{std::string(32, 'a'), 1, *selected.learning};
            check(store.apply(decode_learning(encode_learning(request))), "template event did not persist");
            auto restarted = std::make_shared<UserDictionary>(UserDictionary::parse(store.serialize()));
            Engine next(restarted); convert(next, "/98"); equal(next.candidates()[0].text, U"九八");
            type(next, "X"); check(next.forget_target() && next.forget_target()->key == LookupKey{U"#"} &&
                next.forget_target()->candidate.text == U"#2", "delete did not resolve template");
            auto trial = next; type(trial, "yes"); const auto removed = trial.handle({KeyCode::Enter});
            check(next.forgetting() && removed.forgetting && !removed.forgetting->candidate.origin, "trial or raw delete changed");
            restarted->apply(LearningRequest{std::string(32, 'b'), 1, *removed.forgetting, DictionaryAction::Forget});
            check(restarted->lookup({U"#"}).empty(), "template survived deletion");
        }},
        {"learning/normalized-literal-learns-exact-but-deletes-source", [] {
            auto dictionary = std::make_shared<UserDictionary>();
            dictionary->apply(ImportRequest{std::string(32, 'a'), 1, {{{U"#"}, {}, {"literal", U"＃", {}}}}});
            Engine engine(dictionary); convert(engine, "/99"); auto trial = engine;
            const auto selected = choose(trial, 0);
            check(selected.learning->key == LookupKey{U"99"} && selected.learning->candidate.text == U"＃", "fixed literal was generalized");
            type(engine, "X"); check(engine.forget_target()->key == LookupKey{U"#"}, "literal source could not be deleted");
        }},
        {"learning/okuri-exact-and-generic-scopes", [] {
            auto dictionary = std::make_shared<UserDictionary>();
            dictionary->apply(ImportRequest{std::string(32, 'a'), 1, {
                {{U"#", 'k'}, U"く", {"exact", U"#3", {}}},
                {{U"#", 'k'}, {}, {"generic", U"#2", {}}},
                {{U"#", 'k'}, U"け", {"other", U"#1", {}}}
            }});
            Engine engine(dictionary); type(engine, "Q29Ku"); equal(engine.candidate_text(0), U"二十九く");
            type(engine, "X"); check(engine.forget_target()->okuri == U"く", "numeric okuri scope lost");
            type(engine, "yes"); const auto removed = engine.handle({KeyCode::Enter});
            dictionary->apply(LearningRequest{std::string(32, 'b'), 1, *removed.forgetting, DictionaryAction::Forget});
            engine.handle({KeyCode::Space}); type(engine, "X");
            check(engine.forget_target()->okuri.empty() && engine.forget_target()->candidate.text == U"#2", "generic numeric scope lost");
            check(dictionary->lookup_scope({U"#", 'k'}, U"け").size() == 1, "other okuri changed");
        }},
        {"engine/candidate-movement-and-generation-preserve-identities", [] {
            Engine engine(fixture()); convert(engine, "Dai12kai"); check(engine.promote_candidate(2), "promotion failed");
            const auto selected = engine.handle({KeyCode::Enter}); equal(selected.learning->candidate.text, U"第#3回");
            convert(engine, "Dai12kai");
            check(engine.prepend_generated({{"long", U"別の文", {}, CandidateSource::GeneratedLong}}), "prepend failed");
            const auto original = choose(engine, 2); equal(original.learning->candidate.text, U"第#2回");
            convert(engine, "Dai12kai");
            check(engine.append_generated({{"fallback", U"#2", {}, CandidateSource::GeneratedFallback}}), "append failed");
            const auto generated = engine.handle({KeyCode::Enter}); equal(generated.committed, U"#2");
            check(generated.learning->key == LookupKey{U"だい12かい"} && !generated.learning->candidate.origin, "generation became a numeric template");
        }},
        {"registration/temporary-raw-template-and-cancel-restore", [] {
            Engine engine(std::make_shared<MemoryDictionary>());
            engine.remember_registration({{U"#"}, {}, {"pending", U"#2", {}}});
            convert(engine, "/99"); equal(engine.candidates()[0].text, U"九九");
            engine.handle({KeyCode::Space}); check(engine.registering(), "exhausted candidates did not register");
            engine.handle({KeyCode::Cancel}); equal(engine.preedit(), U"▽99");
            engine.handle({KeyCode::Space}); type(engine, "X");
            check(engine.forget_target() && engine.forget_target()->candidate.text == U"#2", "pending cache identity lost");
            type(engine, "yes"); const auto removed = engine.handle({KeyCode::Enter}); engine.forget_registration(*removed.forgetting);
            engine.handle({KeyCode::Space}); check(engine.registering(), "deleted cache still converted");
            engine.handle({KeyCode::Cancel}); engine.reset();
            convert(engine, "/99"); type(engine, "lword");
            const auto registered = engine.handle({KeyCode::Enter});
            check(registered.registration && registered.learning->key == LookupKey{U"99"}, "literal registration was generalized");
            engine.remember_registration(*registered.learning);
            convert(engine, "/99"); equal(engine.candidates()[0].text, U"word");
            engine.reset(); convert(engine, "/98"); check(engine.registering(), "literal registration applied to another number");
        }}
    };
    int failed = 0;
    for (const auto& [name, run] : cases) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
