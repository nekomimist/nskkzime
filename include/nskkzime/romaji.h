#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace nskkzime {
struct RomajiRule { std::u32string output; std::string carry; };
class RomajiRules {
public:
    static std::shared_ptr<const RomajiRules> defaults();
    // UTF-8 TSV overlay: version=1, then input<TAB>output[<TAB>carry].
    static std::shared_ptr<const RomajiRules> parse(std::string_view);
    static std::shared_ptr<const RomajiRules> load(const std::filesystem::path&);
    const RomajiRule* find(std::string_view) const;
    bool prefix(std::string_view) const;
    std::size_t custom_count() const { return custom_count_; }
private:
    std::map<std::string, RomajiRule, std::less<>> rules_;
    std::size_t custom_count_ = 0;
};
class Romaji {
public:
    Romaji() : Romaji(RomajiRules::defaults()) {}
    explicit Romaji(std::shared_ptr<const RomajiRules> rules);
    std::u32string push(char ch);
    std::u32string flush();
    bool backspace();
    void clear();
    const std::string& pending() const { return pending_; }
    bool accepts(char ch) const; // An explicit continuation, including z + Space.
private:
    std::shared_ptr<const RomajiRules> rules_;
    std::string pending_;
    // The second n in "nn" may start the next syllable (konnichiha).
    bool carried_n_ = false;
};

std::u32string katakana(std::u32string_view text);
std::string utf8(std::u32string_view text);
// Strict decoder: malformed, overlong, surrogate and out-of-range sequences throw.
std::u32string from_utf8(std::string_view text);
std::u32string ascii(std::string_view text);
} // namespace nskkzime
