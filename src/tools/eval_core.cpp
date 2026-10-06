#include "nskkzime/dictionary.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace nskkzime;
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
unsigned count(const Json& input, const char* field, unsigned fallback, unsigned maximum) {
    if (!input.contains(field)) return fallback;
    const auto& value = input.at(field);
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() == 0 || value.get<std::uint64_t>() > maximum)
        throw std::invalid_argument("invalid repeat count");
    return value.get<unsigned>();
}
Json lookup(const SkkDictionary& dictionary, const Json& input) {
    const auto reading = from_utf8(input.at("reading").get<std::string>());
    const auto okuri = from_utf8(input.value("okuri", std::string{}));
    const auto letter = input.value("okuri_letter", std::string{});
    if (reading.empty() || reading.size() > 1024 || okuri.size() > 1024 ||
        (!letter.empty() && (letter.size() != 1 || letter[0] < 'a' || letter[0] > 'z')) || (letter.empty() != okuri.empty()))
        throw std::invalid_argument("invalid lookup key");
    const LookupKey key{reading, letter.empty() ? char{} : letter[0]};
    std::vector<Candidate> candidates;
    Json times = Json::array();
    const auto rounds = count(input, "repeats", 20, 10000);
    for (unsigned i = 0; i <= rounds; ++i) {
        const auto start = Clock::now();
        auto found = dictionary.lookup(key, okuri);
        const auto elapsed = ms(start);
        if (i) times.push_back(elapsed); // One warmup per reading, never included.
        candidates = std::move(found);
    }
    Json words = Json::array();
    for (const auto& candidate : candidates) words.push_back(utf8(candidate.text + okuri));
    return {{"candidates", words}, {"lookup_ms", times}};
}
Json keys(const std::shared_ptr<const Dictionary>& dictionary, const Json& input) {
    // Real default romaji and ordinary dictionary paths. This measures the
    // transactional core path, not TSF locks, rendering, IPC, or OS dispatch.
    const std::vector<std::string> sequences{"kana", "Kanji \n", "Nihon \n", "KaKu\n", "/skk \n", "Koukounokyoushi \n"};
    Engine engine(dictionary, false);
    Json times = Json::array(), conversion_times = Json::array();
    std::size_t checksum = 0;
    const auto rounds = count(input, "rounds", 1000, 10000);
    for (unsigned round = 0; round <= rounds; ++round) {
        for (const auto& sequence : sequences) {
            engine.reset();
            for (const auto ch : sequence) {
                const Key key = ch == ' ' ? Key{KeyCode::Space} : ch == '\n' ? Key{KeyCode::Enter} : Key{KeyCode::Character, ch};
                const auto start = Clock::now();
                const bool handled = engine.would_handle(key);
                bool converted = false;
                if (handled) {
                    auto trial = engine;
                    const auto result = trial.handle(key);
                    checksum += result.committed.size() + trial.composition_text().size();
                    converted = result.conversion_started;
                    engine = std::move(trial);
                }
                const auto elapsed = ms(start);
                if (round) { times.push_back(elapsed); if (converted) conversion_times.push_back(elapsed); }
            }
        }
    }
    return {{"key_ms", times}, {"conversion_key_ms", conversion_times}, {"checksum", checksum}, {"rounds", rounds}};
}
int run(const std::filesystem::path& file) {
    try {
        const auto start = Clock::now();
        const auto dictionary = SkkDictionary::load(file);
        const auto load_ms = ms(start);
        const auto& report = dictionary->report();
        std::cout << Json{{"hello", "nskkzime-eval-core"}, {"load_ms", load_ms}, {"entries", report.entries},
            {"candidates", report.candidates}, {"malformed_lines", report.malformed_lines},
            {"unsupported_candidates", report.unsupported_candidates}}.dump() << std::endl;
        for (std::string line; std::getline(std::cin, line);) {
            try {
                if (line.size() > 16384) throw std::invalid_argument("evaluation request too large");
                const auto input = Json::parse(line);
                const auto op = input.at("op").get<std::string>();
                const auto output = op == "lookup" ? lookup(*dictionary, input) : op == "keys" ? keys(dictionary, input) :
                    throw std::invalid_argument("unknown evaluation operation");
                std::cout << output.dump() << std::endl;
            } catch (...) { std::cout << "{\"error\":\"invalid evaluation request\"}" << std::endl; }
        }
        return 0;
    } catch (...) { std::cerr << "Cannot load evaluation dictionary\n"; return 2; }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return argc == 2 ? run(argv[1]) : 2; }
#else
int main(int argc, char** argv) { return argc == 2 ? run(argv[1]) : 2; }
#endif
