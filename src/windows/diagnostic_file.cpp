#include "diagnostic_file.h"
#include "learning_ipc.h"
#include <sddl.h>
#include <algorithm>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
void need(bool value) { if (!value) throw std::runtime_error("diagnostic log unavailable"); }
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
}
void validate_diagnostic_directory(const std::filesystem::path& path, bool check_exists) {
    const auto& value = path.native();
    need(path.is_absolute() && path.root_name().native().size() == 2 && !value.empty() && value.back() != L' ' &&
         std::none_of(value.begin(), value.end(), [](wchar_t ch) { return ch < 0x20 || ch == 0x7f || ch == L'"'; }));
    if (!check_exists) return;
    const auto drive = GetDriveTypeW(path.root_path().c_str());
    need(drive == DRIVE_FIXED || drive == DRIVE_REMOVABLE);
    for (auto part = path; !part.empty();) {
        const auto attributes = GetFileAttributesW(part.c_str());
        need(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
        const auto parent = part.parent_path();
        if (parent == part) break;
        part = parent;
    }
}
DiagnosticFile::DiagnosticFile(std::filesystem::path directory, std::wstring_view prefix, std::wstring_view lock)
    : directory_(std::move(directory)), prefix_(prefix), lock_(lock) {}
DiagnosticFile::~DiagnosticFile() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
void DiagnosticFile::open() {
    validate_diagnostic_directory(directory_, true);
    Descriptor descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
    HANDLE raw_lock = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10; ++attempt) {
        raw_lock = CreateFileW((directory_ / lock_).c_str(), GENERIC_READ, 0, &attributes, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw_lock != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        need(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION);
        Sleep(5);
    }
    NativeHandle lock(raw_lock);
    need(lock.valid() && GetFileType(lock.get()) == FILE_TYPE_DISK);
    FILE_ATTRIBUTE_TAG_INFO info{};
    need(GetFileInformationByHandleEx(lock.get(), FileAttributeTagInfo, &info, sizeof(info)) && !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
    std::size_t count = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory_)) {
        const auto name = item.path().filename().native();
        if (name.starts_with(prefix_) && name.ends_with(L".jsonl") && ++count >= directory_files) {
            status_ = DiagnosticFileStatus::Full; return;
        }
    }
    const auto id = learning_client_id();
    const auto path = directory_ / (prefix_ + std::wstring(id.begin(), id.end()) + L".jsonl");
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    need(file_ != INVALID_HANDLE_VALUE);
}
DiagnosticFileStatus DiagnosticFile::append(std::string_view line) noexcept {
    if (status_ == DiagnosticFileStatus::Full || status_ == DiagnosticFileStatus::Unavailable) return status_;
    try {
        if (bytes_ + line.size() > file_limit) return status_ = DiagnosticFileStatus::Full;
        if (file_ == INVALID_HANDLE_VALUE) open();
        if (status_ == DiagnosticFileStatus::Full) return status_;
        DWORD written = 0;
        need(WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size());
        bytes_ += line.size();
        return status_ = DiagnosticFileStatus::Written;
    } catch (...) {
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        return status_ = DiagnosticFileStatus::Unavailable;
    }
}
} // namespace nskkzime::windows
