#include "tool_launcher.h"
#include "dictionary_settings.h"
#include "learning_ipc.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <shellapi.h>

using namespace nskkzime::windows;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void until(F condition) {
    const auto deadline = GetTickCount64() + 5000;
    do { if (condition()) return; Sleep(10); } while (GetTickCount64() < deadline);
    throw std::runtime_error("tool launch did not complete");
}
struct Files {
    std::filesystem::path root = std::filesystem::current_path() / (L"tool-launch 日本語 space-" + wide_message(learning_client_id()));
    Files() { std::filesystem::create_directory(root); }
    ~Files() {
        for (const auto* name : {L"nskkzime_settings.exe", L"nskkzime_dictionary_gui.exe", L"設定 space.ini.received", L"学習 辞書.nskkdb.received"}) {
            std::error_code ignored; std::filesystem::remove(root / name, ignored);
        }
        std::error_code ignored; std::filesystem::remove(root, ignored);
    }
};
std::string read(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary); return {std::istreambuf_iterator<char>(stream), {}};
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--open-guis") {
        // Optional native smoke check with the caller's explicitly isolated
        // NSKKZIME_CONFIG / NSKKZIME_USER_DICTIONARY paths; not used by CTest.
        const auto commands = current_tool_commands();
        for (const auto& command : commands) {
            const auto error = launch_tool_process(command);
            if (error) { std::cerr << "GUI startup failed: " << error << '\n'; return 2; }
        }
        return 0;
    }
    if (argc == 3 && (std::wstring_view(argv[1]) == L"--config" || std::wstring_view(argv[1]) == L"--file")) {
        // The production launcher starts this copied child under the GUI's exact
        // name. Record decoded argv and cwd; never touch the requested data file.
        std::ofstream stream(std::filesystem::path(std::wstring(argv[2]) + L".received"), std::ios::binary);
        stream << utf8_message(argv[1]) << '\n' << utf8_message(argv[2]) << '\n' << utf8_message(std::filesystem::current_path().native());
        return stream ? 0 : 2;
    }
    Files files;
    try {
        wchar_t self[32768]{}; check(GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) != 0, "test path missing");
        std::array<ToolCommand, 2> commands{{
            {files.root / L"nskkzime_settings.exe", files.root / L"設定 space.ini", ImeTool::Settings},
            {files.root / L"nskkzime_dictionary_gui.exe", files.root / L"学習 辞書.nskkdb", ImeTool::Dictionary}
        }};
        for (const auto& command : commands) std::filesystem::copy_file(self, command.executable);
        SetEnvironmentVariableW(L"NSKKZIME_CONFIG", commands[0].file.c_str());
        SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", commands[1].file.c_str());
        const auto captured = current_tool_commands();
        SetEnvironmentVariableW(L"NSKKZIME_CONFIG", L"relative-invalid.ini");
        SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", L"relative-invalid.nskkdb");
        check(captured[0].file == commands[0].file && captured[1].file == commands[1].file, "captured environment paths changed");
        check(captured[0].executable.parent_path() == std::filesystem::path(self).parent_path() &&
            captured[1].executable.filename() == L"nskkzime_dictionary_gui.exe", "launcher searched outside module directory");
        const auto invalid = current_tool_commands();
        check(invalid[0].file.empty() && invalid[1].file.empty(), "invalid override fell back to personal data");
        const auto owner = GetCurrentThreadId(); std::atomic<DWORD> worker{owner}, failure{ERROR_SUCCESS};
        {
            ToolLauncher launcher(commands, [&](const ToolCommand& command) {
                worker.store(GetCurrentThreadId()); return launch_tool_process(command);
            }, [&](ImeTool, DWORD error) { failure.store(error); });
            check(launcher.open(ImeTool::Settings) == S_OK && launcher.open(ImeTool::Dictionary) == S_OK, "GUI launch not queued");
            for (const auto& command : commands) {
                const auto expected = std::string(command.tool == ImeTool::Settings ? "--config\n" : "--file\n") +
                    utf8_message(command.file.native()) + '\n' + utf8_message(files.root.native());
                until([&] { return read(command.file.native() + L".received") == expected; });
                check(!std::filesystem::exists(command.file), "opening GUI wrote the data file");
            }
            check(worker.load() != owner && !failure.load(), "launch ran in caller or failed");
        }
        struct Gate {
            std::atomic<bool> entered{false}, released{false}, finished{false};
            std::atomic<unsigned> calls{0}, reports{0};
        };
        const auto gate = std::make_shared<Gate>();
        struct ReleaseOnExit { std::shared_ptr<Gate> gate; ~ReleaseOnExit() { gate->released.store(true); } } release{gate};
        {
            ToolLauncher launcher(commands, [gate](const ToolCommand&) {
                ++gate->calls; gate->entered.store(true); while (!gate->released.load()) Sleep(5); gate->finished.store(true); return ERROR_FILE_NOT_FOUND;
            }, [gate](ImeTool, DWORD) { ++gate->reports; });
            check(launcher.open(ImeTool::Settings) == S_OK, "gated launch not queued");
            until([&] { return gate->entered.load(); });
            check(launcher.open(ImeTool::Settings) == S_FALSE, "rapid duplicate launched twice");
            const auto start = GetTickCount64(); launcher.close();
            check(launcher.open(ImeTool::Dictionary) == S_FALSE && GetTickCount64() - start < 100, "closed launcher remained live or joined worker");
        }
        gate->released.store(true); until([&] { return gate->finished.load(); });
        Sleep(20); check(gate->calls.load() == 1 && !gate->reports.load(), "late error from closed launcher reported");
        {
            auto missing = commands; missing[0].executable = files.root / L"absent.exe";
            std::atomic<DWORD> observed{0};
            ToolLauncher launcher(missing, launch_tool_process, [&](ImeTool tool, DWORD error) {
                if (tool == ImeTool::Settings) observed.store(error);
            });
            launcher.open(ImeTool::Settings); until([&] { return observed.load() != 0; });
            check(observed.load() == ERROR_FILE_NOT_FOUND, "startup failure not reported accurately");
        }
        for (const auto* path : {L"relative.ini", L"C:\\quoted\" --file wrong", L"C:\\control\n.ini"}) {
            auto invalid_command = commands[0]; invalid_command.file = path;
            check(launch_tool_process(invalid_command) == ERROR_INVALID_PARAMETER, "invalid path accepted");
        }
        auto trailing = commands[0]; trailing.file = files.root.native() + L"\\";
        int count = 0; const auto line = tool_command_line(trailing); auto decoded = CommandLineToArgvW(line.c_str(), &count);
        const bool roundtrip = decoded && count == 3 && std::wstring(decoded[2]) == trailing.file.native();
        if (decoded) LocalFree(decoded); check(roundtrip, "trailing backslash broke command line");
        std::cout << "PASS frozen paths, explicit executable/argv/cwd, background launch, teardown, duplicate clicks, errors and quoting\n";
        Sleep(100); // Copied fixture processes have written their result and are exiting.
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
