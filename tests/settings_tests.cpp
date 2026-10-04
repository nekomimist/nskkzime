#include "dictionary_settings.h"
#include "runtime_dictionary.h"
#include <windows.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<typename Action> void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid input was accepted");
}
struct Files {
    std::filesystem::path root = std::filesystem::current_path() /
        ("settings-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<std::filesystem::path> paths;
    Files() { std::filesystem::create_directory(root); }
    ~Files() {
        std::error_code ignored;
        for (const auto& path : paths) std::filesystem::remove(path, ignored);
        std::filesystem::remove(root, ignored); // Never recursively remove an unexpected file.
    }
    std::filesystem::path path(const std::filesystem::path& name) {
        auto result = root / name; paths.push_back(result); return result;
    }
    std::filesystem::path write(const std::filesystem::path& name, std::string_view bytes) {
        const auto result = path(name);
        std::ofstream output(result, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("cannot write test file");
        return result;
    }
};
struct Environment {
    const wchar_t* name;
    std::optional<std::wstring> previous;
    Environment(const wchar_t* key, const wchar_t* value) : name(key), previous(environment_value(key)) {
        require(SetEnvironmentVariableW(name, value) != 0, "cannot set test environment");
    }
    ~Environment() { SetEnvironmentVariableW(name, previous ? previous->c_str() : nullptr); }
};
}
int main() {
    Environment no_learning(L"NSKKZIME_LEARNING", L"0");
    Files files;
    const auto first = files.write(L"優先 = # 辞書.skk", "かな /仮名;優先/カナ/\n");
    const auto second = files.write(L"共通辞書.skk", "かな /仮名;共通/かな/\n");
    const auto missing = files.root / L"missing.skk";
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"settings/utf8-bom-priority-and-literal-path", [&] {
            const auto config = parse_dictionary_settings("\xef\xbb\xbf# comment\r\n; another\r\n version = 1\r\n dictionary = " +
                utf8_message(first.native()) + "\r\ndictionary=" + utf8_message(second.native()));
            require(config.dictionaries == std::vector<std::filesystem::path>{first, second}, "paths or priority changed");
        }},
        {"settings/reject-invalid-version-and-keys", [] {
            for (const auto bytes : {"", "dictionary=C:\\x", "version=2", "version=1\nversion=1", "version=1\nunknown=x", "version=1\ngarbage"})
                rejects([&] { parse_dictionary_settings(bytes); });
        }},
        {"settings/learning-default-toggle-and-invalid-values", [&] {
            require(parse_dictionary_settings("version=1").learning, "old settings did not enable learning");
            require(parse_dictionary_settings("version=1\nlearning=on").learning, "learning on ignored");
            const auto disabled = parse_dictionary_settings("version=1\nlearning=off");
            const auto path = files.path(L"learning-off.ini"); save_dictionary_settings(path, disabled);
            require(!read_dictionary_settings(path).learning, "learning off lost on round trip");
            for (const auto bytes : {"version=1\nlearning=yes", "version=1\nlearning=", "version=1\nlearning=on\nlearning=off"})
                rejects([&] { parse_dictionary_settings(bytes); });
        }},
        {"settings/reject-invalid-text-and-path", [] {
            for (const auto bytes : {"version=1\ndictionary=relative.skk", "version=1\ndictionary=C:x", "version=1\ndictionary=",
                "version=1\ndictionary=C:\\x\tfile", "version=1\ndictionary=C:\\x\x01", "version=1\n#\xff"})
                rejects([&] { parse_dictionary_settings(bytes); });
            rejects([] { parse_dictionary_settings(std::string("version=1\n#\0", 12)); });
        }},
        {"settings/limits", [] {
            rejects([] { parse_dictionary_settings(std::string(65537, ' ')); });
            std::string too_many = "version=1\n";
            for (int i = 0; i < 33; ++i) too_many += "dictionary=C:\\x\n";
            rejects([&] { parse_dictionary_settings(too_many); });
        }},
        {"settings/save-replace-roundtrip", [&] {
            const auto path = files.path(L"保存設定.ini");
            save_dictionary_settings(path, {{first, second}});
            require(read_dictionary_settings(path).dictionaries == std::vector<std::filesystem::path>{first, second}, "save lost dictionary order");
            save_dictionary_settings(path, {{second, first}});
            require(read_dictionary_settings(path).dictionaries == std::vector<std::filesystem::path>{second, first}, "replace did not update order");
        }},
        {"settings/invalid-save-keeps-previous", [&] {
            const auto path = files.path(L"preserve.ini"); save_dictionary_settings(path, {{first}});
            rejects([&] { save_dictionary_settings(path, {{L"relative.skk"}}); });
            rejects([&] { save_dictionary_settings(path, {{first.native() + L" "}}); });
            require(read_dictionary_settings(path).dictionaries == std::vector<std::filesystem::path>{first}, "invalid save damaged previous file");
        }},
        {"settings/locked-replacement-keeps-previous-and-cleans-temp", [&] {
            const auto path = files.path(L"locked.ini"); save_dictionary_settings(path, {{first}});
            const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "cannot lock settings fixture");
            bool rejected = false;
            try { save_dictionary_settings(path, {{second}}); } catch (const std::exception&) { rejected = true; }
            CloseHandle(handle);
            require(rejected, "locked settings replaced");
            require(read_dictionary_settings(path).dictionaries == std::vector<std::filesystem::path>{first}, "failed replacement damaged previous file");
            for (const auto& entry : std::filesystem::directory_iterator(files.root)) require(entry.path().extension() != L".tmp", "temporary file leaked");
        }},
        {"settings/default-missing-vs-explicit-missing", [&] {
            auto config = resolve_dictionary_configuration(missing, {}, true);
            require(config.demo && !load_dictionaries(config).dictionary->lookup({U"かんじ"}).empty(), "default demo missing");
            rejects([&] { resolve_dictionary_configuration(missing, {}, false); });
        }},
        {"settings/environment-overrides-broken-settings", [&] {
            const auto broken = files.write(L"broken.ini", "version=invalid");
            const auto config = resolve_dictionary_configuration(broken, first.native(), false);
            require(config.environment && config.settings.dictionaries == std::vector<std::filesystem::path>{first}, "override did not win");
            require(!config.settings.learning, "development override unexpectedly enabled learning");
        }},
        {"settings/explicit-empty-disables-demo", [&] {
            const auto path = files.path(L"empty.ini"); save_dictionary_settings(path, {});
            const auto loaded = load_dictionaries(resolve_dictionary_configuration(path, {}, false));
            require(!loaded.failed && loaded.dictionary->lookup({U"かんじ"}).empty(), "disabled dictionary used demo");
        }},
        {"runtime/partial-load-priority-dedup-and-diagnostics", [&] {
            DictionaryConfiguration config; config.settings.dictionaries = {missing, first, second};
            const auto loaded = load_dictionaries(config); const auto list = loaded.dictionary->lookup({U"かな"});
            require(loaded.failed && list.size() == 3 && list[0].text == U"仮名" && list[0].annotation == U"優先" &&
                list[1].text == U"カナ" && list[2].text == U"かな", "partial load or priority failed");
            require(loaded.status.find(L"2. " + first.native()) != loaded.status.npos && loaded.status.find(L"3. " + second.native()) != loaded.status.npos,
                "diagnostics lost configured order");
            require(loaded.status.find(L"読み込み失敗") != loaded.status.npos && loaded.status.find(L"1見出し / 2候補") != loaded.status.npos, "missing diagnostics");
            require(loaded.status.find(L"仮名") == loaded.status.npos, "diagnostic leaked candidate text");
        }},
        {"runtime/live-snapshot-sharing-and-file-change", [&] {
            const auto path = files.path(L"cache.ini"); save_dictionary_settings(path, {{first}});
            Environment no_override(L"NSKKZIME_DICTIONARY", nullptr), config_env(L"NSKKZIME_CONFIG", path.c_str());
            auto initial = runtime_dictionary(); auto same = runtime_dictionary();
            require(initial == same, "snapshot not reused");
            save_dictionary_settings(path, {{second}});
            const auto changed = runtime_dictionary();
            require(initial != changed && changed->lookup({U"かな"})[0].annotation == U"共通", "new configuration used stale snapshot");
            require(initial->lookup({U"かな"})[0].annotation == U"優先", "existing snapshot changed");
            require(runtime_dictionary_status().find(second.native()) != std::wstring::npos, "status did not reflect loaded configuration");
        }},
        {"runtime/invalid-config-kana-fallback-with-status", [&] {
            const auto path = files.write(L"invalid-runtime.ini", "version=7");
            Environment no_override(L"NSKKZIME_DICTIONARY", nullptr), config_env(L"NSKKZIME_CONFIG", path.c_str());
            require(runtime_dictionary()->lookup({U"かんじ"}).empty(), "broken configuration fell back to demo");
            require(runtime_dictionary_status().find(L"読み込めません") != std::wstring::npos, "configuration failure invisible");
        }},
        {"runtime/environment-bypasses-invalid-config-path", [&] {
            Environment config_env(L"NSKKZIME_CONFIG", L"relative.ini"), override_env(L"NSKKZIME_DICTIONARY", first.c_str());
            require(runtime_dictionary()->lookup({U"かな"}).size() == 2, "valid dictionary blocked by invalid config path");
            require(runtime_dictionary_status().find(L"NSKKZIME_DICTIONARY") != std::wstring::npos, "override source invisible");
        }}
    };
    std::size_t passed = 0;
    for (const auto& [name, test] : cases) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << '/' << cases.size() << " passed\n";
    return passed == cases.size() ? 0 : 1;
}
