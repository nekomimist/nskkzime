#pragma once
#include "settings_file.h"
#include "runtime_dictionary.h"
#include "app_container.h"
namespace nskkzime::windows {
struct ContainerConfiguration {
    SettingsConfiguration configuration;
    InputProfile input_profile;
    std::string generation;
    bool complete = false; // Every configured source loaded; only these snapshots are retained.
};
std::shared_ptr<const ContainerConfiguration> container_configuration(); // Latest shared dictionary, diagnostics only.
std::shared_ptr<const ContainerConfiguration> container_configuration(const std::shared_ptr<const RuntimeDictionary>&); // Exact snapshot, memory only.
// Starts an off-thread bootstrap. A complete snapshot stays for 10 minutes after
// its last user; reactivation then only confirms that its settings are current.
std::shared_ptr<const RuntimeDictionary> container_runtime_dictionary();
} // namespace nskkzime::windows
