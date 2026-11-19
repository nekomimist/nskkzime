#include "inference_session.h"

namespace nskkzime::windows {
void InferenceSession::configure(const InferenceConfig& config, InferenceClient::Callback callback) {
    policy = config.policy;
    allow_unknown_context = config.allow_unknown_context;
    client = std::make_unique<InferenceClient>(config, std::move(callback));
}
bool InferenceSession::cancel() noexcept {
    ++generation;
    const bool active = pending.has_value();
    pending.reset();
    if (client) client->cancel();
    return active;
}
bool InferenceSession::matches(const PendingInference& request, InputCheckpoint input) const {
    return pending && pending->id == request.id && generation == request.generation && request.input.matches(input);
}
std::optional<InferenceResponse> InferenceSession::score_now(PlannedInference& plan, const SurroundingText& text, ULONGLONG& deadline) {
    if (!client || plan.purpose != InferencePurpose::Rerank || !policy.rerank_wait_ms) return std::nullopt;
    plan.request.id = ++sequence; plan.request.left = text.before; plan.request.right = text.after;
    const auto until = GetTickCount64() + policy.rerank_wait_ms;
    deadline = client->submit(plan.request, policy.score_timeout_ms, true);
    return client->wait_for_response(plan.request.id, (std::min)(until, deadline));
}
bool InferenceSession::submit(PlannedInference plan, SurroundingText text, InputCheckpoint input, ULONGLONG deadline) {
    if (deadline) {
        if (!client->deliver_deferred(plan.request.id)) return false;
    } else {
        plan.request.id = ++sequence; plan.request.left = text.before; plan.request.right = text.after;
        deadline = client->submit(plan.request, plan.purpose == InferencePurpose::Rerank ? policy.score_timeout_ms : 0);
    }
    pending = PendingInference{plan.request.id, generation, deadline, std::move(input), std::move(text), std::move(plan)};
    return true;
}
} // namespace nskkzime::windows
