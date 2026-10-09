#pragma once
#include "app_container.h"
#include <map>
#include <mutex>

namespace nskkzime::windows {
class BrokerDiagnostics {
public:
    explicit BrokerDiagnostics(std::filesystem::path directory) : directory_(std::move(directory)) {}
    void record(const ContainerDiagnostic&) noexcept;
    static constexpr std::size_t file_limit = 1024 * 1024;
private:
    struct Rate { ULONGLONG next = 0; unsigned suppressed = 0; };
    std::filesystem::path directory_;
    std::mutex mutex_;
    std::map<std::string, Rate> rates_;
};
} // namespace nskkzime::windows
