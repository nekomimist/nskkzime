#pragma once
#include "learning_ipc.h"
#include <functional>
#include <array>
#include <stdexcept>
#include <vector>

namespace nskkzime::windows {
struct ProcessIdentity {
    std::wstring user, container;
    DWORD session = 0;
};
ProcessIdentity process_identity(HANDLE process = GetCurrentProcess());
bool in_app_container(); // Throws on an unreadable token; never assumes desktop.
bool redirected_data_context();
bool uses_data_broker(); // AppContainer or a virtualized desktop's default configuration.
std::wstring container_sid(std::wstring_view package);
std::vector<std::wstring> supported_container_packages();
std::filesystem::path container_broker_path();
std::wstring container_pipe_name(const ProcessIdentity&);
// Off-thread only. Fixed operations, authenticated same-user/session peers,
// bounded frames, deadlines and cancellation. No caller-supplied filesystem paths.
inline constexpr std::size_t container_request_limit = 1024 * 1024;
inline constexpr std::size_t container_response_limit = 512 * 1024 * 1024;
std::string container_exchange(std::string_view request, ULONGLONG deadline, HANDLE cancel);
using ContainerHandler = std::function<std::string(std::string_view, const std::function<bool()>&)>;
enum class ContainerEvent { Start, Existing, Replace, Stop, Exit, Failed, ListenerCreated, ListenerFailed,
    ListenerExit, BootstrapReceived, BootstrapComplete, RequestFailed, EndpointState, Snapshot };
enum class ContainerPhase { Startup, Create, Accept, Authenticate, Read, Handle, Write, Acknowledge, Probe };
enum class ContainerOperation { None, Init, Init2, Learning, Inference, Log, Gui, Unknown };
enum class ContainerReason { None, Win32, Timeout, Cancelled, Validation, Internal };
struct ContainerFailure : std::runtime_error {
    ContainerReason reason; DWORD error;
    ContainerFailure(ContainerReason why, DWORD code = 0) : std::runtime_error("AppContainer broker unavailable"), reason(why), error(code) {}
};
struct ContainerDiagnostic {
    ContainerEvent event;
    ContainerPhase phase = ContainerPhase::Startup;
    std::wstring_view container; // Technical SID only; never a request field.
    DWORD error = 0, client = 0;
    ContainerReason reason = ContainerReason::None;
    int instance = -1;
    ContainerOperation operation = ContainerOperation::None;
    ULONGLONG elapsed_ms = 0;
    int sources = -1, failed_sources = -1, romaji_fallback = -1;
    int active = -1;
    std::array<unsigned, 9> phases{};
};
using ContainerObserver = std::function<void(const ContainerDiagnostic&)>;
// Observer failures never affect transport. Called only on broker/background threads.
void serve_container(const std::vector<std::wstring>& packages, const ContainerHandler&, HANDLE stop,
    std::wstring_view desktop_instance = {}, const ContainerObserver& = {});
// A broker serving an explicit configuration file (NSKKZIME_CONFIG / --config)
// uses its own desktop endpoint, so isolated runs never reach the user's broker.
std::wstring config_broker_instance(const std::filesystem::path& config);
NativeHandle container_stop_event(std::wstring_view instance = {});
NativeHandle container_instance_mutex(std::wstring_view instance = {});
} // namespace nskkzime::windows
