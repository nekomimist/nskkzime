#pragma once
#include "learning_ipc.h"
#include <functional>
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
void serve_container(const std::vector<std::wstring>& packages, const ContainerHandler&, HANDLE stop, std::wstring_view desktop_instance = {});
NativeHandle container_stop_event(std::wstring_view instance = {});
NativeHandle container_instance_mutex(std::wstring_view instance = {});
} // namespace nskkzime::windows
