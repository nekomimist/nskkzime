#include "nskkzime/inference_policy.h"
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
            for(const bool enabled:{false,true}) for(const bool dummy:{false,true}) {
                auto s=start("Mikan"); InferencePolicy policy; policy.learn_fallback=enabled;
                const auto plan=*plan_inference(s.before,s.after,s.result,policy); s.after.defer_registration();
                check(apply_inference_result(s.after,plan,{1,{U"蜜柑"},{0}},policy,dummy),"fallback failed");
                const auto result=s.after.handle({KeyCode::Enter}); check(result.learning.has_value()==(enabled&&!dummy),"fallback learning opt-in ignored");
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
