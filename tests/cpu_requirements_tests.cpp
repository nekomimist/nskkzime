#include "cpu_requirements.h"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>

using namespace nskkzime::windows;
namespace {
constexpr auto x64 = ZenzCpuContract::MsvcAvx2;
constexpr auto arm64 = ZenzCpuContract::Arm64Armv8a;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void rejects(const CpuFeatures& features, std::string_view missing, ZenzCpuContract contract = x64) {
    check(!zenz_cpu_supported(features, contract), "unsupported feature set accepted");
    const auto report = nlohmann::json::parse(zenz_cpu_report(features, contract));
    const auto missing_names = report.at("missing").get<std::vector<std::string>>();
    check(report.at("supported") == false && std::find(missing_names.begin(), missing_names.end(), missing) != missing_names.end(),
          "missing feature not explained in JSON");
    try { require_zenz_cpu(features, contract); throw std::logic_error("unsupported CPU did not throw"); }
    catch (const UnsupportedZenzCpu& error) { check(std::string_view(error.what()).find(missing) != std::string_view::npos, "missing feature absent from error"); }
}
void pe_contract_tests() {
    wchar_t directory[MAX_PATH]{}, name[MAX_PATH]{};
    check(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"nzk", 0, name), "create PE fixture");
    struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{name};
    const std::filesystem::path path(name);
    const auto write = [&](WORD machine, bool valid = true) {
        IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = sizeof(dos);
        IMAGE_NT_HEADERS64 pe{}; pe.Signature = valid ? IMAGE_NT_SIGNATURE : 0;
        pe.FileHeader.Machine = machine; pe.FileHeader.SizeOfOptionalHeader = sizeof(pe.OptionalHeader);
        pe.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
        pe.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(&dos), sizeof(dos));
        file.write(reinterpret_cast<const char*>(&pe), sizeof(pe));
        check(static_cast<bool>(file), "write PE fixture");
    };
    write(IMAGE_FILE_MACHINE_AMD64); check(zenz_server_cpu_contract(path) == x64, "x64 PE contract");
    write(IMAGE_FILE_MACHINE_ARM64); check(zenz_server_cpu_contract(path) == arm64, "ARM64 PE contract");
    for (const auto machine : {WORD{IMAGE_FILE_MACHINE_I386}, WORD{0xa641}, WORD{0xa64e}, WORD{0}}) {
        write(machine);
        try { (void)zenz_server_cpu_contract(path); throw std::logic_error("unsupported PE accepted"); }
        catch (const UnsupportedZenzCpu&) {}
    }
    write(IMAGE_FILE_MACHINE_ARM64, false);
    try { (void)zenz_server_cpu_contract(path); throw std::logic_error("corrupt PE accepted"); }
    catch (const UnsupportedZenzCpu&) {}
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << "MZ"; }
    try { (void)zenz_server_cpu_contract(path); throw std::logic_error("truncated PE accepted"); }
    catch (const UnsupportedZenzCpu&) {}
}
}
int main() {
    try {
        const CpuFeatures complete{true, 0x3c981201, 0x128, 0x20, 7};
        check(zenz_cpu_supported(complete, x64) && can_read_xcr0(complete), "AVX2 baseline rejected");
        require_zenz_cpu(complete, x64);
        const auto report = nlohmann::json::parse(zenz_cpu_report(complete, x64));
        check(report.at("format") == "nskkzime-cpu-check" && report.at("version") == 1 &&
              report.at("contract") == "zenz-msvc-avx2-v1" && report.at("missing").empty(), "invalid CPU report");
        for (const auto missing_state : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{3}, std::uint64_t{5}}) {
            auto limited = complete; limited.xcr0 = missing_state; rejects(limited, "OS XMM/YMM state");
        }
        for (unsigned missing_bit : {26U, 27U}) {
            auto limited = complete; limited.leaf1_ecx &= ~(std::uint32_t{1} << missing_bit);
            check(!can_read_xcr0(limited), "XGETBV would execute without OSXSAVE/XSAVE");
            rejects(limited, missing_bit == 26 ? "XSAVE" : "OSXSAVE");
        }
        auto emulated = complete; emulated.native_x64 = false;
        check(!can_read_xcr0(emulated), "emulation executed feature probe"); rejects(emulated, "native Windows x64");
        auto avx_only = complete; avx_only.leaf7_ebx = 0; rejects(avx_only, "AVX2"); rejects(avx_only, "BMI2");
        auto no_fma = complete; no_fma.leaf1_ecx &= ~0x1000U; rejects(no_fma, "FMA");
        auto no_f16c = complete; no_f16c.leaf1_ecx &= ~0x20000000U; rejects(no_f16c, "F16C");
        auto no_bmi1 = complete; no_bmi1.leaf7_ebx &= ~8U; rejects(no_bmi1, "BMI1");
        auto no_lzcnt = complete; no_lzcnt.extended1_ecx = 0; rejects(no_lzcnt, "LZCNT");
        CpuFeatures no_leaves; rejects(no_leaves, "AVX");
        auto unrelated = complete; unrelated.leaf1_ecx |= 0x40000000U; unrelated.xcr0 |= 0xe0;
        check(zenz_cpu_supported(unrelated, x64), "extra CPU/OS state rejected");
        CpuFeatures native_arm; native_arm.native_arm64 = true;
        check(zenz_cpu_supported(native_arm, arm64), "native ARM64 baseline rejected");
        require_zenz_cpu(native_arm, arm64);
        rejects(native_arm, "native Windows x64", x64);
        rejects(complete, "native Windows ARM64", arm64);
        rejects(CpuFeatures{}, "native Windows ARM64", arm64);
        auto emulated_x64 = complete; emulated_x64.native_x64 = false; emulated_x64.native_arm64 = true;
        rejects(emulated_x64, "native Windows x64", x64);
        check(!can_read_xcr0(emulated_x64), "ARM64 emulation allowed XGETBV");
        auto contradictory = complete; contradictory.native_arm64 = true;
        rejects(contradictory, "native Windows ARM64", arm64);
        rejects(contradictory, "native Windows x64", x64);
        pe_contract_tests();
        const auto actual = current_cpu_features();
        const auto actual_report = nlohmann::json::parse(zenz_cpu_report(actual));
        check(actual_report.at("supported").get<bool>() == zenz_cpu_supported(actual), "host report contradicts decision");
        std::cout << "PASS CPU/OS contract, missing state/ISA, emulation, safe XGETBV gate and JSON\n" << zenz_cpu_report(actual) << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
