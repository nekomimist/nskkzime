// zenz model wrapper. All llama.cpp API usage lives in model.cpp.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace zenz {

struct ModelOptions {
    int n_threads = 4;
    std::uint32_t n_ctx = 1024;
    // Upper bound for DecodeOptions::beam_width.
    int max_beam_width = 8;
    // Texts scored together in one decode by Model::score.
    int score_batch = 32;
    bool verbose = false;
};

struct DecodeOptions {
    int n_best = 1;
    int beam_width = 1;  // 1 means greedy decoding
    int max_tokens = 64;
};

struct Candidate {
    std::string text;
    float score = 0.0f;  // sum of token log-probabilities
};

class Model {
public:
    // Returns nullptr and sets *error on failure.
    static std::unique_ptr<Model> load(const std::string& path, const ModelOptions& options,
                                       std::string* error);
    ~Model();

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    // Generates up to options.n_best distinct completions of `prompt`, best first.
    // Returns false and sets *error on failure.
    bool generate(const std::string& prompt, const DecodeOptions& options,
                  std::vector<Candidate>* out, std::string* error);

    // Scores each text as a complete completion of `prompt`: the sum of the
    // log-probabilities of its tokens and the end-of-sequence token. The prompt
    // is decoded once and shared. Returns false and sets *error on failure.
    bool score(const std::string& prompt, const std::vector<std::string>& texts,
               std::vector<float>* out, std::string* error);

    std::vector<std::int32_t> tokenize(const std::string& text, bool add_bos) const;
    std::string detokenize(const std::vector<std::int32_t>& tokens) const;

    // nskkzime: caller serializes use and keeps data alive through each decode.
    void set_abort_callback(bool (*callback)(void*), void* data);

private:
    Model(llama_model* model, llama_context* ctx, const ModelOptions& options);

    llama_model* model_;
    llama_context* ctx_;
    const llama_vocab* vocab_;
    ModelOptions options_;
    bool (*abort_)(void*) = nullptr;
    void* abort_data_ = nullptr;
    bool cancelled() const { return abort_ && abort_(abort_data_); }
};

}  // namespace zenz
