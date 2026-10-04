#pragma once
#include "romaji.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nskkzime {
enum class Mode { Hiragana, Katakana, Latin };
enum class Stage { Direct, Reading, Okuri, Selecting, Registering };
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

struct LearningEvent {
    LookupKey key;
    std::u32string okuri; // Hiragana, separate from the candidate's stem.
    Candidate candidate;
};
struct Result {
    bool consumed = false;
    std::u32string committed;
    bool dictionary_miss = false;
    bool candidates_exhausted = false;
    bool registration = false; // learning is an explicitly registered word (possibly inside another registration).
    bool registration_limit = false;
    // A proposed event, not a dictionary mutation. Hosts may publish it only
    // after successfully applying this Result's text and adopting the Engine.
    // DoNotLearn candidates and long generations never produce this event.
    std::optional<LearningEvent> learning;
};

class Engine {
public:
    explicit Engine(std::shared_ptr<const Dictionary> dictionary, bool registration_enabled = true);
    bool would_handle(Key key) const;
    Result handle(Key key);
    void reset(); // Clear composition, retain input mode.
    Mode mode() const { return input_.mode; }
    Stage stage() const { return registering() ? Stage::Registering : input_.stage; }
    Stage input_stage() const { return input_.stage; }
    bool abbrev() const { return input_.abbrev; }
    bool registering() const { return !registrations_.empty(); }
    std::size_t registration_depth() const { return registrations_.size(); }
    std::u32string registration_reading() const;
    std::u32string registration_text() const;
    bool registration_limit() const { return registration_limit_; }
    // Host calls this only after the edit succeeds and permits learning. This
    // bounded, private cache makes a new word usable before asynchronous saving.
    void remember_registration(const LearningEvent&) noexcept;
    void abandon_registration(); // Host cannot present the editor: restore the outer reading, without a learning event.
    std::uint64_t revision() const { return revision_; }
    std::u32string preedit() const;
    std::u32string composition_text() const; // Native composition, without diagnostic SKK markers.
    std::u32string literal_text() const; // No SKK markers; used on host termination.
    const std::vector<Candidate>& candidates() const { return input_.candidates; }
    std::u32string candidate_text(std::size_t index) const; // Includes the current okuri; empty for an invalid index.
    std::size_t candidate_index() const { return input_.candidate_index; }
private:
    struct Input {
        Mode mode = Mode::Hiragana;
        Stage stage = Stage::Direct;
        bool abbrev = false;
        Romaji romaji;
        std::u32string reading, okuri;
        char okuri_letter = 0;
        std::vector<Candidate> candidates;
        std::size_t candidate_index = 0;
    };
    struct Registration { Input original; std::u32string draft, anchor; };
    Result handle_input(Key);
    Result handle_registration(Key);
    void begin_registration(Result&);
    void cancel_registration();
    std::u32string input_literal() const;
    std::u32string input_preedit() const;
    void clear_composition();
    void append(std::u32string text, Result& result);
    void convert(Result& result);
    void confirm(Result& result);
    std::u32string display(std::u32string_view text) const;
    std::shared_ptr<const Dictionary> dictionary_;
    Input input_;
    std::vector<Registration> registrations_;
    std::vector<LearningEvent> registered_;
    bool registration_enabled_ = true, registration_limit_ = false;
    std::uint64_t revision_ = 0;
};
} // namespace nskkzime
