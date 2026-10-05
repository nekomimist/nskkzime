#pragma once
#include "dictionary.h"
namespace nskkzime {
inline constexpr std::size_t compiled_dictionary_limit = 512 * 1024 * 1024;
// Portable little-endian image containing offsets, never process pointers.
// The caller owns and keeps the bytes immutable throughout this object's life.
class CompiledDictionary final : public Dictionary {
  public:
    CompiledDictionary(std::string_view bytes, std::shared_ptr<const void> owner);
    static std::string compile(const SkkDictionary &);
    std::vector<Candidate> lookup(const LookupKey &, std::u32string_view okuri = {}) const override;
    const DictionaryReport &report() const { return report_; }

  private:
    std::uint32_t word(std::size_t offset) const;
    std::u32string text(std::size_t offset, std::size_t length) const;
    int compare(std::size_t offset, std::size_t length, std::u32string_view value) const;
    std::size_t find(const LookupKey &, std::u32string_view okuri) const;
    std::shared_ptr<const void> owner_;
    std::string_view bytes_;
    DictionaryReport report_;
    std::size_t scopes_ = 0, candidates_ = 0, candidate_offset_ = 0, strings_ = 0;
};
} // namespace nskkzime
