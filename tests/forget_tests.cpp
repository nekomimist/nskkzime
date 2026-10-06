#include "nskkzime/learning.h"
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace nskkzime;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void type(Engine& engine, std::string_view text) { for (const auto ch : text) engine.handle({KeyCode::Character, ch}); }
std::shared_ptr<UserDictionary> fixture() {
    auto result = std::make_shared<UserDictionary>();
    result->apply(ImportRequest{std::string(32, 'a'), 1, {
        {{U"か", 'k'}, U"く", {"exact", U"描", U"この送り"}},
        {{U"か", 'k'}, {}, {"generic", U"描", U"共通"}},
        {{U"か", 'k'}, U"け", {"other", U"描", U"別の送り"}},
        {{U"かんじ"}, {}, {"plain", U"感じ", {}}},
        {{U"SKK"}, {}, {"abbrev", U"日本語", {}}}
    }});
    return result;
}
Result approve(Engine& engine) { type(engine, "yes"); return engine.handle({KeyCode::Enter}); }
}
int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"forget/exact-scope-then-generic-without-touching-other-okuri", [] {
            auto dictionary = fixture(); Engine engine(dictionary); type(engine, "KaKuX");
            check(engine.forgetting() && engine.forget_target()->okuri == U"く", "exact scope not preferred");
            check(engine.composition_text() == U"描く" && engine.literal_text() == U"描く", "prompt leaked into host text");
            auto trial = engine; const auto event = approve(trial);
            check(event.forgetting && !event.learning && event.committed.empty() && trial.composition_text() == U"かく", "deletion committed text or lost reading");
            check(engine.forgetting() && dictionary->revision() == 1, "trial mutated live state or dictionary");
            dictionary->apply(LearningRequest{std::string(32, 'b'), 1, *event.forgetting, DictionaryAction::Forget});
            engine = std::move(trial); engine.forget_registration(*event.forgetting);
            engine.handle({KeyCode::Space}); type(engine, "X");
            check(engine.forget_target() && engine.forget_target()->okuri.empty(), "generic scope not resolved");
            const auto common = approve(engine);
            dictionary->apply(LearningRequest{std::string(32, 'b'), 2, *common.forgetting, DictionaryAction::Forget});
            check(dictionary->lookup({U"か", 'k'}, U"く").empty() && dictionary->lookup_scope({U"か", 'k'}, U"け").size() == 1,
                "deletion affected another okuri");
            check(!engine.handle({KeyCode::Enter}).forgetting, "deletion emitted twice");
        }},
        {"forget/explicit-answer-cancel-backspace-and-input-isolation", [] {
            for (const auto answer : {"", "y", "YES", "no", "yess", "yes1"}) {
                Engine engine(fixture()); type(engine, "Kanji"); engine.handle({KeyCode::Space}); type(engine, "X");
                type(engine, answer); const auto result = engine.handle({KeyCode::Enter});
                check(!result.forgetting && !result.learning && result.committed.empty() && engine.stage() == Stage::Selecting, "non-yes answer deleted or committed");
            }
            Engine engine(fixture()); type(engine, "Kanji"); engine.handle({KeyCode::Space}); type(engine, "Xyes");
            const auto revision = engine.revision();
            check(engine.would_handle({KeyCode::Cancel}) && engine.revision() == revision, "query mutated confirmation");
            engine.handle({KeyCode::SelectCandidate, 0, 0}); engine.handle({KeyCode::PageDown});
            check(engine.forgetting() && engine.forget_answer() == U"yes", "candidate shortcut escaped prompt");
            engine.handle({KeyCode::Backspace}); check(engine.forget_answer() == U"ye", "backspace edited host text");
            type(engine, "s"); engine.handle({KeyCode::Space});
            check(!engine.handle({KeyCode::Enter}).forgetting, "whitespace accepted as yes");
            type(engine, "Xyes"); engine.handle({KeyCode::Cancel});
            check(!engine.forgetting() && engine.stage() == Stage::Selecting, "cancel lost selection");
            type(engine, "Xyes"); engine.reset();
            check(!engine.forgetting() && !engine.handle({KeyCode::Enter}).forgetting, "reset retained deletion");
        }},
        {"forget/frozen-target-and-inference-isolation", [] {
            auto dictionary = fixture(); Engine engine(dictionary); type(engine, "KaKuX");
            check(engine.inference_reading(true).empty() && !engine.can_reorder_candidates() && !engine.promote_candidate(1), "confirmation allowed inference");
            check(!engine.append_generated({{"g", U"別", {}, CandidateSource::GeneratedFallback}}), "late generated candidate entered prompt");
            const auto target = *engine.forget_target();
            dictionary->apply(LearningRequest{std::string(32, 'b'), 1, target, DictionaryAction::Forget});
            const auto result = approve(engine);
            check(result.forgetting && result.forgetting->okuri == U"く", "confirmation silently changed to generic scope");
            dictionary->apply(LearningRequest{std::string(32, 'c'), 1, *result.forgetting, DictionaryAction::Forget});
            check(dictionary->lookup_scope({U"か", 'k'}, {}).size() == 1, "retry removed fallback scope");
        }},
        {"forget/registration-keeps-outer-draft-and-reading", [] {
            Engine engine(fixture()); type(engine, "Michi"); engine.handle({KeyCode::Space}); type(engine, "KaKuX");
            check(engine.registering() && engine.forgetting() && engine.composition_text() == U"みち", "inner prompt changed outer anchor");
            const auto result = approve(engine);
            check(result.forgetting && !result.registration && !result.learning && engine.registering() && engine.registration_text() == U"▽か*く", "inner deletion registered draft");
            check(engine.composition_text() == U"みち", "inner reading leaked into document");
        }},
        {"forget/read-only-candidate-does-not-mutate-or-start-next-word", [] {
            Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space}); type(engine, "X");
            check(engine.forgetting() && !engine.forget_target(), "read-only candidate gained user scope");
            const auto result = approve(engine);
            check(!result.forgetting && result.committed.empty() && engine.composition_text() == U"漢字", "read-only prompt committed text");
        }},
        {"forget/katakana-abbrev-and-local-registration-cache", [] {
            auto dictionary = fixture(); Engine katakana(dictionary); type(katakana, "qKaKuX");
            check(katakana.forget_target()->okuri == U"く" && katakana.composition_text() == U"描ク", "katakana changed stored scope");
            Engine abbrev(dictionary); type(abbrev, "/SKK"); abbrev.handle({KeyCode::Space}); type(abbrev, "X");
            check(abbrev.forget_target()->key.reading == U"SKK", "abbrev key case lost");
            Engine local(dictionary); const auto target = *dictionary->forget_target({U"かんじ"}, {}, U"感じ");
            local.remember_registration(target); local.forget_registration(target);
            dictionary->apply(LearningRequest{std::string(32, 'b'), 1, target, DictionaryAction::Forget});
            type(local, "Kanji"); local.handle({KeyCode::Space});
            check(local.registering(), "local cache resurrected forgotten word");
        }}
    };
    for (const auto& [name, run] : cases) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
}
