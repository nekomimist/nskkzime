#include "nskkzime/learning.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace nskkzime {
namespace {
constexpr std::size_t max_entries = 50000, max_clients = 65536, max_candidates = 32;
void check(bool ok) { if (!ok) throw std::invalid_argument("invalid or oversized user dictionary data"); }
void text_check(std::u32string_view text, bool empty = false) {
    check((empty || !text.empty()) && text.size() <= 1024);
    for (const auto ch : text) check(ch >= 32 && ch != 127 && ch <= 0x10ffff && !(ch >= 0xd800 && ch <= 0xdfff));
}
void event_check(const LearningEvent& event) {
    text_check(event.key.reading); text_check(event.okuri, true);
    text_check(event.candidate.text); text_check(event.candidate.annotation, true);
    check((event.key.okuri == 0 && event.okuri.empty()) || (event.key.okuri >= 'a' && event.key.okuri <= 'z' && !event.okuri.empty()));
    check(event.candidate.learning == LearningPolicy::Learn &&
        (event.candidate.source == CandidateSource::Dictionary || event.candidate.source == CandidateSource::GeneratedFallback));
}
void client_check(std::string_view client) {
    check(client.size() == 32 && std::all_of(client.begin(), client.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }));
}
struct Writer {
    std::string bytes;
    void number(std::uint64_t value) { for (int i = 0; i < 8; ++i) { bytes += static_cast<char>(value & 255); value >>= 8; } }
    void string(std::string_view value) { number(value.size()); bytes += value; check(bytes.size() <= learning_max_bytes); }
    void text(std::u32string_view value) { string(utf8(value)); }
    void event(const LearningEvent& e) {
        text(e.key.reading); number(static_cast<unsigned char>(e.key.okuri)); text(e.okuri);
        text(e.candidate.text); text(e.candidate.annotation); number(static_cast<std::uint64_t>(e.candidate.source));
    }
};
struct Reader {
    std::string_view bytes;
    std::uint64_t number() {
        check(bytes.size() >= 8); std::uint64_t result = 0;
        for (int i = 0; i < 8; ++i) result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)])) << (8 * i);
        bytes.remove_prefix(8); return result;
    }
    std::string string(std::size_t limit = 4096) {
        const auto size = number(); check(size <= limit && size <= bytes.size());
        std::string result(bytes.substr(0, static_cast<std::size_t>(size))); bytes.remove_prefix(static_cast<std::size_t>(size)); return result;
    }
    LearningEvent event() {
        LearningEvent e; e.key.reading = from_utf8(string()); const auto letter = number(); check(letter <= 127);
        e.key.okuri = static_cast<char>(letter); e.okuri = from_utf8(string());
        e.candidate.text = from_utf8(string()); e.candidate.annotation = from_utf8(string());
        const auto source = number(); check(source <= 2); e.candidate.source = static_cast<CandidateSource>(source);
        e.candidate.id = "user:" + utf8(e.key.reading) + ":" + std::to_string(letter) + ":" + utf8(e.candidate.text);
        event_check(e); return e;
    }
};
std::uint32_t checksum(std::string_view bytes) {
    std::uint32_t value = 0xffffffff;
    for (const auto byte : bytes) {
        value ^= static_cast<unsigned char>(byte);
        for (int i = 0; i < 8; ++i) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1)));
    }
    return ~value;
}
}
void validate_learning(const LearningRequest& request) { client_check(request.client); check(request.sequence != 0); event_check(request.event); }
std::string encode_learning(const LearningRequest& request) {
    validate_learning(request); Writer w; w.bytes = "NKL1"; w.string(request.client); w.number(request.sequence); w.event(request.event); return w.bytes;
}
LearningRequest decode_learning(std::string_view bytes) {
    check(bytes.size() <= 20000 && bytes.starts_with("NKL1")); Reader r{bytes.substr(4)};
    LearningRequest request; request.client = r.string(32); request.sequence = r.number(); request.event = r.event();
    check(r.bytes.empty()); validate_learning(request); return request;
}
bool UserDictionary::apply(const LearningRequest& request) {
    validate_learning(request);
    const auto previous = clients_.find(request.client);
    if (previous != clients_.end() && previous->second >= request.sequence) return false;
    check(previous != clients_.end() || clients_.size() < max_clients);
    Key key{request.event.key, request.event.okuri};
    check(entries_.contains(key) || entries_.size() < max_entries);
    check(revision_ != std::numeric_limits<std::uint64_t>::max());
    auto& candidates = entries_[std::move(key)];
    std::erase_if(candidates, [&](const Candidate& candidate) { return candidate.text == request.event.candidate.text; });
    candidates.insert(candidates.begin(), request.event.candidate);
    if (candidates.size() > max_candidates) candidates.resize(max_candidates);
    clients_[request.client] = request.sequence; ++revision_; return true;
}
std::vector<Candidate> UserDictionary::lookup(const LookupKey& key, std::u32string_view okuri) const {
    const auto found = entries_.find({key, std::u32string(okuri)});
    return found == entries_.end() ? std::vector<Candidate>{} : found->second;
}
std::string UserDictionary::serialize() const {
    Writer w; w.bytes = "NKUD1"; w.number(revision_); w.number(clients_.size());
    for (const auto& [client, sequence] : clients_) { w.string(client); w.number(sequence); }
    w.number(entries_.size());
    for (const auto& [key, candidates] : entries_) {
        w.number(candidates.size());
        for (const auto& candidate : candidates) w.event({key.lookup, key.okuri, candidate});
    }
    w.number(checksum(w.bytes)); check(w.bytes.size() <= learning_max_bytes); return w.bytes;
}
UserDictionary UserDictionary::parse(std::string_view bytes) {
    check(bytes.size() >= 37 && bytes.size() <= learning_max_bytes && bytes.starts_with("NKUD1"));
    Reader trailer{bytes.substr(bytes.size() - 8)}; check(trailer.number() == checksum(bytes.substr(0, bytes.size() - 8)));
    Reader r{bytes.substr(5, bytes.size() - 13)}; UserDictionary result; result.revision_ = r.number();
    const auto clients = r.number(); check(clients <= max_clients);
    for (std::uint64_t i = 0; i < clients; ++i) {
        const auto id = r.string(32); client_check(id); const auto sequence = r.number(); check(sequence != 0);
        check(result.clients_.emplace(id, sequence).second);
    }
    const auto entries = r.number(); check(entries <= max_entries);
    for (std::uint64_t i = 0; i < entries; ++i) {
        const auto count = r.number(); check(count > 0 && count <= max_candidates);
        auto e = r.event(); Key key{e.key, e.okuri}; check(!result.entries_.contains(key));
        auto& candidates = result.entries_[key]; candidates.push_back(std::move(e.candidate));
        for (std::uint64_t j = 1; j < count; ++j) {
            e = r.event(); check(e.key == key.lookup && e.okuri == key.okuri);
            check(std::none_of(candidates.begin(), candidates.end(), [&](const Candidate& c) { return c.text == e.candidate.text; }));
            candidates.push_back(std::move(e.candidate));
        }
    }
    check(r.bytes.empty()); return result;
}
} // namespace nskkzime
