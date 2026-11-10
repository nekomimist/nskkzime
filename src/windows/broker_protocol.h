#pragma once
#include <string>
#include <string_view>

namespace nskkzime::windows {
enum class BrokerOperation { Init, Check, Learning, Inference, Log, Gui, Unknown };
struct BrokerRequest {
    BrokerOperation operation;
    std::string_view generation, body;
};
// The envelope is shared by clients and broker; payloads keep their own codecs.
// Views returned by decode refer to the caller's request bytes.
std::string encode_broker_request(BrokerOperation, std::string_view generation = {}, std::string_view body = {});
BrokerRequest decode_broker_request(std::string_view);
// Classification is tolerant of incomplete/unknown frames for diagnostics and
// transport access checks, which run before the broker validates the envelope.
BrokerOperation broker_operation(std::string_view) noexcept;
bool broker_learning_wait(std::string_view) noexcept;
} // namespace nskkzime::windows
