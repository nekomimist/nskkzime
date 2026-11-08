#include "platform.h"
#include <iostream>
#include <set>

using namespace nskkzime::setup;
int wmain(int argc, wchar_t** argv) {
    bool log_ready = false;
    fs::path root;
    std::string action = "Status";
    try {
        assert_native_x64();
        fs::path package = executable_path().parent_path().parent_path();
        std::string version;
        std::wstring expected_sid;
        bool elevated_child = false;
        std::set<std::wstring> options;
        for (int i = 1; i < argc; ++i) {
            const std::wstring option(argv[i]);
            require(options.insert(option).second, "Duplicate option");
            if (option == L"--help") {
                std::cout << "nskkzime_setup [--action Verify|Status|Install|Unregister|Rollback|Recover] [--package-directory PATH] [--version SHA256]\n"
                    "Changes request UAC approval by the same user. Unregister retains program files and user data.\n";
                return 0;
            }
            if (option == L"--elevated-child") { elevated_child = true; continue; }
            require(i + 1 < argc, "Missing option value");
            const std::wstring value(argv[++i]);
            if (option == L"--action") action = utf8(value);
            else if (option == L"--package-directory") package = fs::absolute(value).lexically_normal();
            else if (option == L"--version") version = utf8(value);
            else if (option == L"--expected-user-sid") expected_sid = value;
            else throw std::runtime_error("Unknown option: " + utf8(option));
        }
        require(action == "Verify" || action == "Status" || action == "Install" || action == "Unregister" || action == "Rollback" || action == "Recover", "Unknown action");
        require(action == "Rollback" ? version_id(version) : version.empty(), "--version is required only for Rollback");
        require(expected_sid.empty() || expected_sid == user_sid(), "Elevation changed the user; approve UAC using the same account");
        require(!elevated_child || (elevated() && !expected_sid.empty()), "Invalid elevated handoff");
        if (action == "Verify") {
            const auto bundle = verify_package(package);
            std::cout << Json{{"Verified", true}, {"FormatVersion", bundle.format}, {"ManifestSHA256", bundle.version}, {"Files", bundle.files.size() - 1}}.dump(2) << '\n';
            return 0;
        }
        root = program_root();
        auto backend = windows_backend();
        Installation installation(root, backend);
        if (action == "Status") { std::cout << installation.status().dump(2) << '\n'; return 0; }
        assert_install_context();
        if (action == "Install") (void)verify_package(package); // Reject before UAC or any machine writes.
        if (!elevated()) {
            const auto previous = backend.read_registration(false);
            std::string log_before;
            try { log_before = read_setup_log(root); } catch (const std::exception&) { /* No readable protected log yet. */ }
            std::vector<std::wstring> args{L"--action", wide(action), L"--package-directory", package.native(), L"--expected-user-sid", user_sid(), L"--elevated-child"};
            if (!version.empty()) { args.emplace_back(L"--version"); args.push_back(wide(version)); }
            const auto code = elevate(args);
            if (code != 0) {
                std::cerr << "Setup failed (exit " << code << ").\n";
                try {
                    const auto details = appended_log(log_before, read_setup_log(root));
                    if (!details.empty()) std::cerr << details;
                } catch (const std::exception&) { /* Preserve the original child exit code. */ }
                std::cerr << "Details, if recorded: %ProgramFiles%\\nskkzime\\setup.log. Run Status; use Recover if a transaction is pending.\n";
                return static_cast<int>(code);
            }
            // Only the original, unelevated process may launch a user's broker.
            try {
                if (!previous.empty()) {
                    const auto old = installation.installed(installation.managed(previous));
                    if (!old.broker().empty()) { protected_path(old.broker()); require(run_process(old.broker(), {L"--stop"}) == 0, "Stop old broker failed"); }
                }
                const auto current = installation.managed(backend.read_registration(false));
                if (!current.empty()) {
                    const auto bundle = installation.installed(current);
                    if (!bundle.broker().empty()) { protected_path(bundle.broker()); (void)run_process(bundle.broker(), {L"--replace"}, false); }
                }
            } catch (const std::exception& error) { std::cerr << "Registration succeeded; broker switch deferred until sign-in: " << error.what() << '\n'; }
            std::cout << installation.status().dump(2) << '\n'; return 0;
        }
        // Existing unmanaged registrations must fail before creating a root.
        (void)installation.managed(backend.read_registration(false));
        (void)installation.managed(backend.read_registration(true), true);
        require(fs::exists(root) || action == "Install", "Installation root does not exist");
        initialize_root(root);
        installation.preflight(version);
        const auto lock_path = root / L"operation.lock";
        no_reparse(lock_path);
        Handle lock(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        win_check(lock.value != INVALID_HANDLE_VALUE, "Installation is busy or inaccessible");
        assert_root(root); protected_tree(root); assert_no_registrar();
        installation.preflight(version);
        require(action == "Recover" || installation.journal().is_null(), "Pending transaction; run Recover first");
        ensure_appcontainer_access(root);
        append_log(root, "begin action=" + action); log_ready = true;
        if (action == "Install") {
            const auto bundle = installation.stage(package);
            // Recheck inherited permissions on every copied file before executing any of it.
            protected_tree(root);
            append_log(root, "target=" + bundle.version);
            installation.transition(bundle.version);
        }
        else if (action == "Rollback") installation.transition(version);
        else if (action == "Unregister") installation.transition({});
        else installation.recover();
        append_log(root, "success action=" + action);
        std::cout << installation.status().dump(2) << '\n';
        if (!elevated_child) std::cout << "Sign out and back in to restart the broker as your normal user.\n";
        return 0;
    } catch (const std::exception& error) {
        if (log_ready) try { append_log(root, "failed action=" + action + " cause=" + error.what()); } catch (const std::exception&) {}
        std::cerr << "nskkzime setup: " << error.what() << '\n'; return 1;
    }
}
