#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nskkzime::windows {
// Raw feature inputs keep the decision testable without executing unsupported
// instructions or changing the host CPU, registry or saved settings.
struct CpuFeatures {
    bool native_x64 = false;
    std::uint32_t leaf1_ecx = 0, leaf7_ebx = 0, extended1_ecx = 0;
    std::uint64_t xcr0 = 0;
};
CpuFeatures current_cpu_features() noexcept;
bool can_read_xcr0(const CpuFeatures&) noexcept;
bool zenz_cpu_supported(const CpuFeatures&) noexcept;
std::vector<std::string_view> missing_zenz_cpu_features(const CpuFeatures&);
std::string zenz_cpu_report(const CpuFeatures&); // Fixed-label JSON; no model or user settings.
struct UnsupportedZenzCpu : std::runtime_error { using std::runtime_error::runtime_error; };
void require_zenz_cpu(const CpuFeatures&);
} // namespace nskkzime::windows
