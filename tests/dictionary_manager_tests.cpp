#include "dictionary_manager.h"
#include "local_storage.h"
#include <algorithm>
#include <functional>
#include <iostream>

using namespace nskkzime;
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
    std::filesystem::path root = std::filesystem::current_path() / ("dictionary-manager-test-" + learning_client_id());
    std::vector<std::filesystem::path> files;
    Files() { std::filesystem::create_directory(root); }
    std::filesystem::path path(const wchar_t* name) {
        auto file = root / name;
        files.push_back(file);
        files.push_back(file.native() + L".lock");
        return file;
    }
    ~Files() {
        std::error_code ignored;
        for (const auto& file : files)
            std::filesystem::remove(file, ignored);
        std::filesystem::remove(root, ignored);
    }
};

LearningEvent word(std::u32string reading, std::u32string spelling, char key = 0, std::u32string suffix = {}) {
    return {{std::move(reading), key}, std::move(suffix), {"test", std::move(spelling), U"注釈𠮷"}};
}
void learn(UserDictionary& data, LearningEvent entry) {
    if (entry.key.okuri && entry.okuri.empty())
        data.apply(ImportRequest{learning_client_id(), 1, {std::move(entry)}});
    else
        data.apply(LearningRequest{learning_client_id(), 1, std::move(entry)});
}
} // namespace
int wmain(int argc, wchar_t**) {
    if (argc != 1)
        return 2;
    Files files;
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"rows-filter-unicode-and-exact-scope",
         [&] {
             const auto file = files.path(L"表示.nskkdb");
             UserDictionary data;
             learn(data, word(U"か", U"書", 'k'));
             learn(data, word(U"か", U"書", 'k', U"く"));
             learn(data, word(U"AI", U"人工知能"));
             write_file_atomic(file, data.serialize());
             const auto rows = read_dictionary_rows(file);
             check(rows.size() == 3 && filter_dictionary_rows(rows, L"").size() == 3, "listing lost entries");
             check(filter_dictionary_rows(rows, L"書").size() == 2, "spelling search lost scope");
             check(filter_dictionary_rows(rows, L"注釈𠮷").size() == 3, "Unicode annotation search failed");
             check(filter_dictionary_rows(rows, L"ai").empty(), "literal search changed case");
             const auto exact = filter_dictionary_rows(rows, L"く");
             check(exact.size() == 1 && rows[exact[0]].entry.okuri == U"く" && rows[exact[0]].columns[1] == L"k",
                   "display and deletion identities disagree");
             rejects([&] { read_dictionary_rows(L"relative.nskkdb"); });
         }},
        {"preflight-no-write-invalid-and-empty-import",
         [&] {
             const auto file = files.path(L"preflight.nskkdb"), source = files.path(L"取り込み.skk");
             write_file_atomic(source, "かな /仮名/\n");
             const auto preview = prepare_dictionary_import(file, source);
             check(preview.candidates == 1 && preview.added == 1 && !std::filesystem::exists(file) &&
                       !std::filesystem::exists(file.native() + L".lock"),
                   "preflight wrote or locked the dictionary");
             write_file_atomic(source, "かな /仮名/\nbroken\n");
             rejects([&] { prepare_dictionary_import(file, source); });
             write_file_atomic(source, "かな /(unknown-function)/\n");
             rejects([&] { prepare_dictionary_import(file, source); });
             write_file_atomic(source, ";; empty\n");
             const auto empty = prepare_dictionary_import(file, source);
             check(empty.request.entries.empty() && empty.candidates == 0, "empty import has updates");
             apply_dictionary_import(empty);
             check(!std::filesystem::exists(file), "empty import wrote a dictionary");
             write_file_atomic(file, "broken database");
             rejects([&] { prepare_dictionary_import(file, source); });
             rejects([&] { read_dictionary_rows(file); });
             rejects([&] { prepare_dictionary_import(file, file); });
         }},
        {"frozen-import-preserves-concurrent-learning-and-retry",
         [&] {
             const auto file = files.path(L"merge.nskkdb"), source = files.path(L"snapshot.skk");
             UserDictionary data;
             learn(data, word(U"かんじ", U"漢字"));
             write_file_atomic(file, data.serialize());
             write_file_atomic(source, "かんじ /漢字;置換しない/感じ/\nかな /仮名/\n");
             const auto preview = prepare_dictionary_import(file, source);
             check(preview.candidates == 3 && preview.added == 2, "preflight count wrong");
             write_file_atomic(source, "へんこう /変更/\n");
             const LearningRequest concurrent{learning_client_id(), 1, word(U"かんじ", U"幹事")};
             commit_learning(file, concurrent);
             apply_dictionary_import(preview);
             auto saved = read_user_dictionary(file);
             const auto words = saved.lookup({U"かんじ"});
             check(words.size() == 3 && words[0].text == U"幹事" && words[1].annotation == U"注釈𠮷" &&
                       words[2].text == U"感じ",
                   "import replaced priority or annotation");
             check(saved.lookup({U"へんこう"}).empty() && !saved.lookup({U"かな"}).empty(),
                   "source was reread after approval");
             apply_dictionary_import(preview);
             check(read_user_dictionary(file).serialize() == saved.serialize(),
                   "identical import retry changed the dictionary");
         }},
        {"forget-exact-and-generic-scopes",
         [&] {
             const auto file = files.path(L"delete.nskkdb");
             UserDictionary data;
             learn(data, word(U"か", U"描", 'k'));
             learn(data, word(U"か", U"描", 'k', U"く"));
             learn(data, word(U"か", U"描", 'k', U"け"));
             learn(data, word(U"か", U"書", 'k', U"く"));
             write_file_atomic(file, data.serialize());
             const auto rows = read_dictionary_rows(file);
             const auto row = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
                 return row.entry.okuri == U"く" && row.entry.candidate.text == U"描";
             });
             check(row != rows.end(), "test row missing");
             forget_dictionary_entry(file, row->entry);
             const auto after = read_dictionary_rows(file);
             check(after.size() == 3, "deletion removed wrong number of entries");
             check(std::none_of(
                       after.begin(), after.end(),
                       [](const auto& row) { return row.entry.okuri == U"く" && row.entry.candidate.text == U"描"; }),
                   "selected exact scope remains");
             forget_dictionary_entry(file, word(U"か", U"描", 'k'));
             const auto remaining = read_dictionary_rows(file);
             check(remaining.size() == 2 && std::any_of(remaining.begin(), remaining.end(),
                                                        [](const auto& row) {
                                                            return row.entry.okuri == U"け" &&
                                                                   row.entry.candidate.text == U"描";
                                                        }),
                   "generic deletion affected another suffix");
         }},
        {"export-snapshot-never-replaces-existing-files", [&] {
             const auto file = files.path(L"export.nskkdb"), destination = files.path(L"書き出し.skk");
             UserDictionary data;
             learn(data, word(U"かな", U"仮名"));
             write_file_atomic(file, data.serialize());
             export_user_dictionary(file, destination);
             check(SkkDictionary::load(destination)->lookup({U"かな"})[0].text == U"仮名", "export lost content");
             rejects([&] { export_user_dictionary(file, destination); });
             rejects([&] { export_user_dictionary(file, file); });
             check(read_user_dictionary(file).serialize() == data.serialize(), "export changed source");
             write_file_atomic(file, "broken");
             const auto refused = files.path(L"refused.skk");
             rejects([&] { export_user_dictionary(file, refused); });
             check(!std::filesystem::exists(refused), "corrupt source exported as empty");
         }}};
    int failed = 0;
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
