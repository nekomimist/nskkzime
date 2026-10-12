#include "dictionary_manager.h"
#include "dictionary_settings.h"
#include "local_storage.h"
#include <algorithm>

namespace nskkzime::windows {
namespace {
std::wstring wide(std::u32string_view value) { return wide_message(utf8(value)); }
} // namespace
std::vector<DictionaryRow> read_dictionary_rows(const std::filesystem::path& file) {
    validate_learning_file(file);
    auto entries = read_user_dictionary(file).entries();
    std::vector<DictionaryRow> rows;
    rows.reserve(entries.size());
    for (auto& entry : entries) {
        std::array<std::wstring, 5> columns{
            wide(entry.key.reading), entry.key.okuri ? std::wstring(1, static_cast<wchar_t>(entry.key.okuri)) : L"",
            wide(entry.okuri), wide(entry.candidate.text), wide(entry.candidate.annotation)};
        rows.push_back({std::move(entry), std::move(columns)});
    }
    return rows;
}
std::vector<std::size_t> filter_dictionary_rows(const std::vector<DictionaryRow>& rows, std::wstring_view text) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (text.empty() || std::any_of(rows[i].columns.begin(), rows[i].columns.end(),
                                        [&](const auto& value) { return value.find(text) != std::wstring::npos; }))
            result.push_back(i);
    return result;
}
DictionaryImportPreview prepare_dictionary_import(const std::filesystem::path& file,
                                                  const std::filesystem::path& source, DictionaryEncoding encoding) {
    validate_learning_file(file);
    if (!source.is_absolute())
        throw std::invalid_argument("import requires an absolute source path");
    if (std::filesystem::exists(file) && std::filesystem::equivalent(source, file))
        throw std::invalid_argument("source must be a separate SKK file");
    const auto loaded = SkkDictionary::load(source, encoding);
    const auto& report = loaded->report();
    if (report.malformed_lines || report.unsupported_candidates) {
        std::string message = "entire import rejected: malformed lines or unsupported candidates";
        for (const auto& issue : report.diagnostics)
            message += "\n" + issue;
        throw std::invalid_argument(message);
    }
    if (report.candidates > import_max_candidates)
        throw std::invalid_argument(
            "import exceeds 50000 candidates; configure large shared dictionaries as read-only dictionaries");
    const ImportRequest request{learning_client_id(), 1, loaded->entries()};
    auto proposed = read_user_dictionary(file);
    const auto before = proposed.entries().size();
    DictionaryImportPreview result{file, source, request, request.entries.size(), 0, report.encoding};
    if (!request.entries.empty())
        proposed.apply(request);
    result.added = proposed.entries().size() - before;
    return result;
}
void apply_dictionary_import(const DictionaryImportPreview& preview) {
    if (!preview.request.entries.empty())
        (void)commit_import(preview.file, preview.request, 20000);
}
void forget_dictionary_entry(const std::filesystem::path& file, const LearningEvent& entry) {
    const LearningRequest request{learning_client_id(), 1, entry, DictionaryAction::Forget};
    (void)commit_learning(file, request, 6000);
}
void export_user_dictionary(const std::filesystem::path& file, const std::filesystem::path& destination) {
    validate_learning_file(file);
    write_file_atomic(destination, read_user_dictionary(file).export_skk(), false);
}
} // namespace nskkzime::windows
