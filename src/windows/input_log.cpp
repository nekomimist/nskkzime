#include "input_log.h"
#include "byte_codec.h"
#include <cstdio>

namespace nskkzime::windows {
namespace {
const char* status_name(InferenceStatus status) {
    switch (status) {
    case InferenceStatus::Ok: return "ok";
    case InferenceStatus::Cancelled: return "cancelled";
    case InferenceStatus::Timeout: return "timeout";
    case InferenceStatus::InvalidResponse: return "invalid_response";
    default: return "unavailable";
    }
}
}
InputLogStatus InputLog::write(const InputLogRecord& record) noexcept {
    if (!enabled_) return InputLogStatus::Off;
    if (status_ == InputLogStatus::Full || status_ == InputLogStatus::Unavailable) return status_;
    try {
        const auto request = encode_request(record.request);
        const auto response = record.response ? encode_response(*record.response) : std::string("null");
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char timestamp[32]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
                  utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        const auto line = "{\"format\":\"nskkzime-input-log\",\"version\":1,\"utc\":\"" + std::string(timestamp) +
            "\",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"status\":\"" + status_name(record.status) +
            "\",\"request\":" + request + ",\"response\":" + response + "}\n";
        switch (file_.append(line)) {
        case DiagnosticFileStatus::Written: return status_ = InputLogStatus::Written;
        case DiagnosticFileStatus::Full: return status_ = InputLogStatus::Full;
        default: return status_ = InputLogStatus::Unavailable;
        }
    } catch (...) { return status_ = InputLogStatus::Unavailable; }
}
std::string encode_log_record(const InputLogRecord& record) {
    ByteWriter w("");
    w.number(static_cast<std::uint32_t>(record.status)); w.text(encode_request(record.request));
    w.number(record.response.has_value()); w.text(record.response ? encode_response(*record.response) : std::string{});
    return std::move(w.bytes);
}
InputLogRecord decode_log_record(std::string_view bytes) {
    ByteReader r(bytes, ""); InputLogRecord record;
    const auto status = r.number(); ByteReader::need(status <= static_cast<std::uint32_t>(InferenceStatus::InvalidResponse));
    record.status = static_cast<InferenceStatus>(status);
    record.request = decode_request(r.text(inference_max_line));
    const bool answered = r.flag(); const auto response = r.text(inference_max_line);
    if (answered) record.response = decode_response(response, record.request);
    else ByteReader::need(response.empty());
    r.end(); return record;
}
const wchar_t* input_log_status_text(InputLogStatus status) noexcept {
    switch (status) {
    case InputLogStatus::Ready: return L"入力ログ: 有効・対象応答待ち";
    case InputLogStatus::Written: return L"入力ログ: 保存中";
    case InputLogStatus::Full: return L"入力ログ: 上限に達したため停止";
    case InputLogStatus::Unavailable: return L"入力ログ: 保存できないため停止";
    default: return L"入力ログ: オフ";
    }
}
} // namespace nskkzime::windows
