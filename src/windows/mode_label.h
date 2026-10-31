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
// Menu, tooltip and UI-element descriptions share these names.
inline const wchar_t* mode_name(Mode mode) noexcept {
    switch (mode) {
    case Mode::Hiragana: return L"ひらがな";
    case Mode::Katakana: return L"カタカナ";
    case Mode::FullWidthLatin: return L"全英（全角英数）";
    default: return L"英数（IMEオフ）";
    }
}
} // namespace nskkzime::windows
