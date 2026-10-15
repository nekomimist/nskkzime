#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <unordered_set>

#include "llama.h"

namespace zenz {
namespace {

bool g_verbose = false;

void log_callback(ggml_log_level level, const char* text, void* /*user_data*/) {
    if (g_verbose || level == GGML_LOG_LEVEL_ERROR) {
        std::fputs(text, stderr);
    }
}

void init_backend_once() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        llama_log_set(log_callback, nullptr);
        llama_backend_init();
    });
}

// Thin helper over llama_batch that always requests one sequence per token.
class Batch {
public:
    explicit Batch(int capacity) : batch_(llama_batch_init(capacity, 0, 1)) {}
    ~Batch() { llama_batch_free(batch_); }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    void clear() { batch_.n_tokens = 0; }
    int size() const { return batch_.n_tokens; }
    void add(llama_token token, llama_pos pos, llama_seq_id seq, bool logits) {
        const int i = batch_.n_tokens++;
        batch_.token[i] = token;
        batch_.pos[i] = pos;
        batch_.n_seq_id[i] = 1;
        batch_.seq_id[i][0] = seq;
        batch_.logits[i] = logits ? 1 : 0;
    }
    llama_batch& get() { return batch_; }

private:
    llama_batch batch_;
};

// Returns log(sum(exp(logits))), the log-softmax normalizer.
float log_sum_exp(const float* logits, int n_vocab) {
    float max_logit = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < n_vocab; ++i) {
        max_logit = std::max(max_logit, logits[i]);
    }
    double sum = 0.0;
    for (int i = 0; i < n_vocab; ++i) {
        sum += std::exp(static_cast<double>(logits[i] - max_logit));
    }
    return max_logit + static_cast<float>(std::log(sum));
}

float log_prob(const float* logits, int n_vocab, llama_token token) {
    return logits[token] - log_sum_exp(logits, n_vocab);
}

// Returns log-softmax of `logits` for the top `k` tokens, best first.
std::vector<std::pair<llama_token, float>> top_k_log_probs(const float* logits, int n_vocab,
                                                           int k) {
    const float log_z = log_sum_exp(logits, n_vocab);
    std::vector<std::pair<llama_token, float>> top;
    top.reserve(n_vocab);
    for (int i = 0; i < n_vocab; ++i) {
        top.emplace_back(i, logits[i] - log_z);
    }
    k = std::min(k, n_vocab);
    std::partial_sort(top.begin(), top.begin() + k, top.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    top.resize(k);
    return top;
}

struct Beam {
    std::vector<llama_token> tokens;
    float score = 0.0f;
    llama_seq_id seq = 0;
    int logits_index = 0;  // index into the last decoded batch
};

}  // namespace

std::unique_ptr<Model> Model::load(const std::string& path, const ModelOptions& options,
                                   std::string* error) {
    g_verbose = options.verbose;
    init_backend_once();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 0;
    // Keep llama.cpp's default mapped loading. Newer versions expose load_mode
    // instead of use_mmap; both pinned variants default to mapping CPU weights.
    llama_model* model = llama_model_load_from_file(path.c_str(), model_params);
    if (model == nullptr) {
        *error = "failed to load model: " + path;
        return nullptr;
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = options.n_ctx;
    ctx_params.n_batch = options.n_ctx;
    ctx_params.n_threads = options.n_threads;
    ctx_params.n_threads_batch = options.n_threads;
    // Beams live in two banks of sequences (see Model::generate); scored texts
    // use one sequence each next to the prompt's (see Model::score).
    ctx_params.n_seq_max =
        static_cast<std::uint32_t>(std::max(2 * options.max_beam_width, 1 + options.score_batch));
    // Beams share the prompt's KV cells, which requires a unified cache.
    ctx_params.kv_unified = true;
    ctx_params.no_perf = true;
    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (ctx == nullptr) {
        llama_model_free(model);
        *error = "failed to create llama context";
        return nullptr;
    }
    return std::unique_ptr<Model>(new Model(model, ctx, options));
}

Model::Model(llama_model* model, llama_context* ctx, const ModelOptions& options)
    : model_(model), ctx_(ctx), vocab_(llama_model_get_vocab(model)), options_(options) {}

Model::~Model() {
    llama_free(ctx_);
    llama_model_free(model_);
}

void Model::set_abort_callback(bool (*callback)(void*), void* data) {
    abort_ = callback; abort_data_ = data;
    llama_set_abort_callback(ctx_,callback,data);
}

std::vector<std::int32_t> Model::tokenize(const std::string& text, bool add_bos) const {
    std::vector<llama_token> tokens(text.size() + 2);
    int n = llama_tokenize(vocab_, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), add_bos, false);
    if (n < 0) {
        tokens.resize(-n);
        n = llama_tokenize(vocab_, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), add_bos, false);
    }
    tokens.resize(std::max(n, 0));
    return tokens;
}

std::string Model::detokenize(const std::vector<std::int32_t>& tokens) const {
    std::string out;
    char buf[64];
    for (llama_token token : tokens) {
        int n = llama_token_to_piece(vocab_, token, buf, sizeof(buf), 0, false);
        if (n > 0) {
            out.append(buf, n);
        }
    }
    return out;
}

// Beam search over sequences in a unified KV cache.
//
// The prompt is decoded once on sequence 0. Beams occupy one of two banks of
// sequence IDs ([0, W) and [W, 2W)) and alternate banks every step: each new beam
// copies its parent's sequence into the other bank, then the old bank is cleared.
// Copying a sequence only tags the existing KV cells, so the prompt is shared.
bool Model::generate(const std::string& prompt, const DecodeOptions& options,
                     std::vector<Candidate>* out, std::string* error) {
    out->clear();
    const int width = std::clamp(options.beam_width, 1, options_.max_beam_width);
    const int n_best = std::max(options.n_best, 1);
    const int n_vocab = llama_vocab_n_tokens(vocab_);
    const llama_token eos = llama_vocab_eos(vocab_);

    // The GGUF sets add_bos_token=false, so azooKey's add_bos=true is a no-op too.
    const std::vector<llama_token> prompt_tokens = tokenize(prompt, false);
    const int n_prompt = static_cast<int>(prompt_tokens.size());
    // Every step adds `width` cells, and cleared cells are reused.
    const int max_tokens = std::min(options.max_tokens,
                                    (static_cast<int>(options_.n_ctx) - n_prompt) / width - 1);
    if (n_prompt == 0 || max_tokens <= 0) {
        *error = "prompt is too long";
        return false;
    }

    llama_memory_t mem = llama_get_memory(ctx_);
    llama_memory_clear(mem, true);

    Batch batch(std::max(n_prompt, width));
    for (int i = 0; i < n_prompt; ++i) {
        batch.add(prompt_tokens[i], i, 0, i == n_prompt - 1);
    }
    if (cancelled() || llama_decode(ctx_, batch.get()) != 0) {
        *error = "llama_decode failed on prompt";
        return false;
    }

    std::vector<Beam> beams{Beam{{}, 0.0f, 0, n_prompt - 1}};
    std::vector<Candidate> finished;
    std::unordered_set<std::string> seen;

    for (int step = 0; step < max_tokens && !beams.empty(); ++step) {
        if (cancelled()) { *error = "cancelled"; return false; }
        struct Expansion {
            int parent;
            llama_token token;
            float score;
        };
        std::vector<Expansion> expansions;
        for (int b = 0; b < static_cast<int>(beams.size()); ++b) {
            const float* logits = llama_get_logits_ith(ctx_, beams[b].logits_index);
            for (const auto& [token, log_prob] : top_k_log_probs(logits, n_vocab, width)) {
                expansions.push_back({b, token, beams[b].score + log_prob});
            }
        }
        std::sort(expansions.begin(), expansions.end(),
                  [](const Expansion& a, const Expansion& b) { return a.score > b.score; });

        std::vector<Beam> next;
        std::vector<int> parents;
        for (const Expansion& e : expansions) {
            if (static_cast<int>(next.size()) == width) {
                break;
            }
            if (e.token == eos) {
                std::string text = detokenize(beams[e.parent].tokens);
                if (!text.empty() && seen.insert(text).second) {
                    finished.push_back({std::move(text), e.score});
                }
                continue;
            }
            Beam beam;
            beam.tokens = beams[e.parent].tokens;
            beam.tokens.push_back(e.token);
            beam.score = e.score;
            next.push_back(std::move(beam));
            parents.push_back(e.parent);
        }

        // Scores only decrease, so no live beam can beat the current n-best.
        if (static_cast<int>(finished.size()) >= n_best) {
            std::sort(finished.begin(), finished.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
            if (next.empty() || next.front().score <= finished[n_best - 1].score) {
                break;
            }
        }
        if (next.empty()) {
            break;
        }

        const llama_seq_id bank = static_cast<llama_seq_id>(((step + 1) % 2) * width);
        batch.clear();
        for (int i = 0; i < static_cast<int>(next.size()); ++i) {
            const Beam& parent = beams[parents[i]];
            next[i].seq = bank + i;
            llama_memory_seq_cp(mem, parent.seq, next[i].seq, -1, -1);
            batch.add(next[i].tokens.back(), n_prompt + step, next[i].seq, true);
            next[i].logits_index = i;
        }
        for (const Beam& parent : beams) {
            llama_memory_seq_rm(mem, parent.seq, -1, -1);
        }
        if (cancelled() || llama_decode(ctx_, batch.get()) != 0) {
            *error = "llama_decode failed during generation";
            return false;
        }
        beams = std::move(next);
    }

    std::sort(finished.begin(), finished.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (static_cast<int>(finished.size()) > n_best) {
        finished.resize(n_best);
    }
    *out = std::move(finished);
    return true;
}

// Teacher-forced scoring. The prompt is decoded once on sequence 0; the first
// token of every text is scored from its last logits. Texts are then decoded in
// chunks, each text on its own sequence that shares the prompt's KV cells, and
// every position requests logits for the next token (or end-of-sequence).
bool Model::score(const std::string& prompt, const std::vector<std::string>& texts,
                  std::vector<float>* out, std::string* error) {
    out->assign(texts.size(), 0.0f);
    if (texts.empty()) {
        return true;
    }
    const int n_vocab = llama_vocab_n_tokens(vocab_);
    const llama_token eos = llama_vocab_eos(vocab_);
    const int n_ctx = static_cast<int>(options_.n_ctx);
    const int max_seqs = std::max(options_.score_batch, 1);

    const std::vector<llama_token> prompt_tokens = tokenize(prompt, false);
    const int n_prompt = static_cast<int>(prompt_tokens.size());
    if (n_prompt == 0 || n_prompt >= n_ctx) {
        *error = "prompt is too long";
        return false;
    }
    // Each text becomes its tokens followed by EOS; the EOS itself is never decoded.
    std::vector<std::vector<llama_token>> targets;
    targets.reserve(texts.size());
    for (const std::string& text : texts) {
        std::vector<llama_token> tokens = tokenize(text, false);
        tokens.push_back(eos);
        if (static_cast<int>(tokens.size()) > n_ctx - n_prompt) {
            *error = "candidate is too long";
            return false;
        }
        targets.push_back(std::move(tokens));
    }

    llama_memory_t mem = llama_get_memory(ctx_);
    llama_memory_clear(mem, true);

    Batch batch(n_ctx);
    for (int i = 0; i < n_prompt; ++i) {
        batch.add(prompt_tokens[i], i, 0, i == n_prompt - 1);
    }
    if (cancelled() || llama_decode(ctx_, batch.get()) != 0) {
        *error = "llama_decode failed on prompt";
        return false;
    }
    std::vector<float> first_logits(llama_get_logits_ith(ctx_, n_prompt - 1),
                                    llama_get_logits_ith(ctx_, n_prompt - 1) + n_vocab);
    const float first_log_z = log_sum_exp(first_logits.data(), n_vocab);
    for (std::size_t t = 0; t < targets.size(); ++t) {
        (*out)[t] = first_logits[targets[t].front()] - first_log_z;
    }

    std::size_t next = 0;
    while (next < targets.size()) {
        if (cancelled()) { *error = "cancelled"; return false; }
        // Fill one chunk: at most max_seqs texts whose cells fit in the context.
        batch.clear();
        std::vector<std::pair<std::size_t, int>> chunk;  // (text index, first batch index)
        int cells = n_prompt;
        while (next < targets.size() && static_cast<int>(chunk.size()) < max_seqs) {
            const std::vector<llama_token>& tokens = targets[next];
            const int n_input = static_cast<int>(tokens.size()) - 1;
            if (cells + n_input > n_ctx) {
                break;
            }
            const llama_seq_id seq = static_cast<llama_seq_id>(1 + chunk.size());
            if (n_input > 0) {
                llama_memory_seq_cp(mem, 0, seq, -1, -1);
            }
            chunk.emplace_back(next, batch.size());
            for (int j = 0; j < n_input; ++j) {
                batch.add(tokens[j], n_prompt + j, seq, true);
            }
            cells += n_input;
            ++next;
        }
        if (batch.size() > 0 && (cancelled() || llama_decode(ctx_, batch.get()) != 0)) {
            *error = "llama_decode failed while scoring";
            return false;
        }
        for (const auto& [t, start] : chunk) {
            const std::vector<llama_token>& tokens = targets[t];
            for (int j = 1; j < static_cast<int>(tokens.size()); ++j) {
                (*out)[t] += log_prob(llama_get_logits_ith(ctx_, start + j - 1), n_vocab,
                                      tokens[j]);
            }
        }
        for (int s = 1; s <= static_cast<int>(chunk.size()); ++s) {
            llama_memory_seq_rm(mem, s, -1, -1);
        }
    }
    return true;
}

}  // namespace zenz
