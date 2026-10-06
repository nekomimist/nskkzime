#include "dictionary_manager.h"
#include "dictionary_settings.h"
#include "local_storage.h"
#include <algorithm>

namespace nskkzime::windows {
namespace {
void send_update(const std::filesystem::path& file, const std::filesystem::path& server, const std::string& bytes,
                 DWORD timeout) {
    const auto deadline = GetTickCount64() + timeout;
    bool launched = false;
    while (GetTickCount64() < deadline) {
        std::string response;
        try {
            const auto now = GetTickCount64();
            if (now >= deadline)
                break;
            response = learning_exchange(file, bytes, static_cast<DWORD>(deadline - now));
        } catch (const std::exception&) {
            if (!launched) {
                const auto child = launch_dictionary_server(server, file);
                launched = true;
            }
            Sleep(100);
            continue;
        }
        if (response != "OKAY")
            throw std::runtime_error("dictionary update rejected; check file access and dictionary helper version");
        return;
    }
    throw std::runtime_error("dictionary update could not be confirmed; reload the list before retrying");
}
std::wstring wide(std::u32string_view value) { return wide_message(utf8(value)); }
} // namespace
std::filesystem::path dictionary_manager_helper_path() {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length == ARRAYSIZE(path))
        throw std::runtime_error("cannot locate dictionary helper");
    return std::filesystem::path(path).parent_path() / L"nskkzime_dictionary_server.exe";
}
std::vector<DictionaryRow> read_dictionary_rows(const std::filesystem::path& file) {
    (void)learning_pipe_name(file);
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
    (void)learning_pipe_name(file);
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
    DictionaryImportPreview result{file, source, {}, request.entries.size(), 0, report.encoding};
    if (!request.entries.empty()) {
        result.payload = encode_import(request);
        proposed.apply(request);
    }
    result.added = proposed.entries().size() - before;
    return result;
}
void apply_dictionary_import(const DictionaryImportPreview& preview, const std::filesystem::path& server) {
    if (!preview.payload.empty())
        send_update(preview.file, server, preview.payload, 20000);
}
void forget_dictionary_entry(const std::filesystem::path& file, const LearningEvent& entry,
                             const std::filesystem::path& server) {
    const LearningRequest request{learning_client_id(), 1, entry, DictionaryAction::Forget};
    send_update(file, server, encode_learning(request), 6000);
}
void export_user_dictionary(const std::filesystem::path& file, const std::filesystem::path& destination) {
    (void)learning_pipe_name(file);
    write_file_atomic(destination, read_user_dictionary(file).export_skk(), false);
}
} // namespace nskkzime::windows
