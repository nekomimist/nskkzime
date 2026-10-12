#include "broker_snapshot.h"
#include "input_log.h"
#include "romaji_settings.h"
#include "nskkzime/compiled_dictionary.h"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch (...) { failed = true; } check(failed, "invalid message accepted"); }
struct Files {
    std::filesystem::path root = std::filesystem::current_path() /
        ("broker-snapshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<std::filesystem::path> files;
    Files() { std::filesystem::create_directory(root); }
    ~Files() {
        std::error_code ignored;
        for (const auto& file : files) std::filesystem::remove(file, ignored);
        std::filesystem::remove(root, ignored);
    }
    std::filesystem::path write(std::string_view name, std::string_view bytes) {
        const auto path = root / name;
        files.push_back(path);
        std::ofstream stream(path, std::ios::binary); stream << bytes;
        check(static_cast<bool>(stream), "fixture write"); return path;
    }
};
BrokerBootstrap decode(const BrokerSnapshot& snapshot) { return decode_broker_bootstrap(snapshot.bytes); }
std::vector<Candidate> lookup(const BrokerBootstrap& snapshot) {
    if (snapshot.demo) return demo_dictionary()->lookup({U"かんじ", 0});
    std::vector<std::shared_ptr<const Dictionary>> sources;
    for (const auto& image : snapshot.images) {
        const auto owner = std::make_shared<const std::string>(image);
        sources.push_back(std::make_shared<CompiledDictionary>(*owner, owner));
    }
    return DictionarySet(std::move(sources)).lookup({U"かんじ", 0});
}
}
int main() {
    try {
        Files files;
        const auto first = files.write("first.skk", "かんじ /漢字;優先/感じ/\n");
        const auto second = files.write("second.skk", "かんじ /漢字;下位/幹事/\n");
        DictionaryConfiguration config;
        config.file = files.root / "settings.ini";
        config.settings.dictionaries = {files.root / "missing.skk", first, second};
        config.settings.off_key_exclusions = {L"Code.exe", L"WindowsTerminal.exe"};
        config.settings.startup_ime_off = true;
        const auto partial = build_broker_snapshot(config, dictionary_configuration_id(config));
        const auto response = decode(partial);
        check(parse_dictionary_settings(response.settings).startup_ime_off, "startup mode not transferred to container");
        check(parse_dictionary_settings(response.settings).off_key_exclusions == config.settings.off_key_exclusions,
            "OFF key exclusions not transferred to container");
        check(response.generation == dictionary_configuration_id(config) && wide_message(response.file) == config.file.native(),
            "settings identity not transferred");
        const auto candidates = lookup(response);
        check(candidates.size() == 3 && candidates[0].text == U"漢字" && candidates[0].annotation == U"優先" &&
            candidates[1].text == U"感じ" && candidates[2].text == U"幹事", "partial load priority/dedup");
        check(!partial.learning_allowed && !response.learning, "partial failure enabled learning");
        check(!response.complete, "partial failure marked complete");
        check(wide_message(response.status).find(L"読み込み失敗") != std::wstring::npos, "lost failure diagnostic");
        for (std::size_t size = 0; size < partial.bytes.size(); size += 97)
            rejects([&] { decode_broker_bootstrap(std::string_view(partial.bytes).substr(0, size)); });
        rejects([&] { decode_broker_bootstrap(partial.bytes + "x"); });
        rejects([&] { auto bytes = partial.bytes; bytes[0] = 'X'; decode_broker_bootstrap(bytes); });

        config.settings.dictionaries = {first};
        config.settings.romaji_rules = files.write("broken.tsv", "version=1\nwi\tゐ\nwi\tヰ\n");
        const auto broken = build_broker_snapshot(config, dictionary_configuration_id(config));
        const auto fallback = decode(broken);
        check(lookup(fallback).size() == 2 && fallback.romaji.empty(), "broken romaji stopped dictionary/default rules");
        check(broken.learning_allowed, "romaji fallback disabled dictionary learning");
        check(!fallback.complete, "romaji fallback marked complete");
        check(wide_message(fallback.status).find(L"内蔵ルールで継続") != std::wstring::npos, "lost romaji fallback diagnostic");

        config.settings.romaji_rules = files.write("custom.tsv", "version=1\nwi\tゐ\n");
        const auto custom = decode(build_broker_snapshot(config, dictionary_configuration_id(config)));
        check(RomajiRules::parse(custom.romaji)->find("wi")->output == U"ゐ", "validated rules not transferred");
        check(custom.complete, "valid sources not marked complete");

        const auto cache = files.root / "cache";
        const auto before = build_broker_snapshot(config, dictionary_configuration_id(config), cache);
        const auto source_time = std::filesystem::last_write_time(first);
        const auto source_size = std::filesystem::file_size(first);
        { std::ofstream stream(first, std::ios::binary); stream << "かんじ /新字;優先/感じ/\n"; }
        std::filesystem::last_write_time(first, source_time);
        const auto rules_time = std::filesystem::last_write_time(config.settings.romaji_rules);
        { std::ofstream stream(config.settings.romaji_rules, std::ios::binary); stream << "version=1\nwi\tヰ\n"; }
        std::filesystem::last_write_time(config.settings.romaji_rules, rules_time);
        const auto changed = decode(build_broker_snapshot(config, dictionary_configuration_id(config), cache));
        check(std::filesystem::file_size(first) == source_size && lookup(changed)[0].text == U"新字", "same-size/time dictionary stayed stale");
        check(RomajiRules::parse(changed.romaji)->find("wi")->output == U"ヰ", "same-time rules stayed stale");
        check(lookup(decode(before))[0].text == U"漢字", "old immutable snapshot changed");
        std::filesystem::remove(first);
        const auto missing_cached = decode(build_broker_snapshot(config, dictionary_configuration_id(config), cache));
        check(lookup(missing_cached).empty() && !missing_cached.learning, "missing source reused an old cache");
        for (const auto& entry : std::filesystem::directory_iterator(cache)) files.files.push_back(entry.path());
        files.files.push_back(cache);

        config.settings = {}; config.demo = true;
        const auto demo = decode(build_broker_snapshot(config, dictionary_configuration_id(config)));
        check(demo.demo && !lookup(demo).empty() && !demo.learning, "desktop/container demo mismatch");
        config.demo = false;
        const auto empty = decode(build_broker_snapshot(config, dictionary_configuration_id(config)));
        check(lookup(empty).empty() && !empty.learning, "empty settings enabled demo/learning");

        InputLogRecord record; record.request = {7, U"かな", U"前", U"", 2}; record.status = InferenceStatus::Ok;
        record.response = InferenceResponse{7, {U"仮名"}};
        const auto logged = decode_log_record(encode_log_record(record));
        check(logged.request.reading == U"かな" && logged.response && logged.response->candidates == record.response->candidates,
            "log record changed");
        record.response.reset(); record.status = InferenceStatus::Timeout;
        const auto bytes = encode_log_record(record);
        check(!decode_log_record(bytes).response && decode_log_record(bytes).status == InferenceStatus::Timeout, "unanswered log record changed");
        rejects([&] { decode_log_record(bytes + "x"); });
        rejects([&] { auto bad = bytes; bad[0] = 9; decode_log_record(bad); });
        std::cout << "PASS broker partial sources, priority, learning denial, romaji fallback, validated rules, demo/empty and message codecs\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
