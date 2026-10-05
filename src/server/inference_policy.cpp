#include "nskkzime/inference_policy.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace nskkzime {
namespace {
bool reading_allowed(std::u32string_view reading) {
    return !reading.empty() && reading.size() <= 1024 && std::all_of(reading.begin(),reading.end(),[](char32_t ch) {
        return (ch >= U'ぁ' && ch <= U'ゖ') || ch == U'ゝ' || ch == U'ゞ' || ch == U'ー' || ch == U'、' || ch == U'。' || ch == U'・' || ch == U'！' || ch == U'？';
    });
}
}
void validate_inference_policy(const InferencePolicy& policy) {
    if (policy.long_min_length > 1024 || policy.rerank_limit < 2 || policy.rerank_limit > 64 ||
        !policy.score_timeout_ms || policy.score_timeout_ms > 10000 || !std::isfinite(policy.promote_threshold) ||
        policy.promote_threshold < 0 || policy.promote_threshold > 100 || !std::isfinite(policy.max_score_gap) ||
        policy.max_score_gap < 0 || policy.max_score_gap > 100) throw std::invalid_argument("invalid inference policy");
}
std::optional<PlannedInference> plan_inference(const Engine& before, const Engine& after, const Result& result, const InferencePolicy& policy) {
    validate_inference_policy(policy);
    if (before.registering()) return {};
    const auto reading = after.inference_reading();
    if (!after.generation_attempted() && reading_allowed(reading)) {
        const bool long_reading = result.conversion_started && policy.long_min_length && reading.size() >= policy.long_min_length;
        if (long_reading || result.dictionary_miss || result.candidates_exhausted)
            return PlannedInference{long_reading ? InferencePurpose::LongReading : InferencePurpose::Fallback,{1,reading,{}, {},5}};
    }
    const auto joined = after.inference_reading(true);
    if (!policy.rerank || !result.conversion_started || !after.can_reorder_candidates() || after.candidates().size() < 2 || !reading_allowed(joined)) return {};
    InferenceRequest request{1,joined}; request.operation = InferenceOperation::Score;
    const auto count = (std::min)(after.candidates().size(),static_cast<std::size_t>(policy.rerank_limit));
    for (std::size_t i=0; i<count; ++i) {
        // Persisted user words retain their original GeneratedFallback source.
        // All candidates in this fresh lookup participate without losing that metadata.
        request.candidates.push_back(after.scoring_text(i));
    }
    return PlannedInference{InferencePurpose::Rerank,std::move(request)};
}
bool apply_inference_result(Engine& engine, const PlannedInference& plan, const InferenceResponse& response, const InferencePolicy& policy, bool dummy) {
    validate_inference_policy(policy);
    if (response.id != plan.request.id || response.operation != plan.request.operation) return false;
    if (engine.registering() || engine.inference_reading(plan.purpose == InferencePurpose::Rerank) != plan.request.reading) return false;
    if (plan.purpose == InferencePurpose::Rerank) {
        if (!engine.can_reorder_candidates() || response.scores.size() != plan.request.candidates.size() || response.scores.size() < 2 ||
            response.scores.size() > engine.candidates().size()) return false;
        std::size_t best = 0;
        for (std::size_t i=0; i<response.scores.size(); ++i) {
            if (!std::isfinite(response.scores[i]) || engine.scoring_text(i) != plan.request.candidates[i]) return false;
            if (response.scores[i] > response.scores[best]) best = i;
        }
        if (response.scores[best]-response.scores[0] <= policy.promote_threshold) return false;
        return engine.promote_candidate(best);
    }
    if (response.candidates.size() > plan.request.count || (!response.scores.empty() && response.scores.size() != response.candidates.size())) return false;
    for (const auto score : response.scores) if (!std::isfinite(score)) return false;
    const auto best = response.scores.empty() ? 0.0f : *std::max_element(response.scores.begin(),response.scores.end());
    const bool long_reading = plan.purpose == InferencePurpose::LongReading;
    std::vector<Candidate> candidates;
    for (std::size_t i=0; i<response.candidates.size(); ++i) {
        const auto& word = response.candidates[i];
        if ((!response.scores.empty() && best-response.scores[i] > policy.max_score_gap) || word == plan.request.reading ||
            word == katakana(plan.request.reading) || word.find(U'\ufffd') != word.npos) continue;
        const bool learn = !dummy && !long_reading && policy.learn_fallback && plan.request.reading.size() < 10 && word.size() < 10;
        candidates.push_back({"generated:"+std::to_string(response.id)+":"+std::to_string(i),word,dummy ? U"ダミーIPC" : U"zenz",
            long_reading ? CandidateSource::GeneratedLong : CandidateSource::GeneratedFallback,learn ? LearningPolicy::Learn : LearningPolicy::DoNotLearn});
    }
    return long_reading ? engine.prepend_generated(std::move(candidates)) : engine.append_generated(std::move(candidates));
}
}
