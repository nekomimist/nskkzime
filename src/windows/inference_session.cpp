#include "inference_session.h"

namespace nskkzime::windows {
void InferenceSession::configure(const InferenceConfig& config, InferenceClient::Callback callback) {
    dummy = !config.container && config.model.empty();
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
bool InferenceSession::matches(const PendingInference& request, std::uint64_t revision,
    std::uint64_t session, ITfContext* context, ITfComposition* composition) const {
    return pending && pending->id == request.id && revision == request.revision &&
        session == request.session && generation == request.generation &&
        context == request.context.Get() && composition == request.composition.Get();
}
std::optional<InferenceResponse> InferenceSession::score_now(PlannedInference& plan, const SurroundingText& text) {
    if (!client || plan.purpose != InferencePurpose::Rerank || !policy.rerank_wait_ms) return std::nullopt;
    plan.request.id = ++sequence; plan.request.left = text.before; plan.request.right = text.after;
    return client->exchange_now(plan.request, (std::min)(policy.rerank_wait_ms, policy.score_timeout_ms));
}
void InferenceSession::submit(PlannedInference plan, SurroundingText text, std::uint64_t revision,
    std::uint64_t session, ITfContext* context, ITfComposition* composition) {
    const auto id = ++sequence;
    plan.request.id = id; plan.request.left = text.before; plan.request.right = text.after;
    const auto deadline = client->submit(plan.request, plan.purpose == InferencePurpose::Rerank ? policy.score_timeout_ms : 0);
    pending = PendingInference{id, revision, session, generation, deadline, context, composition, std::move(text), std::move(plan)};
}
} // namespace nskkzime::windows
