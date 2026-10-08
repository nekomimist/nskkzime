#include "app_container.h"
#include "dictionary_settings.h"
#include "inference_client.h"
#include "learning_client.h"
#include "tool_launcher.h"
#include "nskkzime/compiled_dictionary.h"
#include <nlohmann/json.hpp>
#include <bcrypt.h>
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
std::string generation(const DictionaryConfiguration& config) {
    auto bytes = serialize_dictionary_settings(config.settings) + utf8_message(config.file.native());
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, 32) < 0)
        throw std::runtime_error("cannot identify settings");
    std::string result; constexpr char hex[] = "0123456789abcdef";
    for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; } return result;
}
std::string read_romaji(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size < 0 || size > 1024 * 1024) throw std::runtime_error("invalid romaji file");
    std::string bytes(static_cast<std::size_t>(size), '\0'); file.seekg(0);
    if (!file.read(bytes.data(), size)) throw std::runtime_error("cannot read romaji file");
    RomajiRules::parse(bytes); return bytes;
}
struct Broker {
    std::mutex snapshot_mutex, log_mutex;
    std::string snapshot_key, snapshot_bytes, log_key;
    std::unique_ptr<InputLog> logger;
    std::string snapshot(const DictionaryConfiguration& config, const std::string& id) {
        // Only configured paths are opened; no file path comes from a client.
        auto key = id;
        for (const auto& path : config.settings.dictionaries) {
            key += std::to_string(std::filesystem::file_size(path));
            key += std::to_string(std::filesystem::last_write_time(path).time_since_epoch().count());
        }
        if (!config.settings.romaji_rules.empty()) key += std::to_string(std::filesystem::last_write_time(config.settings.romaji_rules).time_since_epoch().count());
        const std::lock_guard lock(snapshot_mutex);
        if (key == snapshot_key && !snapshot_bytes.empty()) return snapshot_bytes;
        nlohmann::json response{{"version", 1}, {"settings", serialize_dictionary_settings(config.settings)},
            {"file", utf8_message(config.file.native())}, {"demo", config.demo}, {"generation", id},
            {"romaji", read_romaji(config.settings.romaji_rules)}, {"images", nlohmann::json::array()}};
        std::size_t total = 0;
        for (const auto& path : config.settings.dictionaries) {
            const auto image = CompiledDictionary::compile(*SkkDictionary::load(path));
            total += image.size(); if (total > container_response_limit - 1024 * 1024) throw std::runtime_error("snapshot too large");
            response["images"].push_back(nlohmann::json::binary(std::vector<std::uint8_t>(image.begin(), image.end())));
        }
        const auto encoded = nlohmann::json::to_cbor(response);
        if (encoded.size() > container_response_limit) throw std::runtime_error("snapshot too large");
        snapshot_bytes.assign(encoded.begin(), encoded.end()); snapshot_key = key; return snapshot_bytes;
    }
    std::string handle(std::string_view bytes, const std::function<bool()>& cancelled) {
        const auto config = current_dictionary_configuration(); const auto id = generation(config);
        if (bytes == "INIT") return snapshot(config, id);
        if (bytes.size() < 68 || bytes.substr(4, 64) != id) throw std::runtime_error("stale settings");
        const auto operation = bytes.substr(0, 4), body = bytes.substr(68);
        if (operation == "GUI1") {
            // The transport rejects this operation from AppContainer peers.
            // No caller-supplied path or command line crosses the boundary.
            ToolCommand command;
            if (body == "settings") {
                command.tool = ImeTool::Settings; command.file = config.file;
                command.executable = container_broker_path().parent_path() / L"nskkzime_settings.exe";
            } else if (body == "dictionary") {
                command.tool = ImeTool::Dictionary; command.file = learning_file_path();
                command.executable = container_broker_path().parent_path() / L"nskkzime_dictionary_gui.exe";
            } else throw std::runtime_error("unknown GUI");
            if (launch_tool_process(command) != ERROR_SUCCESS) throw std::runtime_error("GUI unavailable");
            return "OKAY";
        }
        if (operation == "LRN1") {
            if (!config.settings.learning || config.demo || config.settings.dictionaries.empty()) throw std::runtime_error("learning disabled");
            if (body.starts_with("NKL1") || body.starts_with("NKF1")) (void)decode_learning(body);
            else if (body.size() != 12 || !body.starts_with("GET1")) throw std::runtime_error("unsupported learning operation");
            const auto file = learning_file_path();
            try { return learning_exchange(file, body); }
            catch (...) {
                const auto child = launch_dictionary_server(learning_helper_path(), file);
                for (int i = 0; i < 100 && !cancelled(); ++i) {
                    try { return learning_exchange(file, body); } catch (...) { Sleep(10); }
                }
                throw std::runtime_error("learning helper unavailable");
            }
        }
        if (operation == "INF1") {
            const auto settings = current_inference_config(); if (!settings) throw std::runtime_error("inference disabled");
            const auto request = decode_request(std::string(body));
            const auto timeout = request.operation == InferenceOperation::Score ? settings->policy.score_timeout_ms : settings->timeout_ms;
            const auto deadline = GetTickCount64() + timeout;
            NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            std::jthread monitor([&](std::stop_token stopped) { while (!stopped.stop_requested()) {
                if (cancelled()) { SetEvent(cancel.get()); return; } Sleep(5);
            }});
            bool launched = false;
            for (;;) {
                if (cancelled() || GetTickCount64() >= deadline) throw std::runtime_error("cancelled inference");
                try { return encode_response(inference_exchange(settings->token, request, deadline, cancel.get())); }
                catch (const InferenceFailure& failure) {
                    if (failure.status != InferenceStatus::Unavailable || launched) throw;
                    const auto child = settings->model.empty() ? launch_dummy_server(settings->server, settings->token, settings->delay_ms) :
                        launch_zenz_server(settings->server, settings->model, settings->token, settings->threads,
                            (std::max)(settings->timeout_ms, static_cast<DWORD>(settings->policy.score_timeout_ms)));
                    launched = true;
                    // The child initializes its pipe before loading the model.
                    for (int i = 0; i < 100 && !cancelled() && GetTickCount64() < deadline; ++i) {
                        try { return encode_response(inference_exchange(settings->token, request, deadline, cancel.get())); }
                        catch (const InferenceFailure& retry) { if (retry.status != InferenceStatus::Unavailable) throw; Sleep(10); }
                    }
                    throw std::runtime_error("inference helper unavailable");
                }
            }
        }
        if (operation == "LOG1") {
            if (!config.settings.zenz.enabled || !config.settings.zenz.log_input) return "OFF";
            const auto record = nlohmann::json::from_cbor(body);
            if (!record.is_object() || record.size() != 3) throw std::runtime_error("invalid log record");
            InputLogRecord item; item.request = decode_request(record.at("request").get<std::string>());
            if (!record.at("response").is_null()) item.response = decode_response(record.at("response").get<std::string>(), item.request);
            const auto status = record.at("status").get<int>();
            if (status < 0 || status > static_cast<int>(InferenceStatus::InvalidResponse)) throw std::runtime_error("invalid log status");
            item.status = static_cast<InferenceStatus>(status);
            const std::lock_guard lock(log_mutex);
            if (!logger || log_key != id) { logger = std::make_unique<InputLog>(config.settings.zenz.log_directory); log_key = id; }
            return std::to_string(static_cast<int>(logger->write(item)));
        }
        throw std::runtime_error("unsupported broker operation");
    }
};
}
int wmain(int argc, wchar_t** argv) {
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
            } else if (arg == L"--test-package" && i + 1 < argc && explicit_config) { packages = {argv[++i]}; instance = container_sid(packages.front()); }
            else throw std::runtime_error("usage: nskkzime_broker [--replace|--stop] [--config PATH [--test-package NAME]]");
        }
        if (in_app_container()) throw std::runtime_error("broker requires a desktop process");
        if (!explicit_config && redirected_data_context()) throw std::runtime_error("broker requires an unvirtualized desktop process");
        const auto stop = container_stop_event(instance);
        if (stop_only) { SetEvent(stop.get()); return 0; }
        const auto mutex = container_instance_mutex(instance);
        auto acquired = WaitForSingleObject(mutex.get(), 0);
        if (acquired == WAIT_TIMEOUT && !replace) return 0;
        if (acquired == WAIT_TIMEOUT) { SetEvent(stop.get()); acquired = WaitForSingleObject(mutex.get(), 35000); }
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) throw std::runtime_error("broker is busy");
        ResetEvent(stop.get());
        Broker broker;
        try { serve_container(packages, [&](auto request, const auto& cancelled) { return broker.handle(request, cancelled); }, stop.get(), instance); }
        catch (...) { ReleaseMutex(mutex.get()); throw; }
        ReleaseMutex(mutex.get()); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 1;
    const auto result = wmain(count, args); LocalFree(args); return result;
}
