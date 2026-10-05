#include "dictionary_settings.h"
#include "nskkzime/dictionary.h"
#include "learning_ipc.h"
#include "local_storage.h"
#include <iostream>
#include <stdexcept>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void help() {
    std::cout << "Usage: nskkzime_user_dictionary [--file ABSOLUTE_PATH] ACTION\n"
        "  --list                               List saved entries as UTF-8 JSON Lines\n"
        "  --export ABSOLUTE_PATH               Write a new UTF-8 SKK file (no overwrite)\n"
        "  --check-import ABSOLUTE_PATH         Validate an SKK import without saving\n"
        "  --import ABSOLUTE_PATH               Append an entire SKK file atomically\n"
        "             [--encoding auto|utf-8|euc-jp]\n"
        "  --forget READING TEXT                Remove one spelling from the user dictionary\n"
        "             [--okuri LETTER SUFFIX]    Select an exact okuri key and hiragana suffix\n"
        "             [--okuri-key LETTER]       Select generic candidates for an okuri key\n"
        "Default file: same user dictionary as the IME; environment overrides apply.\n"
        "Shared dictionaries are unchanged. No word text is printed by --forget.\n";
}
std::u32string text(const wchar_t* value) { return from_utf8(utf8_message(value)); }
std::string json_string(std::u32string_view value) {
    std::string result = "\"";
    for (const auto ch : utf8(value)) {
        if (ch == '\\' || ch == '"') result += '\\';
        result += ch; // Stored strings have already been validated to exclude controls.
    }
    return result + '"';
}
std::filesystem::path helper_path() {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length == ARRAYSIZE(path)) throw std::runtime_error("cannot locate dictionary helper");
    return std::filesystem::path(path).parent_path() / L"nskkzime_dictionary_server.exe";
}
void send_update(const std::filesystem::path& file, const std::string& bytes, DWORD timeout) {
    const auto deadline = GetTickCount64() + timeout;
    bool launched = false;
    while (GetTickCount64() < deadline) {
        std::string response;
        try {
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            response = learning_exchange(file,bytes,static_cast<DWORD>(deadline-now));
        }
        catch (const std::exception&) {
            if (!launched) { const auto child = launch_dictionary_server(helper_path(), file); launched = true; }
            Sleep(100); continue;
        }
        if (response != "OKAY") throw std::runtime_error("dictionary update rejected; check file access and dictionary helper version");
        return;
    }
    throw std::runtime_error("dictionary update could not be confirmed; inspect --list before retrying");
}
void import_dictionary(const std::filesystem::path& file, const std::filesystem::path& source, DictionaryEncoding encoding, bool check_only) {
    if (!source.is_absolute()) throw std::invalid_argument("import requires an absolute source path");
    if (std::filesystem::exists(file) && std::filesystem::equivalent(source,file))
        throw std::invalid_argument("source must be a separate SKK file");
    const auto loaded = SkkDictionary::load(source,encoding);
    const auto& report = loaded->report();
    if (report.malformed_lines || report.unsupported_candidates) {
        for (const auto& issue : report.diagnostics) std::cerr << issue << '\n';
        throw std::invalid_argument("entire import rejected: malformed lines or unsupported candidates");
    }
    if (report.candidates > import_max_candidates)
        throw std::invalid_argument("import exceeds 50000 candidates; configure large shared dictionaries as read-only dictionaries");
    const ImportRequest request{learning_client_id(),1,loaded->entries()};
    auto proposed = read_user_dictionary(file);
    const auto before = proposed.entries().size();
    std::string bytes;
    if (!request.entries.empty()) {
        bytes = encode_import(request); proposed.apply(request);
    }
    const auto added = proposed.entries().size() - before;
    std::cout << "Validated " << request.entries.size() << " source candidates (" << encoding_name(report.encoding)
        << "): " << added << " new, " << request.entries.size() - added << " already present.\n";
    if (check_only || bytes.empty()) { std::cout << "User dictionary unchanged.\n"; return; }
    // Preflight used a snapshot. The helper merges against its latest generation
    // in one transaction, so concurrent learning cannot be overwritten.
    send_update(file,bytes,20000);
    std::cout << "Import saved. Existing priorities and annotations were preserved.\n";
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help")) { help(); return 0; }
        int index = 1;
        std::filesystem::path file;
        if (std::wstring_view(argv[index]) == L"--file") {
            if (argc < 4) { help(); return 2; }
            file = argv[++index]; ++index;
        } else file = learning_file_path();
        // Validates paths even for the read-only commands.
        (void)learning_pipe_name(file);
        const std::wstring_view action(argv[index++]);
        if (action == L"--import" || action == L"--check-import") {
            if (argc-index != 1 && argc-index != 3) { help(); return 2; }
            auto encoding = DictionaryEncoding::Auto;
            if (argc-index == 3) {
                if (std::wstring_view(argv[index+1]) != L"--encoding") { help(); return 2; }
                const std::wstring_view name(argv[index+2]);
                if (name == L"utf-8") encoding = DictionaryEncoding::Utf8;
                else if (name == L"euc-jp") encoding = DictionaryEncoding::EucJp;
                else if (name != L"auto") throw std::invalid_argument("encoding must equal auto, utf-8 or euc-jp");
            }
            import_dictionary(file,argv[index],encoding,action == L"--check-import"); return 0;
        }
        if (action == L"--forget") {
            if (argc - index != 2 && argc - index != 4 && argc - index != 5) { help(); return 2; }
            LearningRequest request{learning_client_id(), 1, {{text(argv[index])}, {}, {"", text(argv[index + 1])}}, DictionaryAction::Forget};
            index += 2;
            if (index != argc) {
                const std::wstring_view letter(argv[index + 1]);
                const std::wstring_view option(argv[index]);
                if (letter.size() != 1 || letter[0] < L'a' || letter[0] > L'z' ||
                    !((option == L"--okuri" && argc-index == 3) || (option == L"--okuri-key" && argc-index == 2)))
                    throw std::invalid_argument("use --okuri LETTER SUFFIX or --okuri-key LETTER with a lowercase ASCII letter");
                request.event.key.okuri = static_cast<char>(letter[0]);
                if (option == L"--okuri") {
                    request.event.okuri = text(argv[index+2]);
                    if (request.event.okuri.empty()) throw std::invalid_argument("--okuri requires a nonempty suffix; use --okuri-key for generic candidates");
                }
            }
            send_update(file,encode_learning(request),6000);
            std::cout << "User dictionary entry forgotten. Shared dictionary candidates remain available.\n"; return 0;
        }
        if (action != L"--list" && action != L"--export") { help(); return 2; }
        if (index + (action == L"--export" ? 1 : 0) != argc) { help(); return 2; }
        const auto dictionary = read_user_dictionary(file);
        if (action == L"--export") {
            // Refuse to replace any existing path, including the source database,
            // its aliases, or shared dictionaries. The final rename enforces this.
            write_file_atomic(argv[index], dictionary.export_skk(), false);
            std::cout << "Exported saved entries to a new UTF-8 SKK file.\n";
        } else {
            for (const auto& entry : dictionary.entries()) {
                const auto letter = entry.key.okuri ? std::u32string(1, static_cast<char32_t>(entry.key.okuri)) : std::u32string{};
                std::cout << "{\"reading\":" << json_string(entry.key.reading) << ",\"okuri_key\":" << json_string(letter)
                    << ",\"okuri\":" << json_string(entry.okuri) << ",\"text\":" << json_string(entry.candidate.text)
                    << ",\"annotation\":" << json_string(entry.candidate.annotation) << "}\n";
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nskkzime_user_dictionary: " << error.what() << '\n'; return 2;
    }
}
