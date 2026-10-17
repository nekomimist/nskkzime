#include "llama.h"
#include "skk-zenz/prompt.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Json = nlohmann::json;

std::string hex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

Json snapshot(const char* path) {
    auto options = llama_model_default_params();
    options.vocab_only = true;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
        llama_model_load_from_file(path, options), llama_model_free);
    if (!model) throw std::runtime_error("Cannot load tokenizer model");
    const auto* vocab = llama_model_get_vocab(model.get());
    char pre[128]{};
    if (llama_model_meta_val_str(model.get(), "tokenizer.ggml.pre", pre, sizeof(pre)) < 0)
        throw std::runtime_error("Missing tokenizer.ggml.pre");
    Json result{{"pre", pre}, {"bos", llama_vocab_bos(vocab)}, {"eos", llama_vocab_eos(vocab)},
                {"pad", llama_vocab_pad(vocab)}, {"cases", Json::array()}};
    // Include byte fallback, GPT-2 whitespace handling, control tokens and the
    // actual prompt builder. This fixture is captured from the pinned fork.
    const std::vector<std::string> texts{
        "", "高校の教師", "かな漢字カナABC123", "ＡＢＣ１２３", "Hello, world!",
        " ", "  ", " leading", "trailing ", "a  b", "\n", "a\nb", "a\r\nb", "\t", "a\tb",
        "😀🐈", "か\u3099", "\uEE00ア\uEE01", "[UNK]", "[PAD]", "[BOS]", "[EOS]",
        zenz::build_prompt({"かいとう", "試験問題の", ""}),
        zenz::build_prompt({"かな", "前の文章\n", "後の文章"})};
    for (const auto& text : texts) {
        for (bool add_special : {false, true}) {
            for (bool parse_special : {false, true}) {
                std::vector<llama_token> tokens(text.size() * 4 + 32);
                auto count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                    tokens.data(), static_cast<int32_t>(tokens.size()), add_special, parse_special);
                if (count < 0) throw std::runtime_error("Tokenizer output exceeded buffer");
                tokens.resize(static_cast<std::size_t>(count));
                Json pieces = Json::array();
                for (const auto token : tokens) {
                    std::string piece(256, '\0');
                    auto size = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, true);
                    if (size < 0) {
                        piece.resize(static_cast<std::size_t>(-size));
                        size = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, true);
                    }
                    if (size < 0) throw std::runtime_error("Cannot decode token piece");
                    piece.resize(static_cast<std::size_t>(size));
                    pieces.push_back(hex(piece)); // A byte token need not be valid UTF-8 by itself.
                }
                result["cases"].push_back({{"text", text}, {"add_special", add_special},
                    {"parse_special", parse_special}, {"tokens", tokens}, {"pieces_hex", pieces}});
            }
        }
    }
    return result;
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    if (!std::filesystem::exists(argv[1])) return 77;
    try {
        llama_log_set([](ggml_log_level level, const char* message, void*) {
            if (level == GGML_LOG_LEVEL_ERROR) std::cerr << message;
        }, nullptr);
        const auto actual = snapshot(argv[1]);
        if (std::string_view(argv[2]) == "--snapshot") {
            std::cout << actual.dump(2) << '\n';
            return 0;
        }
        std::ifstream expected_file(argv[2]);
        if (!expected_file) throw std::runtime_error("Cannot read pinned tokenizer fixture");
        const auto difference = Json::diff(Json::parse(expected_file), actual);
        if (!difference.empty()) {
            std::cerr << "FAIL tokenizer differs from pinned azooKey fork: " << difference.dump() << '\n';
            return 1;
        }
        std::cout << "PASS tokenizer IDs, whitespace, special tokens, prompt and token pieces ("
                  << actual["cases"].size() << " cases)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
