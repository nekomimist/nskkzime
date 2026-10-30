#pragma once
#include "nskkzime/engine.h"

namespace nskkzime::windows {
inline const wchar_t* mode_label(Mode mode) noexcept {
    switch (mode) {
    case Mode::Hiragana: return L"あ";
    case Mode::Katakana: return L"ア";
    case Mode::FullWidthLatin: return L"Ａ";
    default: return L"A";
    }
}
} // namespace nskkzime::windows
