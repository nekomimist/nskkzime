#pragma once
#include <string>
#include <string_view>

namespace nskkzime {
class Romaji {
public:
    std::u32string push(char ch);
    std::u32string flush();
    bool backspace();
    void clear();
    const std::string& pending() const { return pending_; }
private:
    std::string pending_;
    // The second n in "nn" may start the next syllable (konnichiha).
    bool carried_n_ = false;
};

std::u32string katakana(std::u32string_view text);
std::string utf8(std::u32string_view text);
std::u32string ascii(std::string_view text);
} // namespace nskkzime
