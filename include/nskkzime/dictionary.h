#pragma once
#include "engine.h"
#include <filesystem>
#include <functional>
#include <string_view>

namespace nskkzime {
enum class DictionaryEncoding { Auto, Utf8, EucJp };
const char* encoding_name(DictionaryEncoding encoding);
struct DictionaryReport {
    DictionaryEncoding encoding = DictionaryEncoding::Utf8;
    std::size_t lines = 0;
    std::size_t entries = 0;
    std::size_t candidates = 0; // Accepted unique entries, including okuri blocks.
    std::size_t malformed_lines = 0;
    std::size_t unsupported_candidates = 0;
    std::vector<std::string> diagnostics; // First 32 issues only; no input text.
};

// Immutable after construction; all lookups use memory, not the filesystem.
class SkkDictionary final : public Dictionary {
public:
    static std::shared_ptr<const SkkDictionary> load(const std::filesystem::path& path,
        DictionaryEncoding encoding = DictionaryEncoding::Auto);
    static std::shared_ptr<const SkkDictionary> parse(std::u32string_view text);
    static std::shared_ptr<const SkkDictionary> parse_bytes(std::string_view bytes,
        DictionaryEncoding encoding = DictionaryEncoding::Auto);
    std::vector<Candidate> lookup(const LookupKey& key, std::u32string_view okuri = {}) const override;
    // Unmerged scopes: generic candidates have empty okuri, blocks retain theirs.
    std::vector<LearningEvent> entries() const;
    void visit_scopes(const std::function<void(const LookupKey&, std::u32string_view, const std::vector<Candidate>&)>&) const;
    const DictionaryReport& report() const { return report_; }
private:
    struct Entry {
        std::vector<Candidate> candidates;
        std::map<std::u32string, std::vector<Candidate>, std::less<>> okuri;
    };
    static std::shared_ptr<SkkDictionary> parse_impl(std::u32string_view text);
    std::map<LookupKey, Entry> entries_;
    DictionaryReport report_;
};
std::string skk_candidate_id(const LookupKey&, std::u32string_view text);

// Dictionary order is priority order; the first spelling (and annotation) wins.
class DictionarySet final : public Dictionary {
public:
    explicit DictionarySet(std::vector<std::shared_ptr<const Dictionary>> dictionaries);
    std::vector<Candidate> lookup(const LookupKey& key, std::u32string_view okuri = {}) const override;
private:
    std::vector<std::shared_ptr<const Dictionary>> dictionaries_;
};
} // namespace nskkzime
