#include "broker_diagnostics.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <set>

using namespace nskkzime::windows;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(BrokerDiagnostics& log, int count, bool delay = false) {
    ContainerDiagnostic event{ContainerEvent::EndpointState}; event.phase = ContainerPhase::Probe;
    event.active = 4; event.phases[static_cast<std::size_t>(ContainerPhase::Accept)] = 4;
    for (int i = 0; i < count; ++i) { log.record(event); if (delay) Sleep(1); }
}
std::vector<nlohmann::json> read(const std::filesystem::path& file) {
    std::vector<nlohmann::json> records; std::ifstream stream(file, std::ios::binary); std::string line;
    while (std::getline(stream, line)) records.push_back(nlohmann::json::parse(line));
    return records;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--writer") {
            BrokerDiagnostics log(argv[2]); write(log, 350, true); return 0;
        }
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        const auto root = std::filesystem::path(executable).parent_path() /
            (L"diagnostics-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(root);
        const auto directory = root / L"diagnostics", file = directory / L"broker.jsonl";
        BrokerDiagnostics log(directory);
        ContainerDiagnostic event{ContainerEvent::BootstrapComplete}; event.phase = ContainerPhase::Acknowledge;
        event.operation = ContainerOperation::Init2; event.client = 123;
        SetLastError(ERROR_ACCESS_DENIED); log.record(event);
        check(GetLastError() == ERROR_ACCESS_DENIED, "logger changed pending native error");
        auto records = read(file);
        check(records.size() == 1 && records[0]["event"] == "bootstrap_response_acked" &&
            records[0]["operation"] == "INI2" && !records[0].contains("request") && !records[0].contains("win32_code"), "bootstrap record claimed client initialization or included input");
        event.event = ContainerEvent::RequestFailed; event.phase = ContainerPhase::Read;
        event.reason = ContainerReason::Timeout; log.record(event); log.record(event);
        records = read(file); check(records.size() == 2 && records.back()["reason"] == "timeout" && !records.back().contains("win32_code"), "timeout misreported or repeated failure not suppressed");
        event.event = ContainerEvent::EndpointState; event.phase = ContainerPhase::Probe;
        event.reason = ContainerReason::Win32; event.error = ERROR_FILE_NOT_FOUND; log.record(event);
        event.reason = ContainerReason::None; event.error = 0; log.record(event);
        records = read(file); check(records[2]["state"] == "missing" && records[2]["win32_code"] == 2 &&
            records[3]["state"] == "available", "missing and recovery not distinguished");
        event.container = L"private user input\n漢字"; log.record(event);
        check(read(file).size() == records.size(), "arbitrary text accepted as endpoint metadata"); event.container = {};
        {
            NativeHandle lock(CreateFileW((directory / L"broker.lock").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
            check(lock.valid(), "lock fixture unavailable"); const auto started = GetTickCount64(); log.record(event);
            check(GetTickCount64() - started < 250 && read(file).size() == records.size(), "contended logging blocked or bypassed lock");
        }
        const auto broken = root / L"broken"; { std::ofstream stream(broken); stream << "fixture"; }
        BrokerDiagnostics unavailable(broken); unavailable.record(event);
        check(std::filesystem::is_regular_file(broken), "broken logging changed destination");
        const auto target = root / L"link-target", link = root / L"link";
        std::filesystem::create_directory(target);
        if (CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            BrokerDiagnostics redirected(link / L"new-diagnostics"); redirected.record(event);
            check(!std::filesystem::exists(target / L"new-diagnostics"), "logger created files through directory symlink");
        } else std::cout << "directory symlink fixture unavailable on this system\n";
        // Exercise actual rotation, not a mock size check.
        write(log, 5000);
        const auto previous = directory / L"broker.previous.jsonl";
        check(std::filesystem::exists(previous) && std::filesystem::file_size(previous) <= BrokerDiagnostics::file_limit &&
            std::filesystem::file_size(file) <= BrokerDiagnostics::file_limit, "rotation exceeded bounds");
        check(!read(previous).empty() && !read(file).empty(), "rotated JSONL is invalid or empty");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        auto command = L"\"" + std::wstring(executable) + L"\" --writer \"" + directory.native() + L"\"";
        check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "launch concurrent writer");
        CloseHandle(process.hThread); NativeHandle child(process.hProcess);
        write(log, 100, true);
        check(WaitForSingleObject(child.get(), 10000) == WAIT_OBJECT_0, "concurrent writer stalled");
        DWORD code = 0; GetExitCodeProcess(child.get(), &code); check(code == 0, "concurrent writer failed");
        std::set<DWORD> processes;
        for (const auto& row : read(file)) processes.insert(row.at("pid").get<DWORD>());
        for (const auto& row : read(previous)) processes.insert(row.at("pid").get<DWORD>());
        check(processes.contains(GetCurrentProcessId()) && processes.contains(process.dwProcessId), "cross-process records lost or mixed");
        check(std::filesystem::file_size(file) <= BrokerDiagnostics::file_limit, "concurrent append exceeded bound");
        std::cout << "diagnostic metadata, suppression, contention, failure isolation, rotation and concurrent JSONL passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
