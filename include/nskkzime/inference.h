#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace nskkzime {
inline constexpr std::size_t inference_max_line = 16384;
enum class InferenceOperation { Convert, Score };
struct InferenceRequest {
    std::uint64_t id = 0;
    std::u32string reading, left, right;
    unsigned count = 5;
    InferenceOperation operation = InferenceOperation::Convert;
    std::vector<std::u32string> candidates; // Score inputs, in dictionary order.
};
struct InferenceResponse {
    std::uint64_t id = 0;
    std::vector<std::u32string> candidates;
    std::vector<float> scores;
    InferenceOperation operation = InferenceOperation::Convert;
};
std::string encode_request(const InferenceRequest&);
InferenceRequest decode_request(std::string_view);
std::string encode_response(const InferenceResponse&);
InferenceResponse decode_response(std::string_view, std::uint64_t expected_id, unsigned maximum);
InferenceResponse decode_response(std::string_view, const InferenceRequest&);
// First line of the server's --stdio and --version output.
std::string inference_hello(std::string_view backend = "dummy");
}
