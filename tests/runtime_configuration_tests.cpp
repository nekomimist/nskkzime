#include "runtime_configuration.h"
#include "runtime_dictionary.h"
#include "romaji_settings.h"
#include "dictionary_wait.h"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        const auto root = std::filesystem::current_path() /
            ("runtime-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root);
        const auto config = root / "settings.ini", first = root / "first.skk", second = root / "second.skk";
        const auto rules = root / "first.tsv", replacement = root / "second.tsv";
        { std::ofstream out(first, std::ios::binary); out << "かな /仮名/\n"; }
        { std::ofstream out(second, std::ios::binary); out << "かな /加奈/\n"; }
        { std::ofstream out(rules, std::ios::binary); out << "version=1\nwi\tゐ\n"; }
        { std::ofstream out(replacement, std::ios::binary); out << "version=1\nwi\tヰ\n"; }
        DictionarySettings initial{{first}, false, rules}; initial.candidates.show_after = 2;
        save_dictionary_settings(config, initial);
        SetEnvironmentVariableW(L"NSKKZIME_CONFIG", config.c_str());
        // An activation keeps what it captured even after the file changes.
        const auto captured = capture_runtime_configuration();
        auto changed = initial; changed.dictionaries = {second}; changed.romaji_rules = replacement; changed.candidates.show_after = 5;
        save_dictionary_settings(config, changed);
        const auto fresh = capture_runtime_configuration();
        const auto old_dictionary = runtime_dictionary(captured), new_dictionary = runtime_dictionary(fresh);
        wait_for_dictionary([&] { return old_dictionary->ready() && new_dictionary->ready(); });
        check(old_dictionary->lookup({U"かな", 0})[0].text == U"仮名" && new_dictionary->lookup({U"かな", 0})[0].text == U"加奈", "dictionary configuration reread");
        check(runtime_romaji_rules(captured)->find("wi")->output == U"ゐ" && runtime_romaji_rules(fresh)->find("wi")->output == U"ヰ", "romaji rules reread");
        check(captured.dictionaries.settings.candidates.show_after == 2 && fresh.dictionaries.settings.candidates.show_after == 5, "candidate settings reread");
        check(captured.user_file == root / "user-dictionary.nskkdb" && captured.tools[0].file == config &&
            captured.tools[1].file == captured.user_file, "settings folder did not own the user dictionary");
        // Display-only changes still share the loaded dictionary.
        changed.dictionaries = {first}; save_dictionary_settings(config, changed);
        const auto same_source = capture_runtime_configuration();
        check(runtime_dictionary(same_source) == old_dictionary, "display settings prevented dictionary sharing");
        check(same_source.dictionaries.settings.candidates.show_after == 5 && captured.dictionaries.settings.candidates.show_after == 2, "shared dictionary replaced activation settings");
        std::cout << "PASS captured settings, independent activation settings and shared dictionaries\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
