#include "local_storage.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace nskkzime::windows {
void validate_storage_path(const std::filesystem::path& path) {
    const auto& value = path.native();
    if (!path.is_absolute() || std::any_of(value.begin(), value.end(), [](wchar_t ch) { return ch < 0x20 || ch == 0x7f; }) ||
        value.back() == L' ') throw std::invalid_argument("path must be absolute, without control characters or trailing spaces");
}
namespace {
struct TemporaryFile {
    std::filesystem::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool created = false;
    ~TemporaryFile() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (created) DeleteFileW(path.c_str()); // Only the file created by this save.
    }
};
}
void write_file_atomic(const std::filesystem::path& path, std::string_view bytes, bool replace_existing) {
    validate_storage_path(path);
    if (bytes.size() > 64 * 1024 * 1024) throw std::invalid_argument("file exceeds 64 MiB");
    std::filesystem::create_directories(path.parent_path());
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("cannot allocate temporary settings name");
    wchar_t suffix[40]{}; StringFromGUID2(guid, suffix, ARRAYSIZE(suffix));
    TemporaryFile temporary;
    temporary.path = path.native() + L"." + suffix + L".tmp";
    // Readers may open the published name while this handle still flushes the rename.
    temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporary.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create temporary settings file");
    temporary.created = true;
    DWORD written = 0;
    if (!WriteFile(temporary.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
        !FlushFileBuffers(temporary.handle)) throw std::runtime_error("cannot write settings; previous file was kept");
    // POSIX semantics replace the name even while a reader holds the old file
    // with FILE_SHARE_DELETE (e.g. IMEs reading the user dictionary); the reader
    // keeps its complete old generation. Other file systems use MoveFileEx.
    bool renamed = false;
    if (replace_existing) {
        const auto& target = path.native();
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, target.c_str(), info->FileNameLength);
        renamed = SetFileInformationByHandle(temporary.handle, FileRenameInfoEx, info, static_cast<DWORD>(buffer.size())) != FALSE;
        if (renamed) { temporary.created = false; FlushFileBuffers(temporary.handle); }
    }
    CloseHandle(temporary.handle); temporary.handle = INVALID_HANDLE_VALUE;
    if (!renamed && !MoveFileExW(temporary.path.c_str(), path.c_str(), (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot replace settings; previous file was kept");
    temporary.created = false;
}
} // namespace nskkzime::windows
