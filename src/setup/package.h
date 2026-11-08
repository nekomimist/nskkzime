#pragma once
#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace nskkzime::setup {
namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void require(bool condition, const std::string& message);
void win_check(bool condition, const std::string& operation);
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
bool same_path(const fs::path& a, const fs::path& b);
void no_reparse(const fs::path& path);
std::string read_file(const fs::path& path, size_t limit = 128 * 1024 * 1024);
void write_new(const fs::path& path, const std::string& data);
std::string sha256(const std::string& bytes);
Json parse_json(const std::string& bytes);
bool version_id(const std::string& value);
std::vector<std::string> package_names(int format);
struct Bundle {
    fs::path directory;
    std::string version;
    int format = 0;
    std::vector<std::string> files;
    fs::path dll() const { return directory / L"bin/nskkzime_tsf.dll"; }
    fs::path x86() const { return format >= 4 ? directory / L"bin/x86/nskkzime_tsf.dll" : fs::path{}; }
    fs::path broker() const { return format >= 3 ? directory / L"bin/nskkzime_broker.exe" : fs::path{}; }
};
Bundle verify_package(const fs::path& directory);
}
