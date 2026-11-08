#include "platform.h"
#include <shellapi.h>
#include <iostream>

using namespace nskkzime::setup;
namespace {
void check(bool value, const char* message) { require(value, message); }
template<class Function> void fails(Function action, const std::string& text = {}) {
    try { action(); } catch (const std::exception& error) {
        check(text.empty() || std::string(error.what()).find(text) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error("Expected failure");
}
void replace_file(const fs::path& path, const std::string& bytes) { fs::remove(path); write_new(path, bytes); }
Json build_info(bool x86) {
    return {{"format_version", 1}, {"source_revision", std::string(40, 'a')}, {"source_sha256", std::string(64, 'b')},
        {"source_modified", false}, {"configuration", "Release"}, {"compiler", "MSVC 19.44.35207.1"},
        {"variant", x86 ? "x86-companion" : "zenz-cpu"}, {"cmake_cache_sha256", std::string(64, 'c')},
        {"llama_revision", x86 ? Json(nullptr) : Json(std::string(40, 'd'))},
        {"llama_source_sha256", x86 ? Json(nullptr) : Json(std::string(64, 'e'))}};
}
std::string image(const std::string& name) {
    const bool x86 = name.starts_with("bin/x86/");
    auto metadata = build_info(x86);
    metadata["binary"] = utf8(fs::path(wide(name)).filename().native());
    std::string bytes(4608, '\0');
    auto put = [&](size_t offset, uint32_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 255);
    };
    put(0, 0x5a4d, 2); put(60, 64, 4); put(64, 0x4550, 4); put(68, x86 ? 0x14c : 0x8664, 2);
    put(70, 1, 2); put(84, 240, 2); put(88, x86 ? 0x10b : 0x20b, 2);
    bytes.replace(328, 6, ".nskkz"); put(344, 4096, 4); put(348, 512, 4); put(364, 0x40000000, 4);
    const auto json = metadata.dump(); bytes.replace(512, json.size(), json);
    return bytes;
}
Bundle fixture(const fs::path& path, int format, const std::string& label) {
    fs::create_directories(path);
    Json manifest{{"format_version", format}, {"product", "nskkzime"}, {"architecture", "x64"}, {"variant", "zenz-cpu"},
        {"configuration", "Release"}, {"version", "0.2.0"}, {"first_party_license", "MIT"},
        {"binary_build_metadata", build_info(false)}, {"x86_build_metadata", build_info(true)},
        {"binary_build_revision", std::string(40, 'a')}, {"files", Json::array()}};
    for (const auto& name : package_names(format)) {
        auto bytes = name.starts_with("bin/") ? image(name) : label;
        const auto target = path / wide(name); fs::create_directories(target.parent_path()); write_new(target, bytes);
        manifest["files"].push_back({{"path", name}, {"size", bytes.size()}, {"sha256", sha256(bytes)}});
    }
    write_new(path / L"manifest.json", manifest.dump());
    return verify_package(path);
}
void rehash(const fs::path& path, const std::string& name) {
    auto manifest = parse_json(read_file(path / L"manifest.json"));
    const auto bytes = read_file(path / wide(name));
    for (auto& entry : manifest["files"]) if (entry["path"] == name) { entry["size"] = bytes.size(); entry["sha256"] = sha256(bytes); }
    replace_file(path / L"manifest.json", manifest.dump());
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--process-child") return 23;
    // All filesystem changes stay in this uniquely named CTest working directory.
    const auto root = fs::absolute(fs::path("native-setup-test-" + unique_suffix()));
    try {
        for (const bool x86_view : {false, true}) {
            const auto registrar = registrar_path(x86_view);
            check(registrar.is_absolute(), ("Non-absolute registrar: " + utf8(registrar.native())).c_str());
            check(fs::is_regular_file(registrar), "Windows registrar was not found");
        }
        check(run_process(executable_path(), {L"--process-child"}) == 23, "Native child exit code was lost");
        check(appended_log("old error\n", "old error\nnew error\n") == "new error\n", "Elevated failure details were lost");
        check(appended_log("old error\n", "old error\n").empty(), "Stale failure must not be reported again");
        check(appended_log("old error\n", "replaced log\n").empty(), "Replaced log must not be treated as appended records");
        check(appended_log({}, std::string(32768, 'x')).size() < 16500, "Failure output must be bounded");
        fs::create_directory(root);
        const auto source = root / L"package 日本語 space";
        auto a = fixture(source, 8, "A");
        const auto legacy = fixture(root / L"legacy", 5, "legacy");
        const auto old_release = fixture(root / L"legacy6", 6, "release");
        const auto oldest = fixture(root / L"legacy2", 2, "oldest");
        check(a.format == 8 && legacy.format == 5, "Formats 5 and 8 must verify");
        check(a.version == sha256(read_file(source / L"manifest.json")), "Manifest identity must use original bytes");
        fails([] { (void)parse_json("{\"x\":1,\"X\":2}"); }, "Duplicate");
        fails([] { (void)parse_json("{\"x\":1,\"x\":2}"); }, "Duplicate");
        fails([] { (void)parse_json(std::string("{\"x\":\"") + '\xff' + "\"}"); });
        const auto manifest_bytes = read_file(source / L"manifest.json");
        auto manifest = parse_json(manifest_bytes); manifest["format_version"] = 8.0;
        replace_file(source / L"manifest.json", manifest.dump()); fails([&] { verify_package(source); });
        replace_file(source / L"manifest.json", manifest_bytes);
        manifest = parse_json(manifest_bytes); manifest["binary_build_metadata"]["format_version"] = 1.0;
        replace_file(source / L"manifest.json", manifest.dump()); fails([&] { verify_package(source); }, "metadata mismatch");
        replace_file(source / L"manifest.json", manifest_bytes);
        write_new(source / L"extra", "x"); fails([&] { verify_package(source); }, "Unexpected"); fs::remove(source / L"extra");
        replace_file(source / L"README.md", "tampered"); fails([&] { verify_package(source); }, "mismatch");
        replace_file(source / L"README.md", "A");
        const auto original_image = read_file(source / L"bin/nskkzime_setup.exe");
        auto bad_image = original_image; bad_image[68] = char(0x4c); bad_image[69] = 1;
        replace_file(source / L"bin/nskkzime_setup.exe", bad_image); rehash(source, "bin/nskkzime_setup.exe");
        fails([&] { verify_package(source); }, "architecture");
        replace_file(source / L"bin/nskkzime_setup.exe", original_image); replace_file(source / L"manifest.json", manifest_bytes);
        bad_image = original_image; bad_image[348] = '\xff'; bad_image[349] = '\xff'; bad_image[350] = '\xff'; bad_image[351] = '\x7f';
        replace_file(source / L"bin/nskkzime_setup.exe", bad_image); rehash(source, "bin/nskkzime_setup.exe");
        fails([&] { verify_package(source); }, "metadata section");
        replace_file(source / L"bin/nskkzime_setup.exe", original_image); replace_file(source / L"manifest.json", manifest_bytes);
        fails([&] { protected_path(root); }); // A user's writable directory is never a production root.
        const std::vector<std::wstring> arguments{L"", L"日本語 space", L"C:\\space dir\\", L"quote\"slash\\", L"--action"};
        auto command = quote(L"setup.exe");
        for (const auto& argument : arguments) command += L" " + quote(argument);
        int count = 0; auto parsed = CommandLineToArgvW(command.c_str(), &count);
        check(parsed && count == static_cast<int>(arguments.size() + 1), "Argument count");
        for (size_t i = 0; i < arguments.size(); ++i) check(arguments[i] == parsed[i + 1], "Quoted argument lost");
        LocalFree(parsed);
        const auto managed_root = root / L"managed"; fs::create_directory(managed_root);
        fs::path native, x86, startup;
        std::string failure;
        int failures_left = 0, calls = 0;
        auto fault = [&](const std::string& operation) { if (failure == operation && failures_left > 0) { --failures_left; throw std::runtime_error("injected " + operation); } };
        Backend backend{
            [&](bool companion) { return companion ? x86 : native; },
            [&](const fs::path& dll, bool remove) {
                ++calls; const bool companion = dll.parent_path().filename() == L"x86";
                fault(std::string(remove ? "remove-" : "add-") + (companion ? "x86" : "native"));
                (companion ? x86 : native) = remove ? fs::path{} : dll;
            },
            [&](const fs::path& broker) { fault("startup"); startup = broker; }};
        Installation install(managed_root, backend);
        a = install.stage(source);
        install.transition(a.version);
        check(same_path(native, a.dll()) && same_path(x86, a.x86()) && same_path(startup, a.broker()), "Initial installation");
        auto b = install.stage(legacy.directory);
        for (const auto& operation : {"remove-x86", "remove-native", "add-native", "add-x86", "startup"}) {
            failure = operation; failures_left = 1;
            fails([&] { install.transition(b.version); }, "restored");
            check(same_path(native, a.dll()) && same_path(x86, a.x86()) && same_path(startup, a.broker()) && install.journal().is_null(), "Failure must restore both views and startup");
        }
        failure.clear(); install.transition(b.version); install.transition(a.version);
        failure = "add-native"; failures_left = 2;
        fails([&] { install.transition(b.version); }, "journal retained");
        check(!install.journal().is_null(), "Failed recovery must retain journal");
        failure.clear(); install.recover();
        check(same_path(native, a.dll()) && same_path(x86, a.x86()) && install.journal().is_null(), "Recovery after restart");
        // A transaction written by the PowerShell installer remains readable.
        Json pending{{"format_version", 2}, {"previous", a.version}, {"target", b.version}};
        write_new(managed_root / L"transaction.json", pending.dump());
        native = b.dll(); x86 = b.x86();
        fails([&] { install.transition(a.version); }, "Pending"); install.recover();
        check(same_path(native, a.dll()) && install.journal().is_null(), "Legacy journal recovery");
        const auto calls_before = calls;
        native = root / L"unmanaged.dll";
        fails([&] { install.transition(b.version); }, "Unmanaged"); check(calls == calls_before, "Unmanaged registration mutated"); native = a.dll();
        replace_file(a.directory / L"README.md", "corrupt");
        fails([&] { install.transition(b.version); }, "mismatch"); check(calls == calls_before, "Corrupt old version mutated registry");
        replace_file(a.directory / L"README.md", "A");
        install.transition({}); check(native.empty() && x86.empty() && startup.empty() && fs::exists(a.dll()), "Unregister must retain files");
        install.transition({}); install.recover();
        const auto release = install.stage(old_release.directory);
        install.transition(release.version); install.transition(a.version);
        const auto first = install.stage(oldest.directory);
        install.transition(first.version);
        check(x86.empty() && startup.empty(), "Legacy package without companion or broker");
        pending = {{"format_version", 1}, {"previous", nullptr}, {"target", first.version}};
        write_new(managed_root / L"transaction.json", pending.dump()); install.recover();
        check(native.empty() && install.journal().is_null(), "Version-1 journal compatibility");
        const auto before_reject = calls;
        write_new(managed_root / L"transaction.json", "{\"format_version\":2,\"previous\":\"../escape\",\"target\":null}");
        fails([&] { install.recover(); }, "Invalid journal"); check(calls == before_reject, "Malformed journal changed registration");
        fs::remove(managed_root / L"transaction.json");
        check(!install.status().at("AvailableVersions").empty(), "Status must retain available versions");
        std::cout << "PASS native package validation, legacy migration, both ABI transitions, failure rollback, journal recovery, quoting and ACL rejection\n";
        no_reparse(root); fs::remove_all(root); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << " (fixtures retained at " << utf8(root.native()) << ")\n"; return 1; }
}
