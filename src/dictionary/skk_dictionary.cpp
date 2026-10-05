#include "nskkzime/dictionary.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace nskkzime {
namespace {
constexpr std::size_t max_file_bytes = 128 * 1024 * 1024;
constexpr std::size_t max_line_chars = 65536;
bool whitespace(char32_t ch) { return ch == U' ' || ch == U'\t'; }
std::u32string_view trim(std::u32string_view text) {
    while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
    return text;
}
bool printable(std::u32string_view text) {
    return std::all_of(text.begin(), text.end(), [](char32_t ch) {
        return ch >= 0x20 && ch != 0x7f && ch != 0xfffd && ch <= 0x10ffff && !(ch >= 0xd800 && ch <= 0xdfff);
    });
}
void issue(DictionaryReport& report, std::size_t line, const char* reason) {
    if (report.diagnostics.size() < 32) report.diagnostics.push_back("line " + std::to_string(line) + ": " + reason);
}

// Interpret only concat of quoted literals. No Lisp code is evaluated.
bool literal(std::u32string_view input, std::u32string& output) {
    output.clear();
    // Parenthesized Japanese abbreviations such as (株) are plain SKK text.
    if (!input.starts_with(U"(") || (input.size() > 1 && input[1] >= 0x80)) {
        output = input; return printable(output);
    }
    if (!input.starts_with(U"(concat") || input.size() < 9 || !whitespace(input[7])) return false;
    std::size_t i = 7;
    bool had_string = false;
    for (;;) {
        while (i < input.size() && whitespace(input[i])) ++i;
        if (i == input.size()) return false;
        if (input[i] == U')') return had_string && i + 1 == input.size() && printable(output);
        if (input[i++] != U'"') return false;
        had_string = true;
        bool closed = false;
        while (i < input.size()) {
            char32_t ch = input[i++];
            if (ch == U'"') { closed = true; break; }
            if (ch == U'\\') {
                if (i == input.size()) return false;
                ch = input[i++];
                if (ch >= U'0' && ch <= U'7') {
                    char32_t code = ch - U'0';
                    for (int n = 1; n < 3 && i < input.size() && input[i] >= U'0' && input[i] <= U'7'; ++n)
                        code = code * 8 + (input[i++] - U'0');
                    ch = code;
                } else if (ch != U'\\' && ch != U'"') return false;
            }
            output += ch;
        }
        if (!closed) return false;
    }
}
std::size_t annotation_position(std::u32string_view text) {
    if (!text.starts_with(U"(")) return text.find(U';');
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (escaped) { escaped = false; continue; }
        if (quoted && ch == U'\\') { escaped = true; continue; }
        if (ch == U'"') quoted = !quoted;
        if (ch == U';' && !quoted) return i;
    }
    return std::u32string_view::npos;
}
bool candidate(std::u32string_view raw, const LookupKey& key, Candidate& out) {
    const auto separator = annotation_position(raw);
    if (!literal(raw.substr(0, separator), out.text) || out.text.empty()) return false;
    if (separator != std::u32string_view::npos && !literal(raw.substr(separator + 1), out.annotation)) {
        // Unsupported annotation expressions are retained as text, never executed.
        out.annotation = raw.substr(separator + 1);
        if (!printable(out.annotation)) return false;
    }
    const auto reading = utf8(key.reading);
    out.id = "skk:" + std::to_string(reading.size()) + ":" + reading + ":" + std::to_string(static_cast<unsigned char>(key.okuri)) + ":" + utf8(out.text);
    return true;
}
std::size_t merge_unique(std::vector<Candidate>& destination, const std::vector<Candidate>& source) {
    std::set<std::u32string> known;
    for (const auto& item : destination) known.insert(item.text);
    std::size_t added = 0;
    for (const auto& item : source) if (known.insert(item.text).second) { destination.push_back(item); ++added; }
    return added;
}
std::u32string decode_euc_jp(std::string_view bytes) {
    // Validate EUC-JP byte structure before invoking the platform converter.
    for (std::size_t i = 0; i < bytes.size();) {
        const auto ch = static_cast<unsigned char>(bytes[i++]);
        if (ch < 0x80) continue;
        // Windows CP20932 does not reliably decode the standard three-byte SS3 form.
        if (ch == 0x8f) throw std::invalid_argument("EUC-JP SS3 (JIS X 0212) is unsupported; convert the dictionary to UTF-8");
        if (ch != 0x8e && (ch < 0xa1 || ch > 0xfe)) throw std::invalid_argument("invalid EUC-JP lead byte");
        if (i == bytes.size()) throw std::invalid_argument("truncated EUC-JP sequence");
        const auto next = static_cast<unsigned char>(bytes[i++]);
        if (next < 0xa1 || next > (ch == 0x8e ? 0xdf : 0xfe)) throw std::invalid_argument("invalid EUC-JP continuation");
    }
#ifdef _WIN32
    if (bytes.empty()) return {};
    constexpr UINT codepage = 20932; // JIS X 0208 and SS2 halfwidth kana; SS3 rejected above.
    const auto length = static_cast<int>(bytes.size());
    const int required = MultiByteToWideChar(codepage, 0, bytes.data(), length, nullptr, 0);
    if (!required) throw std::runtime_error("Windows EUC-JP converter unavailable");
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(codepage, 0, bytes.data(), length, wide.data(), required) != required)
        throw std::runtime_error("EUC-JP decoding failed");
    BOOL substituted = FALSE;
    const int roundtrip_size = WideCharToMultiByte(codepage, 0, wide.data(), required, nullptr, 0, nullptr, &substituted);
    if (roundtrip_size <= 0 || substituted) throw std::invalid_argument("unmappable EUC-JP input");
    std::string roundtrip(static_cast<std::size_t>(roundtrip_size), '\0');
    if (WideCharToMultiByte(codepage, 0, wide.data(), required, roundtrip.data(), roundtrip_size, nullptr, &substituted) != roundtrip_size || substituted || roundtrip != bytes)
        throw std::invalid_argument("EUC-JP input does not round-trip without loss");
    std::u32string text;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        char32_t ch = wide[i];
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (i + 1 == wide.size() || wide[i + 1] < 0xdc00 || wide[i + 1] > 0xdfff) throw std::invalid_argument("invalid decoded surrogate");
            ch = 0x10000 + ((ch - 0xd800) << 10) + (wide[++i] - 0xdc00);
        } else if (ch >= 0xdc00 && ch <= 0xdfff) throw std::invalid_argument("invalid decoded surrogate");
        text += ch;
    }
    return text;
#else
    throw std::runtime_error("EUC-JP import currently requires Windows; convert the file to UTF-8 first");
#endif
}
}

const char* encoding_name(DictionaryEncoding encoding) {
    switch (encoding) {
    case DictionaryEncoding::Auto: return "auto";
    case DictionaryEncoding::Utf8: return "utf-8";
    case DictionaryEncoding::EucJp: return "euc-jp";
    }
    return "unknown";
}
std::shared_ptr<const SkkDictionary> SkkDictionary::load(const std::filesystem::path& path, DictionaryEncoding encoding) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open dictionary file");
    const auto size = input.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(max_file_bytes)) throw std::runtime_error("dictionary exceeds 128 MiB or size cannot be read");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("cannot read dictionary file");
    if (bytes.starts_with("\xef\xbb\xbf")) {
        if (encoding == DictionaryEncoding::EucJp) throw std::invalid_argument("UTF-8 BOM conflicts with EUC-JP encoding");
        encoding = DictionaryEncoding::Utf8; bytes.erase(0, 3);
    }
    if (encoding == DictionaryEncoding::Auto) {
        const auto header = std::string_view(bytes).substr(0, bytes.find('\n'));
        const auto coding = header.find("coding:");
        if (coding != std::string_view::npos && header.starts_with(";;")) {
            auto value = header.substr(coding + 7);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            value = value.substr(0, value.find_first_of(" ;\r\t"));
            if (value == "utf-8" || value == "utf-8-unix") encoding = DictionaryEncoding::Utf8;
            else if (value == "euc-jp" || value == "euc-jp-unix") encoding = DictionaryEncoding::EucJp;
            else throw std::invalid_argument("unsupported dictionary encoding in coding header");
        }
    }
    std::u32string decoded;
    if (encoding == DictionaryEncoding::Auto) {
        try { decoded = from_utf8(bytes); encoding = DictionaryEncoding::Utf8; }
        catch (const std::invalid_argument&) { decoded = decode_euc_jp(bytes); encoding = DictionaryEncoding::EucJp; }
    } else decoded = encoding == DictionaryEncoding::Utf8 ? from_utf8(bytes) : decode_euc_jp(bytes);
    auto dictionary = parse_impl(decoded);
    dictionary->report_.encoding = encoding;
    return dictionary;
}
std::shared_ptr<const SkkDictionary> SkkDictionary::parse(std::u32string_view text) { return parse_impl(text); }

std::shared_ptr<SkkDictionary> SkkDictionary::parse_impl(std::u32string_view text) {
    auto dictionary = std::shared_ptr<SkkDictionary>(new SkkDictionary);
    auto& report = dictionary->report_;
    enum class Section { Unspecified, Ari, Nasi };
    Section section = Section::Unspecified;
    if (text.starts_with(U"\ufeff")) text.remove_prefix(1);
    while (!text.empty()) {
        ++report.lines;
        const auto newline = text.find(U'\n');
        auto line = text.substr(0, newline);
        text = newline == std::u32string_view::npos ? std::u32string_view{} : text.substr(newline + 1);
        if (!line.empty() && line.back() == U'\r') line.remove_suffix(1);
        if (line == U";; okuri-ari entries.") { section = Section::Ari; continue; }
        if (line == U";; okuri-nasi entries.") { section = Section::Nasi; continue; }
        if (line.empty() || line.starts_with(U";")) continue;
        auto invalid = [&] { ++report.malformed_lines; issue(report, report.lines, "malformed dictionary entry"); };
        if (line.size() > max_line_chars) { invalid(); continue; }
        const auto divider = line.find_first_of(U" \t");
        if (divider == std::u32string_view::npos || divider == 0) { invalid(); continue; }
        auto reading = line.substr(0, divider);
        if (!printable(reading)) { invalid(); continue; }
        auto values = trim(line.substr(divider));
        if (values.size() < 2 || values.front() != U'/' || values.back() != U'/') { invalid(); continue; }
        char okuri_letter = 0;
        const bool last_letter = reading.back() >= U'a' && reading.back() <= U'z';
        const bool has_kana = std::any_of(reading.begin(), reading.end(), [](char32_t ch) { return ch >= U'ぁ' && ch <= U'ゖ'; });
        if (section == Section::Ari || (section == Section::Unspecified && last_letter && has_kana)) {
            if (!last_letter || reading.size() < 2) { invalid(); continue; }
            okuri_letter = static_cast<char>(reading.back()); reading.remove_suffix(1);
        }
        const LookupKey key{std::u32string(reading), okuri_letter};
        Entry entry;
        std::u32string block;
        bool broken = false;
        values.remove_prefix(1);
        while (!values.empty()) {
            const auto slash = values.find(U'/');
            if (slash == std::u32string_view::npos) { broken = true; break; }
            const auto raw = values.substr(0, slash);
            values.remove_prefix(slash + 1);
            if (okuri_letter && raw.starts_with(U"[")) {
                if (!block.empty() || raw.size() < 2 || !printable(raw.substr(1))) { broken = true; break; }
                block = raw.substr(1);
            } else if (okuri_letter && raw == U"]") {
                if (block.empty()) { broken = true; break; }
                block.clear();
            } else {
                Candidate item;
                if (!candidate(raw, key, item)) {
                    ++report.unsupported_candidates;
                    issue(report, report.lines, "unsupported or empty candidate");
                    continue;
                }
                auto& destination = block.empty() ? entry.candidates : entry.okuri[block];
                destination.push_back(std::move(item));
            }
        }
        if (broken || !block.empty()) { invalid(); continue; }
        if (entry.candidates.empty() && entry.okuri.empty()) continue;
        auto& destination = dictionary->entries_[key];
        report.candidates += merge_unique(destination.candidates, entry.candidates);
        for (const auto& [okuri, candidates] : entry.okuri) report.candidates += merge_unique(destination.okuri[okuri], candidates);
    }
    report.entries = dictionary->entries_.size();
    return dictionary;
}
std::vector<LearningEvent> SkkDictionary::entries() const {
    std::vector<LearningEvent> result;
    result.reserve(report_.candidates);
    for (const auto& [key, entry] : entries_) {
        for (const auto& value : entry.candidates) result.push_back({key,{},value});
        for (const auto& [okuri, values] : entry.okuri)
            for (const auto& value : values) result.push_back({key,okuri,value});
    }
    return result;
}
std::vector<Candidate> SkkDictionary::lookup(const LookupKey& key, std::u32string_view okuri) const {
    const auto found = entries_.find(key);
    if (found == entries_.end()) return {};
    std::vector<Candidate> result;
    if (const auto block = found->second.okuri.find(okuri); block != found->second.okuri.end()) merge_unique(result, block->second);
    merge_unique(result, found->second.candidates);
    return result;
}
DictionarySet::DictionarySet(std::vector<std::shared_ptr<const Dictionary>> dictionaries) : dictionaries_(std::move(dictionaries)) {
    for (const auto& dictionary : dictionaries_) if (!dictionary) throw std::invalid_argument("null dictionary in DictionarySet");
}
std::vector<Candidate> DictionarySet::lookup(const LookupKey& key, std::u32string_view okuri) const {
    std::vector<Candidate> result;
    for (const auto& dictionary : dictionaries_) merge_unique(result, dictionary->lookup(key, okuri));
    return result;
}
} // namespace nskkzime
