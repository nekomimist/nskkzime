#include "cpu_requirements.h"
#include <windows.h>
#include <array>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif
#if defined(__AVX__) || defined(__AVX2__) || defined(__AVX512F__)
#error CPU preflight must be compiled for the baseline x64 instruction set.
#endif

namespace nskkzime::windows {
namespace {
constexpr std::uint32_t bit(unsigned index) { return std::uint32_t{1} << index; }
struct Requirement { std::string_view name; std::uint32_t CpuFeatures::*leaf; unsigned bit; };
// Conservative MSVC /arch:AVX2 contract, including the non-vector operations
// available to its optimizer. Keep cmake/zenz.cmake and the distribution docs aligned.
constexpr std::array requirements{
    Requirement{"SSE3", &CpuFeatures::leaf1_ecx, 0},
    Requirement{"SSSE3", &CpuFeatures::leaf1_ecx, 9},
    Requirement{"SSE4.1", &CpuFeatures::leaf1_ecx, 19},
    Requirement{"SSE4.2", &CpuFeatures::leaf1_ecx, 20},
    Requirement{"POPCNT", &CpuFeatures::leaf1_ecx, 23},
    Requirement{"AVX", &CpuFeatures::leaf1_ecx, 28},
    Requirement{"FMA", &CpuFeatures::leaf1_ecx, 12},
    Requirement{"F16C", &CpuFeatures::leaf1_ecx, 29},
    Requirement{"XSAVE", &CpuFeatures::leaf1_ecx, 26},
    Requirement{"OSXSAVE", &CpuFeatures::leaf1_ecx, 27},
    Requirement{"AVX2", &CpuFeatures::leaf7_ebx, 5},
    Requirement{"BMI1", &CpuFeatures::leaf7_ebx, 3},
    Requirement{"BMI2", &CpuFeatures::leaf7_ebx, 8},
    Requirement{"LZCNT", &CpuFeatures::extended1_ecx, 5},
};
bool present(const CpuFeatures& features, const Requirement& requirement) noexcept {
    return (features.*(requirement.leaf) & bit(requirement.bit)) != 0;
}
bool avx_state(const CpuFeatures& features) noexcept {
    return can_read_xcr0(features) && (features.xcr0 & 6) == 6; // XMM and YMM state are enabled by the OS.
}
}
bool can_read_xcr0(const CpuFeatures& features) noexcept {
    return features.native_x64 && (features.leaf1_ecx & (bit(26) | bit(27))) == (bit(26) | bit(27));
}
CpuFeatures current_cpu_features() noexcept {
    CpuFeatures features;
#if defined(_M_X64) || defined(_M_IX86)
    // GetNativeSystemInfo can report x64 on ARM64 emulation. Ask for the host
    // architecture explicitly, and fail closed if that query is unavailable.
    using QueryArchitecture = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
#pragma warning(push)
#pragma warning(disable: 4191)
    const auto query = reinterpret_cast<QueryArchitecture>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
#pragma warning(pop)
    USHORT process = 0, native = 0;
    features.native_x64 = query && query(GetCurrentProcess(), &process, &native) &&
        (process == IMAGE_FILE_MACHINE_UNKNOWN || process == IMAGE_FILE_MACHINE_I386) && native == IMAGE_FILE_MACHINE_AMD64;
    if (!features.native_x64) return features; // ARM64 emulation is outside the current product contract.
    int registers[4]{};
    __cpuidex(registers, 0, 0); const auto maximum = registers[0];
    if (maximum >= 1) { __cpuidex(registers, 1, 0); features.leaf1_ecx = static_cast<std::uint32_t>(registers[2]); }
    if (maximum >= 7) { __cpuidex(registers, 7, 0); features.leaf7_ebx = static_cast<std::uint32_t>(registers[1]); }
    __cpuidex(registers, static_cast<int>(0x80000000U), 0);
    if (static_cast<std::uint32_t>(registers[0]) >= 0x80000001U) {
        __cpuidex(registers, static_cast<int>(0x80000001U), 0); features.extended1_ecx = static_cast<std::uint32_t>(registers[2]);
    }
    // XGETBV itself is not legal until the CPU and OS advertise XSAVE support.
    if (can_read_xcr0(features)) features.xcr0 = _xgetbv(0);
#endif
    return features;
}
bool zenz_cpu_supported(const CpuFeatures& features) noexcept {
    if (!features.native_x64 || !avx_state(features)) return false;
    for (const auto& requirement : requirements) if (!present(features, requirement)) return false;
    return true;
}
std::vector<std::string_view> missing_zenz_cpu_features(const CpuFeatures& features) {
    std::vector<std::string_view> missing;
    if (!features.native_x64) missing.push_back("native Windows x64");
    for (const auto& requirement : requirements) if (!present(features, requirement)) missing.push_back(requirement.name);
    if (!avx_state(features)) missing.push_back("OS XMM/YMM state");
    return missing;
}
void require_zenz_cpu(const CpuFeatures& features) {
    if (zenz_cpu_supported(features)) return;
    std::string message = "zenz CPU/OS requirements not met: ";
    for (const auto name : missing_zenz_cpu_features(features)) { if (message.back() != ' ') message += ", "; message += name; }
    throw UnsupportedZenzCpu(message);
}
std::string zenz_cpu_report(const CpuFeatures& features) {
    std::string json = "{\"format\":\"nskkzime-cpu-check\",\"version\":1,\"contract\":\"zenz-msvc-avx2-v1\",\"supported\":";
    json += zenz_cpu_supported(features) ? "true" : "false";
    json += ",\"native_x64\":"; json += features.native_x64 ? "true" : "false";
    json += ",\"features\":{";
    for (const auto& requirement : requirements) {
        if (json.back() != '{') json += ',';
        json += '"'; json += requirement.name; json += "\":"; json += present(features, requirement) ? "true" : "false";
    }
    json += ",\"OS XMM/YMM state\":"; json += avx_state(features) ? "true" : "false";
    json += "},\"missing\":[";
    for (const auto name : missing_zenz_cpu_features(features)) {
        if (json.back() != '[') json += ',';
        json += '"'; json += name; json += '"';
    }
    json += "]}";
    return json;
}
} // namespace nskkzime::windows
