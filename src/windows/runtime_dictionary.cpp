#include "runtime_dictionary.h"
#include "nskkzime/dictionary.h"
#include <windows.h>
#include <mutex>

namespace nskkzime::windows {
std::shared_ptr<const Dictionary> runtime_dictionary() {
    const auto length = GetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", nullptr, 0);
    if (!length) return demo_dictionary();
    std::wstring path(length, L'\0');
    const auto copied = GetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", path.data(), length);
    if (!copied || copied >= length) return std::make_shared<MemoryDictionary>();
    path.resize(copied);
    // Reuse an immutable snapshot while another TIP in this process uses it.
    static std::mutex mutex;
    static std::wstring cached_path;
    static std::weak_ptr<const Dictionary> cached;
    const std::lock_guard lock(mutex);
    if (path == cached_path) if (auto dictionary = cached.lock()) return dictionary;
    try {
        auto dictionary = SkkDictionary::load(std::filesystem::path(path));
        cached_path = path; cached = dictionary;
        if (dictionary->report().malformed_lines || dictionary->report().unsupported_candidates)
            OutputDebugStringW(L"nskkzime: some dictionary entries were skipped; use skk_trace --dict to inspect diagnostics.\n");
        return dictionary;
    } catch (const std::exception& error) {
        OutputDebugStringA("nskkzime: dictionary load failed; kana input remains available: ");
        OutputDebugStringA(error.what()); OutputDebugStringA("\n");
        // Do not silently use demo candidates when a configured file fails.
        return std::make_shared<MemoryDictionary>();
    }
}
}
