#include "prompt.h"
#include "test_util.h"

using zenz::PromptInput;
using zenz::PromptOptions;

static void test_to_katakana() {
    CHECK_EQ(zenz::to_katakana("きょうはいいてんき"), "キョウハイイテンキ");
    CHECK_EQ(zenz::to_katakana("ぁゖゝゞ"), "ァヶヽヾ");
    CHECK_EQ(zenz::to_katakana("カタカナとー123abc漢字"), "カタカナトー123abc漢字");
    CHECK_EQ(zenz::to_katakana(""), "");
}

static void test_normalize_for_model() {
    CHECK_EQ(zenz::normalize_for_model("a b\tc"), "a\u3000b\u3000c");
    CHECK_EQ(zenz::normalize_for_model("一行目\r\n二行目\n"), "一行目二行目");
}

static void test_utf8_helpers() {
    CHECK_EQ(zenz::utf8_length("あいうabc"), 6u);
    CHECK_EQ(zenz::utf8_suffix("あいうえお", 2), "えお");
    CHECK_EQ(zenz::utf8_suffix("あい", 5), "あい");
    CHECK_EQ(zenz::utf8_prefix("あいうえお", 2), "あい");
    CHECK_EQ(zenz::utf8_prefix("あい", 5), "あい");
    // Malformed bytes become U+FFFD instead of being dropped or split.
    CHECK_EQ(zenz::utf8_prefix("\xE3\x81", 5), "\uFFFD\uFFFD");
}

static void test_build_prompt() {
    CHECK_EQ(zenz::build_prompt(PromptInput{"かいとう", "", ""}),
             "\uEE00カイトウ\uEE01");
    CHECK_EQ(zenz::build_prompt(PromptInput{"かいとう", "試験問題の", ""}),
             "\uEE02試験問題の\uEE00カイトウ\uEE01");
    CHECK_EQ(zenz::build_prompt(PromptInput{"かいとう", "", "を提出した"}),
             "\uEE07を提出した\uEE00カイトウ\uEE01");
    CHECK_EQ(zenz::build_prompt(PromptInput{"かいとう", "問題の", "を提出"}),
             "\uEE02問題の\uEE07を提出\uEE00カイトウ\uEE01");
}

static void test_build_prompt_truncates_context() {
    PromptOptions options;
    options.max_left_chars = 3;
    options.max_right_chars = 2;
    CHECK_EQ(zenz::build_prompt(PromptInput{"あ", "一二三四五", "六七八"}, options),
             "\uEE02三四五\uEE07六七\uEE00ア\uEE01");
    // Newlines are removed before truncation so they do not use up the budget.
    CHECK_EQ(zenz::build_prompt(PromptInput{"あ", "一二\n三", ""}, options),
             "\uEE02一二三\uEE00ア\uEE01");
}

int main() {
    test_to_katakana();
    test_normalize_for_model();
    test_utf8_helpers();
    test_build_prompt();
    test_build_prompt_truncates_context();
    return test_util::finish();
}
