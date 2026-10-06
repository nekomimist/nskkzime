#include "nskkzime/engine.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nskkzime {
void MemoryDictionary::add(LookupKey key, std::vector<Candidate> candidates) {
    entries_.insert_or_assign(std::move(key), std::move(candidates));
}
std::vector<Candidate> MemoryDictionary::lookup(const LookupKey& key, std::u32string_view) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? std::vector<Candidate>{} : it->second;
}
std::shared_ptr<const Dictionary> demo_dictionary() {
    auto dictionary = std::make_shared<MemoryDictionary>();
    dictionary->add({U"かんじ"}, {{"kanji-1", U"漢字", U"文字"}, {"kanji-2", U"感じ", U"感覚"}});
    dictionary->add({U"にほん"}, {{"nihon-1", U"日本", U""}});
    dictionary->add({U"かいとう"}, {{"kaitou-1", U"回答", U""}, {"kaitou-2", U"解答", U""}, {"kaitou-3", U"解凍", U""}});
    dictionary->add({U"か", 'k'}, {{"kaku-1", U"書", U""}, {"kaku-2", U"描", U""}});
    dictionary->add({U"た", 'b'}, {{"taberu-1", U"食", U""}});
    return dictionary;
}
Engine::Engine(std::shared_ptr<const Dictionary> dictionary, bool registration_enabled, std::shared_ptr<const RomajiRules> rules)
    : dictionary_(std::move(dictionary)), rules_(std::move(rules)), registration_enabled_(registration_enabled) {
    if (!dictionary_) throw std::invalid_argument("dictionary must not be null");
    input_.romaji = Romaji(rules_);
}
void Engine::clear_composition() {
    forget_.reset();
    input_.stage = Stage::Direct;
    input_.abbrev = false;
    input_.romaji.clear(); input_.reading.clear(); input_.okuri.clear(); input_.okuri_letter = 0;
    input_.candidates.clear(); input_.candidate_index = 0;
    input_.candidates_touched = input_.generation_attempted = false;
}
void Engine::reset() {
    if (registering()) input_.mode = registrations_.front().original.mode;
    registrations_.clear(); registered_.clear(); registration_limit_ = false;
    clear_composition(); ++revision_;
}
void Engine::set_mode(Mode mode) { reset(); input_.mode = mode; }
std::u32string Engine::inference_reading(bool include_okuri) const {
    if (forgetting()) return {};
    if (registrations_.size() > 1) return {};
    const auto& input = registering() ? registrations_.front().original : input_;
    if (input.abbrev || (input.okuri_letter && !include_okuri) || !input.romaji.pending().empty() ||
        (input.stage != Stage::Reading && input.stage != Stage::Selecting)) return {};
    return input.reading + (include_okuri ? input.okuri : U"");
}
std::u32string Engine::scoring_text(std::size_t index) const {
    return !registering() && index < input_.candidates.size() ? input_.candidates[index].text + input_.okuri : std::u32string{};
}
bool Engine::can_reorder_candidates() const {
    return !forgetting() && !registering() && input_.stage == Stage::Selecting && !input_.candidates_touched && input_.candidate_index == 0;
}
bool Engine::generation_attempted() const {
    return registering() ? registrations_.front().original.generation_attempted : input_.generation_attempted;
}
void Engine::note_generation_attempt() {
    auto& input = registering() ? registrations_.front().original : input_;
    if (!input.generation_attempted) { input.generation_attempted = true; ++revision_; }
}
bool Engine::promote_candidate(std::size_t index) {
    if (!can_reorder_candidates() || index == 0 || index >= input_.candidates.size()) return false;
    std::rotate(input_.candidates.begin(),input_.candidates.begin()+static_cast<std::ptrdiff_t>(index),input_.candidates.begin()+static_cast<std::ptrdiff_t>(index+1));
    ++revision_; return true;
}
bool Engine::prepend_generated(std::vector<Candidate> candidates) {
    if (registering() || inference_reading().empty() || (input_.stage == Stage::Selecting && !can_reorder_candidates()) || candidates.empty()) return false;
    // Reuse the same validation and stable deduplication as fallback generation,
    // on a copy so an invalid beam cannot partially change the live list.
    auto trial = *this; trial.input_.candidates.clear();
    for (const auto& candidate : candidates) if (candidate.source != CandidateSource::GeneratedLong) return false;
    if (!trial.append_generated(std::move(candidates))) return false;
    for (const auto& old : input_.candidates)
        if (std::none_of(trial.input_.candidates.begin(),trial.input_.candidates.end(),[&](const Candidate& item) { return item.text == old.text; })) trial.input_.candidates.push_back(old);
    trial.input_.candidate_index = 0; *this = std::move(trial); return true;
}
void Engine::defer_registration() {
    if (registrations_.size() != 1 || !registrations_.front().draft.empty() || !input_literal().empty()) return;
    input_ = std::move(registrations_.front().original); registrations_.clear(); ++revision_;
}
bool Engine::append_generated(std::vector<Candidate> candidates) {
    if (registering() || inference_reading().empty() || candidates.size() > 8) return false;
    auto next = input_.candidates; const auto first = next.size();
    for (auto& candidate : candidates) {
        if (candidate.source == CandidateSource::Dictionary || candidate.text.empty() || candidate.text.size() > 1024) return false;
        for (const auto ch : candidate.text) if (ch < 0x20 || ch == 0x7f || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff) || ch == 0xfffc) return false;
        if (std::none_of(next.begin(),next.end(),[&](const Candidate& old) { return old.text == candidate.text; })) next.push_back(std::move(candidate));
    }
    if (next.size() == first) return false;
    input_.candidates = std::move(next); input_.candidate_index = first; input_.stage = Stage::Selecting; ++revision_; return true;
}
bool Engine::resume_registration() {
    if (registering() || inference_reading().empty()) return false;
    Result result; begin_registration(result); if (!registering()) return false;
    ++revision_; return true;
}
std::u32string Engine::display(std::u32string_view text) const {
    return input_.mode == Mode::Katakana ? katakana(text) : std::u32string(text);
}
std::u32string Engine::input_literal() const {
    if (input_.stage == Stage::Selecting) return candidate_text(input_.candidate_index);
    // Flushing a copy also resolves terminal n, without changing the engine.
    auto roman = input_.romaji;
    return display(input_.reading + input_.okuri + roman.flush());
}
std::u32string Engine::literal_text() const {
    return registering() ? registrations_.front().anchor : input_literal();
}
std::u32string Engine::candidate_text(std::size_t index) const {
    return index < input_.candidates.size() ? input_.candidates[index].text + display(input_.okuri) : std::u32string{};
}
std::u32string Engine::input_preedit() const {
    if (input_.stage == Stage::Direct) return ascii(input_.romaji.pending());
    if (input_.stage == Stage::Selecting) return U"▼" + input_literal();
    auto text = U"▽" + display(input_.reading);
    if (input_.okuri_letter) text += U"*" + display(input_.okuri);
    return text + ascii(input_.romaji.pending());
}
std::u32string Engine::registration_reading() const {
    if (!registering()) return {};
    const auto& original = registrations_.back().original;
    auto text = original.reading;
    if (original.okuri_letter) text += U"*" + original.okuri;
    return text;
}
std::u32string Engine::registration_text() const {
    return registering() ? registrations_.back().draft + input_preedit() : std::u32string{};
}
std::u32string Engine::preedit() const {
    if (forgetting()) return U"[削除確認: " + forget_answer() + U"]";
    return registering() ? U"[登録 " + registration_reading() + U": " + registration_text() + U"]" : input_preedit();
}
std::u32string Engine::composition_text() const {
    if (registering()) return registrations_.front().anchor;
    if (input_.stage == Stage::Selecting) return literal_text();
    return display(input_.reading + input_.okuri) + ascii(input_.romaji.pending());
}
bool Engine::would_handle(Key key) const {
    if (key.code == KeyCode::PageDown || key.code == KeyCode::PageUp || key.code == KeyCode::SelectCandidate)
        return input_.stage == Stage::Selecting;
    if (registering()) return key.code != KeyCode::Character || (key.character >= 0x20 && key.character <= 0x7e);
    if (key.code == KeyCode::Hiragana) return true;
    if (input_.mode == Mode::Latin) return false;
    if (key.code == KeyCode::Character) return key.character >= 0x20 && key.character <= 0x7e;
    return input_.stage != Stage::Direct || !input_.romaji.pending().empty();
}
void Engine::append(std::u32string text, Result& result) {
    if (input_.stage == Stage::Direct) result.committed += display(text);
    else if (input_.stage == Stage::Okuri) input_.okuri += text;
    else input_.reading += text;
}
void Engine::convert(Result& result) {
    append(input_.romaji.flush(), result);
    if (input_.reading.empty()) return;
    if (!dictionary_->ready()) { result.dictionary_pending = true; return; }
    result.conversion_started = true;
    input_.candidates_touched = input_.generation_attempted = false;
    const LookupKey lookup{input_.reading, input_.okuri_letter};
    auto stored = dictionary_->lookup(lookup, input_.okuri);
    // Once a registered spelling is available in the shared snapshot, defer to
    // its ordering. A temporary cache must not pin an old choice forever.
    bool refresh = false;
    std::erase_if(registered_, [&](const LearningEvent& item) {
        if (item.key != lookup || item.okuri != input_.okuri) return false;
        if (!dictionary_->registration_pending(item)) { refresh = true; return true; }
        return std::any_of(stored.begin(), stored.end(), [&](const Candidate& candidate) { return candidate.text == item.candidate.text; });
    });
    // A worker can publish a snapshot between lookup and the delivery check.
    if (refresh) stored = dictionary_->lookup(lookup, input_.okuri);
    input_.candidates.clear();
    for (const auto& item : registered_)
        if (item.key == lookup && item.okuri == input_.okuri) input_.candidates.push_back(item.candidate);
    if (input_.candidates.empty()) input_.candidates = std::move(stored);
    else {
        const auto cached_count = input_.candidates.size();
        for (const auto& candidate : stored)
            if (std::none_of(input_.candidates.begin(), input_.candidates.begin() + static_cast<std::ptrdiff_t>(cached_count),
                [&](const Candidate& item) { return item.text == candidate.text; })) input_.candidates.push_back(candidate);
    }
    input_.candidate_index = 0;
    if (input_.candidates.empty()) { result.dictionary_miss = true; begin_registration(result); }
    else input_.stage = Stage::Selecting;
}
void Engine::confirm(Result& result) {
    result.committed += input_literal();
    if (input_.stage == Stage::Selecting && input_.candidates[input_.candidate_index].learning == LearningPolicy::Learn &&
        input_.candidates[input_.candidate_index].source != CandidateSource::GeneratedLong)
        result.learning = LearningEvent{{input_.reading, input_.okuri_letter}, input_.okuri, input_.candidates[input_.candidate_index]};
    clear_composition();
}
Result Engine::handle(Key key) {
    Result result;
    if (!would_handle(key)) return result;
    result.consumed = true;
    ++revision_;
    registration_limit_ = false;
    if (forgetting()) return handle_forgetting(key);
    if (input_.stage == Stage::Selecting && key.code == KeyCode::Character && key.character == 'X') {
        input_.candidates_touched = true;
        const auto& candidate = input_.candidates[input_.candidate_index];
        auto target = dictionary_->forget_target({input_.reading, input_.okuri_letter}, input_.okuri, candidate.text);
        if (!target) {
            for (const auto& item : registered_)
                if (item.key == LookupKey{input_.reading, input_.okuri_letter} && item.okuri == input_.okuri &&
                    item.candidate.text == candidate.text && dictionary_->registration_pending(item)) { target = item; break; }
        }
        forget_ = Forget{std::move(target), {}};
        return result;
    }
    if (!registering()) return handle_input(key);
    // Keep an oversized edit transactional, including an attempted nested registration.
    const auto old_input = input_;
    const auto old_registrations = registrations_;
    result = handle_registration(key);
    if (registering() && registrations_.back().draft.size() + input_literal().size() > 1024) {
        input_ = old_input; registrations_ = old_registrations;
        result = {}; result.consumed = true; result.registration_limit = true;
    }
    registration_limit_ = result.registration_limit;
    return result;
}
void Engine::begin_registration(Result& result) {
    if (!registration_enabled_ || !dictionary_->registration_allowed()) return;
    if (registrations_.size() >= 8 || input_.reading.size() > 1024 || input_.okuri.size() > 1024) {
        result.registration_limit = registration_limit_ = true; return;
    }
    auto anchor = display(input_.reading + input_.okuri);
    registrations_.push_back({input_, {}, std::move(anchor)});
    input_ = {}; // Registration always starts with normal hiragana input.
    input_.romaji = Romaji(rules_);
}
void Engine::cancel_registration() {
    input_ = std::move(registrations_.back().original); registrations_.pop_back();
    input_.stage = input_.okuri_letter ? Stage::Okuri : Stage::Reading;
    input_.candidates.clear(); input_.candidate_index = 0;
}
void Engine::abandon_registration() {
    forget_.reset();
    if (!registering()) return;
    input_ = std::move(registrations_.front().original); registrations_.clear();
    input_.stage = input_.okuri_letter ? Stage::Okuri : Stage::Reading;
    input_.candidates.clear(); input_.candidate_index = 0; registration_limit_ = false; ++revision_;
}
void Engine::remember_registration(const LearningEvent& event) noexcept {
    try {
        auto next = registered_;
        std::erase_if(next, [&](const LearningEvent& item) {
            return item.key == event.key && item.okuri == event.okuri && item.candidate.text == event.candidate.text;
        });
        next.insert(next.begin(), event);
        if (next.size() > 128) next.resize(128);
        registered_ = std::move(next);
    } catch (...) { /* The durable dictionary can supply it after synchronization. */ }
}
void Engine::forget_registration(const LearningEvent& event) noexcept {
    std::erase_if(registered_, [&](const LearningEvent& item) {
        return item.key == event.key && item.okuri == event.okuri && item.candidate.text == event.candidate.text;
    });
}
const std::optional<LearningEvent>& Engine::forget_target() const {
    static const std::optional<LearningEvent> none;
    return forget_ ? forget_->target : none;
}
std::u32string Engine::forget_answer() const { return forget_ ? forget_->answer : std::u32string{}; }
void Engine::cancel_forgetting() { if (forget_) { forget_.reset(); ++revision_; } }
Result Engine::handle_forgetting(Key key) {
    Result result; result.consumed = true;
    if (key.code == KeyCode::Cancel || key.code == KeyCode::Hiragana) { cancel_forgetting(); return result; }
    if (key.code == KeyCode::Enter) {
        if (forget_->target && forget_->answer == U"yes") {
            result.forgetting = forget_->target;
            input_.stage = input_.okuri_letter ? Stage::Okuri : Stage::Reading;
            input_.candidates.clear(); input_.candidate_index = 0;
        }
        cancel_forgetting();
    } else if (key.code == KeyCode::Backspace) {
        if (!forget_->answer.empty()) forget_->answer.pop_back();
    } else if (key.code == KeyCode::Character && forget_->target && forget_->answer.size() < 16) {
        forget_->answer += static_cast<unsigned char>(key.character);
    } else if (key.code == KeyCode::Space && forget_->target && forget_->answer.size() < 16) {
        forget_->answer += U' ';
    }
    return result;
}
Result Engine::handle_registration(Key key) {
    Result result; result.consumed = true;
    if (key.code == KeyCode::Enter) {
        const auto word = registrations_.back().draft + input_literal();
        if (word.empty()) { cancel_registration(); return result; }
        if (word.size() > 1024) { result.registration_limit = true; return result; }
        const auto original = registrations_.back().original;
        result.learning = LearningEvent{{original.reading, original.okuri_letter}, original.okuri,
            {"registered:" + utf8(original.reading), word, {}}};
        result.registration = true;
        input_.mode = original.mode; clear_composition(); registrations_.pop_back();
        const auto committed = word + display(original.okuri);
        if (registering()) registrations_.back().draft += committed;
        else result.committed = committed;
        return result;
    }
    if (key.code == KeyCode::Cancel && input_.stage == Stage::Direct) {
        cancel_registration(); return result;
    }
    if (key.code == KeyCode::Backspace && input_.stage == Stage::Direct && input_.romaji.pending().empty()) {
        if (!registrations_.back().draft.empty()) registrations_.back().draft.pop_back();
        return result;
    }
    const auto parent = registrations_.size() - 1;
    if (input_.mode == Mode::Latin && key.code == KeyCode::Character)
        result.committed += static_cast<unsigned char>(key.character);
    else result = handle_input(key);
    registrations_[parent].draft += result.committed;
    result.committed.clear(); result.learning.reset(); // Ordinary inner conversions do not learn tentative text.
    return result;
}
Result Engine::handle_input(Key key) {
    Result result; result.consumed = true;
    if (input_.stage == Stage::Selecting) input_.candidates_touched = true;
    if (key.code == KeyCode::SelectCandidate || (input_.stage == Stage::Selecting &&
        key.code == KeyCode::Character && key.character >= '1' && key.character <= '7')) {
        const auto index = key.code == KeyCode::SelectCandidate ? key.candidate_index :
            input_.candidate_index / candidate_page_size * candidate_page_size + static_cast<std::size_t>(key.character - '1');
        // A missing row on the last page must not confirm a different word or
        // leak the shortcut into the document. Registration keeps the word in its draft.
        if (index < input_.candidates.size()) { input_.candidate_index = index; confirm(result); }
        return result;
    }
    if (key.code == KeyCode::PageDown || key.code == KeyCode::PageUp) {
        if (key.code == KeyCode::PageDown)
            input_.candidate_index += std::min(candidate_page_size, input_.candidates.size() - 1 - input_.candidate_index);
        else input_.candidate_index -= std::min(candidate_page_size, input_.candidate_index);
        return result; // Reaching either edge never confirms or starts registration.
    }
    if (key.code == KeyCode::Hiragana) {
        confirm(result);
        input_.mode = Mode::Hiragana;
        return result;
    }
    if (key.code == KeyCode::Cancel) {
        if (input_.stage == Stage::Selecting) {
            input_.stage = input_.okuri_letter ? Stage::Okuri : Stage::Reading;
            input_.candidates.clear(); input_.candidate_index = 0;
        } else clear_composition();
        return result;
    }
    if (key.code == KeyCode::Enter) { confirm(result); return result; }
    if (key.code == KeyCode::Backspace) {
        if (input_.stage == Stage::Selecting) {
            input_.stage = input_.okuri_letter ? Stage::Okuri : Stage::Reading;
            input_.candidates.clear(); input_.candidate_index = 0;
        } else if (!input_.romaji.backspace()) {
            if (!input_.okuri.empty()) input_.okuri.pop_back();
            else if (input_.okuri_letter) { input_.okuri_letter = 0; input_.stage = Stage::Reading; }
            else if (!input_.reading.empty()) input_.reading.pop_back();
            else clear_composition();
        }
        return result;
    }
    if (key.code == KeyCode::Space) {
        if (!input_.abbrev && input_.stage != Stage::Selecting && !input_.romaji.pending().empty() && input_.romaji.accepts(' ')) {
            append(input_.romaji.push(' '),result);
            if (input_.stage == Stage::Okuri && !input_.okuri.empty() && input_.romaji.pending().empty()) convert(result);
            return result;
        }
        if (input_.stage == Stage::Selecting) {
            if (input_.candidate_index + 1 < input_.candidates.size()) ++input_.candidate_index;
            else { result.candidates_exhausted = true; begin_registration(result); }
        } else if (input_.stage == Stage::Direct) {
            append(input_.romaji.flush(), result); result.committed += U" ";
        } else convert(result);
        return result;
    }
    char ch = key.character;
    if (input_.stage == Stage::Selecting) {
        if (ch == 'x') {
            if (input_.candidate_index > 0) --input_.candidate_index;
            else { input_.stage = input_.okuri_letter ? Stage::Okuri : Stage::Reading; input_.candidates.clear(); }
            return result;
        }
        confirm(result); // Reprocess this key after committing the selected word.
    }
    if (input_.abbrev) {
        // ASCII spelling and case are the dictionary key; q/l are ordinary letters.
        input_.reading += static_cast<unsigned char>(ch);
        return result;
    }
    if (input_.stage == Stage::Direct && input_.romaji.pending().empty() && ch == '/') {
        input_.stage = Stage::Reading; input_.abbrev = true;
        return result;
    }
    if (input_.romaji.pending().empty() && ch == 'q') {
        if (input_.stage == Stage::Direct) input_.mode = input_.mode == Mode::Hiragana ? Mode::Katakana : Mode::Hiragana;
        else {
            auto text = input_.reading + input_.okuri;
            result.committed += input_.mode == Mode::Hiragana ? katakana(text) : text;
            clear_composition();
        }
        return result;
    }
    if (input_.stage == Stage::Direct && input_.romaji.pending().empty() && ch == 'l') {
        input_.mode = Mode::Latin;
        return result;
    }
    if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
        if (input_.stage == Stage::Direct) {
            append(input_.romaji.flush(), result);
            input_.stage = Stage::Reading;
        } else if (input_.stage == Stage::Reading && !input_.reading.empty()) {
            append(input_.romaji.flush(), result);
            input_.stage = Stage::Okuri; input_.okuri_letter = ch;
        }
        // While forming the first kana or an established okuri, an extra
        // shifted letter continues the romaji (KAnji, KYOu, KaKU, KOroNDe).
    }
    append(input_.romaji.push(ch), result);
    if (input_.stage == Stage::Okuri && !input_.okuri.empty() && input_.romaji.pending().empty()) convert(result);
    return result;
}
} // namespace nskkzime
