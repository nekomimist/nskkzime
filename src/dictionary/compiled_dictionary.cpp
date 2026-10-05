#include "nskkzime/compiled_dictionary.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace nskkzime {
namespace {
// Header: magic[8], byte size, scope/candidate/diagnostic counts, string offset,
// encoding, source lines/entries/malformed/unsupported counts (all u32 LE).
// Scope[32]: reading offset/length, okuri letter, suffix offset/length,
// first candidate, candidate count, zero reserved word. Sorted by key/suffix.
// Candidate[16]: spelling offset/length, annotation offset/length.
// Diagnostic[8]: UTF-8 offset/byte length. Other lengths count UTF-32 scalars.
constexpr std::size_t header = 48, scope_size = 32, candidate_size = 16;
void need(bool ok) {
    if (!ok)
        throw std::invalid_argument("invalid compiled dictionary");
}
void put(std::string &bytes, std::size_t offset, std::size_t value) {
    need(value <= UINT32_MAX && offset <= bytes.size() && bytes.size() - offset >= 4);
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<char>((value >> (i * 8)) & 0xff);
}
void room(std::size_t existing, std::size_t extra) {
    need(existing <= compiled_dictionary_limit && extra <= compiled_dictionary_limit - existing);
}
} // namespace
std::uint32_t CompiledDictionary::word(std::size_t offset) const {
    need(offset <= bytes_.size() && bytes_.size() - offset >= 4);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset + i])) << (i * 8);
    return value;
}
std::u32string CompiledDictionary::text(std::size_t offset, std::size_t length) const {
    std::u32string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i)
        result += static_cast<char32_t>(word(offset + i * 4));
    return result;
}
int CompiledDictionary::compare(std::size_t offset, std::size_t length, std::u32string_view value) const {
    for (std::size_t i = 0; i < (std::min)(length, value.size()); ++i) {
        const auto ch = word(offset + i * 4);
        if (ch != value[i])
            return ch < value[i] ? -1 : 1;
    }
    return length == value.size() ? 0 : length < value.size() ? -1 : 1;
}
CompiledDictionary::CompiledDictionary(std::string_view bytes, std::shared_ptr<const void> owner)
    : owner_(std::move(owner)), bytes_(bytes) {
    need(owner_ && bytes.size() >= header && bytes.size() <= compiled_dictionary_limit &&
         bytes.substr(0, 8) == "NSKKDC01" && word(8) == bytes.size());
    scopes_ = word(12);
    candidates_ = word(16);
    const auto diagnostics = word(20);
    strings_ = word(24);
    need(scopes_ <= bytes.size() / scope_size && candidates_ <= bytes.size() / candidate_size && diagnostics <= 32);
    candidate_offset_ = header + scopes_ * scope_size;
    const auto diagnostic_offset = candidate_offset_ + candidates_ * candidate_size;
    need(strings_ == diagnostic_offset + diagnostics * 8 && strings_ <= bytes.size());
    const auto encoding = word(28);
    need(encoding == static_cast<unsigned>(DictionaryEncoding::Utf8) ||
         encoding == static_cast<unsigned>(DictionaryEncoding::EucJp));
    report_.encoding = static_cast<DictionaryEncoding>(encoding);
    report_.lines = word(32);
    report_.entries = word(36);
    report_.candidates = candidates_;
    report_.malformed_lines = word(40);
    report_.unsupported_candidates = word(44);
    auto valid_text = [&](std::size_t offset, std::size_t length, bool empty) {
        need((empty || length) && length <= 65536 && offset >= strings_ && offset <= bytes.size() && offset % 4 == 0 &&
             length <= (bytes.size() - offset) / 4);
        for (std::size_t i = 0; i < length; ++i) {
            const auto ch = word(offset + i * 4);
            need(ch >= 0x20 && ch != 0x7f && ch != 0xfffd && ch <= 0x10ffff && !(ch >= 0xd800 && ch <= 0xdfff));
        }
    };
    std::size_t next_candidate = 0, entries = 0;
    LookupKey previous;
    std::u32string previous_okuri;
    for (std::size_t i = 0; i < scopes_; ++i) {
        const auto offset = header + i * scope_size;
        valid_text(word(offset), word(offset + 4), false);
        valid_text(word(offset + 12), word(offset + 16), true);
        const auto okuri = word(offset + 8);
        need(!okuri || (okuri >= 'a' && okuri <= 'z'));
        need(okuri || !word(offset + 16));
        LookupKey current{text(word(offset), word(offset + 4)), static_cast<char>(okuri)};
        auto suffix = text(word(offset + 12), word(offset + 16));
        if (i)
            need(previous < current || (previous == current && previous_okuri < suffix));
        if (!i || previous != current)
            ++entries;
        previous = std::move(current);
        previous_okuri = std::move(suffix);
        const auto first = word(offset + 20), count = word(offset + 24);
        need(count && first == next_candidate && first <= candidates_ && count <= candidates_ - first &&
             word(offset + 28) == 0);
        next_candidate += count;
    }
    need(next_candidate == candidates_ && entries == report_.entries && report_.malformed_lines <= report_.lines);
    for (std::size_t i = 0; i < candidates_; ++i) {
        const auto offset = candidate_offset_ + i * candidate_size;
        valid_text(word(offset), word(offset + 4), false);
        valid_text(word(offset + 8), word(offset + 12), true);
    }
    for (std::size_t i = 0; i < diagnostics; ++i) {
        const auto offset = word(diagnostic_offset + i * 8), length = word(diagnostic_offset + i * 8 + 4);
        need(offset >= strings_ && offset <= bytes.size() && length <= 1024 && length <= bytes.size() - offset);
        const auto value = bytes.substr(offset, length);
        const auto decoded = from_utf8(value);
        need(std::all_of(decoded.begin(), decoded.end(), [](char32_t ch) { return ch >= 0x20 && ch != 0x7f; }));
        report_.diagnostics.emplace_back(value);
    }
}
std::string CompiledDictionary::compile(const SkkDictionary &source) {
    std::size_t scopes = 0, candidates = 0;
    source.visit_scopes([&](const auto &, auto, const auto &values) {
        ++scopes;
        candidates += values.size();
    });
    const auto &report = source.report();
    need(candidates == report.candidates && report.diagnostics.size() <= 32);
    const auto strings = header + scopes * scope_size + candidates * candidate_size + report.diagnostics.size() * 8;
    room(0, strings);
    std::string bytes(strings, '\0');
    bytes.replace(0, 8, "NSKKDC01");
    put(bytes, 12, scopes);
    put(bytes, 16, candidates);
    put(bytes, 20, report.diagnostics.size());
    put(bytes, 24, strings);
    put(bytes, 28, static_cast<unsigned>(report.encoding));
    put(bytes, 32, report.lines);
    put(bytes, 36, report.entries);
    put(bytes, 40, report.malformed_lines);
    put(bytes, 44, report.unsupported_candidates);
    auto add_text = [&](std::size_t position, std::u32string_view value) {
        room(bytes.size(), value.size() * 4);
        put(bytes, position, bytes.size());
        put(bytes, position + 4, value.size());
        const auto start = bytes.size();
        bytes.resize(start + value.size() * 4);
        for (std::size_t i = 0; i < value.size(); ++i)
            put(bytes, start + i * 4, value[i]);
    };
    std::size_t scope_index = 0, candidate_index = 0;
    source.visit_scopes([&](const LookupKey &key, std::u32string_view okuri, const auto &values) {
        const auto offset = header + scope_index++ * scope_size;
        add_text(offset, key.reading);
        put(bytes, offset + 8, static_cast<unsigned char>(key.okuri));
        add_text(offset + 12, okuri);
        put(bytes, offset + 20, candidate_index);
        put(bytes, offset + 24, values.size());
        for (const auto &value : values) {
            const auto position = header + scopes * scope_size + candidate_index++ * candidate_size;
            add_text(position, value.text);
            add_text(position + 8, value.annotation);
        }
    });
    for (std::size_t i = 0; i < report.diagnostics.size(); ++i) {
        const auto &value = report.diagnostics[i];
        room(bytes.size(), value.size());
        const auto position = header + scopes * scope_size + candidates * candidate_size + i * 8;
        put(bytes, position, bytes.size());
        put(bytes, position + 4, value.size());
        bytes += value;
    }
    put(bytes, 8, bytes.size());
    return bytes;
}
std::size_t CompiledDictionary::find(const LookupKey &key, std::u32string_view okuri) const {
    std::size_t low = 0, high = scopes_;
    auto order = [&](std::size_t i) {
        const auto offset = header + i * scope_size;
        if (const auto reading = compare(word(offset), word(offset + 4), key.reading))
            return reading;
        const auto letter = word(offset + 8);
        const auto wanted = static_cast<unsigned char>(key.okuri);
        if (letter != wanted)
            return letter < wanted ? -1 : 1;
        return compare(word(offset + 12), word(offset + 16), okuri);
    };
    while (low < high) {
        const auto mid = low + (high - low) / 2;
        if (order(mid) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low < scopes_ && !order(low) ? low : scopes_;
}
std::vector<Candidate> CompiledDictionary::lookup(const LookupKey &key, std::u32string_view okuri) const {
    std::vector<Candidate> result;
    std::set<std::u32string> known;
    auto append = [&](std::size_t scope) {
        if (scope == scopes_)
            return;
        const auto offset = header + scope * scope_size;
        const auto first = word(offset + 20), count = word(offset + 24);
        for (std::size_t i = first; i < first + count; ++i) {
            const auto position = candidate_offset_ + i * candidate_size;
            auto spelling = text(word(position), word(position + 4));
            if (!known.insert(spelling).second)
                continue;
            result.push_back(
                {skk_candidate_id(key, spelling), std::move(spelling), text(word(position + 8), word(position + 12))});
        }
    };
    if (!okuri.empty())
        append(find(key, okuri));
    append(find(key, {}));
    return result;
}
} // namespace nskkzime
