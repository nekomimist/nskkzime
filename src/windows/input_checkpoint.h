#pragma once
#include <msctf.h>
#include <wrl/client.h>
#include <cstdint>

namespace nskkzime::windows {
// Retain COM identities across host calls. Session prevents reuse of a context
// from reviving old work; revision also rejects changes within that session.
struct InputCheckpoint {
    Microsoft::WRL::ComPtr<ITfContext> context;
    Microsoft::WRL::ComPtr<ITfComposition> composition;
    std::uint64_t session = 0, revision = 0;
    bool same_composition(const InputCheckpoint& other) const noexcept {
        return context.Get() == other.context.Get() && composition.Get() == other.composition.Get() && session == other.session;
    }
    bool matches(const InputCheckpoint& other) const noexcept {
        return revision == other.revision && same_composition(other);
    }
};
} // namespace nskkzime::windows
