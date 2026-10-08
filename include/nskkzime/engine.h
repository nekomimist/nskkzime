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
enum class KeyCode { Character, Space, Enter, Backspace, Cancel, Hiragana, PageDown, PageUp, SelectCandidate };
inline constexpr std::size_t candidate_page_size = 7;
struct Key {
    KeyCode code = KeyCode::Character;
    char character = 0;
    std::size_t candidate_index = 0; // Absolute index for a host's displayed candidate page.
};
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
struct LearningEvent {
    LookupKey key;
    std::u32string okuri; // Hiragana, separate from the candidate's stem.
    Candidate candidate;
};
class Dictionary {
public:
    virtual ~Dictionary() = default;
    virtual std::vector<Candidate> lookup(const LookupKey& key, std::u32string_view okuri = {}) const = 0;
    // An asynchronous host dictionary distinguishes loading from a true miss.
    virtual bool ready() const { return true; }
    virtual bool registration_allowed() const { return true; }
    // Immutable/CLI dictionaries keep local registrations until lookup observes
    // them. A persistence wrapper can release the cache after durable delivery.
    virtual bool registration_pending(const LearningEvent&) const { return true; }
    // Resolve an actual user entry, including its exact or generic okuri scope.
    virtual std::optional<LearningEvent> forget_target(const LookupKey&, std::u32string_view, std::u32string_view) const { return {}; }
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
    bool dictionary_pending = false;
    bool candidates_exhausted = false;
    bool conversion_started = false; // A fresh dictionary lookup, including automatic okuri conversion.
    bool registration = false; // learning is an explicitly registered word (possibly inside another registration).
    bool registration_limit = false;
    // A proposed event, not a dictionary mutation. Hosts may publish it only
    // after successfully applying this Result's text and adopting the Engine.
    // DoNotLearn candidates and long generations never produce this event.
    std::optional<LearningEvent> learning;
    std::optional<LearningEvent> forgetting; // Same transactional delivery contract as learning.
};

class Engine {
public:
    explicit Engine(std::shared_ptr<const Dictionary> dictionary, bool registration_enabled = true,
        std::shared_ptr<const RomajiRules> rules = RomajiRules::defaults());
    bool would_handle(Key key) const;
    // GUI hosts can map visible rows to SelectCandidate themselves.
    void set_numeric_candidate_keys(bool enabled) { numeric_candidate_keys_ = enabled; }
    Result handle(Key key);
    void reset(); // Clear composition, retain input mode.
    void set_mode(Mode mode); // Host has ended its composition; discard input and select an explicit mode.
    Mode mode() const { return input_.mode; }
    Mode outer_mode() const { return registering() ? registrations_.front().original.mode : input_.mode; }
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
    void forget_registration(const LearningEvent&) noexcept;
    bool forgetting() const { return forget_.has_value(); }
    const std::optional<LearningEvent>& forget_target() const;
    std::u32string forget_answer() const;
    void cancel_forgetting();
    void abandon_registration(); // Host cannot present the editor: restore the outer reading, without a learning event.
    std::u32string inference_reading(bool include_okuri = false) const; // Generation excludes okuri; score includes it.
    std::u32string scoring_text(std::size_t index) const; // Stem + hiragana okuri, no annotation.
    void defer_registration(); // Preserve the source reading/candidates while awaiting generation.
    bool append_generated(std::vector<Candidate>);
    bool prepend_generated(std::vector<Candidate>); // Long reading: only before candidate interaction.
    bool promote_candidate(std::size_t index); // Keep all other candidates and metadata in order.
    bool can_reorder_candidates() const;
    bool generation_attempted() const;
    void note_generation_attempt(); // Once per fresh conversion, even after timeout/cancellation.
    bool resume_registration(); // Empty/failed response falls back to the ordinary registration flow.
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
        bool candidates_touched = false, generation_attempted = false;
    };
    struct Registration { Input original; std::u32string draft, anchor; };
    Result handle_input(Key);
    Result handle_registration(Key);
    Result handle_forgetting(Key);
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
    std::shared_ptr<const RomajiRules> rules_;
    Input input_;
    std::vector<Registration> registrations_;
    std::vector<LearningEvent> registered_;
    struct Forget { std::optional<LearningEvent> target; std::u32string answer; };
    std::optional<Forget> forget_;
    bool registration_enabled_ = true, registration_limit_ = false;
    bool numeric_candidate_keys_ = true;
    std::uint64_t revision_ = 0;
};
} // namespace nskkzime
