#pragma once
#include "engine.h"

namespace nskkzime {
struct NumericReading {
    LookupKey key;
    std::vector<std::u32string> numbers;
};
// DDSKK's default integer runs: periods/signs remain part of the key, while
// fullwidth digits and commas between digits are normalized before extraction.
NumericReading numeric_reading(const LookupKey&);
struct NumericExpansion {
    std::u32string text;
    bool converted = false;
};
// Unsupported types, missing numbers and oversized expansions are omitted.
// With no input numbers, dictionary spellings (including #n) remain literal.
std::optional<NumericExpansion> expand_numeric(std::u32string_view, std::span<const std::u32string> numbers);
}
