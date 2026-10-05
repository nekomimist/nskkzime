#include "nskkzime/inference.h"
#include "nskkzime/romaji.h"
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
namespace nskkzime {
namespace {
using Json = nlohmann::json;
void need(bool ok) { if (!ok) throw std::invalid_argument("invalid inference message"); }
Json parse(std::string_view line) {
    need(!line.empty() && line.size() <= inference_max_line);
    // Bound depth before a DOM is allocated; reject duplicate keys as well.
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        need(depth <= 8);
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key) need(!keys.empty() && keys.back().insert(value.get<std::string>()).second);
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    auto value = Json::parse(line, callback); need(value.is_object()); return value;
}
std::u32string string(const Json& object, const char* key, std::size_t maximum, bool nonempty = false) {
    const auto& value = object.at(key); need(value.is_string());
    auto text = from_utf8(value.get<std::string>());
    need(text.size() <= maximum && (!nonempty || !text.empty()));
    for (const auto ch : text) need(ch != 0 && ch != 0xfffc);
    return text;
}
std::uint64_t id(const Json& value) {
    const auto& number = value.at("id"); need(number.is_number_unsigned());
    const auto result = number.get<std::uint64_t>(); need(result != 0); return result;
}
}
std::string encode_request(const InferenceRequest& request) {
    const auto line = Json{{"id",request.id},{"op","convert"},{"kana",utf8(request.reading)},
        {"left",utf8(request.left)},{"right",utf8(request.right)},{"n",request.count}}.dump();
    decode_request(line); return line;
}
InferenceRequest decode_request(std::string_view line) {
    const auto value = parse(line); need(value.at("op") == "convert");
    InferenceRequest request; request.id = id(value);
    request.reading = string(value, "kana", 1024, true); request.left = string(value,"left",40); request.right = string(value,"right",40);
    need(value.at("n").is_number_unsigned()); const auto count = value.at("n").get<std::uint64_t>(); need(count >= 1 && count <= 8);
    request.count = static_cast<unsigned>(count); return request;
}
std::string encode_response(const InferenceResponse& response) {
    auto values = Json::array(); auto scores = Json::array();
    for (const auto& text : response.candidates) { values.push_back(utf8(text)); scores.push_back(0.0); }
    const auto line = Json{{"id",response.id},{"candidates",values},{"scores",scores}}.dump();
    decode_response(line, response.id, 8); return line;
}
InferenceResponse decode_response(std::string_view line, std::uint64_t expected_id, unsigned maximum) {
    const auto value = parse(line); InferenceResponse response; response.id = id(value); need(response.id == expected_id);
    need(!value.contains("error")); const auto& candidates = value.at("candidates");
    need(candidates.is_array() && candidates.size() <= maximum && candidates.size() <= 8);
    for (const auto& candidate : candidates) {
        need(candidate.is_string()); auto text = from_utf8(candidate.get<std::string>()); need(!text.empty() && text.size() <= 1024);
        for (const auto ch : text) need(ch >= 0x20 && ch != 0x7f && ch != 0xfffc);
        response.candidates.push_back(std::move(text));
    }
    return response;
}
std::string inference_hello() { return R"({"hello":"nkkz-ime-server","protocol":2,"backend":"dummy","version":"0.0.1"})"; }
void validate_inference_hello(std::string_view line) {
    const auto value = parse(line); need(value.at("hello") == "nkkz-ime-server" && value.at("protocol") == 2);
}
}
