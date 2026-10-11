#include "broker_diagnostics.h"
#include "dictionary_settings.h"
#include <nlohmann/json.hpp>
#include <sddl.h>
#include <cstdio>
#include <climits>

namespace nskkzime::windows {
namespace {
void need(bool ok) { if (!ok) throw std::runtime_error("diagnostics unavailable"); }
const char* name(ContainerEvent value) {
    switch (value) {
    case ContainerEvent::Start: return "broker_start";
    case ContainerEvent::Existing: return "broker_existing";
    case ContainerEvent::Replace: return "broker_replace";
    case ContainerEvent::Stop: return "broker_stop_requested";
    case ContainerEvent::Exit: return "broker_exit";
    case ContainerEvent::Failed: return "broker_failed";
    case ContainerEvent::ListenerCreated: return "listener_created";
    case ContainerEvent::ListenerFailed: return "listener_failed";
    case ContainerEvent::ListenerExit: return "listener_exit";
    case ContainerEvent::BootstrapReceived: return "bootstrap_received";
    case ContainerEvent::BootstrapComplete: return "bootstrap_response_acked";
    case ContainerEvent::RequestFailed: return "request_failed";
    case ContainerEvent::EndpointState: return "endpoint_state";
    case ContainerEvent::Snapshot: return "snapshot_ready";
    }
    return "unknown";
}
const char* name(ContainerPhase value) {
    switch (value) {
    case ContainerPhase::Startup: return "startup";
    case ContainerPhase::Create: return "create";
    case ContainerPhase::Accept: return "accept";
    case ContainerPhase::Authenticate: return "authenticate";
    case ContainerPhase::Read: return "read";
    case ContainerPhase::Handle: return "handle";
    case ContainerPhase::Write: return "write";
    case ContainerPhase::Acknowledge: return "acknowledge";
    case ContainerPhase::Probe: return "probe";
    }
    return "unknown";
}
const char* name(ContainerOperation value) {
    switch (value) {
    case ContainerOperation::None: return "none";
    case ContainerOperation::Init: return "INIT";
    case ContainerOperation::Init2: return "INI2";
    case ContainerOperation::Learning: return "LRN1";
    case ContainerOperation::Inference: return "INF1";
    case ContainerOperation::Log: return "LOG1";
    case ContainerOperation::Gui: return "GUI1";
    case ContainerOperation::Unknown: return "unknown";
    case ContainerOperation::LearningWait: return "LRN1/WAT1";
    }
    return "unknown";
}
const char* name(ContainerReason value) {
    switch (value) {
    case ContainerReason::None: return "none";
    case ContainerReason::Win32: return "win32";
    case ContainerReason::Timeout: return "timeout";
    case ContainerReason::Cancelled: return "cancelled";
    case ContainerReason::Validation: return "validation";
    case ContainerReason::Internal: return "internal";
    }
    return "unknown";
}
struct Descriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    Descriptor() {
        const auto user = process_identity().user;
        need(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            (L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + user + L")").c_str(), SDDL_REVISION_1, &value, nullptr) != FALSE);
    }
    ~Descriptor() { LocalFree(value); }
};
void check_file(HANDLE file) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    need(GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)) &&
        !(info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)));
}
}
void BrokerDiagnostics::record(const ContainerDiagnostic& event) noexcept {
    // Preserve a pending native API failure, including for callers outside the transport.
    struct PreserveError { DWORD error = GetLastError(); ~PreserveError() { SetLastError(error); } } preserve_error;
    try {
        const std::lock_guard guard(mutex_);
        need(event.container.empty() || (event.container.size() <= 192 && event.container.starts_with(L"S-") &&
            event.container.find_first_not_of(L"S-0123456789") == event.container.npos));
        unsigned suppressed = 0;
        if (event.event == ContainerEvent::ListenerFailed || event.event == ContainerEvent::RequestFailed) {
            const auto key = std::string(name(event.event)) + name(event.phase) + name(event.operation) +
                name(event.reason) + std::to_string(event.error) + utf8_message(event.container);
            if (rates_.size() >= 128 && !rates_.contains(key)) return;
            auto& rate = rates_[key]; const auto now = GetTickCount64();
            if (now < rate.next) { if (rate.suppressed != UINT_MAX) ++rate.suppressed; return; }
            suppressed = rate.suppressed; rate.suppressed = 0; rate.next = now + 60000;
        }
        SYSTEMTIME utc{}; GetSystemTime(&utc); char timestamp[32]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
            utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
        nlohmann::json record{{"format", "nskkzime-broker-diagnostics"}, {"version", 1}, {"utc", timestamp},
            {"pid", GetCurrentProcessId()}, {"tid", GetCurrentThreadId()}, {"session", session}, {"uptime_ms", GetTickCount64()},
            {"event", name(event.event)}, {"phase", name(event.phase)}, {"container", utf8_message(event.container)},
            {"reason", name(event.reason)}, {"client_pid", event.client}, {"instance", event.instance},
            {"operation", name(event.operation)}, {"elapsed_ms", event.elapsed_ms}, {"suppressed", suppressed}};
        if (event.reason == ContainerReason::Win32) record["win32_code"] = event.error;
        if (event.event == ContainerEvent::Start) {
            wchar_t binary[32768]{}; const auto size = GetModuleFileNameW(nullptr, binary, ARRAYSIZE(binary));
            if (size && size < ARRAYSIZE(binary)) record["binary"] = utf8_message(std::wstring_view(binary, size));
        }
        if (event.event == ContainerEvent::EndpointState)
            record["state"] = event.error == 0 ? "available" : event.error == ERROR_FILE_NOT_FOUND ? "missing" :
                event.error == ERROR_SEM_TIMEOUT || event.error == ERROR_PIPE_BUSY ? "busy" : "error";
        if (event.active >= 0) {
            record["listeners_active"] = event.active; record["listeners_expected"] = 4;
            for (std::size_t i = 0; i < event.phases.size(); ++i)
                record["phase_counts"][name(static_cast<ContainerPhase>(i))] = event.phases[i];
            std::uint64_t pending = 0;
            for (const auto& rate : rates_) pending += rate.second.suppressed;
            record["failures_suppressed_pending"] = pending;
        }
        if (event.sources >= 0) {
            record["sources"] = event.sources; record["failed_sources"] = event.failed_sources;
            record["romaji_fallback"] = event.romaji_fallback != 0;
        }
        const auto line = record.dump() + "\n"; need(line.size() <= 4096);
        need(directory_.is_absolute() && directory_.root_name().native().size() == 2);
        // Validate existing ancestors before creating anything underneath them.
        for (auto part = directory_; !part.empty();) {
            const auto flags = GetFileAttributesW(part.c_str());
            if (flags == INVALID_FILE_ATTRIBUTES) {
                const auto error = GetLastError(); need(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
            } else need((flags & FILE_ATTRIBUTE_DIRECTORY) && !(flags & FILE_ATTRIBUTE_REPARSE_POINT));
            const auto parent = part.parent_path(); if (parent == part) break; part = parent;
        }
        std::filesystem::create_directories(directory_.parent_path());
        Descriptor descriptor; SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
        if (!CreateDirectoryW(directory_.c_str(), &attributes)) need(GetLastError() == ERROR_ALREADY_EXISTS);
        for (auto part = directory_; !part.empty();) {
            const auto flags = GetFileAttributesW(part.c_str());
            need(flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) && !(flags & FILE_ATTRIBUTE_REPARSE_POINT));
            const auto parent = part.parent_path(); if (parent == part) break; part = parent;
        }
        // Nonblocking across processes/sessions. Losing a log write cannot block input or startup.
        NativeHandle lock(CreateFileW((directory_ / L"broker.lock").c_str(), GENERIC_READ, 0, &attributes,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        need(lock.valid()); check_file(lock.get());
        const auto current = directory_ / L"broker.jsonl", previous = directory_ / L"broker.previous.jsonl";
        {
            NativeHandle file(CreateFileW(current.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, &attributes,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            need(file.valid()); check_file(file.get()); LARGE_INTEGER size{}; need(GetFileSizeEx(file.get(), &size));
            if (size.QuadPart + static_cast<LONGLONG>(line.size()) <= static_cast<LONGLONG>(file_limit)) {
                DWORD written = 0; need(WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size());
                return;
            }
        }
        if (GetFileAttributesW(previous.c_str()) != INVALID_FILE_ATTRIBUTES) {
            NativeHandle file(CreateFileW(previous.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            need(file.valid()); check_file(file.get());
        }
        need(MoveFileExW(current.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        NativeHandle file(CreateFileW(current.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        need(file.valid()); DWORD written = 0;
        need(WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size());
    } catch (...) { /* Diagnostics are best effort and never contain exception text. */ }
}
} // namespace nskkzime::windows
