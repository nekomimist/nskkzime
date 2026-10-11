#pragma once
#include "dictionary_settings.h"
#include "runtime_dictionary.h"
#include "app_container.h"
namespace nskkzime::windows {
struct ContainerConfiguration {
    DictionaryConfiguration configuration;
    std::shared_ptr<const RomajiRules> romaji;
    std::string generation;
    bool complete = false; // Every configured source loaded; only these snapshots are retained.
};
std::shared_ptr<const ContainerConfiguration> container_configuration(); // Latest shared dictionary, diagnostics only.
std::shared_ptr<const ContainerConfiguration> container_configuration(const std::shared_ptr<const Dictionary>&); // Exact snapshot, memory only.
// Starts an off-thread bootstrap. A complete snapshot stays for 10 minutes after
// its last user; reactivation then only confirms that its settings are current.
std::shared_ptr<const Dictionary> container_runtime_dictionary();
std::string container_operation(std::string_view generation, std::string_view operation, std::string_view body, ULONGLONG deadline, HANDLE cancel);
} // namespace nskkzime::windows
