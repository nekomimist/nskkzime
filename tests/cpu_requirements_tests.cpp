#include "cpu_requirements.h"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void rejects(const CpuFeatures& features, std::string_view missing) {
    check(!zenz_cpu_supported(features), "unsupported feature set accepted");
    const auto report = nlohmann::json::parse(zenz_cpu_report(features));
    const auto missing_names = report.at("missing").get<std::vector<std::string>>();
    check(report.at("supported") == false && std::find(missing_names.begin(), missing_names.end(), missing) != missing_names.end(),
          "missing feature not explained in JSON");
    try { require_zenz_cpu(features); throw std::logic_error("unsupported CPU did not throw"); }
    catch (const UnsupportedZenzCpu& error) { check(std::string_view(error.what()).find(missing) != std::string_view::npos, "missing feature absent from error"); }
}
}
int main() {
    try {
        const CpuFeatures complete{true, 0x3c981201, 0x128, 0x20, 7};
        check(zenz_cpu_supported(complete) && can_read_xcr0(complete), "AVX2 baseline rejected");
        require_zenz_cpu(complete);
        const auto report = nlohmann::json::parse(zenz_cpu_report(complete));
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
        check(zenz_cpu_supported(unrelated), "extra CPU/OS state rejected");
        const auto actual = current_cpu_features();
        const auto actual_report = nlohmann::json::parse(zenz_cpu_report(actual));
        check(actual_report.at("supported").get<bool>() == zenz_cpu_supported(actual), "host report contradicts decision");
        std::cout << "PASS CPU/OS contract, missing state/ISA, emulation, safe XGETBV gate and JSON\n" << zenz_cpu_report(actual) << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
