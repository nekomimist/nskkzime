#include "nskkzime/inference_policy.h"
#include "nskkzime/learning.h"
#include "nskkzime/dictionary.h"
#include <functional>
#include <iostream>
#include <limits>
using namespace nskkzime;
namespace {
void check(bool ok,const char* why) { if (!ok) throw std::runtime_error(why); }
Result type(Engine& engine,std::string_view input) { Result result; for (auto ch:input) result=engine.handle({KeyCode::Character,ch}); return result; }
struct Start { Engine before, after; Result result; };
Start start(std::string_view text="Kaitou",std::shared_ptr<const Dictionary> dictionary=demo_dictionary()) {
    Engine engine(dictionary); type(engine,text); auto before=engine; const auto result=engine.handle({KeyCode::Space}); return {before,engine,result};
}
InferenceResponse scored(const PlannedInference& plan,std::vector<float> scores) {return {plan.request.id,{},std::move(scores),InferenceOperation::Score};}
template<class F> void rejects(F action) { bool rejected=false; try {action();} catch(const std::exception&) {rejected=true;} check(rejected,"invalid policy accepted"); }
}
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> cases={
        {"policy/default-and-threshold-boundaries",[] {
            auto short_word=start(); InferencePolicy policy;
            check(!plan_inference(short_word.before,short_word.after,short_word.result,policy),"default reordered dictionary");
            auto boundary=start("Arigatougozaimasu"); auto plan=plan_inference(boundary.before,boundary.after,boundary.result,policy);
            check(plan&&plan->purpose==InferencePurpose::LongReading&&plan->request.reading.size()==10,"10-kana boundary");
            policy.long_min_length=11; check(plan_inference(boundary.before,boundary.after,boundary.result,policy)->purpose==InferencePurpose::Fallback,"below boundary must use fallback");
            policy.long_min_length=0; check(plan_inference(boundary.before,boundary.after,boundary.result,policy)->purpose==InferencePurpose::Fallback,"disabled long priority");
        }},
        {"policy/promote-stable-metadata-and-learning",[] {
            auto s=start(); InferencePolicy policy; policy.rerank=true;
            const auto plan=*plan_inference(s.before,s.after,s.result,policy); const auto old=s.after.candidates();
            check(plan.purpose==InferencePurpose::Rerank&&plan.request.candidates==std::vector<std::u32string>{U"回答",U"解答",U"解凍"},"score order");
            check(apply_inference_result(s.after,plan,scored(plan,{-3,0,-5}),policy),"best candidate not promoted");
            check(s.after.candidates()[0].id==old[1].id&&s.after.candidates()[1].id==old[0].id&&s.after.candidates()[2].id==old[2].id,"other ranks or IDs changed");
            const auto result=s.after.handle({KeyCode::Enter}); check(result.committed==U"解答"&&result.learning&&result.learning->candidate.source==CandidateSource::Dictionary,"rerank changed learning policy");
        }},
        {"policy/rerank-learns-raw-gap-and-other-context-can-win",[] {
            auto user = std::make_shared<UserDictionary>();
            auto dictionary = std::make_shared<DictionarySet>(std::vector<std::shared_ptr<const Dictionary>>{user, demo_dictionary()});
            InferencePolicy policy; policy.rerank = true;
            const auto convert = [&] { return start("Kaitou", dictionary); };
            auto first = convert();
            const auto plan = *plan_inference(first.before, first.after, first.result, policy);
            check(apply_inference_result(first.after, plan, scored(plan, {-5, 0, -2}), policy), "initial promotion missing");
            auto choice = first.after.handle({KeyCode::SelectCandidate, 0, 1});
            check(choice.learning && choice.learning->rerank_gap == 5.0, "override did not learn raw gap");
            user->apply({std::string(32, 'a'), 1, *choice.learning});
            auto next = convert(); const auto again = *plan_inference(next.before, next.after, next.result, policy);
            check(!apply_inference_result(next.after, again, scored(again, {-5, 0, -2}), policy) && next.after.composition_text() == U"回答", "boost did not preserve learned first word");
            choice = next.after.handle({KeyCode::Enter});
            check(choice.learning && !choice.learning->rerank_gap, "accepting first changed boost");
            auto other = convert(); const auto context = *plan_inference(other.before, other.after, other.result, policy);
            check(apply_inference_result(other.after, context, scored(context, {-9, 0, -2}), policy), "other context could not overturn boost");
            choice = other.after.handle({KeyCode::SelectCandidate, 0, 1});
            check(choice.learning->rerank_gap == 9.0, "boost fed back into learned raw gap");
            user->apply({std::string(32, 'a'), 2, *choice.learning});
            auto replace = convert(); const auto replacement = *plan_inference(replace.before, replace.after, replace.result, policy);
            apply_inference_result(replace.after, replacement, scored(replacement, {-5, 0, -2}), policy);
            choice = replace.after.handle({KeyCode::SelectCandidate, 0, 2});
            check(choice.learning->candidate.text == U"解凍" && choice.learning->rerank_gap == 2.0, "second preference was not replaced");
            auto clear = convert(); const auto clearing = *plan_inference(clear.before, clear.after, clear.result, policy);
            apply_inference_result(clear.after, clearing, scored(clearing, {-5, 0, -2}), policy);
            choice = clear.after.handle({KeyCode::SelectCandidate, 0, 1});
            check(choice.learning->rerank_gap == 0.0, "raw best selection did not clear boost");
            policy.rerank_learn = false;
            auto disabled = convert(); const auto no_learning = *plan_inference(disabled.before, disabled.after, disabled.result, policy);
            check(apply_inference_result(disabled.after, no_learning, scored(no_learning, {-5, 0, -2}), policy), "disabled learning used stored boost");
            check(!disabled.after.handle({KeyCode::SelectCandidate, 0, 1}).learning->rerank_gap, "disabled learning captured scores");
        }},
        {"policy/rerank-no-promotion-unscored-cancel-and-registration",[] {
            InferencePolicy policy; policy.rerank = true; policy.rerank_limit = 2;
            auto s = start(); auto plan = *plan_inference(s.before, s.after, s.result, policy);
            const auto revision = s.after.revision();
            check(!apply_inference_result(s.after, plan, scored(plan, {0, -3}), policy) && s.after.revision() > revision, "unchanged order lost scoring evidence");
            auto choice = s.after.handle({KeyCode::SelectCandidate, 0, 1});
            check(choice.learning->rerank_gap == 3.0, "no-promotion override not learned");
            s = start(); plan = *plan_inference(s.before, s.after, s.result, policy);
            apply_inference_result(s.after, plan, scored(plan, {0, -3}), policy);
            check(s.after.handle({KeyCode::SelectCandidate, 0, 2}).learning->rerank_gap == 0.0, "unscored word did not clear boost");
            s = start(); plan = *plan_inference(s.before, s.after, s.result, policy);
            apply_inference_result(s.after, plan, scored(plan, {0, -3}), policy);
            s.after.handle({KeyCode::Cancel}); s.after.handle({KeyCode::Space});
            check(!s.after.handle({KeyCode::SelectCandidate, 0, 1}).learning->rerank_gap, "cancelled conversion leaked scores");
            s = start(); plan = *plan_inference(s.before, s.after, s.result, policy);
            apply_inference_result(s.after, plan, scored(plan, {0, -3}), policy);
            s.after.handle({KeyCode::Space}); s.after.handle({KeyCode::Space}); s.after.handle({KeyCode::Space});
            check(s.after.registering(), "registration fixture did not exhaust candidates");
            type(s.after, "mikan"); choice = s.after.handle({KeyCode::Enter});
            check(choice.registration && choice.learning->rerank_gap == 0.0, "registration did not clear original conversion boost");
            s = start(); plan = *plan_inference(s.before, s.after, s.result, policy);
            apply_inference_result(s.after, plan, scored(plan, {0, -3}), policy);
            s.after.append_generated({{"new", U"新語", {}, CandidateSource::GeneratedFallback, LearningPolicy::DoNotLearn}});
            choice = s.after.handle({KeyCode::Enter});
            check(!choice.learning && choice.rerank_learning && choice.rerank_learning->rerank_gap == 0.0, "unlearned fallback did not clear boost separately");
        }},
        {"policy/rerank-boost-wins-tie-below-first-candidate",[] {
            auto user = std::make_shared<UserDictionary>();
            user->apply({std::string(32, 'a'), 1, {{U"かいとう"}, {}, {"user", U"解凍", {}}, 4.0}, DictionaryAction::Rerank});
            auto dictionary = std::make_shared<DictionarySet>(std::vector<std::shared_ptr<const Dictionary>>{user, demo_dictionary()});
            InferencePolicy policy; policy.rerank = true;
            auto s = start("Kaitou", dictionary); const auto plan = *plan_inference(s.before, s.after, s.result, policy);
            check(plan.request.candidates == std::vector<std::u32string>{U"回答", U"解答", U"解凍"}, "boost-only entry changed vocabulary order");
            // 解凍 reaches the raw best exactly (1 + 4 == 5) and must beat 解答 at a lower index.
            check(apply_inference_result(s.after, plan, scored(plan, {0, 5, 1}), policy) && s.after.composition_text() == U"解凍",
                "learned gap lost its tie to an earlier candidate");
        }},
        {"policy/rerank-okuri-boost-is-a-stem-preference",[] {
            auto user = std::make_shared<UserDictionary>();
            user->apply({std::string(32, 'a'), 1, {{U"か", 'k'}, U"く", {"user", U"描", U"注釈"}, 8.0}});
            auto dictionary = std::make_shared<DictionarySet>(std::vector<std::shared_ptr<const Dictionary>>{user, demo_dictionary()});
            InferencePolicy policy; policy.rerank = true;
            for (const auto input : {"KaK", "qKaK"}) {
                Engine engine(dictionary); type(engine, input); const auto before = engine; const auto result = type(engine, "e");
                const auto plan = *plan_inference(before, engine, result, policy);
                check(plan.request.candidates == std::vector<std::u32string>{U"書け", U"描け"}, "okuri fixture or scoring suffix changed");
                check(apply_inference_result(engine, plan, scored(plan, {0, -5}), policy), "boost did not transfer across actual okuri");
                const auto accepted = engine.handle({KeyCode::Enter});
                check(accepted.learning->key == LookupKey{U"か", 'k'} && accepted.learning->candidate.text == U"描" && !accepted.learning->rerank_gap, "okuri preference retained suffix or rewrote accepted boost");
            }
        }},
        {"policy/promote-confidence-and-malformed-score",[] {
            auto s=start(); InferencePolicy policy; policy.rerank=true; const auto plan=*plan_inference(s.before,s.after,s.result,policy);
            for(const auto numbers:{std::vector<float>{0,1,-5},std::vector<float>{0,0,0},std::vector<float>{0},std::vector<float>{0,std::numeric_limits<float>::infinity(),-5}})
                check(!apply_inference_result(s.after,plan,scored(plan,numbers),policy)&&s.after.composition_text()==U"回答","uncertain/invalid score moved candidates");
            auto wrong=scored(plan,{0,5,-5}); ++wrong.id; check(!apply_inference_result(s.after,plan,wrong,policy),"wrong ID");
        }},
        {"policy/learned-generated-word-can-be-scored",[] {
            auto dictionary=std::make_shared<MemoryDictionary>();dictionary->add({U"かいとう"},{{"dict",U"回答",{}},{"user",U"解答",U"zenz",CandidateSource::GeneratedFallback,LearningPolicy::Learn}});
            auto s=start("Kaitou",dictionary);InferencePolicy policy;policy.rerank=true;
            const auto plan=plan_inference(s.before,s.after,s.result,policy);
            check(plan&&apply_inference_result(s.after,*plan,scored(*plan,{-5,0}),policy),"learned generation excluded from reranking");
            const auto result=s.after.handle({KeyCode::Enter});check(result.learning&&result.learning->candidate.id=="user"&&result.learning->candidate.source==CandidateSource::GeneratedFallback,"learned word lost metadata");
        }},
        {"policy/interaction-freezes-order-even-returning-to-first",[] {
            InferencePolicy policy; policy.rerank=true;
            for(const auto key:{Key{KeyCode::Space},Key{KeyCode::PageDown},Key{KeyCode::PageUp},Key{KeyCode::Character,'7'}}) {
                auto s=start(); const auto plan=*plan_inference(s.before,s.after,s.result,policy); s.after.handle(key);
                check(!apply_inference_result(s.after,plan,scored(plan,{0,5,-5}),policy),"operated candidate list reordered");
            }
            auto s=start(); const auto plan=*plan_inference(s.before,s.after,s.result,policy); s.after.handle({KeyCode::Space}); s.after.handle({KeyCode::Character,'x'});
            check(s.after.candidate_index()==0&&!apply_inference_result(s.after,plan,scored(plan,{0,5,-5}),policy),"returning to first reopened ranking");
        }},
        {"policy/okuri-stems-full-text-and-katakana",[] {
            for(const auto input:{"KaK","qKaK"}) {
                Engine engine(demo_dictionary()); type(engine,input); const auto before=engine; const auto result=type(engine,"u");
                InferencePolicy policy; policy.rerank=true; const auto plan=plan_inference(before,engine,result,policy);
                check(result.conversion_started&&plan&&plan->request.reading==U"かく"&&plan->request.candidates==std::vector<std::u32string>{U"書く",U"描く"},"okuri scoring omitted suffix or used display kana");
                check(engine.inference_reading().empty(),"okuri generation enabled");
                check(apply_inference_result(engine,*plan,scored(*plan,{-5,0}),policy),"okuri promotion failed");
                const auto committed=engine.handle({KeyCode::Enter});
                check(committed.learning&&committed.learning->key.okuri=='k'&&committed.learning->okuri==U"く"&&committed.learning->candidate.text==U"描","okuri learning lost stem");
            }
        }},
        {"policy/long-merge-dedup-filter-and-no-learning",[] {
            auto dictionary=std::make_shared<MemoryDictionary>(); dictionary->add({U"ありがとうございます"},{{"old1",U"元一",U"注釈"},{"common",U"共通",{}},{"old3",U"元三",{}}});
            auto s=start("Arigatougozaimasu",dictionary); InferencePolicy policy; policy.learn_fallback=true;
            const auto plan=*plan_inference(s.before,s.after,s.result,policy); s.after.note_generation_attempt();
            const InferenceResponse response{1,{U"新規",U"共通",U"新規",U"ありがとうございます",U"不適切"},{0,-1,-2,-3,-20}};
            check(apply_inference_result(s.after,plan,response,policy),"long merge failed");
            const auto& c=s.after.candidates(); check(c.size()==4&&c[0].text==U"新規"&&c[1].text==U"共通"&&c[2].id=="old1"&&c[2].annotation==U"注釈"&&c[3].id=="old3","merge order/dedup/metadata");
            check(c[0].source==CandidateSource::GeneratedLong&&!s.after.handle({KeyCode::Enter}).learning,"long candidate learned");
        }},
        {"policy/long-operation-freeze-and-once-per-conversion",[] {
            InferencePolicy policy; policy.long_min_length=4; auto s=start(); const auto plan=*plan_inference(s.before,s.after,s.result,policy);
            s.after.note_generation_attempt(); auto before=s.after; auto result=s.after.handle({KeyCode::Space});
            check(!apply_inference_result(s.after,plan,{1,{U"新しい候補"},{0}},policy),"long generation moved operated candidates");
            s.after.handle({KeyCode::Space}); before=s.after; result=s.after.handle({KeyCode::Space});
            check(result.candidates_exhausted&&!plan_inference(before,s.after,result,policy),"exhaustion retried long generation");
            s.after.defer_registration(); s.after.handle({KeyCode::Cancel}); before=s.after; result=s.after.handle({KeyCode::Space});
            check(plan_inference(before,s.after,result,policy).has_value(),"fresh conversion cannot generate");
        }},
        {"policy/fallback-learning-opt-in-short-only",[] {
            for(const bool enabled:{false,true}) {
                auto s=start("Mikan"); InferencePolicy policy; policy.learn_fallback=enabled;
                const auto plan=*plan_inference(s.before,s.after,s.result,policy); s.after.defer_registration();
                check(apply_inference_result(s.after,plan,{1,{U"蜜柑"},{0}},policy),"fallback failed");
                const auto result=s.after.handle({KeyCode::Enter}); check(result.learning.has_value()==enabled,"fallback learning opt-in ignored");
            }
            auto s=start("Arigatougozaimasu"); InferencePolicy policy; policy.long_min_length=0; policy.learn_fallback=true;
            const auto plan=*plan_inference(s.before,s.after,s.result,policy); s.after.defer_registration();
            check(apply_inference_result(s.after,plan,{1,{U"有難う"},{0}},policy)&&!s.after.handle({KeyCode::Enter}).learning,"long fallback learned");
        }},
        {"policy/excluded-inputs-and-score-limit",[] {
            InferencePolicy policy; policy.rerank=true;
            auto abbrev=start("/abc"); check(!plan_inference(abbrev.before,abbrev.after,abbrev.result,policy),"abbrev inferred");
            auto registration=start("Mikan"); auto before=registration.after; type(registration.after,"Kaitou"); auto result=registration.after.handle({KeyCode::Space});
            check(!plan_inference(before,registration.after,result,policy),"registration editor inferred");
            auto dictionary=std::make_shared<MemoryDictionary>(); std::vector<Candidate> candidates(25,{"id",U"候補",{}}); dictionary->add({U"かいとう"},candidates);
            auto s=start("Kaitou",dictionary); check(plan_inference(s.before,s.after,s.result,policy)->request.candidates.size()==20,"score limit ignored");
        }},
        {"policy/invalid-settings",[] {
            InferencePolicy policy;
            for(const auto value:{-1.0f,std::numeric_limits<float>::quiet_NaN(),101.0f}) {policy.promote_threshold=value; rejects([&]{validate_inference_policy(policy);});}
            policy={}; policy.rerank_limit=65; rejects([&]{validate_inference_policy(policy);});
            policy={}; policy.score_timeout_ms=0; rejects([&]{validate_inference_policy(policy);});
            policy={}; policy.rerank_wait_ms=1001; rejects([&]{validate_inference_policy(policy);});
            policy={}; policy.rerank_wait_ms=0; validate_inference_policy(policy);
        }}
    };
    unsigned failures=0;
    for(const auto& [name,test]:cases) try {test();std::cout<<"PASS "<<name<<'\n';} catch(const std::exception& e) {++failures;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    return failures ? 1 : 0;
}
