#include "runtime_configuration.h"
#include "runtime_dictionary.h"
#include "romaji_settings.h"
#include "inference_client.h"
#include "dictionary_wait.h"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Environment {
    const wchar_t* name;
    std::optional<std::wstring> previous;
    Environment(const wchar_t* key, const wchar_t* value) : name(key), previous(environment_value(key)) {
        check(SetEnvironmentVariableW(name, value) != FALSE, "set environment");
    }
    ~Environment() { SetEnvironmentVariableW(name, previous ? previous->c_str() : nullptr); }
};
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
        Environment saved(L"NSKKZIME_CONFIG", config.c_str()), dictionary(L"NSKKZIME_DICTIONARY", nullptr),
            cache(L"NSKKZIME_DICTIONARY_CACHE", L"off"), learning(L"NSKKZIME_LEARNING", L"0"),
            custom(L"NSKKZIME_ROMAJI", rules.c_str()), dummy(L"NSKKZIME_INFERENCE_DUMMY", L"1"),
            timeout(L"NSKKZIME_INFERENCE_TIMEOUT_MS", L"345"), delay(L"NSKKZIME_INFERENCE_DELAY_MS", L"25"),
            token(L"NSKKZIME_INFERENCE_TOKEN", L"captured-runtime-config"),
            user(L"NSKKZIME_USER_DICTIONARY", (root / "first.nskkdb").c_str());
        const auto captured = capture_runtime_configuration();
        auto changed = initial; changed.dictionaries = {second}; changed.romaji_rules = replacement; changed.candidates.show_after = 5;
        save_dictionary_settings(config, changed);
        SetEnvironmentVariableW(L"NSKKZIME_ROMAJI", replacement.c_str());
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY", L"0");
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TIMEOUT_MS", L"999");
        SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", (root / "second.nskkdb").c_str());
        const auto fresh = capture_runtime_configuration();
        const auto old_dictionary = runtime_dictionary(captured), new_dictionary = runtime_dictionary(fresh);
        wait_for_dictionary([&] { return old_dictionary->ready() && new_dictionary->ready(); });
        check(old_dictionary->lookup({U"かな", 0})[0].text == U"仮名" && new_dictionary->lookup({U"かな", 0})[0].text == U"加奈", "dictionary configuration reread");
        check(runtime_romaji_rules(captured)->find("wi")->output == U"ゐ" && runtime_romaji_rules(fresh)->find("wi")->output == U"ヰ", "romaji override reread");
        const auto inference = inference_config(captured);
        check(inference && inference->timeout_ms == 345 && inference->delay_ms == 25 &&
            inference->token == L"captured-runtime-config" && !inference_config(fresh), "inference overrides reread");
        check(captured.dictionaries.settings.candidates.show_after == 2 && fresh.dictionaries.settings.candidates.show_after == 5, "candidate settings reread");
        check(captured.tools[1].file == root / "first.nskkdb" && fresh.tools[1].file == root / "second.nskkdb", "tool destination reread");

        changed.dictionaries = {first}; save_dictionary_settings(config, changed);
        const auto same_source = capture_runtime_configuration();
        check(runtime_dictionary(same_source) == old_dictionary, "display/inference settings prevented dictionary sharing");
        check(same_source.dictionaries.settings.candidates.show_after == 5 && captured.dictionaries.settings.candidates.show_after == 2, "shared dictionary replaced activation settings");
        auto invalid_inference = captured; invalid_inference.inference.dummy = L"invalid";
        bool rejected = false; try { (void)inference_config(invalid_inference); } catch (...) { rejected = true; }
        check(rejected && !runtime_dictionary(invalid_inference)->lookup({U"かな", 0}).empty(), "optional inference failure stopped dictionary");
        SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", nullptr);
        SetEnvironmentVariableW(L"NSKKZIME_CONFIG", (root / "elsewhere" / "settings.ini").c_str());
        const auto derived = capture_runtime_configuration(captured.dictionaries);
        check(derived.user_file == root / "user-dictionary.nskkdb" && derived.tools[0].file == config &&
            derived.tools[1].file == derived.user_file, "resolved settings did not own default destinations");
        std::cout << "PASS captured settings/environment, independent activation settings, shared dictionaries and optional inference failure\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
