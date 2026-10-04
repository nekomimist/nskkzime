#pragma once
#include "romaji.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nskkzime {
enum class Mode { Hiragana, Katakana, Latin };
enum class Stage { Direct, Reading, Okuri, Selecting };
enum class KeyCode { Character, Space, Enter, Backspace, Cancel, Hiragana };
struct Key { KeyCode code = KeyCode::Character; char character = 0; };
enum class CandidateSource { Dictionary, GeneratedLong, GeneratedFallback };
enum class LearningPolicy { Learn, DoNotLearn };
struct Candidate {
    std::string id;
    std::u32string text;
    std::u32string annotation;
    CandidateSource source = CandidateSource::Dictionary;
    LearningPolicy learning = LearningPolicy::Learn;
};
struct LookupKey {
    std::u32string reading;
    char okuri = 0;
    auto operator<=>(const LookupKey&) const = default;
};
class Dictionary {
public:
    virtual ~Dictionary() = default;
    virtual std::vector<Candidate> lookup(const LookupKey& key, std::u32string_view okuri = {}) const = 0;
};
class MemoryDictionary final : public Dictionary {
public:
    void add(LookupKey key, std::vector<Candidate> candidates);
    std::vector<Candidate> lookup(const LookupKey& key, std::u32string_view okuri = {}) const override;
private:
    std::map<LookupKey, std::vector<Candidate>> entries_;
};
// Explicit fixture for the first vertical slice, NOT a production SKK dictionary.
std::shared_ptr<const Dictionary> demo_dictionary();

struct Result {
    bool consumed = false;
    std::u32string committed;
    bool dictionary_miss = false;
    bool candidates_exhausted = false;
};

class Engine {
public:
    explicit Engine(std::shared_ptr<const Dictionary> dictionary);
    bool would_handle(Key key) const;
    Result handle(Key key);
    void reset(); // Clear composition, retain input mode.
    Mode mode() const { return mode_; }
    Stage stage() const { return stage_; }
    std::uint64_t revision() const { return revision_; }
    std::u32string preedit() const;
    std::u32string literal_text() const; // No SKK markers; used on host termination.
    const std::vector<Candidate>& candidates() const { return candidates_; }
    std::size_t candidate_index() const { return candidate_index_; }
private:
    void clear_composition();
    void append(std::u32string text, Result& result);
    void convert(Result& result);
    void confirm(Result& result);
    std::u32string display(std::u32string_view text) const;
    std::shared_ptr<const Dictionary> dictionary_;
    Mode mode_ = Mode::Hiragana;
    Stage stage_ = Stage::Direct;
    Romaji romaji_;
    std::u32string reading_;
    std::u32string okuri_;
    char okuri_letter_ = 0;
    std::vector<Candidate> candidates_;
    std::size_t candidate_index_ = 0;
    std::uint64_t revision_ = 0;
};
} // namespace nskkzime
