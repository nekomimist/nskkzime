#pragma once
#include "nskkzime/dictionary.h"
namespace nskkzime::windows {
enum class DictionaryCacheUse { Shared, Disk, Created, Fallback };
struct CachedDictionary {
    std::shared_ptr<const Dictionary> dictionary;
    DictionaryReport report;
    DictionaryCacheUse use = DictionaryCacheUse::Fallback;
    std::filesystem::path file;
};
// Synchronous worker operation. Cache failure falls back to the same source
// bytes; a missing/invalid source never uses stale data from the cache.
CachedDictionary load_cached_dictionary(const std::filesystem::path &source, const std::filesystem::path &cache_root,
                                        DictionaryEncoding encoding = DictionaryEncoding::Auto);
} // namespace nskkzime::windows
