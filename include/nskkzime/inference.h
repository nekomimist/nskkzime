#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace nskkzime {
inline constexpr std::size_t inference_max_line = 16384;
struct InferenceRequest {
    std::uint64_t id = 0;
    std::u32string reading, left, right;
    unsigned count = 5;
};
struct InferenceResponse { std::uint64_t id = 0; std::vector<std::u32string> candidates; };
std::string encode_request(const InferenceRequest&);
InferenceRequest decode_request(std::string_view);
std::string encode_response(const InferenceResponse&);
InferenceResponse decode_response(std::string_view, std::uint64_t expected_id, unsigned maximum);
std::string inference_hello();
void validate_inference_hello(std::string_view);
}
