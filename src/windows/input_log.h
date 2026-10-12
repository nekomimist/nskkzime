#pragma once
#include "inference_ipc.h"
#include "diagnostic_file.h"
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
    explicit InputLog(std::filesystem::path directory) : enabled_(!directory.empty()), file_(std::move(directory), L"nskkzime-input-", L"nskkzime-input-log.lock") {}
    InputLog(const InputLog&) = delete;
    InputLog& operator=(const InputLog&) = delete;
    InputLogStatus write(const InputLogRecord&) noexcept;
    static constexpr std::size_t file_limit = DiagnosticFile::file_limit, directory_files = DiagnosticFile::directory_files;
private:
    bool enabled_;
    DiagnosticFile file_;
    InputLogStatus status_ = InputLogStatus::Ready;
};
// The broker's LOG2 body, for AppContainer services that cannot write logs.
std::string encode_log_record(const InputLogRecord&);
InputLogRecord decode_log_record(std::string_view);
const wchar_t* input_log_status_text(InputLogStatus) noexcept;
} // namespace nskkzime::windows
