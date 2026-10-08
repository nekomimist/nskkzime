#pragma once
#include "dictionary_settings.h"
#include "runtime_dictionary.h"
#include "app_container.h"
namespace nskkzime::windows {
struct ContainerConfiguration {
    DictionaryConfiguration configuration;
    std::shared_ptr<const RomajiRules> romaji;
    std::string generation;
};
std::shared_ptr<const ContainerConfiguration> container_configuration(); // Memory only.
std::shared_ptr<const Dictionary> container_runtime_dictionary(); // Starts an off-thread bootstrap.
std::string container_operation(std::string_view operation, std::string_view body, ULONGLONG deadline, HANDLE cancel);
} // namespace nskkzime::windows
