#include "input_log.h"
#include <sddl.h>
#include <cstdio>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
constexpr wchar_t prefix[] = L"nskkzime-input-";
void need(bool value) { if (!value) throw std::runtime_error("input log unavailable"); }
struct Descriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    Descriptor() {
        HANDLE raw = nullptr;
        need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != 0);
        NativeHandle token(raw);
        DWORD size = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        need(GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) != 0);
        LPWSTR sid = nullptr;
        need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid) != 0);
        std::wstring acl;
        try { acl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")"; }
        catch (...) { LocalFree(sid); throw; }
        LocalFree(sid);
        need(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &value, nullptr) != 0);
    }
    ~Descriptor() { LocalFree(value); }
};
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
InputLog::~InputLog() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
void InputLog::open() {
    need(directory_.is_absolute() && directory_.root_name().native().size() == 2);
    const auto drive = GetDriveTypeW(directory_.root_path().c_str());
    need(drive == DRIVE_FIXED || drive == DRIVE_REMOVABLE);
    // Do not follow directory junctions/symlinks into an unintended destination.
    for (auto part = directory_; !part.empty();) {
        const auto attributes = GetFileAttributesW(part.c_str());
        need(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
        const auto parent = part.parent_path();
        if (parent == part) break;
        part = parent;
    }
    Descriptor descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
    // A shared directory has a bounded number of files across processes and
    // Windows sessions. Never truncate/delete existing logs or other files.
    const auto lock_path = directory_ / L"nskkzime-input-log.lock";
    HANDLE raw_lock = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10; ++attempt) {
        raw_lock = CreateFileW(lock_path.c_str(), GENERIC_READ, 0, &attributes, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw_lock != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        need(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION);
        Sleep(5); // Worker only; allow another process to finish its short creation.
    }
    NativeHandle lock(raw_lock);
    need(lock.valid() && GetFileType(lock.get()) == FILE_TYPE_DISK);
    FILE_ATTRIBUTE_TAG_INFO lock_info{};
    need(GetFileInformationByHandleEx(lock.get(), FileAttributeTagInfo, &lock_info, sizeof(lock_info)) &&
         !(lock_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
    std::size_t count = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory_)) {
        const auto name = item.path().filename().native();
        if (name.starts_with(prefix) && name.ends_with(L".jsonl") && ++count >= directory_files) {
            status_ = InputLogStatus::Full;
            return;
        }
    }
    const auto id = learning_client_id();
    const auto path = directory_ / (std::wstring(prefix) + std::wstring(id.begin(), id.end()) + L".jsonl");
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    need(file_ != INVALID_HANDLE_VALUE);
}
InputLogStatus InputLog::write(const InputLogRecord& record) noexcept {
    if (directory_.empty()) return InputLogStatus::Off;
    if (status_ == InputLogStatus::Full || status_ == InputLogStatus::Unavailable) return status_;
    try {
        // Encoding validates lengths and escapes every supplied text field.
        const auto request = encode_request(record.request);
        const auto response = record.response ? encode_response(*record.response) : std::string("null");
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char timestamp[32]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
                  utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        const auto line = "{\"format\":\"nskkzime-input-log\",\"version\":1,\"utc\":\"" + std::string(timestamp) +
            "\",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"status\":\"" + status_name(record.status) +
            "\",\"request\":" + request + ",\"response\":" + response + "}\n";
        if (bytes_ + line.size() > file_limit) return status_ = InputLogStatus::Full;
        if (file_ == INVALID_HANDLE_VALUE) open();
        if (status_ == InputLogStatus::Full) return status_;
        DWORD written = 0;
        need(WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size());
        bytes_ += line.size();
        return status_ = InputLogStatus::Written;
    } catch (...) {
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        return status_ = InputLogStatus::Unavailable;
    }
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
