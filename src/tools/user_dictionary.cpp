#include "dictionary_settings.h"
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
        "  --forget READING TEXT                Remove one spelling from the user dictionary\n"
        "             [--okuri LETTER SUFFIX]    Select an exact okuri key and hiragana suffix\n"
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
void forget(const std::filesystem::path& file, const LearningRequest& request) {
    const auto bytes = encode_learning(request); // Reject invalid arguments before launching a helper.
    const auto deadline = GetTickCount64() + 6000;
    bool launched = false;
    while (GetTickCount64() < deadline) {
        std::string response;
        try { response = learning_exchange(file, bytes); }
        catch (const std::exception&) {
            if (!launched) { const auto child = launch_dictionary_server(helper_path(), file); launched = true; }
            Sleep(100); continue;
        }
        if (response != "OKAY") throw std::runtime_error("dictionary update rejected; check file access and dictionary helper version");
        std::cout << "User dictionary entry forgotten. Shared dictionary candidates remain available.\n";
        return;
    }
    throw std::runtime_error("dictionary update could not be confirmed; inspect --list before retrying");
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
        if (action == L"--forget") {
            if (argc - index != 2 && argc - index != 5) { help(); return 2; }
            LearningRequest request{learning_client_id(), 1, {{text(argv[index])}, {}, {"", text(argv[index + 1])}}, DictionaryAction::Forget};
            index += 2;
            if (index != argc) {
                const std::wstring_view letter(argv[index + 1]);
                if (std::wstring_view(argv[index]) != L"--okuri" || letter.size() != 1 || letter[0] < L'a' || letter[0] > L'z')
                    throw std::invalid_argument("--okuri requires one lowercase ASCII letter and a suffix");
                request.event.key.okuri = static_cast<char>(letter[0]); request.event.okuri = text(argv[index + 2]);
            }
            forget(file, request); return 0;
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
