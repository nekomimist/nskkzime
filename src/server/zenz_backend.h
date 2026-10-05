#pragma once
#include "nskkzime/inference.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
namespace zenz { class Model; }
namespace nskkzime {
class ZenzBackend {
public:
    explicit ZenzBackend(const std::filesystem::path& model, int threads = 4);
    ~ZenzBackend();
    InferenceResponse run(const InferenceRequest&, const std::function<bool()>& cancelled = {});
private:
    std::unique_ptr<zenz::Model> model_;
    std::timed_mutex mutex_; // One context, bounded and cancellable contention.
};
}
