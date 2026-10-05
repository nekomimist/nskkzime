#include "nskkzime/inference.h"
#include "nskkzime/romaji.h"
#include <nlohmann/json.hpp>
#include <cmath>
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
std::vector<float> scores(const Json& value, std::size_t count) {
    need(value.is_array() && value.size() == count);
    std::vector<float> result;
    for (const auto& score : value) {
        need(score.is_number()); const auto number = score.get<float>(); need(std::isfinite(number)); result.push_back(number);
    }
    return result;
}
}
std::string encode_request(const InferenceRequest& request) {
    Json value{{"id",request.id},{"kana",utf8(request.reading)},{"left",utf8(request.left)},{"right",utf8(request.right)}};
    if (request.operation == InferenceOperation::Score) {
        value["op"] = "score"; value["candidates"] = Json::array();
        for (const auto& text : request.candidates) value["candidates"].push_back(utf8(text));
    } else { need(request.operation == InferenceOperation::Convert); value["op"] = "convert"; value["n"] = request.count; }
    const auto line = value.dump();
    decode_request(line); return line;
}
InferenceRequest decode_request(std::string_view line) {
    const auto value = parse(line); need(value.at("op") == "convert" || value.at("op") == "score");
    InferenceRequest request; request.id = id(value);
    request.reading = string(value, "kana", 1024, true); request.left = string(value,"left",40); request.right = string(value,"right",40);
    if (value.at("op") == "score") {
        request.operation = InferenceOperation::Score;
        const auto& texts = value.at("candidates"); need(texts.is_array() && texts.size() <= 64);
        for (const auto& text : texts) {
            need(text.is_string()); auto decoded = from_utf8(text.get<std::string>()); need(decoded.size() <= 1024);
            for (const auto ch : decoded) need(ch != 0 && ch != 0xfffc);
            request.candidates.push_back(std::move(decoded));
        }
    } else {
        need(value.at("n").is_number_unsigned()); const auto count = value.at("n").get<std::uint64_t>(); need(count >= 1 && count <= 8);
        request.count = static_cast<unsigned>(count);
    }
    return request;
}
std::string encode_response(const InferenceResponse& response) {
    Json value{{"id",response.id}};
    auto numbers = response.scores;
    if (response.operation == InferenceOperation::Convert) {
        value["candidates"] = Json::array();
        for (const auto& text : response.candidates) value["candidates"].push_back(utf8(text));
        if (numbers.empty()) numbers.resize(response.candidates.size(),0.0f);
    } else need(response.operation == InferenceOperation::Score && response.candidates.empty() && numbers.size() <= 64);
    for (const auto number : numbers) need(std::isfinite(number));
    value["scores"] = numbers;
    const auto line = value.dump(); need(line.size() <= inference_max_line && response.id != 0);
    if (response.operation == InferenceOperation::Convert) decode_response(line, response.id, 8);
    return line;
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
    if (value.contains("scores")) response.scores = scores(value.at("scores"),response.candidates.size());
    return response;
}
InferenceResponse decode_response(std::string_view line, const InferenceRequest& request) {
    if (request.operation == InferenceOperation::Convert) return decode_response(line,request.id,request.count);
    need(request.operation == InferenceOperation::Score);
    const auto value = parse(line); InferenceResponse response; response.id = id(value);
    need(response.id == request.id && !value.contains("error") && !value.contains("candidates") && request.candidates.size() <= 64);
    response.operation = InferenceOperation::Score; response.scores = scores(value.at("scores"),request.candidates.size()); return response;
}
std::string inference_hello(std::string_view backend) {
    return Json{{"hello","nkkz-ime-server"},{"protocol",2},{"backend",backend},{"version","0.0.1"}}.dump();
}
void validate_inference_hello(std::string_view line) {
    const auto value = parse(line); need(value.at("hello") == "nkkz-ime-server" && value.at("protocol") == 2);
}
}
