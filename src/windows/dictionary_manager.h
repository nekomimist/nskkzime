#pragma once
#include "learning_ipc.h"
#include "nskkzime/dictionary.h"
#include <array>

namespace nskkzime::windows {
struct DictionaryRow {
    LearningEvent entry;
    // Reading, okuri key, exact suffix (empty means generic), text, annotation.
    std::array<std::wstring, 5> columns;
};
struct DictionaryImportPreview {
    std::filesystem::path file, source;
    std::string payload; // Frozen at preflight, including the retry identity.
    std::size_t candidates = 0, added = 0;
    DictionaryEncoding encoding = DictionaryEncoding::Auto;
};
std::filesystem::path dictionary_manager_helper_path();
std::vector<DictionaryRow> read_dictionary_rows(const std::filesystem::path&);
std::vector<std::size_t> filter_dictionary_rows(const std::vector<DictionaryRow>&, std::wstring_view);
DictionaryImportPreview prepare_dictionary_import(const std::filesystem::path& file,
                                                  const std::filesystem::path& source,
                                                  DictionaryEncoding = DictionaryEncoding::Auto);
// Synchronous I/O: GUI callers use background workers. Updates go through the
// single writer and never replace a live database with the preflight snapshot.
void apply_dictionary_import(const DictionaryImportPreview&, const std::filesystem::path& server);
void forget_dictionary_entry(const std::filesystem::path&, const LearningEvent&, const std::filesystem::path& server);
void export_user_dictionary(const std::filesystem::path& file, const std::filesystem::path& destination);
} // namespace nskkzime::windows
