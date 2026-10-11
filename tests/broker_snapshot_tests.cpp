#include "broker_snapshot.h"
#include "romaji_settings.h"
#include "nskkzime/compiled_dictionary.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
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
std::vector<Candidate> lookup(const nlohmann::json& snapshot) {
    if (snapshot.at("demo").get<bool>()) return demo_dictionary()->lookup({U"かんじ", 0});
    std::vector<std::shared_ptr<const Dictionary>> sources;
    for (const auto& image : snapshot.at("images")) {
        const auto& bytes = image.get_binary();
        const auto owner = std::make_shared<const std::string>(bytes.begin(), bytes.end());
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
        const auto partial = build_broker_snapshot(config, dictionary_configuration_id(config));
        const auto response = nlohmann::json::from_cbor(partial.bytes);
        check(parse_dictionary_settings(response.at("settings").get<std::string>()).off_key_exclusions ==
            config.settings.off_key_exclusions, "OFF key exclusions not transferred to container");
        const auto candidates = lookup(response);
        check(candidates.size() == 3 && candidates[0].text == U"漢字" && candidates[0].annotation == U"優先" &&
            candidates[1].text == U"感じ" && candidates[2].text == U"幹事", "partial load priority/dedup");
        check(!partial.learning_allowed && !response.at("learning").get<bool>(), "partial failure enabled learning");
        check(!response.at("complete").get<bool>(), "partial failure marked complete");
        check(wide_message(response.at("status").get<std::string>()).find(L"読み込み失敗") != std::wstring::npos, "lost failure diagnostic");
        bool legacy_rejected = false;
        try { (void)build_broker_snapshot(config, dictionary_configuration_id(config), {}, true); }
        catch (...) { legacy_rejected = true; }
        check(legacy_rejected, "legacy client accepted degraded learning capability");

        config.settings.dictionaries = {first};
        config.settings.romaji_rules = files.write("broken.tsv", "version=1\nwi\tゐ\nwi\tヰ\n");
        const auto broken = build_broker_snapshot(config, dictionary_configuration_id(config));
        const auto fallback = nlohmann::json::from_cbor(broken.bytes);
        check(lookup(fallback).size() == 2 && fallback.at("romaji").get<std::string>().empty(), "broken romaji stopped dictionary/default rules");
        check(broken.learning_allowed, "romaji fallback disabled dictionary learning");
        check(!fallback.at("complete").get<bool>(), "romaji fallback marked complete");
        check(wide_message(fallback.at("status").get<std::string>()).find(L"内蔵ルールで継続") != std::wstring::npos, "lost romaji fallback diagnostic");

        config.settings.romaji_rules = files.write("custom.tsv", "version=1\nwi\tゐ\n");
        const auto custom = nlohmann::json::from_cbor(build_broker_snapshot(config, dictionary_configuration_id(config)).bytes);
        check(RomajiRules::parse(custom.at("romaji").get<std::string>())->find("wi")->output == U"ゐ", "validated rules not transferred");
        check(custom.at("complete").get<bool>(), "valid sources not marked complete");
        const auto legacy = nlohmann::json::from_cbor(build_broker_snapshot(config, dictionary_configuration_id(config), {}, true).bytes);
        check(legacy.size() == 7 && legacy.at("version") == 1 && lookup(legacy).size() == 2, "legacy bootstrap compatibility");

        const auto cache = files.root / "cache";
        const auto before = build_broker_snapshot(config, dictionary_configuration_id(config), cache);
        const auto source_time = std::filesystem::last_write_time(first);
        const auto source_size = std::filesystem::file_size(first);
        { std::ofstream stream(first, std::ios::binary); stream << "かんじ /新字;優先/感じ/\n"; }
        std::filesystem::last_write_time(first, source_time);
        const auto rules_time = std::filesystem::last_write_time(config.settings.romaji_rules);
        { std::ofstream stream(config.settings.romaji_rules, std::ios::binary); stream << "version=1\nwi\tヰ\n"; }
        std::filesystem::last_write_time(config.settings.romaji_rules, rules_time);
        const auto changed = nlohmann::json::from_cbor(build_broker_snapshot(config, dictionary_configuration_id(config), cache).bytes);
        check(std::filesystem::file_size(first) == source_size && lookup(changed)[0].text == U"新字", "same-size/time dictionary stayed stale");
        check(RomajiRules::parse(changed.at("romaji").get<std::string>())->find("wi")->output == U"ヰ", "same-time rules stayed stale");
        check(lookup(nlohmann::json::from_cbor(before.bytes))[0].text == U"漢字", "old immutable snapshot changed");
        std::filesystem::remove(first);
        const auto missing_cached = nlohmann::json::from_cbor(build_broker_snapshot(config, dictionary_configuration_id(config), cache).bytes);
        check(lookup(missing_cached).empty() && !missing_cached.at("learning").get<bool>(), "missing source reused an old cache");
        for (const auto& entry : std::filesystem::directory_iterator(cache)) files.files.push_back(entry.path());
        files.files.push_back(cache);

        config.settings = {}; config.demo = true;
        const auto demo = nlohmann::json::from_cbor(build_broker_snapshot(config, dictionary_configuration_id(config)).bytes);
        check(!lookup(demo).empty() && !demo.at("learning").get<bool>(), "desktop/container demo mismatch");
        config.demo = false;
        const auto empty = nlohmann::json::from_cbor(build_broker_snapshot(config, dictionary_configuration_id(config)).bytes);
        check(lookup(empty).empty() && !empty.at("learning").get<bool>(), "empty settings enabled demo/learning");
        std::cout << "PASS broker partial sources, priority, learning denial, romaji fallback, validated rules, demo/empty and legacy bootstrap\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
