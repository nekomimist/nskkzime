// Prompt construction for zenz v3.x models.
//
// Format (see docs/ARCHITECTURE.md):
//   U+EE02 <left context> U+EE07 <right context> U+EE00 <katakana> U+EE01
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace zenz {

struct PromptInput {
    std::string kana;   // reading, hiragana or katakana
    std::string left;   // text before the conversion target
    std::string right;  // text after the conversion target
};

struct PromptOptions {
    std::size_t max_left_chars = 40;
    std::size_t max_right_chars = 40;
};

// Converts hiragana to katakana. Other characters are kept as is.
std::string to_katakana(std::string_view utf8);

// Applies the substitutions azooKey performs before tokenizing:
// ASCII space becomes U+3000 and newlines are removed. The zenz tokenizer maps
// both to [UNK] otherwise.
std::string normalize_for_model(std::string_view utf8);

// Returns the last / first N code points of a UTF-8 string.
std::string utf8_suffix(std::string_view utf8, std::size_t n_chars);
std::string utf8_prefix(std::string_view utf8, std::size_t n_chars);

// Returns the number of code points in a UTF-8 string.
std::size_t utf8_length(std::string_view utf8);

// Builds a candidate generation prompt ending with the output tag.
std::string build_prompt(const PromptInput& input, const PromptOptions& options = {});

}  // namespace zenz
