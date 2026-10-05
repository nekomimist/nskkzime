#include "dictionary_cache.h"
#include "nskkzime/compiled_dictionary.h"
#include "learning_ipc.h"
#include "local_storage.h"
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <stdexcept>
namespace nskkzime::windows {
namespace {
// Envelope: magic[8], identity SHA-256[32], payload SHA-256[32], payload bytes(u64 LE).
constexpr std::size_t envelope = 80, source_limit = 128 * 1024 * 1024;
using Digest = std::array<unsigned char, 32>;
void need(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
Digest hash(std::string_view bytes) {
    need(bytes.size() <= ULONG_MAX, "cache hash input too large");
    Digest result{};
    need(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes.data())),
                    static_cast<ULONG>(bytes.size()), result.data(), static_cast<ULONG>(result.size())) >= 0,
         "cache hash failed");
    return result;
}
std::wstring hex(const Digest &bytes) {
    std::wstring text;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (auto ch : bytes) {
        text += digits[ch >> 4];
        text += digits[ch & 15];
    }
    return text;
}
std::string read_bytes(const std::filesystem::path &path, std::size_t maximum) {
    NativeHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    need(file.valid() && GetFileType(file.get()) == FILE_TYPE_DISK, "cannot open dictionary/cache file");
    LARGE_INTEGER size{};
    need(GetFileSizeEx(file.get(), &size) && size.QuadPart >= 0 && static_cast<ULONGLONG>(size.QuadPart) <= maximum,
         "dictionary/cache file exceeds limit");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    for (std::size_t offset = 0; offset < bytes.size();) {
        DWORD done = 0;
        const auto amount = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{1024 * 1024}));
        need(ReadFile(file.get(), bytes.data() + offset, amount, &done, nullptr) && done,
             "dictionary/cache file read failed");
        offset += done;
    }
    return bytes;
}
std::wstring sid() {
    HANDLE raw = nullptr;
    need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != 0, "cache token unavailable");
    NativeHandle token(raw);
    DWORD length = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> buffer(length);
    need(GetTokenInformation(token.get(), TokenUser, buffer.data(), length, &length) != 0, "cache SID unavailable");
    LPWSTR value = nullptr;
    need(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &value) != 0,
         "cache SID conversion failed");
    std::wstring result;
    try {
        result = value;
    } catch (...) {
        LocalFree(value);
        throw;
    }
    LocalFree(value);
    return result;
}
struct Descriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    explicit Descriptor(const std::wstring &user) {
        const auto text = L"D:P(A;;GR;;;" + user + L")";
        need(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &value, nullptr) != 0,
             "cache ACL unavailable");
    }
    ~Descriptor() { LocalFree(value); }
};
struct BuildLock {
    NativeHandle file;
    OVERLAPPED position{};
    bool held = false;
    explicit BuildLock(const std::filesystem::path &root)
        : file(CreateFileW((root / L"build.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        need(file.valid(), "cache build lock unavailable");
        const auto deadline = GetTickCount64() + 5000;
        for (;;) {
            if (LockFileEx(file.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &position)) {
                held = true;
                break;
            }
            need(GetLastError() == ERROR_LOCK_VIOLATION && GetTickCount64() < deadline, "cache build lock timed out");
            Sleep(10);
        }
    }
    ~BuildLock() {
        if (held)
            UnlockFileEx(file.get(), 0, 1, 0, &position);
    }
};
std::size_t payload_size(std::string_view image, const Digest &source) {
    need(image.size() >= envelope && image.substr(0, 8) == "NSKKCF01" &&
             !std::memcmp(image.data() + 8, source.data(), 32),
         "cache identity mismatch");
    std::uint64_t length = 0;
    for (unsigned i = 0; i < 8; ++i)
        length |= static_cast<std::uint64_t>(static_cast<unsigned char>(image[72 + i])) << (i * 8);
    need(length <= compiled_dictionary_limit && length <= image.size() - envelope, "cache length mismatch");
    const auto digest = hash(image.substr(envelope, static_cast<std::size_t>(length)));
    need(!std::memcmp(image.data() + 40, digest.data(), 32), "cache checksum mismatch");
    return static_cast<std::size_t>(length);
}
struct Mapping {
    NativeHandle handle;
    const char *view = nullptr;
    std::size_t size = 0;
    explicit Mapping(NativeHandle raw) : handle(std::move(raw)) {
        need(handle.valid(), "cache section unavailable");
        view = static_cast<const char *>(MapViewOfFile(handle.get(), FILE_MAP_READ, 0, 0, 0));
        need(view != nullptr, "cache section cannot be mapped");
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(view, &info, sizeof(info)) || info.State != MEM_COMMIT || info.Protect != PAGE_READONLY) {
            UnmapViewOfFile(view);
            view = nullptr;
            throw std::runtime_error("cache section size/protection unavailable");
        }
        size = info.RegionSize;
    }
    ~Mapping() {
        if (view)
            UnmapViewOfFile(view);
    }
};
std::shared_ptr<const CompiledDictionary> mapped(const std::shared_ptr<Mapping> &mapping, const Digest &source) {
    // Pagefile-backed shared memory avoids file-view in-page I/O errors during
    // key handling. Cache disk reads use ReadFile and its normal error path.
    const auto length = payload_size({mapping->view, mapping->size}, source);
    return std::make_shared<CompiledDictionary>(std::string_view(mapping->view + envelope, length), mapping);
}
std::shared_ptr<const CompiledDictionary> create_mapping(const std::wstring &name, const std::wstring &user,
                                                         std::string_view image, const Digest &source) {
    Descriptor descriptor(user);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
    const auto raw = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(image.size()), name.c_str());
    const auto error = GetLastError();
    NativeHandle handle(raw);
    need(handle.valid(), "cannot create cache section");
    // Never write an existing section, including a corrupt/stale one.
    need(error != ERROR_ALREADY_EXISTS, "cache section already exists");
    auto *write = MapViewOfFile(handle.get(), FILE_MAP_WRITE, 0, 0, image.size());
    need(write != nullptr, "cannot initialize cache section");
    std::memcpy(write, image.data(), image.size());
    UnmapViewOfFile(write);
    NativeHandle read(OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()));
    return mapped(std::make_shared<Mapping>(std::move(read)), source);
}
bool owned_file(std::wstring_view name) {
    return name.size() == 73 && name[64] == L'-' && name[65] >= L'0' && name[65] <= L'2' &&
           name.substr(66) == L"-v1.bin" && std::all_of(name.begin(), name.begin() + 64, [](wchar_t ch) {
               return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
           });
}
void prune(const std::filesystem::path &root, const std::filesystem::path &keep) noexcept {
    try {
        struct Item {
            std::filesystem::path file;
            std::uintmax_t size;
            std::filesystem::file_time_type time;
        };
        std::vector<Item> items;
        std::uintmax_t total = 0;
        for (const auto &entry : std::filesystem::directory_iterator(root))
            if (entry.is_regular_file() && owned_file(entry.path().filename().native())) {
                const auto size = entry.file_size();
                total += size;
                items.push_back({entry.path(), size, entry.last_write_time()});
            }
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.time < b.time; });
        auto count = items.size();
        for (const auto &item : items) {
            if (count <= 64 && total <= 1024ull * 1024 * 1024)
                break;
            if (item.file == keep)
                continue;
            std::error_code error;
            if (std::filesystem::remove(item.file, error)) {
                --count;
                total -= item.size;
            }
        }
    } catch (...) {
    }
}
} // namespace
CachedDictionary load_cached_dictionary(const std::filesystem::path &source, const std::filesystem::path &root,
                                        DictionaryEncoding encoding) {
    const auto bytes = read_bytes(source, source_limit);
    std::shared_ptr<const SkkDictionary> parsed;
    std::exception_ptr parse_error;
    auto parse = [&] {
        if (parse_error)
            std::rethrow_exception(parse_error);
        if (!parsed)
            try {
                parsed = SkkDictionary::parse_bytes(bytes, encoding);
            } catch (...) {
                parse_error = std::current_exception();
                throw;
            }
        return parsed;
    };
    std::filesystem::path destination;
    try {
        const auto source_digest = hash(bytes);
        std::string identity(reinterpret_cast<const char *>(source_digest.data()), source_digest.size());
        identity += static_cast<char>(encoding);
        identity += "nskkzime-cache-v1";
        const auto digest = hash(identity);
        need(root.is_absolute() && !root.filename().empty(), "cache path must be absolute");
        std::filesystem::create_directories(root);
        destination = root / (hex(digest) + L"-" + std::to_wstring(static_cast<unsigned>(encoding)) + L"-v1.bin");
        BuildLock lock(root);
        auto section_key = destination.lexically_normal().native();
        CharLowerBuffW(section_key.data(), static_cast<DWORD>(section_key.size()));
        const auto user = sid();
        const auto name =
            L"Local\\nskkzime-dictionary-v1-" + user + L"-" +
            hex(hash({reinterpret_cast<const char *>(section_key.data()), section_key.size() * sizeof(wchar_t)}));
        if (const auto existing = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str())) {
            NativeHandle handle(existing);
            const auto dictionary = mapped(std::make_shared<Mapping>(std::move(handle)), digest);
            return {dictionary, dictionary->report(), DictionaryCacheUse::Shared, destination};
        }
        std::string image;
        bool hit = false;
        try {
            image = read_bytes(destination, compiled_dictionary_limit + envelope);
            const auto size = payload_size(image, digest);
            need(image.size() == envelope + size, "trailing cache data");
            // Validate all offsets/order/Unicode before publishing a named view.
            const auto owner = std::make_shared<std::string>(std::move(image));
            {
                CompiledDictionary checked(std::string_view(*owner).substr(envelope), owner);
            }
            image = std::move(*owner);
            hit = true;
        } catch (...) {
            const auto packed = CompiledDictionary::compile(*parse());
            const auto checksum = hash(packed);
            image.assign(envelope, '\0');
            image.replace(0, 8, "NSKKCF01");
            std::memcpy(image.data() + 8, digest.data(), 32);
            std::memcpy(image.data() + 40, checksum.data(), 32);
            for (unsigned i = 0; i < 8; ++i)
                image[72 + i] = static_cast<char>((static_cast<std::uint64_t>(packed.size()) >> (i * 8)) & 0xff);
            image += packed;
            write_file_atomic(destination, image);
            prune(root, destination);
        }
        const auto dictionary = create_mapping(name, user, image, digest);
        return {dictionary, dictionary->report(), hit ? DictionaryCacheUse::Disk : DictionaryCacheUse::Created,
                destination};
    } catch (...) {
        const auto dictionary = parse();
        return {dictionary, dictionary->report(), DictionaryCacheUse::Fallback, destination};
    }
}
} // namespace nskkzime::windows
