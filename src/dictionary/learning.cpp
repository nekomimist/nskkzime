#include "nskkzime/learning.h"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace nskkzime {
namespace {
// Older files may hold up to max_clients IDs; writes keep only retained_clients.
constexpr std::size_t max_entries = 50000, max_clients = 65536, retained_clients = 1024, max_candidates = 32;
void check(bool ok) { if (!ok) throw std::invalid_argument("invalid or oversized user dictionary data"); }
void text_check(std::u32string_view text, bool empty = false) {
    check((empty || !text.empty()) && text.size() <= 1024);
    for (const auto ch : text) check(ch >= 32 && ch != 127 && ch <= 0x10ffff && !(ch >= 0xd800 && ch <= 0xdfff));
}
void event_check(const LearningEvent& event, bool generic_okuri = false) {
    check(!event.candidate.origin); // Engine must publish the raw identity, never a display expansion.
    text_check(event.key.reading); text_check(event.okuri, true);
    text_check(event.candidate.text); text_check(event.candidate.annotation, true);
    check((event.key.okuri == 0 && event.okuri.empty()) || (event.key.okuri >= 'a' && event.key.okuri <= 'z' && (generic_okuri || !event.okuri.empty())));
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
    LearningEvent event(bool generic_okuri = false) {
        LearningEvent e; e.key.reading = from_utf8(string()); const auto letter = number(); check(letter <= 127);
        e.key.okuri = static_cast<char>(letter); e.okuri = from_utf8(string());
        e.candidate.text = from_utf8(string()); e.candidate.annotation = from_utf8(string());
        const auto source = number(); check(source <= 2); e.candidate.source = static_cast<CandidateSource>(source);
        e.candidate.id = "user:" + utf8(e.key.reading) + ":" + std::to_string(letter) + ":" + utf8(e.candidate.text);
        event_check(e,generic_okuri); return e;
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
void validate_learning(const LearningRequest& request) {
    client_check(request.client); check(request.sequence != 0); event_check(request.event,request.action == DictionaryAction::Forget);
    check(request.action == DictionaryAction::Learn || request.action == DictionaryAction::Forget);
}
std::string encode_learning(const LearningRequest& request) {
    validate_learning(request); Writer w; w.bytes = request.action == DictionaryAction::Learn ? "NKL1" : "NKF1";
    w.string(request.client); w.number(request.sequence); w.event(request.event); return w.bytes;
}
LearningRequest decode_learning(std::string_view bytes) {
    check(bytes.size() <= 20000 && (bytes.starts_with("NKL1") || bytes.starts_with("NKF1"))); Reader r{bytes.substr(4)};
    LearningRequest request; request.client = r.string(32); request.sequence = r.number(); request.event = r.event(bytes.starts_with("NKF1"));
    request.action = bytes.starts_with("NKF1") ? DictionaryAction::Forget : DictionaryAction::Learn;
    check(r.bytes.empty()); validate_learning(request); return request;
}
void validate_import(const ImportRequest& request) {
    client_check(request.client); check(request.sequence != 0);
    check(!request.entries.empty() && request.entries.size() <= import_max_candidates);
    for (const auto& entry : request.entries) {
        event_check(entry,true); check(entry.candidate.source == CandidateSource::Dictionary);
    }
}
namespace {
template<class Clients> auto find_client(Clients& clients, std::string_view client) {
    return std::find_if(clients.begin(), clients.end(), [&](const auto& item) { return item.first == client; });
}
}
void UserDictionary::acknowledge(const std::string& client, std::uint64_t sequence) {
    if (const auto found = find_client(clients_, client); found != clients_.end()) clients_.erase(found);
    clients_.emplace_back(client, sequence);
    if (clients_.size() > retained_clients)
        clients_.erase(clients_.begin(), clients_.begin() + static_cast<std::ptrdiff_t>(clients_.size() - retained_clients));
}
bool UserDictionary::apply(const ImportRequest& request) {
    validate_import(request);
    const auto previous = find_client(clients_, request.client);
    if (previous != clients_.end() && previous->second >= request.sequence) return false;
    check(revision_ != std::numeric_limits<std::uint64_t>::max());
    auto next = *this;
    for (const auto& entry : request.entries) {
        Key key{entry.key,entry.okuri};
        check(next.entries_.contains(key) || next.entries_.size() < max_entries);
        auto& candidates = next.entries_[std::move(key)];
        if (std::any_of(candidates.begin(),candidates.end(),[&](const Candidate& old) { return old.text == entry.candidate.text; })) continue;
        check(candidates.size() < max_candidates); candidates.push_back(entry.candidate);
    }
    next.acknowledge(request.client, request.sequence); ++next.revision_;
    (void)next.serialize(); // Validate the complete transaction before changing this instance.
    *this = std::move(next); return true;
}
bool UserDictionary::apply(const LearningRequest& request) {
    validate_learning(request);
    const auto previous = find_client(clients_, request.client);
    if (previous != clients_.end() && previous->second >= request.sequence) return false;
    Key key{request.event.key, request.event.okuri};
    check(request.action == DictionaryAction::Forget || entries_.contains(key) || entries_.size() < max_entries);
    check(revision_ != std::numeric_limits<std::uint64_t>::max());
    if (request.action == DictionaryAction::Forget) {
        if (const auto found = entries_.find(key); found != entries_.end()) {
            std::erase_if(found->second, [&](const Candidate& candidate) { return candidate.text == request.event.candidate.text; });
            if (found->second.empty()) entries_.erase(found);
        }
    } else {
        auto& candidates = entries_[std::move(key)];
        std::erase_if(candidates, [&](const Candidate& candidate) { return candidate.text == request.event.candidate.text; });
        candidates.insert(candidates.begin(), request.event.candidate);
        if (candidates.size() > max_candidates) candidates.resize(max_candidates);
    }
    acknowledge(request.client, request.sequence); ++revision_; return true;
}
std::vector<Candidate> UserDictionary::lookup_scope(const LookupKey& key, std::u32string_view okuri) const {
    const auto found = entries_.find({key, std::u32string(okuri)});
    return found == entries_.end() ? std::vector<Candidate>{} : found->second;
}
std::optional<LearningEvent> UserDictionary::forget_target(const LookupKey& key, std::u32string_view okuri, std::u32string_view text) const {
    for (const auto& candidate : lookup_scope(key, okuri))
        if (candidate.text == text) return LearningEvent{key, std::u32string(okuri), candidate};
    if (key.okuri && !okuri.empty()) return forget_target(key, {}, text);
    return {};
}
std::vector<Candidate> UserDictionary::lookup(const LookupKey& key, std::u32string_view okuri) const {
    auto result = lookup_scope(key, okuri);
    if (key.okuri && !okuri.empty()) {
        if (const auto generic = entries_.find({key,{}}); generic != entries_.end())
            for (const auto& candidate : generic->second)
                if (std::none_of(result.begin(),result.end(),[&](const Candidate& old) { return old.text == candidate.text; })) result.push_back(candidate);
    }
    return result;
}
std::vector<LearningEvent> UserDictionary::entries() const {
    std::vector<LearningEvent> result;
    for (const auto& [key, candidates] : entries_)
        for (const auto& candidate : candidates) result.push_back({key.lookup, key.okuri, candidate});
    return result;
}
std::string UserDictionary::serialize() const {
    // Old readers must reject scopes they cannot interpret, rather than erase
    // or silently broaden them. Exact-only dictionaries remain version 1.
    const auto generic = std::any_of(entries_.begin(),entries_.end(),[](const auto& entry) { return entry.first.lookup.okuri && entry.first.okuri.empty(); });
    Writer w; w.bytes = generic ? "NKUD2" : "NKUD1"; w.number(revision_); w.number(clients_.size());
    for (const auto& [client, sequence] : clients_) { w.string(client); w.number(sequence); }
    w.number(entries_.size());
    for (const auto& [key, candidates] : entries_) {
        w.number(candidates.size());
        for (const auto& candidate : candidates) w.event({key.lookup, key.okuri, candidate});
    }
    w.number(checksum(w.bytes)); check(w.bytes.size() <= learning_max_bytes); return w.bytes;
}
UserDictionary UserDictionary::parse(std::string_view bytes) {
    check(bytes.size() >= 37 && bytes.size() <= learning_max_bytes && (bytes.starts_with("NKUD1") || bytes.starts_with("NKUD2")));
    const bool generic_okuri = bytes.starts_with("NKUD2");
    Reader trailer{bytes.substr(bytes.size() - 8)}; check(trailer.number() == checksum(bytes.substr(0, bytes.size() - 8)));
    Reader r{bytes.substr(5, bytes.size() - 13)}; UserDictionary result; result.revision_ = r.number();
    const auto clients = r.number(); check(clients <= max_clients);
    std::set<std::string> seen;
    for (std::uint64_t i = 0; i < clients; ++i) {
        auto id = r.string(32); client_check(id); const auto sequence = r.number(); check(sequence != 0);
        check(seen.insert(id).second); result.clients_.emplace_back(std::move(id), sequence);
    }
    const auto entries = r.number(); check(entries <= max_entries);
    for (std::uint64_t i = 0; i < entries; ++i) {
        const auto count = r.number(); check(count > 0 && count <= max_candidates);
        auto e = r.event(generic_okuri); Key key{e.key, e.okuri}; check(!result.entries_.contains(key));
        auto& candidates = result.entries_[key]; candidates.push_back(std::move(e.candidate));
        for (std::uint64_t j = 1; j < count; ++j) {
            e = r.event(generic_okuri); check(e.key == key.lookup && e.okuri == key.okuri);
            check(std::none_of(candidates.begin(), candidates.end(), [&](const Candidate& c) { return c.text == e.candidate.text; }));
            candidates.push_back(std::move(e.candidate));
        }
    }
    check(r.bytes.empty()); return result;
}
} // namespace nskkzime
