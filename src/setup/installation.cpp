#include "installation.h"
#include <bcrypt.h>
#include <algorithm>

namespace nskkzime::setup {
std::string unique_suffix() {
    std::string random(32, '\0');
    require(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "Random ID failed");
    return sha256(random);
}
std::string Installation::managed(const fs::path& dll, bool x86) const {
    if (dll.empty()) return {};
    require(dll.is_absolute(), "Registered DLL is not an absolute path");
    const auto normalized = dll.lexically_normal();
    auto parent = normalized.parent_path().parent_path();
    if (x86) parent = parent.parent_path();
    const auto version = utf8(parent.filename().native());
    const auto expected = root_ / L"versions" / wide(version) / (x86 ? L"bin/x86/nskkzime_tsf.dll" : L"bin/nskkzime_tsf.dll");
    require(version_id(version) && same_path(normalized, expected), "Unmanaged IME registration; use its original installer to remove it");
    return version;
}
Bundle Installation::installed(const std::string& version) const {
    require(version_id(version), "Version must be a lowercase SHA-256 ID");
    const auto bundle = verify_(root_ / L"versions" / wide(version));
    require(bundle.version == version, "Installed directory and manifest ID differ");
    return bundle;
}
Json Installation::journal() const {
    const auto path = root_ / L"transaction.json";
    no_reparse(path);
    if (!fs::exists(path)) return nullptr;
    auto record = parse_json(read_file(path, 16 * 1024));
    require(record.at("format_version").is_number_integer() && (record.at("format_version") == 1 || record.at("format_version") == 2), "Unsupported transaction journal");
    for (const auto* key : {"previous", "target"})
        require(record.at(key).is_null() || (record.at(key).is_string() && version_id(record.at(key).get<std::string>())), "Invalid journal version ID");
    return record;
}
Json Installation::status() const {
    no_reparse(root_);
    const auto dll = backend_.read_registration(false), x86 = backend_.read_registration(true);
    std::string version;
    try { version = managed(dll); } catch (const std::exception&) { /* Status still reports unmanaged paths. */ }
    auto available = Json::array();
    const auto versions = root_ / L"versions";
    no_reparse(versions);
    if (fs::exists(versions)) for (const auto& item : fs::directory_iterator(versions)) {
        no_reparse(item.path());
        if (item.is_directory() && version_id(utf8(item.path().filename().native()))) available.push_back(utf8(item.path().filename().native()));
    }
    std::sort(available.begin(), available.end());
    return {{"Root", utf8(root_.native())}, {"RegisteredDll", utf8(dll.native())}, {"RegisteredX86Dll", utf8(x86.native())},
        {"ManagedVersion", version}, {"Pending", journal()}, {"AvailableVersions", available}};
}
void Installation::preflight(const std::string& target) const {
    const auto current = managed(backend_.read_registration(false));
    const auto x86 = managed(backend_.read_registration(true), true);
    for (const auto& version : {current, x86, target}) if (!version.empty()) (void)installed(version);
    const auto pending = journal();
    if (!pending.is_null()) for (const auto* key : {"previous", "target"}) if (!pending.at(key).is_null()) (void)installed(pending.at(key).get<std::string>());
}
Bundle Installation::stage(const fs::path& source) {
    const auto bundle = verify_(source);
    const auto destination = root_ / L"versions" / wide(bundle.version);
    no_reparse(destination);
    if (fs::exists(destination)) return installed(bundle.version);
    fs::create_directory(root_ / L"versions");
    const auto staging = root_ / wide("staging-" + unique_suffix());
    fs::create_directory(staging);
    // The verified fixed inventory is authoritative; never follow arbitrary manifest paths.
    for (const auto& name : bundle.files) {
        const auto target = staging / wide(name);
        no_reparse(bundle.directory / wide(name));
        fs::create_directories(target.parent_path());
        fs::copy_file(bundle.directory / wide(name), target, fs::copy_options::none);
    }
    require(verify_(staging).version == bundle.version, "Package changed while copying; staging retained");
    win_check(MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH), "Commit staged version");
    return installed(bundle.version);
}
void Installation::assert_registration(bool x86, const fs::path& expected) const {
    require(same_path(backend_.read_registration(x86), expected), "IME registration changed unexpectedly");
}
void Installation::set_target(const std::string& target, const std::vector<std::string>& allowed) {
    const auto current = backend_.read_registration(false), companion = backend_.read_registration(true);
    const auto version = managed(current), x86_version = managed(companion, true);
    for (const auto& observed : {version, x86_version})
        require(observed.empty() || std::find(allowed.begin(), allowed.end(), observed) != allowed.end(), "Registration points outside transaction");
    const auto next = target.empty() ? Bundle{} : installed(target);
    const auto old = version.empty() ? Bundle{} : installed(version);
    const auto old_x86 = x86_version.empty() ? Bundle{} : installed(x86_version);
    if (!x86_version.empty()) require(same_path(old_x86.x86(), companion), "Companion path differs from verified package");
    assert_registration(false, current); assert_registration(true, companion);
    if (!x86_version.empty()) {
        backend_.register_dll(old_x86.x86(), true);
        assert_registration(true, {}); assert_registration(false, current);
    }
    if (!version.empty()) { backend_.register_dll(old.dll(), true); assert_registration(false, {}); }
    if (!target.empty()) {
        assert_registration(false, {}); assert_registration(true, {});
        backend_.register_dll(next.dll(), false); assert_registration(false, next.dll());
        if (!next.x86().empty()) { backend_.register_dll(next.x86(), false); assert_registration(true, next.x86()); }
    }
    assert_registration(true, target.empty() ? fs::path{} : next.x86());
    backend_.set_startup(target.empty() ? fs::path{} : next.broker());
}
void Installation::transition(const std::string& target) {
    require(journal().is_null(), "Pending transaction; run Recover first");
    preflight(target);
    const auto previous = managed(backend_.read_registration(false));
    assert_registration(true, previous.empty() ? fs::path{} : installed(previous).x86());
    if (previous == target) { backend_.set_startup(target.empty() ? fs::path{} : installed(target).broker()); return; }
    const auto temporary = root_ / wide("transaction-" + unique_suffix() + ".tmp");
    const Json record{{"format_version", 2}, {"previous", previous.empty() ? Json(nullptr) : Json(previous)}, {"target", target.empty() ? Json(nullptr) : Json(target)}};
    write_new(temporary, record.dump() + "\n");
    win_check(MoveFileExW(temporary.c_str(), (root_ / L"transaction.json").c_str(), MOVEFILE_WRITE_THROUGH), "Commit journal");
    try {
        set_target(target, {previous, target});
        win_check(DeleteFileW((root_ / L"transaction.json").c_str()), "Remove completed journal");
    } catch (const std::exception& original) {
        const std::string error = original.what();
        try {
            set_target(previous, {previous, target});
            win_check(DeleteFileW((root_ / L"transaction.json").c_str()), "Remove restored journal");
        } catch (const std::exception& recovery) {
            throw std::runtime_error("Update and recovery failed; journal retained. Run Recover. Cause: " + error + "; recovery: " + recovery.what());
        }
        throw std::runtime_error("Update failed; previous registration restored. Cause: " + error);
    }
}
void Installation::recover() {
    preflight();
    const auto record = journal();
    if (record.is_null()) {
        const auto current = managed(backend_.read_registration(false));
        assert_registration(true, current.empty() ? fs::path{} : installed(current).x86());
        backend_.set_startup(current.empty() ? fs::path{} : installed(current).broker());
        return;
    }
    require(record.at("format_version") != 1 || backend_.read_registration(true).empty(), "Legacy journal contains a companion registration");
    const auto previous = record.at("previous").is_null() ? std::string{} : record.at("previous").get<std::string>();
    const auto target = record.at("target").is_null() ? std::string{} : record.at("target").get<std::string>();
    set_target(previous, {previous, target});
    win_check(DeleteFileW((root_ / L"transaction.json").c_str()), "Remove recovered journal");
}
}
