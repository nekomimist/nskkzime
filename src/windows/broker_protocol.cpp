#include "broker_protocol.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
constexpr std::size_t operation_size = 4, generation_size = 64;
constexpr auto header_size = operation_size + generation_size;
struct OperationCode { BrokerOperation operation; std::string_view code; };
constexpr std::array codes{
    OperationCode{BrokerOperation::Init, "INI3"}, OperationCode{BrokerOperation::Check, "CHK1"},
    OperationCode{BrokerOperation::Learning, "LRN1"}, OperationCode{BrokerOperation::Inference, "INF1"},
    OperationCode{BrokerOperation::Log, "LOG2"}, OperationCode{BrokerOperation::Gui, "GUI1"}
};
}
BrokerOperation broker_operation(std::string_view bytes) noexcept {
    for (const auto& [operation, code] : codes)
        if (operation == BrokerOperation::Init ? bytes == code : bytes.starts_with(code)) return operation;
    return BrokerOperation::Unknown;
}
bool broker_learning_wait(std::string_view bytes) noexcept {
    return broker_operation(bytes) == BrokerOperation::Learning && bytes.size() >= header_size &&
        bytes.substr(header_size).starts_with("WAT1");
}
std::string encode_broker_request(BrokerOperation operation, std::string_view generation, std::string_view body) {
    const auto entry = std::ranges::find(codes, operation, &OperationCode::operation);
    if (entry == codes.end()) throw std::invalid_argument("unsupported broker operation");
    if (operation == BrokerOperation::Init) {
        if (!generation.empty() || !body.empty()) throw std::invalid_argument("unexpected bootstrap payload");
        return std::string(entry->code);
    }
    if (generation.size() != generation_size) throw std::invalid_argument("AppContainer initialization pending");
    if (operation == BrokerOperation::Check && !body.empty()) throw std::invalid_argument("unexpected generation-check payload");
    std::string result(entry->code);
    result.append(generation); result.append(body);
    return result;
}
BrokerRequest decode_broker_request(std::string_view bytes) {
    const auto operation = broker_operation(bytes);
    if (operation == BrokerOperation::Init) return {operation, {}, {}};
    // Legacy CHK1 requests of any length receive STAL, rather than a transport
    // error. Preserve that behavior; callers also check that body is empty.
    if (operation == BrokerOperation::Check && bytes.size() < header_size)
        return {operation, bytes.substr(operation_size), {}};
    if (bytes.size() < header_size) throw std::invalid_argument("truncated broker envelope");
    return {operation, bytes.substr(operation_size, generation_size), bytes.substr(header_size)};
}
} // namespace nskkzime::windows
