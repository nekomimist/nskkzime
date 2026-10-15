#pragma once
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nskkzime::windows {
// Raw feature inputs keep the decision testable without executing unsupported
// instructions or changing the host CPU, registry or saved settings.
struct CpuFeatures {
    bool native_x64 = false; // Host is AMD64, including an x86 caller under WOW64.
    std::uint32_t leaf1_ecx = 0, leaf7_ebx = 0, extended1_ecx = 0;
    std::uint64_t xcr0 = 0;
    bool native_arm64 = false;
};
enum class ZenzCpuContract { MsvcAvx2, Arm64Armv8a };
constexpr ZenzCpuContract bundled_zenz_cpu_contract() noexcept {
#if defined(_M_ARM64) || defined(_M_ARM64EC)
    return ZenzCpuContract::Arm64Armv8a;
#else
    return ZenzCpuContract::MsvcAvx2;
#endif
}
std::string_view zenz_cpu_contract_name(ZenzCpuContract) noexcept;
// Project servers and development overrides must use the documented ISA
// contract for their PE architecture. This does not detect arbitrary compiler flags.
ZenzCpuContract zenz_server_cpu_contract(const std::filesystem::path&);
CpuFeatures current_cpu_features() noexcept;
bool can_read_xcr0(const CpuFeatures&) noexcept;
bool zenz_cpu_supported(const CpuFeatures&, ZenzCpuContract = bundled_zenz_cpu_contract()) noexcept;
std::vector<std::string_view> missing_zenz_cpu_features(const CpuFeatures&, ZenzCpuContract = bundled_zenz_cpu_contract());
std::string zenz_cpu_report(const CpuFeatures&, ZenzCpuContract = bundled_zenz_cpu_contract()); // No model or user settings.
struct UnsupportedZenzCpu : std::runtime_error { using std::runtime_error::runtime_error; };
void require_zenz_cpu(const CpuFeatures&, ZenzCpuContract = bundled_zenz_cpu_contract());
} // namespace nskkzime::windows
