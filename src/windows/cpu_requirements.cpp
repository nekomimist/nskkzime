#include "cpu_requirements.h"
#include <windows.h>
#include <array>
#include <fstream>
#if (defined(_M_X64) || defined(_M_IX86)) && !defined(_M_ARM64EC)
#include <intrin.h>
#endif
#if defined(__AVX__) || defined(__AVX2__) || defined(__AVX512F__)
#error CPU preflight must be compiled without x86 ISA extensions.
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
    return features.native_x64 && !features.native_arm64 && (features.leaf1_ecx & (bit(26) | bit(27))) == (bit(26) | bit(27));
}
CpuFeatures current_cpu_features() noexcept {
    CpuFeatures features;
    // GetNativeSystemInfo can report x64 on ARM64 emulation. Ask for the host
    // architecture explicitly, and fail closed if that query is unavailable.
    using QueryArchitecture = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
#pragma warning(push)
#pragma warning(disable: 4191)
    const auto query = reinterpret_cast<QueryArchitecture>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
#pragma warning(pop)
    USHORT process = 0, native = 0;
    if (!query || !query(GetCurrentProcess(), &process, &native)) return features;
    features.native_arm64 = native == IMAGE_FILE_MACHINE_ARM64;
    features.native_x64 =
        (process == IMAGE_FILE_MACHINE_UNKNOWN || process == IMAGE_FILE_MACHINE_I386) && native == IMAGE_FILE_MACHINE_AMD64;
#if (defined(_M_X64) || defined(_M_IX86)) && !defined(_M_ARM64EC)
    if (!features.native_x64) return features; // Never run x86 feature probes on an ARM64 host.
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
std::string_view zenz_cpu_contract_name(ZenzCpuContract contract) noexcept {
    return contract == ZenzCpuContract::Arm64Armv8a ? "zenz-arm64-armv8a-v1" : "zenz-msvc-avx2-v1";
}
ZenzCpuContract zenz_server_cpu_contract(const std::filesystem::path& program) {
    // Read headers as data. Loading the DLL/EXE could execute ISA-specific initializers.
    std::ifstream file(program, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 pe{};
    if (!file.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024 ||
        !file.seekg(dos.e_lfanew) || !file.read(reinterpret_cast<char*>(&pe), sizeof(pe)) ||
        pe.Signature != IMAGE_NT_SIGNATURE || pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !(pe.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) || (pe.FileHeader.Characteristics & IMAGE_FILE_DLL))
        throw UnsupportedZenzCpu("invalid inference server PE image");
    if (pe.FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64) return ZenzCpuContract::Arm64Armv8a;
    if (pe.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) return ZenzCpuContract::MsvcAvx2;
    throw UnsupportedZenzCpu("unsupported inference server PE architecture");
}
bool zenz_cpu_supported(const CpuFeatures& features, ZenzCpuContract contract) noexcept {
    if (contract == ZenzCpuContract::Arm64Armv8a) return features.native_arm64 && !features.native_x64;
    if (!features.native_x64 || features.native_arm64 || !avx_state(features)) return false;
    for (const auto& requirement : requirements) if (!present(features, requirement)) return false;
    return true;
}
std::vector<std::string_view> missing_zenz_cpu_features(const CpuFeatures& features, ZenzCpuContract contract) {
    std::vector<std::string_view> missing;
    if (contract == ZenzCpuContract::Arm64Armv8a) {
        if (!features.native_arm64 || features.native_x64) missing.push_back("native Windows ARM64");
        return missing;
    }
    if (!features.native_x64 || features.native_arm64) missing.push_back("native Windows x64");
    for (const auto& requirement : requirements) if (!present(features, requirement)) missing.push_back(requirement.name);
    if (!avx_state(features)) missing.push_back("OS XMM/YMM state");
    return missing;
}
void require_zenz_cpu(const CpuFeatures& features, ZenzCpuContract contract) {
    if (zenz_cpu_supported(features, contract)) return;
    std::string message = "zenz CPU/OS requirements not met: ";
    for (const auto name : missing_zenz_cpu_features(features, contract)) { if (message.back() != ' ') message += ", "; message += name; }
    throw UnsupportedZenzCpu(message);
}
std::string zenz_cpu_report(const CpuFeatures& features, ZenzCpuContract contract) {
    std::string json = "{\"format\":\"nskkzime-cpu-check\",\"version\":1,\"contract\":\"";
    json += zenz_cpu_contract_name(contract); json += "\",\"supported\":";
    json += zenz_cpu_supported(features, contract) ? "true" : "false";
    json += ",\"native_x64\":"; json += features.native_x64 ? "true" : "false";
    json += ",\"native_arm64\":"; json += features.native_arm64 ? "true" : "false";
    json += ",\"features\":{";
    if (contract == ZenzCpuContract::Arm64Armv8a) {
        json += "\"Armv8-A\":"; json += zenz_cpu_supported(features, contract) ? "true" : "false";
    } else {
        for (const auto& requirement : requirements) {
            if (json.back() != '{') json += ',';
            json += '"'; json += requirement.name; json += "\":"; json += present(features, requirement) ? "true" : "false";
        }
        json += ",\"OS XMM/YMM state\":"; json += avx_state(features) ? "true" : "false";
    }
    json += "},\"missing\":[";
    for (const auto name : missing_zenz_cpu_features(features, contract)) {
        if (json.back() != '[') json += ',';
        json += '"'; json += name; json += '"';
    }
    json += "]}";
    return json;
}
} // namespace nskkzime::windows
