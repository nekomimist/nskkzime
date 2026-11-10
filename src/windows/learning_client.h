#pragma once
#include "nskkzime/dictionary.h"
#include "nskkzime/learning.h"
#include <filesystem>

namespace nskkzime::windows {
// Activation starts a background worker. lookup() reads memory; learn() only
// queues an event. Neither method opens a file or pipe.
class LearningDictionary final : public Dictionary {
public:
    LearningDictionary(std::shared_ptr<const Dictionary> base, std::filesystem::path file, std::string container_generation = {});
    ~LearningDictionary() override;
    std::vector<Candidate> lookup(const LookupKey&, std::u32string_view okuri = {}) const override;
    std::vector<LookupCandidate> lookup_many(std::span<const LookupKey>, std::u32string_view okuri = {}) const override;
    bool registration_pending(const LearningEvent&) const override;
    std::optional<LearningEvent> forget_target(const LookupKey&, std::u32string_view, std::u32string_view) const override;
    std::optional<RerankBoost> rerank_boost(const LookupKey&) const override;
    bool forget(const LearningEvent&) const noexcept;
    void learn(const LearningEvent&) const noexcept;
    void learn_rerank(const LearningEvent&) const noexcept;
    void skipped(std::wstring reason) const noexcept;
    std::wstring status() const;
private:
    bool enqueue(const LearningEvent&, DictionaryAction) const noexcept;
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace nskkzime::windows
