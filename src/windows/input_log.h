#pragma once
#include "inference_ipc.h"
#include <optional>

namespace nskkzime::windows {
enum class InputLogStatus { Off, Ready, Written, Full, Unavailable };
struct InputLogRecord {
    InferenceRequest request;
    std::optional<InferenceResponse> response;
    InferenceStatus status = InferenceStatus::Ok;
};
// Synchronous writer: only the inference worker may call write(). No file is
// created until a TSF response has passed the current document/privacy checks.
class InputLog {
public:
    explicit InputLog(std::filesystem::path directory) : directory_(std::move(directory)) {}
    ~InputLog();
    InputLog(const InputLog&) = delete;
    InputLog& operator=(const InputLog&) = delete;
    InputLogStatus write(const InputLogRecord&) noexcept;
    static constexpr std::size_t file_limit = 1024 * 1024, directory_files = 64;
private:
    void open();
    std::filesystem::path directory_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::size_t bytes_ = 0;
    InputLogStatus status_ = InputLogStatus::Ready;
};
const wchar_t* input_log_status_text(InputLogStatus) noexcept;
} // namespace nskkzime::windows
