#include "zenz_backend.h"
#include "nskkzime/romaji.h"
#include "model.h"
#include "prompt.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
namespace nskkzime {
namespace {
struct Cancellation {
    const std::function<bool()>& predicate;
    std::atomic<bool> stopped = false;
    static bool check(void* pointer) noexcept {
        auto& self = *static_cast<Cancellation*>(pointer);
        try { if (self.predicate && self.predicate()) self.stopped = true; }
        catch (...) { self.stopped = true; }
        return self.stopped;
    }
};
struct ResetAbort {
    zenz::Model& model;
    ~ResetAbort() { model.set_abort_callback(nullptr,nullptr); }
};
}
ZenzBackend::ZenzBackend(const std::filesystem::path& model, int threads) {
    if (threads < 1 || threads > 64) throw std::invalid_argument("threads must be 1 to 64");
    const auto path = model.u8string(); zenz::ModelOptions options; options.n_threads = threads;
    std::string error; model_ = zenz::Model::load({path.begin(),path.end()},options,&error);
    if (!model_) throw std::runtime_error("cannot load zenz model");
}
ZenzBackend::~ZenzBackend() = default;
InferenceResponse ZenzBackend::run(const InferenceRequest& request, const std::function<bool()>& cancelled) {
    (void)encode_request(request); // Validate direct callers as well as wire requests.
    Cancellation cancellation{cancelled};
    std::unique_lock lock(mutex_,std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(5))) {
        if (Cancellation::check(&cancellation)) throw std::runtime_error("inference cancelled");
    }
    if (Cancellation::check(&cancellation)) throw std::runtime_error("inference cancelled");
    model_->set_abort_callback(Cancellation::check,&cancellation); ResetAbort reset{*model_};
    const auto prompt = zenz::build_prompt({utf8(request.reading),utf8(request.left),utf8(request.right)});
    InferenceResponse response; response.id = request.id; response.operation = request.operation;
    std::string error; bool ok = false;
    if (request.operation == InferenceOperation::Score) {
        std::vector<std::string> texts;
        for (const auto& value : request.candidates) texts.push_back(zenz::normalize_for_model(utf8(value)));
        ok = model_->score(prompt,texts,&response.scores,&error);
    } else {
        zenz::DecodeOptions options; options.n_best = options.beam_width = static_cast<int>(request.count);
        options.max_tokens = static_cast<int>(request.reading.size())+8;
        std::vector<zenz::Candidate> candidates;
        ok = model_->generate(prompt,options,&candidates,&error);
        if (ok) for (const auto& candidate : candidates) {
            try {
                auto text = from_utf8(candidate.text);
                InferenceResponse single{request.id,{text},{candidate.score}};
                (void)encode_response(single); // Discard incomplete UTF-8/control output.
                response.candidates.push_back(std::move(text)); response.scores.push_back(candidate.score);
            } catch (const std::invalid_argument&) { /* Invalid beam never reaches the input document. */ }
        }
    }
    if (Cancellation::check(&cancellation)) throw std::runtime_error("inference cancelled");
    if (!ok) throw std::runtime_error("zenz inference failed (input may exceed model context)");
    (void)encode_response(response); return response;
}
}
