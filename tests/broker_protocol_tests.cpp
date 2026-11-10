#include "broker_protocol.h"
#include <iostream>
#include <stdexcept>

using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F run) {
    try { run(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("malformed envelope accepted");
}
}
int main() {
    try {
        // Fixed legacy wire fixtures protect compatibility with running brokers
        // and IMEs from the previous build, including binary learning payloads.
        const std::string generation = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const std::string binary("NKL2\0\x01\xff", 7);
        check(encode_broker_request(BrokerOperation::Init) == "INI3", "bootstrap wire changed");
        check(encode_broker_request(BrokerOperation::Check, generation) == "CHK1" + generation, "check wire changed");
        const auto learning = "LRN1" + generation + binary;
        check(encode_broker_request(BrokerOperation::Learning, generation, binary) == learning, "learning wire changed");
        check(encode_broker_request(BrokerOperation::Inference, generation, "{}") == "INF1" + generation + "{}", "inference wire changed");
        check(encode_broker_request(BrokerOperation::Log, generation, "{}") == "LOG2" + generation + "{}", "log wire changed");
        check(encode_broker_request(BrokerOperation::Gui, generation, "settings") == "GUI1" + generation + "settings", "GUI wire changed");
        const auto request = decode_broker_request(learning);
        check(request.operation == BrokerOperation::Learning && request.generation == generation && request.body == binary,
            "binary learning payload or generation lost");
        check(decode_broker_request("INI3").operation == BrokerOperation::Init, "legacy bootstrap rejected");
        for (std::size_t size = 0; size < 68; ++size)
            rejects([&] { (void)decode_broker_request(std::string_view(learning).substr(0, size)); });
        // CHK1 is deliberately permissive: malformed checks must produce STAL.
        for (std::size_t size = 0; size < generation.size(); ++size) {
            const auto bytes = "CHK1" + generation.substr(0, size);
            const auto partial = decode_broker_request(bytes);
            check(partial.operation == BrokerOperation::Check && partial.generation != generation, "short check matches generation");
        }
        const auto extra_bytes = "CHK1" + generation + "x";
        check(!decode_broker_request(extra_bytes).body.empty(), "trailing check payload ignored");
        rejects([] { (void)decode_broker_request("INI3x"); });
        rejects([&] { (void)encode_broker_request(BrokerOperation::Init, generation); });
        rejects([&] { (void)encode_broker_request(BrokerOperation::Check, generation, "x"); });
        rejects([&] { (void)encode_broker_request(BrokerOperation::Learning, generation.substr(1), binary); });
        rejects([&] { (void)encode_broker_request(BrokerOperation::Learning, generation + "x", binary); });
        rejects([&] { (void)encode_broker_request(BrokerOperation::Unknown, generation); });
        const auto unknown = "FILE" + generation + "anything";
        check(decode_broker_request(unknown).operation == BrokerOperation::Unknown, "unknown operation mapped to a command");
        check(broker_operation("GUI1") == BrokerOperation::Gui && broker_operation("GUI1garbage") == BrokerOperation::Gui,
            "incomplete GUI frame escaped transport restriction");
        check(broker_learning_wait("LRN1" + generation + "WAT1") && !broker_learning_wait("LRN1" + generation + "WAT") &&
            !broker_learning_wait("LRN1WAT1") && !broker_learning_wait("INF1" + generation + "WAT1"), "learning wait classification changed");
        std::cout << "PASS broker legacy envelopes, binary payloads, boundaries and classification\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
