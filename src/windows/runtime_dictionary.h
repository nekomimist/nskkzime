#pragma once
#include "nskkzime/engine.h"
#include <functional>
namespace nskkzime::windows {
struct RuntimeConfiguration;
struct ContainerConfiguration;
struct RuntimeDictionarySnapshot {
    std::shared_ptr<const Dictionary> dictionary;
    std::wstring status;
    bool registration = false;
    std::shared_ptr<const ContainerConfiguration> container;
};
// Publish one complete snapshot off-thread. Destruction never joins a worker
// or calls back into a TSF object. Lookup/status only read memory.
class RuntimeDictionary final : public Dictionary {
public:
    explicit RuntimeDictionary(std::function<RuntimeDictionarySnapshot()> loader, bool retry = false);
    explicit RuntimeDictionary(RuntimeDictionarySnapshot immediate);
    ~RuntimeDictionary() override;
    std::vector<Candidate> lookup(const LookupKey&, std::u32string_view okuri = {}) const override;
    bool ready() const override;
    bool registration_allowed() const override;
    bool registration_pending(const LearningEvent&) const override;
    std::optional<LearningEvent> forget_target(const LookupKey&, std::u32string_view, std::u32string_view) const override;
    bool forget(const LearningEvent&) const noexcept;
    void learn(const LearningEvent&) const noexcept;
    void skipped(std::wstring reason) const noexcept;
    std::wstring status() const;
    std::shared_ptr<const ContainerConfiguration> container_configuration() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
// Reads the small settings file on activation; dictionary bodies load off-thread.
std::shared_ptr<const Dictionary> runtime_dictionary();
std::shared_ptr<const Dictionary> runtime_dictionary(const RuntimeConfiguration&);
// Last snapshot loaded in this process. Diagnostics only; performs no file I/O.
std::wstring runtime_dictionary_status();
bool runtime_dictionary_ready();
void publish_runtime_learning(const std::shared_ptr<const Dictionary>&, const LearningEvent&) noexcept;
bool publish_runtime_forgetting(const std::shared_ptr<const Dictionary>&, const LearningEvent&) noexcept;
void skip_runtime_learning(const std::shared_ptr<const Dictionary>&, std::wstring reason) noexcept;
}
