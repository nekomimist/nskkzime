#include "dictionary_settings.h"
#include "runtime_dictionary.h"
#include "romaji_settings.h"
#include "dictionary_wait.h"
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
        {"settings/candidate-options-defaults-roundtrip-and-bounds", [&] {
            const auto defaults = parse_dictionary_settings("version=1\n").candidates;
            require(defaults.show_after == 1 && defaults.keys == CandidateKeys::Numbers, "old settings changed candidate behavior");
            auto settings = parse_dictionary_settings("version=1\ncandidate-show-after=3\ncandidate-keys=asdfjkl\n");
            const auto file = files.path(L"candidates.ini"); save_dictionary_settings(file, settings);
            const auto loaded = read_dictionary_settings(file).candidates;
            require(loaded.show_after == 3 && loaded.keys == CandidateKeys::Asdfjkl, "candidate settings lost");
            settings.candidates = {}; save_dictionary_settings(file, settings);
            require(read_dictionary_settings(file).candidates.show_after == 1 && read_dictionary_settings(file).candidates.keys == CandidateKeys::Numbers, "candidate defaults not restored");
            require(parse_dictionary_settings("version=1\ncandidate-show-after=99\ncandidate-keys=numbers\n").candidates.show_after == 99, "upper bound rejected");
            for (const auto invalid : {"candidate-show-after=0", "candidate-show-after=100", "candidate-show-after=-1", "candidate-show-after=", "candidate-show-after=3x",
                 "candidate-show-after=2\ncandidate-show-after=3", "candidate-keys=ASDFJKL", "candidate-keys=qwerty", "candidate-keys=numbers\ncandidate-keys=asdfjkl"})
                rejects([&] { parse_dictionary_settings(std::string("version=1\n") + invalid); });
            settings.candidates.show_after = 0; rejects([&] { serialize_dictionary_settings(settings); });
        }},
        {"settings/input-log-explicit-opt-in-and-path", [&] {
            auto settings = parse_dictionary_settings("version=1\n");
            require(!settings.zenz.log_input && settings.zenz.log_directory.empty(), "old settings enabled input log");
            settings.zenz.log_directory = files.root;
            const auto file = files.path(L"input-log.ini");
            save_dictionary_settings(file, settings);
            require(!read_dictionary_settings(file).zenz.log_input, "path alone enabled input log");
            settings.zenz.log_input = true; save_dictionary_settings(file, settings);
            const auto loaded = read_dictionary_settings(file);
            require(loaded.zenz.log_input && loaded.zenz.log_directory == files.root, "log settings lost");
            validate_zenz_settings(loaded.zenz, true);
            settings.zenz.log_directory = missing;
            rejects([&] { validate_zenz_settings(settings.zenz, true); });
            for (const auto invalid : {"input-log=yes", "input-log=on", "input-log=off\ninput-log=off",
                 "input-log-directory=relative", "input-log-directory=\\\\server\\share", "input-log-directory=C:\\a\ninput-log-directory=C:\\b"})
                rejects([&] { parse_dictionary_settings(std::string("version=1\n") + invalid); });
        }},
        {"settings/unknown-context-is-explicit-and-independent", [&] {
            auto settings = parse_dictionary_settings("version=1\n");
            require(!settings.zenz.allow_unknown_context,"old settings enabled unknown context");
            settings.zenz.allow_unknown_context = true;
            const auto file = files.path(L"unknown-context.ini"); save_dictionary_settings(file,settings);
            const auto loaded=read_dictionary_settings(file);
            require(loaded.zenz.allow_unknown_context && !loaded.zenz.log_input && !loaded.zenz.policy.learn_fallback,"context opt-in changed persistence");
            settings.zenz.allow_unknown_context=false; save_dictionary_settings(file,settings);
            require(!read_dictionary_settings(file).zenz.allow_unknown_context,"context opt-out lost");
            require(!parse_dictionary_settings("version=1\nzenz-unknown-context=off\n").zenz.allow_unknown_context,"explicit off lost");
            for(const auto invalid:{"zenz-unknown-context=yes","zenz-unknown-context=on\nzenz-unknown-context=off"})
                rejects([&]{parse_dictionary_settings(std::string("version=1\n")+invalid);});
        }},
        {"settings/zenz-policy-roundtrip-and-bounds", [&] {
            const auto text = "version=1\nzenz-min-length=12\nzenz-rerank=on\nzenz-learn-fallback=on\nzenz-rerank-limit=8\nzenz-score-timeout-ms=250\nzenz-promote-threshold=1.5\nzenz-max-score-gap=7.5\n";
            const auto settings=parse_dictionary_settings(text); const auto file=files.path(L"zenz-policy.ini"); save_dictionary_settings(file,settings);
            const auto policy=read_dictionary_settings(file).zenz.policy;
            require(policy.long_min_length==12&&policy.rerank&&policy.learn_fallback&&policy.rerank_limit==8&&policy.score_timeout_ms==250&&policy.promote_threshold==1.5f&&policy.max_score_gap==7.5f,"policy roundtrip");
            // The wait key is written only when changed, so older IMEs keep reading default files.
            require(policy.rerank_wait_ms==InferencePolicy{}.rerank_wait_ms&&serialize_dictionary_settings(settings).find("zenz-rerank-wait-ms")==std::string::npos,"default wait was written");
            const auto waiting=parse_dictionary_settings(std::string(text)+"zenz-rerank-wait-ms=0\n"); save_dictionary_settings(file,waiting);
            require(read_dictionary_settings(file).zenz.policy.rerank_wait_ms==0,"rerank wait roundtrip");
            require(serialize_dictionary_settings(settings).find("zenz-idle-seconds")==std::string::npos,"default idle time was written");
            save_dictionary_settings(file,parse_dictionary_settings(std::string(text)+"zenz-idle-seconds=86400\n"));
            require(read_dictionary_settings(file).zenz.idle_seconds==86400,"idle time roundtrip");
            for(const auto invalid:{"zenz-idle-seconds=0","zenz-idle-seconds=86401"})
                rejects([&]{parse_dictionary_settings(std::string("version=1\n")+invalid);});
            for(const auto invalid:{"zenz-min-length=1025","zenz-rerank=yes","zenz-learn-fallback=yes","zenz-rerank-limit=1","zenz-rerank-limit=65",
                "zenz-rerank-wait-ms=1001","zenz-rerank-wait-ms=-1","zenz-rerank-wait-ms=0\nzenz-rerank-wait-ms=0",
                "zenz-score-timeout-ms=0","zenz-score-timeout-ms=10001","zenz-promote-threshold=nan","zenz-promote-threshold=-1",
                "zenz-max-score-gap=inf","zenz-max-score-gap=101","zenz-min-length=0\nzenz-min-length=10"})
                rejects([&]{parse_dictionary_settings(std::string("version=1\n")+invalid);});
        }},
        {"settings/zenz-roundtrip-and-files", [&] {
            const auto model = files.write(L"モデル 検証.gguf","test"), server = files.write(L"サーバー.exe","test");
            DictionarySettings settings{{first},false,{}, {true,model,server,2,800}};
            const auto file = files.path(L"zenz.ini"); save_dictionary_settings(file,settings);
            const auto loaded = read_dictionary_settings(file);
            require(loaded.zenz.enabled && loaded.zenz.model==model && loaded.zenz.server==server && loaded.zenz.threads==2 &&
                loaded.zenz.timeout_ms==800 && !loaded.learning && loaded.dictionaries==settings.dictionaries,"zenz settings roundtrip");
            validate_zenz_settings(loaded.zenz,true);
            settings.zenz.model=missing; rejects([&]{validate_zenz_settings(settings.zenz,true);});
            require(!parse_dictionary_settings("version=1").zenz.enabled,"old config enabled inference");
        }},
        {"settings/zenz-invalid-preserves-file", [&] {
            const auto file = files.path(L"zenz-preserve.ini"); save_dictionary_settings(file,{{first}});
            DictionarySettings invalid{{second}}; invalid.zenz.enabled=true;
            rejects([&]{save_dictionary_settings(file,invalid);});
            require(read_dictionary_settings(file).dictionaries==std::vector<std::filesystem::path>{first},"invalid zenz update changed settings");
            for (const auto text : {"inference=unknown", "inference=zenz", "inference=off\ninference=off",
                "zenz-model=relative.gguf", "zenz-server=", "zenz-server=C:\\a.exe\nzenz-server=C:\\b.exe",
                "inference-threads=0", "inference-threads=65", "inference-threads=-1", "inference-threads=+4",
                "inference-timeout-ms=0", "inference-timeout-ms=10001", "inference-timeout-ms=10x", "zenz-model=C:\\a\"b.gguf"})
                rejects([&]{parse_dictionary_settings(std::string("version=1\n")+text);});
        }},
        {"settings/romaji-roundtrip-invalid-and-fallback", [&] {
            const auto rules = files.write(L"ローマ字.tsv","version=1\nwi\tゐ\n");
            const auto file = files.path(L"romaji.ini"); DictionarySettings proposed{{first},false,rules};
            save_dictionary_settings(file,proposed); const auto loaded = read_dictionary_settings(file);
            require(loaded.romaji_rules==rules&&!loaded.learning&&loaded.dictionaries==proposed.dictionaries,"romaji roundtrip damaged settings");
            auto config=resolve_dictionary_configuration(file,{},false);
            const auto custom=load_romaji_rules(config,{}); require(!custom.failed&&custom.rules->find("wi")->output==U"ゐ","romaji file not loaded");
            require(load_romaji_rules(config,L"builtin").rules->find("wi")->output==U"うぃ","builtin override ignored");
            const auto broken=files.write(L"broken-romaji.tsv","version=1\nwi\t私的な出力\nwi\t別の値");
            const auto failed=load_romaji_rules(config,broken.native());
            require(failed.failed&&failed.rules->find("wi")->output==U"うぃ","invalid rules did not fall back");
            require(failed.status.find(L"私的な出力")==std::wstring::npos,"diagnostic leaked output");
            for(const auto text:{"version=1\nromaji=relative.tsv","version=1\nromaji=","version=1\nromaji=C:\\a\nromaji=C:\\b"})
                rejects([&]{parse_dictionary_settings(text);});
        }},
        {"runtime/romaji-independent-snapshots-and-override", [&] {
            const auto rules = files.write(L"snapshot.tsv","version=1\nwi\tゐ\n");
            const auto file = files.path(L"snapshot.ini"); save_dictionary_settings(file,{{first},false,rules});
            Environment no_override(L"NSKKZIME_DICTIONARY",nullptr), config_env(L"NSKKZIME_CONFIG",file.c_str()), rules_env(L"NSKKZIME_ROMAJI",nullptr);
            const auto old=runtime_romaji_rules(); require(old->find("wi")->output==U"ゐ","saved romaji ignored");
            {std::ofstream output(rules,std::ios::binary|std::ios::trunc); output<<"version=1\nwi\tうぃー\n";}
            require(runtime_romaji_rules()->find("wi")->output==U"うぃー"&&old->find("wi")->output==U"ゐ","existing romaji snapshot changed");
            Environment invalid_config(L"NSKKZIME_CONFIG",L"relative.ini"), explicit_rules(L"NSKKZIME_ROMAJI",rules.c_str());
            require(runtime_romaji_rules()->find("wi")->output==U"うぃー","explicit rules blocked by invalid config");
        }},
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
            wait_for_dictionary([&] { return initial->ready(); });
            require(initial == same, "snapshot not reused");
            save_dictionary_settings(path, {{second}});
            const auto changed = runtime_dictionary();
            wait_for_dictionary([&] { return changed->ready(); });
            require(initial != changed && changed->lookup({U"かな"})[0].annotation == U"共通", "new configuration used stale snapshot");
            require(initial->lookup({U"かな"})[0].annotation == U"優先", "existing snapshot changed");
            require(runtime_dictionary_status().find(second.native()) != std::wstring::npos, "status did not reflect loaded configuration");
        }},
        {"runtime/invalid-config-kana-fallback-with-status", [&] {
            const auto path = files.write(L"invalid-runtime.ini", "version=7");
            Environment no_override(L"NSKKZIME_DICTIONARY", nullptr), config_env(L"NSKKZIME_CONFIG", path.c_str());
            const auto dictionary = runtime_dictionary();
            wait_for_dictionary([&] { return dictionary->ready(); });
            require(dictionary->lookup({U"かんじ"}).empty(), "broken configuration fell back to demo");
            require(runtime_dictionary_status().find(L"読み込めません") != std::wstring::npos, "configuration failure invisible");
        }},
        {"runtime/environment-bypasses-invalid-config-path", [&] {
            Environment config_env(L"NSKKZIME_CONFIG", L"relative.ini"), override_env(L"NSKKZIME_DICTIONARY", first.c_str());
            const auto dictionary = runtime_dictionary();
            wait_for_dictionary([&] { return dictionary->ready(); });
            require(dictionary->lookup({U"かな"}).size() == 2, "valid dictionary blocked by invalid config path");
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
