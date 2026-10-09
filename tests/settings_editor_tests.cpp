#include "settings_editor.h"
#include "local_storage.h"
#include "learning_ipc.h"
#include <fstream>
#include <iostream>
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F action) {
    bool caught = false;
    try {
        action();
    } catch (const std::exception&) {
        caught = true;
    }
    check(caught, "operation unexpectedly succeeded");
}
struct Files {
    std::filesystem::path root = std::filesystem::current_path() / ("settings-editor-test-" + learning_client_id());
    std::vector<std::filesystem::path> files;
    Files() { std::filesystem::create_directory(root); }
    std::filesystem::path path(const wchar_t* name) {
        auto file = root / name;
        files.push_back(file);
        return file;
    }
    ~Files() {
        std::error_code error;
        for (const auto& file : files)
            std::filesystem::remove(file, error);
        std::filesystem::remove(root, error);
    }
};
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 3)
        return 2;
    Files files;
    const std::filesystem::path dictionary = argv[1], romaji = argv[2];
    int failed = 0;
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"new-document-no-write-before-save",
         [&] {
             const auto path = files.path(L"新しい 設定.ini");
             auto document = open_settings_document(path);
             check(!document.original && document.settings.learning && !std::filesystem::exists(path),
                   "opening created settings");
             auto settings = document.settings;
             settings.dictionaries = {dictionary};
             settings.learning = false;
             settings.romaji_rules = romaji;
             settings.candidates = {5, CandidateKeys::Asdfjkl};
             check(check_settings_draft(settings).valid, "valid configuration rejected");
             save_settings_document(document, settings);
             const auto loaded = open_settings_document(path);
             check(loaded.original == document.original && loaded.settings.dictionaries == settings.dictionaries &&
                       !loaded.settings.learning && loaded.settings.romaji_rules == romaji &&
                       loaded.settings.candidates.show_after == 5 && loaded.settings.candidates.keys == CandidateKeys::Asdfjkl,
                   "saved configuration changed");
         }},
        {"all-inference-options-survive-disabled-save",
         [&] {
             const auto path = files.path(L"disabled.ini");
             auto document = open_settings_document(path);
             auto settings = document.settings;
             settings.zenz.model = files.root / L"not-present.gguf";
             settings.zenz.server = files.root / L"not-present.exe";
             settings.zenz.threads = 7;
             settings.zenz.timeout_ms = 42;
             settings.zenz.log_input = true;
             settings.zenz.log_directory = files.root;
             settings.zenz.allow_unknown_context = true;
             auto& policy = settings.zenz.policy;
             policy.long_min_length = 15;
             policy.rerank = true;
             policy.learn_fallback = true;
             policy.rerank_limit = 3;
             policy.score_timeout_ms = 17;
             policy.promote_threshold = 2.5f;
             policy.max_score_gap = 9.5f;
             check(check_settings_draft(settings).valid, "disabled paths required existing files");
             save_settings_document(document, settings);
             check(serialize_dictionary_settings(open_settings_document(path).settings) ==
                       serialize_dictionary_settings(settings),
                   "inference option lost");
         }},
        {"fatal-source-failures-do-not-change-settings",
         [&] {
             const auto path = files.path(L"validation.ini");
             auto document = open_settings_document(path);
             save_settings_document(document, {});
             const auto before = document.original;
             DictionarySettings settings;
             settings.dictionaries = {files.root / L"missing.skk"};
             check(!check_settings_draft(settings).valid, "missing dictionary accepted");
             settings.dictionaries.clear();
             settings.romaji_rules = files.root / L"missing.tsv";
             check(!check_settings_draft(settings).valid, "missing rules accepted");
             settings.romaji_rules.clear();
             settings.zenz.enabled = true;
             settings.zenz.model = dictionary;
             settings.zenz.server = files.root / L"missing.exe";
             check(!check_settings_draft(settings).valid && open_settings_document(path).original == before,
                   "validation wrote settings");
         }},
        {"invalid-settings-and-nonfatal-dictionary-diagnostics",
         [&] {
             DictionarySettings settings;
             settings.dictionaries = {L"relative.skk"};
             check(!check_settings_draft(settings).valid, "relative path accepted");
             settings.dictionaries.clear();
             settings.zenz.policy.rerank_limit = 1;
             check(!check_settings_draft(settings).valid, "invalid policy accepted");
             settings.zenz = {};
             const auto source = files.path(L"partial.skk");
             write_file_atomic(source, "かな /仮名/\nbroken\n");
             settings.dictionaries = {source};
             const auto checked = check_settings_draft(settings);
             check(checked.valid && checked.report.find(L"不正行 1") != std::wstring::npos,
                   "partial dictionary warning lost");
         }},
        {"outside-update-create-delete-conflicts",
         [&] {
             const auto path = files.path(L"conflict.ini");
             auto document = open_settings_document(path);
             DictionarySettings external;
             external.learning = false;
             save_dictionary_settings(path, external);
             rejects([&] { save_settings_document(document, {}); });
             check(!read_dictionary_settings(path).learning, "outside creation overwritten");
             document = open_settings_document(path);
             save_dictionary_settings(path, {});
             rejects([&] { save_settings_document(document, external); });
             check(read_dictionary_settings(path).learning, "outside update overwritten");
             document = open_settings_document(path);
             std::filesystem::remove(path);
             rejects([&] { save_settings_document(document, {}); });
             check(!std::filesystem::exists(path), "outside deletion ignored");
         }},
        {"failed-write-keeps-document-and-file",
         [&] {
             const auto path = files.path(L"locked.ini");
             auto document = open_settings_document(path);
             save_settings_document(document, {});
             const auto before = document.original;
             NativeHandle lock(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr));
             check(lock.valid(), "cannot lock settings");
             auto next = document.settings;
             next.learning = false;
             rejects([&] { save_settings_document(document, next); });
             check(document.original == before && document.settings.learning && read_dictionary_settings(path).learning,
                   "failed write changed state");
         }},
        {"malformed-original-is-not-replaced",
         [&] {
             const auto path = files.path(L"broken.ini");
             write_file_atomic(path, "version=unknown\n");
             rejects([&] { open_settings_document(path); });
             std::ifstream input(path);
             std::string text;
             std::getline(input, text);
             check(text == "version=unknown", "bad settings overwritten");
         }}};
    for (const auto& [name, run] : cases)
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    return failed ? 1 : 0;
}
