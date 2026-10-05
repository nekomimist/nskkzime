// Tests that need the real model. Skipped unless $ZENZ_MODEL names a file.
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>

#include "model.h"
#include "prompt.h"
#include "test_util.h"

namespace {

zenz::Model* g_model = nullptr;

std::vector<zenz::Candidate> convert(const std::string& kana, const std::string& left,
                                     const std::string& right, int n_best, int beam_width) {
    zenz::DecodeOptions options;
    options.n_best = n_best;
    options.beam_width = beam_width;
    options.max_tokens = static_cast<int>(zenz::utf8_length(kana)) + 8;
    std::vector<zenz::Candidate> out;
    std::string error;
    if (!g_model->generate(zenz::build_prompt({kana, left, right}), options, &out, &error)) {
        std::cerr << "generate failed: " << error << "\n";
        ++test_util::failures();
    }
    return out;
}

std::string best(const std::string& kana, const std::string& left = "",
                 const std::string& right = "") {
    auto out = convert(kana, left, right, 1, 1);
    return out.empty() ? std::string() : out.front().text;
}

void test_tokenizer_matches_fork_behavior() {
    // The GGUF disables BOS insertion, so add_bos has no effect (as in azooKey).
    const std::string prompt = "\uEE00\u30A2\uEE01";
    CHECK(g_model->tokenize(prompt, true) == g_model->tokenize(prompt, false));
    // Each private-use tag is three byte tokens.
    CHECK_EQ(g_model->tokenize("\uEE00", false).size(), 3u);
    CHECK_EQ(g_model->detokenize(g_model->tokenize("高校の教師", false)), "高校の教師");
}

void test_greedy() {
    CHECK_EQ(best("こうこうのきょうし"), "高校の教師");
    CHECK_EQ(best("きょうはいいてんきですね"), "今日はいい天気ですね");
}

void test_context_changes_result() {
    CHECK_EQ(best("かいとう", "試験問題の"), "解答");
    CHECK_EQ(best("かいとう", "冷凍食品を電子レンジで"), "解凍");
}

void test_beam_search() {
    const auto greedy = best("きしゃ");
    const auto out = convert("きしゃ", "", "", 5, 5);
    CHECK_EQ(out.size(), 5u);
    if (out.empty()) {
        return;
    }
    CHECK_EQ(out.front().text, greedy);
    std::set<std::string> unique;
    for (std::size_t i = 0; i < out.size(); ++i) {
        unique.insert(out[i].text);
        if (i > 0) {
            CHECK(out[i - 1].score >= out[i].score);
        }
    }
    CHECK_EQ(unique.size(), out.size());
}

std::vector<float> score(const std::string& kana, const std::string& left,
                         const std::vector<std::string>& texts) {
    std::vector<float> out;
    std::string error;
    if (!g_model->score(zenz::build_prompt({kana, left, ""}), texts, &out, &error)) {
        std::cerr << "score failed: " << error << "\n";
        ++test_util::failures();
    }
    return out;
}

void test_score_matches_generation() {
    // Greedy decoding follows the canonical tokenization here, so the
    // teacher-forced score must equal the generation score.
    const auto out = convert("こうこうのきょうし", "", "", 1, 1);
    CHECK_EQ(out.size(), 1u);
    if (out.empty()) {
        return;
    }
    const auto scores = score("こうこうのきょうし", "", {out.front().text});
    CHECK_EQ(scores.size(), 1u);
    CHECK(!scores.empty() && std::fabs(scores.front() - out.front().score) < 1e-3f);
}

void test_score_uses_context() {
    const std::vector<std::string> texts = {"回答", "解答", "解凍", ""};
    auto s = score("かいとう", "試験問題の", texts);
    CHECK_EQ(s.size(), texts.size());
    CHECK(s.size() == texts.size() && s[1] > s[0] && s[1] > s[2]);
    s = score("かいとう", "冷凍食品を電子レンジで", texts);
    CHECK(s.size() == texts.size() && s[2] > s[0] && s[2] > s[1]);
    // An empty text scores only the end-of-sequence token and is unlikely.
    CHECK(s.size() == texts.size() && s[3] < s[2]);
    CHECK(score("かいとう", "", {}).empty());
}

void test_score_chunks(const char* path) {
    // Scores must not depend on how texts are split into decode chunks.
    std::vector<std::string> texts;
    for (const char* t : {"記者", "汽車", "貴社", "帰社", "喜捨", "騎射", "きしゃ", "キシャ"}) {
        texts.push_back(t);
    }
    const auto all = score("きしゃ", "", texts);
    zenz::ModelOptions options;
    options.score_batch = 3;
    std::string error;
    auto small = zenz::Model::load(path, options, &error);
    CHECK(small != nullptr);
    if (!small) {
        return;
    }
    std::vector<float> chunked;
    CHECK(small->score(zenz::build_prompt({"きしゃ", "", ""}), texts, &chunked, &error));
    CHECK_EQ(chunked.size(), all.size());
    // A mix-up of sequences or KV cells changes scores by whole units. ARM's
    // i8mm kernels multiply rows in pairs, so the batch layout changes the
    // summation order and moves a score by up to about 0.01.
    for (std::size_t i = 0; i < std::min(chunked.size(), all.size()); ++i) {
        CHECK(std::fabs(chunked[i] - all[i]) < 0.05f);
    }
}

}  // namespace

int main() {
    const char* path = std::getenv("ZENZ_MODEL");
    if (path == nullptr || !std::ifstream(path).good()) {
        std::cerr << "ZENZ_MODEL not set or missing; skipping\n";
        return test_util::kSkip;
    }
    zenz::ModelOptions options;
    std::string error;
    auto model = zenz::Model::load(path, options, &error);
    if (!model) {
        std::cerr << error << "\n";
        return 1;
    }
    g_model = model.get();

    test_tokenizer_matches_fork_behavior();
    test_greedy();
    test_context_changes_result();
    test_beam_search();
    test_score_matches_generation();
    test_score_uses_context();
    test_score_chunks(path);
    return test_util::finish();
}
