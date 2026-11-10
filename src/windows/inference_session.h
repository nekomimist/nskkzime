#pragma once
#include "inference_client.h"
#include "context_text.h"
#include "input_checkpoint.h"
#include <msctf.h>
#include <wrl/client.h>

namespace nskkzime::windows {
struct PendingInference {
    std::uint64_t id, generation;
    ULONGLONG deadline;
    InputCheckpoint input;
    SurroundingText text;
    PlannedInference plan;
};
// Owns the request identity and cancellation protocol. TSF retains permission
// checks inside its edit sessions, including checks after reentrant host calls.
class InferenceSession {
public:
    std::unique_ptr<InferenceClient> client;
    bool allow_unknown_context = false;
    InferencePolicy policy;
    std::optional<PendingInference> pending;
    std::uint64_t sequence = 0, generation = 0;
    void configure(const InferenceConfig&, InferenceClient::Callback);
    bool cancel() noexcept;
    bool matches(const PendingInference&, InputCheckpoint) const;
    void submit(PlannedInference, SurroundingText, InputCheckpoint);
    // Rerank only: score within policy.rerank_wait_ms before the candidates are
    // first shown. nullopt (disabled, slow or unavailable) means use submit().
    std::optional<InferenceResponse> score_now(PlannedInference&, const SurroundingText&);
};
} // namespace nskkzime::windows
