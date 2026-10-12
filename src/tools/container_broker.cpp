#include "app_container.h"
#include "broker_snapshot.h"
#include "broker_diagnostics.h"
#include "runtime_configuration.h"
#include "dictionary_settings.h"
#include "inference_client.h"
#include "learning_client.h"
#include "tool_launcher.h"
#include "nskkzime/compiled_dictionary.h"
#include <bcrypt.h>
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
struct Broker {
    BrokerDiagnostics* diagnostics = nullptr;
    std::mutex snapshot_mutex, log_mutex;
    std::string log_key;
    std::string snapshot_generation;
    bool snapshot_learning = false;
    std::unique_ptr<InputLog> logger;
    std::string snapshot(const DictionaryConfiguration& config, const std::string& id) {
        // Only configured paths are opened; no file path comes from a client.
        const std::lock_guard lock(snapshot_mutex);
        // Read source bytes on every bootstrap. The shared dictionary cache
        // hashes those exact bytes, including same-size/same-time replacements.
        auto loaded = build_broker_snapshot(config, id, dictionary_cache_path(config));
        snapshot_generation = id; snapshot_learning = loaded.learning_allowed;
        if (diagnostics) {
            ContainerDiagnostic event{ContainerEvent::Snapshot}; event.phase = ContainerPhase::Handle;
            event.sources = loaded.sources; event.failed_sources = loaded.failed_sources;
            event.romaji_fallback = loaded.romaji_fallback ? 1 : 0; diagnostics->record(event);
        }
        return std::move(loaded.bytes);
    }
    bool learning_allowed(const DictionaryConfiguration& config, const std::string& id) {
        {
            const std::lock_guard lock(snapshot_mutex);
            if (snapshot_generation == id) return snapshot_learning;
        }
        (void)snapshot(config, id);
        const std::lock_guard lock(snapshot_mutex);
        return snapshot_generation == id && snapshot_learning;
    }
    std::string handle(std::string_view bytes, const std::function<bool()>& cancelled) {
        const auto config = current_dictionary_configuration(); const auto id = dictionary_configuration_id(config);
        if (bytes == "INI3") return snapshot(config, id);
        // Reactivation reuses a retained snapshot while its settings are current.
        if (bytes.starts_with("CHK1")) return bytes.size() == 68 && bytes.substr(4) == id ? "SAME" : "STAL";
        if (bytes.size() < 68) throw std::runtime_error("stale settings");
        const auto operation = bytes.substr(0, 4), body = bytes.substr(68);
        if (bytes.substr(4, 64) != id) {
            // Learning workers outlive a settings change until the next activation.
            if (operation == "LRN1") return "STAL";
            throw std::runtime_error("stale settings");
        }
        const auto runtime = capture_runtime_configuration(config);
        if (operation == "GUI1") {
            // The transport rejects this operation from AppContainer peers.
            // No caller-supplied path or command line crosses the boundary.
            ToolCommand command;
            if (body == "settings") {
                command.tool = ImeTool::Settings; command.file = config.file;
                command.executable = container_broker_path().parent_path() / L"nskkzime_settings.exe";
            } else if (body == "dictionary") {
                command.tool = ImeTool::Dictionary; command.file = runtime.user_file;
                command.executable = container_broker_path().parent_path() / L"nskkzime_dictionary_gui.exe";
            } else throw std::runtime_error("unknown GUI");
            if (launch_tool_process(command) != ERROR_SUCCESS) throw std::runtime_error("GUI unavailable");
            return "OKAY";
        }
        if (operation == "LRN1") {
            if (!learning_allowed(config, id)) throw std::runtime_error("learning disabled");
            const auto& file = runtime.user_file;
            // Snapshots come from the saved file, so polling containers never keep the helper alive.
            if (body.size() == 12 && body.starts_with("GET1")) return learning_snapshot_response(file, body);
            if (body.size() == 12 && body.starts_with("WAT1")) {
                auto response = learning_wait_response(file, body, cancelled);
                // The client closes a wait to send a write or to stop; not a fault.
                if (!response) throw ContainerFailure(ContainerReason::Cancelled);
                return std::move(*response);
            }
            if (!body.starts_with("NKL1") && !body.starts_with("NKF1")) throw std::runtime_error("unsupported learning operation");
            (void)commit_learning(file, decode_learning(body));
            return "OKAY";
        }
        if (operation == "INF1") {
            const auto settings = inference_config(runtime); if (!settings) throw std::runtime_error("inference disabled");
            const auto request = decode_request(std::string(body));
            const auto timeout = request.operation == InferenceOperation::Score ? settings->policy.score_timeout_ms : settings->timeout_ms;
            const auto deadline = GetTickCount64() + timeout;
            NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            std::jthread monitor([&](std::stop_token stopped) { while (!stopped.stop_requested()) {
                if (cancelled()) { SetEvent(cancel.get()); return; } Sleep(5);
            }});
            if (cancelled()) throw ContainerFailure(ContainerReason::Cancelled);
            InferenceLauncher launch;
            if (!settings->server.empty()) launch = [&](DWORD idle) {
                return launch_zenz_server(settings->server, settings->model, settings->token, settings->threads,
                    (std::max)(settings->timeout_ms, static_cast<DWORD>(settings->policy.score_timeout_ms)), idle);
            };
            try { return encode_response(inference_exchange_launching(settings->token, request, deadline, cancel.get(), launch, settings->idle_ms)); }
            catch (const InferenceFailure& failure) {
                // Only cancelled() sets the event: the client closed, or the broker stops or gives up.
                if (failure.status == InferenceStatus::Cancelled) throw ContainerFailure(ContainerReason::Cancelled);
                throw;
            }
        }
        if (operation == "LOG2") {
            if (!config.settings.zenz.enabled || !config.settings.zenz.log_input) return "OFF";
            const auto item = decode_log_record(body);
            const std::lock_guard lock(log_mutex);
            if (!logger || log_key != id) { logger = std::make_unique<InputLog>(config.settings.zenz.log_directory); log_key = id; }
            return std::to_string(static_cast<int>(logger->write(item)));
        }
        throw std::runtime_error("unsupported broker operation");
    }
};
}
int wmain(int argc, wchar_t** argv) {
    std::unique_ptr<BrokerDiagnostics> diagnostics;
    const auto record = [&](ContainerEvent event, ContainerReason reason = ContainerReason::None) {
        if (diagnostics) { ContainerDiagnostic item{event}; item.reason = reason; diagnostics->record(item); }
    };
    try {
        bool replace = false, stop_only = false; auto packages = supported_container_packages();
        bool explicit_config = false; std::wstring instance;
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg(argv[i]);
            if (arg == L"--replace") replace = true;
            else if (arg == L"--stop") stop_only = true;
            else if (arg == L"--config" && i + 1 < argc) {
                const std::filesystem::path file(argv[++i]); if (!file.is_absolute()) throw std::runtime_error("absolute configuration required");
                SetEnvironmentVariableW(L"NSKKZIME_CONFIG", file.c_str()); explicit_config = true;
                instance = config_broker_instance(file); // The same endpoint that IMEs using this file connect to.
            } else if (arg == L"--test-package" && i + 1 < argc && explicit_config) { packages = {argv[++i]}; (void)container_sid(packages.front()); }
            else throw std::runtime_error("usage: nskkzime_broker [--replace|--stop] [--config PATH [--test-package NAME]]");
        }
        try { diagnostics = std::make_unique<BrokerDiagnostics>(dictionary_settings_path().parent_path() / L"diagnostics"); }
        catch (...) { /* Diagnostics cannot prevent startup. */ }
        record(ContainerEvent::Start);
        if (in_app_container()) throw std::runtime_error("broker requires a desktop process");
        if (!explicit_config && redirected_data_context()) throw std::runtime_error("broker requires an unvirtualized desktop process");
        const auto stop = container_stop_event(instance);
        if (stop_only) { record(ContainerEvent::Stop); SetEvent(stop.get()); record(ContainerEvent::Exit); return 0; }
        const auto mutex = container_instance_mutex(instance);
        auto acquired = WaitForSingleObject(mutex.get(), 0);
        if (acquired == WAIT_TIMEOUT && !replace) { record(ContainerEvent::Existing); record(ContainerEvent::Exit); return 0; }
        if (acquired == WAIT_TIMEOUT) { record(ContainerEvent::Replace); SetEvent(stop.get()); acquired = WaitForSingleObject(mutex.get(), 35000); }
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) throw std::runtime_error("broker is busy");
        ResetEvent(stop.get());
        Broker broker; broker.diagnostics = diagnostics.get();
        const ContainerObserver observer = [&](const ContainerDiagnostic& item) { if (diagnostics) diagnostics->record(item); };
        try { serve_container(packages, [&](auto request, const auto& cancelled) { return broker.handle(request, cancelled); }, stop.get(), instance, observer); }
        catch (...) { ReleaseMutex(mutex.get()); throw; }
        ReleaseMutex(mutex.get()); record(ContainerEvent::Exit); return 0;
    } catch (const ContainerFailure& error) {
        if (diagnostics) { ContainerDiagnostic item{ContainerEvent::Failed}; item.reason = error.reason;
            item.error = error.error; diagnostics->record(item); }
        std::cerr << error.what() << '\n'; return 1;
    } catch (const std::exception& error) { record(ContainerEvent::Failed, ContainerReason::Internal); std::cerr << error.what() << '\n'; return 1; }
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 1;
    const auto result = wmain(count, args); LocalFree(args); return result;
}
