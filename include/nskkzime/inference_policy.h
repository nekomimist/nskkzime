#pragma once
#include "engine.h"
#include "inference.h"
namespace nskkzime {
struct InferencePolicy {
    unsigned long_min_length = 10; // Zero disables long-reading priority.
    bool rerank = false, learn_fallback = false;
    unsigned rerank_limit = 20, score_timeout_ms = 300;
    float promote_threshold = 1.0f, max_score_gap = 8.0f;
};
enum class InferencePurpose { Fallback, LongReading, Rerank };
struct PlannedInference {
    InferencePurpose purpose;
    InferenceRequest request; // ID/context are filled by the host after its edit succeeds.
};
void validate_inference_policy(const InferencePolicy&);
std::optional<PlannedInference> plan_inference(const Engine& before, const Engine& after, const Result&, const InferencePolicy&);
// Only mutates on a usable result. The host validates revisions, context and deadlines first.
bool apply_inference_result(Engine&, const PlannedInference&, const InferenceResponse&, const InferencePolicy&, bool dummy = false);
}
