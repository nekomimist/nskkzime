#include "nskkzime/engine.h"
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
Engine::Engine(std::shared_ptr<const Dictionary> dictionary) : dictionary_(std::move(dictionary)) {
    if (!dictionary_) throw std::invalid_argument("dictionary must not be null");
}
void Engine::clear_composition() {
    stage_ = Stage::Direct;
    romaji_.clear(); reading_.clear(); okuri_.clear(); okuri_letter_ = 0;
    candidates_.clear(); candidate_index_ = 0;
}
void Engine::reset() { clear_composition(); ++revision_; }
std::u32string Engine::display(std::u32string_view text) const {
    return mode_ == Mode::Katakana ? katakana(text) : std::u32string(text);
}
std::u32string Engine::literal_text() const {
    if (stage_ == Stage::Selecting) return candidate_text(candidate_index_);
    // Flushing a copy also resolves terminal n, without changing the engine.
    auto roman = romaji_;
    return display(reading_ + okuri_ + roman.flush());
}
std::u32string Engine::candidate_text(std::size_t index) const {
    return index < candidates_.size() ? candidates_[index].text + display(okuri_) : std::u32string{};
}
std::u32string Engine::preedit() const {
    if (stage_ == Stage::Direct) return ascii(romaji_.pending());
    if (stage_ == Stage::Selecting) return U"▼" + literal_text();
    auto text = U"▽" + display(reading_);
    if (okuri_letter_) text += U"*" + display(okuri_);
    return text + ascii(romaji_.pending());
}
std::u32string Engine::composition_text() const {
    if (stage_ == Stage::Selecting) return literal_text();
    return display(reading_ + okuri_) + ascii(romaji_.pending());
}
bool Engine::would_handle(Key key) const {
    if (key.code == KeyCode::Hiragana) return true;
    if (mode_ == Mode::Latin) return false;
    if (key.code == KeyCode::Character) return key.character >= 0x20 && key.character <= 0x7e;
    return stage_ != Stage::Direct || !romaji_.pending().empty();
}
void Engine::append(std::u32string text, Result& result) {
    if (stage_ == Stage::Direct) result.committed += display(text);
    else if (stage_ == Stage::Okuri) okuri_ += text;
    else reading_ += text;
}
void Engine::convert(Result& result) {
    append(romaji_.flush(), result);
    if (reading_.empty()) return;
    candidates_ = dictionary_->lookup({reading_, okuri_letter_}, okuri_);
    candidate_index_ = 0;
    if (candidates_.empty()) result.dictionary_miss = true;
    else stage_ = Stage::Selecting;
}
void Engine::confirm(Result& result) {
    result.committed += literal_text();
    clear_composition();
}
Result Engine::handle(Key key) {
    Result result;
    if (!would_handle(key)) return result;
    result.consumed = true;
    ++revision_;
    if (key.code == KeyCode::Hiragana) {
        confirm(result);
        mode_ = Mode::Hiragana;
        return result;
    }
    if (key.code == KeyCode::Cancel) {
        if (stage_ == Stage::Selecting) {
            stage_ = okuri_letter_ ? Stage::Okuri : Stage::Reading;
            candidates_.clear(); candidate_index_ = 0;
        } else clear_composition();
        return result;
    }
    if (key.code == KeyCode::Enter) { confirm(result); return result; }
    if (key.code == KeyCode::Backspace) {
        if (stage_ == Stage::Selecting) {
            stage_ = okuri_letter_ ? Stage::Okuri : Stage::Reading;
            candidates_.clear(); candidate_index_ = 0;
        } else if (!romaji_.backspace()) {
            if (!okuri_.empty()) okuri_.pop_back();
            else if (okuri_letter_) { okuri_letter_ = 0; stage_ = Stage::Reading; }
            else if (!reading_.empty()) reading_.pop_back();
            else clear_composition();
        }
        return result;
    }
    if (key.code == KeyCode::Space) {
        if (stage_ == Stage::Selecting) {
            if (candidate_index_ + 1 < candidates_.size()) ++candidate_index_;
            else result.candidates_exhausted = true;
        } else if (stage_ == Stage::Direct) {
            append(romaji_.flush(), result); result.committed += U" ";
        } else convert(result);
        return result;
    }
    char ch = key.character;
    if (stage_ == Stage::Selecting) {
        if (ch == 'x') {
            if (candidate_index_ > 0) --candidate_index_;
            else { stage_ = okuri_letter_ ? Stage::Okuri : Stage::Reading; candidates_.clear(); }
            return result;
        }
        confirm(result); // Reprocess this key after committing the selected word.
    }
    if (romaji_.pending().empty() && ch == 'q') {
        if (stage_ == Stage::Direct) mode_ = mode_ == Mode::Hiragana ? Mode::Katakana : Mode::Hiragana;
        else {
            auto text = reading_ + okuri_;
            result.committed += mode_ == Mode::Hiragana ? katakana(text) : text;
            clear_composition();
        }
        return result;
    }
    if (stage_ == Stage::Direct && romaji_.pending().empty() && ch == 'l') {
        mode_ = Mode::Latin;
        return result;
    }
    if (ch >= 'A' && ch <= 'Z') {
        append(romaji_.flush(), result);
        ch = static_cast<char>(ch - 'A' + 'a');
        if (stage_ == Stage::Direct) stage_ = Stage::Reading;
        else if (stage_ == Stage::Reading && !reading_.empty()) {
            stage_ = Stage::Okuri; okuri_letter_ = ch;
        }
    }
    append(romaji_.push(ch), result);
    if (stage_ == Stage::Okuri && !okuri_.empty() && romaji_.pending().empty()) convert(result);
    return result;
}
} // namespace nskkzime
