#include "package.h"
#include <bcrypt.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

namespace nskkzime::setup {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void win_check(bool condition, const std::string& operation) {
    if (!condition) throw std::runtime_error(operation + " (Win32 " + std::to_string(GetLastError()) + ")");
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    win_check(size > 0, "Invalid Unicode path");
    std::string result(static_cast<size_t>(size), '\0');
    win_check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == size, "Encode path");
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    win_check(size > 0, "Invalid UTF-8");
    std::wstring result(static_cast<size_t>(size), L'\0');
    win_check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) == size, "Decode path");
    return result;
}
bool same_path(const fs::path& a, const fs::path& b) {
    const auto left = a.lexically_normal().make_preferred(), right = b.lexically_normal().make_preferred();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
void no_reparse(const fs::path& path) {
    for (auto current = fs::absolute(path).lexically_normal(); !current.empty();) {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "Cannot inspect path: " + utf8(current.native()));
        } else require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Reparse point is not allowed: " + utf8(current.native()));
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}
std::string read_file(const fs::path& path, size_t limit) {
    no_reparse(path);
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    win_check(file.value != INVALID_HANDLE_VALUE, "Open " + utf8(path.native()));
    BY_HANDLE_FILE_INFORMATION info{};
    win_check(GetFileInformationByHandle(file.value, &info), "Inspect file");
    require(!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "Expected ordinary file");
    const auto length = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    require(length <= limit, "File size limit exceeded");
    std::string bytes(static_cast<size_t>(length), '\0');
    DWORD read = 0;
    win_check(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr), "Read file");
    require(read == bytes.size(), "File length changed");
    return bytes;
}
void write_new(const fs::path& path, const std::string& data) {
    no_reparse(path);
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    win_check(file.value != INVALID_HANDLE_VALUE, "Create " + utf8(path.native()));
    DWORD written = 0;
    win_check(WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr), "Write file");
    require(written == data.size(), "Incomplete file write");
    win_check(FlushFileBuffers(file.value), "Flush file");
}
std::string sha256(const std::string& bytes) {
    unsigned char digest[32]{};
    require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) >= 0, "SHA-256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
Json parse_json(const std::string& bytes) {
    require(bytes.size() <= 1024 * 1024, "JSON size limit exceeded");
    std::vector<std::set<std::wstring>> keys;
    // Reject ambiguous properties, including case variants, before DOM construction.
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 64, "JSON nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key) {
            auto key = wide(value.get<std::string>());
            for (auto& c : key) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            require(keys.back().insert(key).second, "Duplicate JSON property");
        }
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
bool version_id(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::vector<std::string> package_names(int format) {
    require((format >= 2 && format <= 6) || format == 8, "Unsupported x64 package format");
    std::vector<std::string> names{"bin/nskkzime_tsf.dll", "bin/nskkzime_settings.exe", "bin/nskkzime_dictionary_gui.exe",
        "bin/nskkzime_config.exe", "bin/nskkzime_user_dictionary.exe", "bin/ime_probe.exe", "bin/nkkz-ime-server.exe",
        "licenses/nlohmann-MIT.txt", "licenses/Unicode-3.0.txt", "licenses/skk-zenz-MIT.txt", "licenses/llama-MIT.txt",
        "README.md", "tools/package.ps1", "tools/install.ps1", "tools/installation.psm1"};
    if (format >= 3) names.emplace_back("bin/nskkzime_broker.exe");
    if (format >= 4) { names.emplace_back("bin/x86/nskkzime_tsf.dll"); names.emplace_back("bin/x86/ime_probe.exe"); }
    if (format <= 4) names.emplace_back("bin/nskkzime_dictionary_server.exe");
    if (format >= 6) { names.emplace_back("LICENSE"); names.emplace_back("tools/scoop.ps1"); }
    if (format == 8) names.emplace_back("bin/nskkzime_setup.exe");
    std::sort(names.begin(), names.end());
    return names;
}
namespace {
uint32_t number(const std::string& bytes, size_t offset, size_t count) {
    require(offset <= bytes.size() && count <= bytes.size() - offset, "Truncated PE image");
    uint32_t result = 0;
    for (size_t i = 0; i < count; ++i) result |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return result;
}
bool matches(const Json& value, const char* pattern) { return value.is_string() && std::regex_match(value.get<std::string>(), std::regex(pattern)); }
bool metadata_equal(const Json& left, const Json& right) {
    if (!left.is_object() || !right.is_object() || left.size() != right.size()) return false;
    for (auto it = left.begin(); it != left.end(); ++it) {
        if (!right.contains(it.key())) return false;
        const auto& value = right.at(it.key());
        if (it->is_number_integer() ? !value.is_number_integer() : it->type() != value.type()) return false;
        if (*it != value) return false;
    }
    return true;
}
Json metadata(const std::string& bytes, const std::string& name) {
    const bool x86 = name.starts_with("bin/x86/");
    require(number(bytes, 0, 2) == 0x5a4d, "Invalid DOS image: " + name);
    const auto pe = static_cast<size_t>(number(bytes, 60, 4));
    require(number(bytes, pe, 4) == 0x4550 && number(bytes, pe + 4, 2) == (x86 ? 0x14cu : 0x8664u) &&
        number(bytes, pe + 24, 2) == (x86 ? 0x10bu : 0x20bu), "Wrong PE architecture: " + name);
    const auto count = number(bytes, pe + 6, 2);
    const auto table = pe + 24 + number(bytes, pe + 20, 2);
    require(count >= 1 && count <= 96 && table <= bytes.size() && count * 40 <= bytes.size() - table, "Invalid PE sections");
    Json found;
    for (size_t i = 0; i < count; ++i) {
        const auto section = table + i * 40;
        if (bytes.substr(section, 8) != std::string(".nskkz\0\0", 8)) continue;
        require(found.is_null(), "Duplicate build metadata section");
        const auto size = static_cast<size_t>(number(bytes, section + 16, 4));
        const auto offset = static_cast<size_t>(number(bytes, section + 20, 4));
        require(size >= 2 && size <= 8192 && offset >= table + count * 40 && offset <= bytes.size() && size <= bytes.size() - offset &&
            (number(bytes, section + 36, 4) & 0xe0000000u) == 0x40000000u, "Invalid build metadata section");
        const auto end = bytes.find('\0', offset);
        require(end != std::string::npos && end > offset + 1 && end < offset + size && end - offset <= 4096, "Invalid build metadata length");
        found = parse_json(bytes.substr(offset, end - offset));
    }
    const std::vector<std::string> fields{"format_version", "source_revision", "source_sha256", "source_modified", "configuration", "compiler",
        "variant", "cmake_cache_sha256", "llama_revision", "llama_source_sha256", "binary"};
    require(found.is_object() && found.size() == fields.size(), "Missing build metadata");
    for (const auto& key : fields) require(found.contains(key), "Missing metadata property: " + key);
    require(found["format_version"].is_number_integer() && found["format_version"] == 1 &&
        matches(found["source_revision"], "([a-f0-9]{40}|[a-f0-9]{64})") && matches(found["source_sha256"], "[a-f0-9]{64}") &&
        matches(found["cmake_cache_sha256"], "[a-f0-9]{64}") && found["source_modified"].is_boolean() && found["configuration"] == "Release" &&
        matches(found["compiler"], "MSVC [0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?") &&
        found["variant"] == (x86 ? "x86-companion" : "zenz-cpu") && found["binary"] == utf8(fs::path(wide(name)).filename().native()), "Invalid build metadata");
    require(x86 ? (found["llama_revision"].is_null() && found["llama_source_sha256"].is_null()) :
        (matches(found["llama_revision"], "[a-f0-9]{40}") && matches(found["llama_source_sha256"], "[a-f0-9]{64}")), "Invalid backend provenance");
    found.erase("binary");
    return found;
}
}
Bundle verify_package(const fs::path& directory) {
    Bundle bundle;
    bundle.directory = fs::absolute(directory).lexically_normal();
    no_reparse(bundle.directory);
    const auto manifest_bytes = read_file(bundle.directory / L"manifest.json", 1024 * 1024);
    const auto manifest = parse_json(manifest_bytes);
    require(manifest.at("format_version").is_number_integer(), "Invalid format version");
    bundle.format = manifest.at("format_version").get<int>();
    bundle.files = package_names(bundle.format);
    require(manifest.at("product") == "nskkzime" && manifest.at("architecture") == "x64" && manifest.at("variant") == "zenz-cpu" &&
        manifest.at("configuration") == "Release", "Not an x64 release package");
    if (bundle.format >= 6) require(matches(manifest.at("version"), "[0-9]+\\.[0-9]+\\.[0-9]+") && manifest.at("first_party_license") == "MIT", "Invalid release identity");
    auto expected = std::set<std::string>(bundle.files.begin(), bundle.files.end());
    expected.insert("manifest.json");
    size_t total = manifest_bytes.size();
    std::set<std::string> seen;
    const auto& files = manifest.at("files");
    require(files.is_array() && files.size() == bundle.files.size(), "Invalid package inventory");
    std::map<std::string, Json> identities;
    for (const auto& entry : files) {
        const auto name = entry.at("path").get<std::string>();
        require(name != "manifest.json" && expected.contains(name) && seen.insert(name).second, "Unexpected or duplicate package file: " + name);
        const auto bytes = read_file(bundle.directory / wide(name));
        total += bytes.size();
        require(total <= 256 * 1024 * 1024, "Package size limit exceeded");
        require(entry.at("size").is_number_integer() && entry.at("size") == bytes.size() && entry.at("sha256") == sha256(bytes), "File hash or size mismatch: " + name);
        if (name.starts_with("bin/") && (bundle.format >= 4 || manifest.contains("binary_build_metadata"))) {
            const auto info = metadata(bytes, name);
            const std::string group = name.starts_with("bin/x86/") ? "x86" : "x64";
            require(!identities.contains(group) || metadata_equal(identities[group], info), "Mixed binary builds");
            identities[group] = info;
        }
    }
    for (const auto& item : fs::recursive_directory_iterator(bundle.directory)) {
        no_reparse(item.path());
        const auto relative = utf8(item.path().lexically_relative(bundle.directory).generic_wstring());
        if (item.is_directory()) require(relative == "bin" || relative == "bin/x86" || relative == "tools" || relative == "licenses", "Unexpected package directory");
        else require(item.is_regular_file() && expected.contains(relative), "Unexpected package file: " + relative);
    }
    if (!identities.empty()) {
        require(metadata_equal(identities.at("x64"), manifest.at("binary_build_metadata")) &&
            manifest.at("binary_build_revision") == identities.at("x64").at("source_revision"), "Manifest build metadata mismatch");
        if (bundle.format >= 4) {
            require(metadata_equal(identities.at("x86"), manifest.at("x86_build_metadata")), "Companion metadata mismatch");
            for (const auto* key : {"source_revision", "source_sha256", "source_modified", "configuration", "compiler"})
                require(identities.at("x64").at(key) == identities.at("x86").at(key), "Companion source mismatch");
        }
    }
    bundle.files.emplace_back("manifest.json");
    bundle.version = sha256(manifest_bytes);
    return bundle;
}
}
