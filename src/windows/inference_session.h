#pragma once
#include "inference_client.h"
#include "context_text.h"
#include <msctf.h>
#include <wrl/client.h>

namespace nskkzime::windows {
struct PendingInference {
    std::uint64_t id, revision, session, generation;
    ULONGLONG deadline;
    Microsoft::WRL::ComPtr<ITfContext> context;
    Microsoft::WRL::ComPtr<ITfComposition> composition;
    SurroundingText text;
    PlannedInference plan;
};
// Owns the request identity and cancellation protocol. TSF retains permission
// checks inside its edit sessions, including checks after reentrant host calls.
class InferenceSession {
public:
    std::unique_ptr<InferenceClient> client;
    bool dummy = false, allow_unknown_context = false;
    InferencePolicy policy;
    std::optional<PendingInference> pending;
    std::uint64_t sequence = 0, generation = 0;
    void configure(const InferenceConfig&, InferenceClient::Callback);
    bool cancel() noexcept;
    bool matches(const PendingInference&, std::uint64_t revision, std::uint64_t session,
        ITfContext*, ITfComposition*) const;
    void submit(PlannedInference, SurroundingText, std::uint64_t revision, std::uint64_t session,
        ITfContext*, ITfComposition*);
};
} // namespace nskkzime::windows
