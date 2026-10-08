#pragma once
#include <string_view>

namespace nskkzime::windows {
enum class CandidateKeys { Numbers, Asdfjkl };
struct CandidateSettings {
    unsigned int show_after = 1; // Initial conversion is step 1, including automatic okuri conversion.
    CandidateKeys keys = CandidateKeys::Numbers;
};
inline std::string_view candidate_keys(CandidateKeys keys) noexcept {
    return keys == CandidateKeys::Asdfjkl ? "asdfjkl" : "1234567";
}
inline std::wstring_view candidate_labels(CandidateKeys keys) noexcept {
    return keys == CandidateKeys::Asdfjkl ? L"ASDFJKL" : L"1234567";
}
} // namespace nskkzime::windows
