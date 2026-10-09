#pragma once
#include <windows.h>
#include <filesystem>
#include <string_view>

namespace nskkzime::windows {
enum class DiagnosticFileStatus { Ready, Written, Full, Unavailable };
// Worker-only, bounded, append-only JSONL sink. Each family has its own quota.
class DiagnosticFile {
public:
    DiagnosticFile(std::filesystem::path directory, std::wstring_view prefix, std::wstring_view lock);
    ~DiagnosticFile();
    DiagnosticFile(const DiagnosticFile&) = delete;
    DiagnosticFile& operator=(const DiagnosticFile&) = delete;
    DiagnosticFileStatus append(std::string_view) noexcept;
    static constexpr std::size_t file_limit = 1024 * 1024, directory_files = 64;
private:
    void open();
    std::filesystem::path directory_;
    std::wstring prefix_, lock_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::size_t bytes_ = 0;
    DiagnosticFileStatus status_ = DiagnosticFileStatus::Ready;
};
void validate_diagnostic_directory(const std::filesystem::path&, bool check_exists);
} // namespace nskkzime::windows
