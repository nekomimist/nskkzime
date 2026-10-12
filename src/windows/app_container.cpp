#include "binary_layout.h"
#include "app_container.h"
#include "dictionary_settings.h"
#include <appmodel.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <userenv.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <thread>
#include <stdexcept>
#include <atomic>
#include <memory>
#include <mutex>

namespace nskkzime::windows {
namespace {
int anchor;
using Failure = ContainerFailure;
void need(bool ok) { if (!ok) throw Failure(ContainerReason::Validation); }
void native_need(bool ok) { if (!ok) throw Failure(ContainerReason::Win32, GetLastError()); }
void check_deadline(ULONGLONG deadline, HANDLE cancel) {
    const auto waited = WaitForSingleObject(cancel, 0);
    if (waited == WAIT_FAILED) throw Failure(ContainerReason::Win32, GetLastError());
    if (waited == WAIT_OBJECT_0) throw Failure(ContainerReason::Cancelled);
    if (GetTickCount64() >= deadline) throw Failure(ContainerReason::Timeout);
}
// Reports pipe failures with the broker's diagnostic reasons.
template<class F> auto piped(F&& action) {
    try { return action(); }
    catch (const PipeFailure& error) {
        if (error.reason == PipeError::Timeout) throw Failure(ContainerReason::Timeout);
        if (error.reason == PipeError::Cancelled) throw Failure(ContainerReason::Cancelled);
        if (error.reason == PipeError::Win32) throw Failure(ContainerReason::Win32, error.error);
        throw Failure(ContainerReason::Validation);
    }
}
void emit(const ContainerObserver& observer, const ContainerDiagnostic& event) noexcept {
    try { if (observer) observer(event); } catch (...) {}
}
std::wstring sid_text(PSID sid) {
    LPWSTR raw = nullptr; native_need(ConvertSidToStringSidW(sid, &raw) != FALSE);
    try { std::wstring result(raw); LocalFree(raw); return result; }
    catch (...) { LocalFree(raw); throw; }
}
std::vector<unsigned char> token_info(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0; GetTokenInformation(token, kind, nullptr, 0, &size);
    need(size && size <= 65536); std::vector<unsigned char> bytes(size);
    native_need(GetTokenInformation(token, kind, bytes.data(), size, &size) != FALSE); return bytes;
}
void grant_query(HANDLE object, DWORD rights, const std::vector<std::wstring>& sids) {
    PACL previous = nullptr, next = nullptr; PSECURITY_DESCRIPTOR raw = nullptr;
    const auto read_error = GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &previous, nullptr, &raw);
    if (read_error != ERROR_SUCCESS) throw Failure(ContainerReason::Win32, read_error);
    try {
        std::vector<EXPLICIT_ACCESSW> entries(sids.size()); std::vector<PSID> parsed(sids.size());
        try {
            for (std::size_t i = 0; i < sids.size(); ++i) {
                need(ConvertStringSidToSidW(sids[i].c_str(), &parsed[i]) != FALSE);
                auto& e = entries[i]; e.grfAccessPermissions = rights; e.grfAccessMode = GRANT_ACCESS;
                e.Trustee.TrusteeForm = TRUSTEE_IS_SID; e.Trustee.ptstrName = static_cast<LPWSTR>(parsed[i]);
            }
            const auto acl_error = SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), previous, &next);
            if (acl_error != ERROR_SUCCESS) throw Failure(ContainerReason::Win32, acl_error);
        } catch (...) { for (auto p : parsed) LocalFree(p); throw; }
        for (auto p : parsed) LocalFree(p);
        const auto write_error = SetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, next, nullptr);
        if (write_error != ERROR_SUCCESS) throw Failure(ContainerReason::Win32, write_error);
    } catch (...) { LocalFree(next); LocalFree(raw); throw; }
    LocalFree(next); LocalFree(raw);
}
DWORD check_peer(HANDLE pipe, bool server, const ProcessIdentity& expected) {
    ULONG pid = 0; native_need((server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)) != FALSE);
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); native_need(process.valid());
    const auto identity = process_identity(process.get());
    need(identity.user == expected.user && identity.session == expected.session);
    if (server) need(identity.container == expected.container);
    else {
        need(identity.container.empty());
        std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
        native_need(QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) != FALSE); path.resize(size);
        const auto wanted = container_broker_path().native();
        need(CompareStringOrdinal(path.c_str(), -1, wanted.c_str(), -1, TRUE) == CSTR_EQUAL);
    }
    return pid;
}
NativeHandle control_object(bool mutex, std::wstring_view instance) {
    need(instance.size() <= 192 && instance.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == instance.npos);
    const auto identity = process_identity(); need(identity.container.empty());
    SecurityDescriptor descriptor(owner_only_sddl(identity.user));
    const auto name = L"Local\\nskkzime-container-v1-" + identity.user + std::wstring(instance) + (mutex ? L"-instance" : L"-stop");
    NativeHandle result(mutex ? CreateMutexW(descriptor.attributes(), FALSE, name.c_str()) : CreateEventW(descriptor.attributes(), TRUE, FALSE, name.c_str()));
    native_need(result.valid()); return result;
}
}
ProcessIdentity process_identity(HANDLE process) {
    HANDLE raw = nullptr; native_need(OpenProcessToken(process, TOKEN_QUERY, &raw) != FALSE); NativeHandle token(raw);
    ProcessIdentity result;
    const auto user = token_info(token.get(), TokenUser); result.user = sid_text(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid);
    const auto session = token_info(token.get(), TokenSessionId); result.session = *reinterpret_cast<const DWORD*>(session.data());
    const auto container = token_info(token.get(), TokenIsAppContainer);
    if (*reinterpret_cast<const DWORD*>(container.data())) {
        const auto info = token_info(token.get(), TokenAppContainerSid);
        result.container = sid_text(reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(info.data())->TokenAppContainer);
    }
    return result;
}
bool in_app_container() { static const bool value = !process_identity().container.empty(); return value; }
bool redirected_data_context() {
    UINT32 size = 0;
    if (GetCurrentPackageFullName(&size, nullptr) != APPMODEL_ERROR_NO_PACKAGE) return true;
    // Descendants can inherit file virtualization without package identity.
    // Compare the opened file, not just the logical KnownFolder path.
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY | KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &raw)))
        throw std::runtime_error("cannot locate shared settings");
    std::filesystem::path file;
    try { file = std::filesystem::path(raw) / L"nskkzime" / L"settings.ini"; }
    catch (...) { CoTaskMemFree(raw); throw; }
    CoTaskMemFree(raw);
    NativeHandle handle(CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle.valid()) return false;
    wchar_t final[32768]{};
    const auto count = GetFinalPathNameByHandleW(handle.get(), final, ARRAYSIZE(final), FILE_NAME_NORMALIZED);
    if (!count || count >= ARRAYSIZE(final)) throw std::runtime_error("cannot identify shared settings");
    std::wstring_view opened(final, count);
    if (opened.starts_with(L"\\\\?\\")) opened.remove_prefix(4);
    return CompareStringOrdinal(opened.data(), static_cast<int>(opened.size()), file.c_str(), -1, TRUE) != CSTR_EQUAL;
}
bool uses_data_broker() {
    if (in_app_container()) return true;
    // An explicit configuration file remains private to its host.
    if (GetEnvironmentVariableW(L"NSKKZIME_CONFIG", nullptr, 0)) return false;
    static const bool redirected = redirected_data_context();
    return redirected;
}
std::wstring container_sid(std::wstring_view package) {
    need(!package.empty() && package.size() <= 256 && package.find_first_of(L"\\/\r\n\t\" ") == package.npos);
    PSID sid = nullptr; need(SUCCEEDED(DeriveAppContainerSidFromAppContainerName(std::wstring(package).c_str(), &sid)));
    try { auto result = sid_text(sid); FreeSid(sid); return result; } catch (...) { FreeSid(sid); throw; }
}
std::vector<std::wstring> supported_container_packages() {
    return {L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy", L"Microsoft.WindowsStore_8wekyb3d8bbwe"};
}
std::filesystem::path container_broker_path() {
    HMODULE module = nullptr; need(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchor), &module) != FALSE);
    wchar_t path[32768]{}; const auto size = GetModuleFileNameW(module, path, ARRAYSIZE(path)); need(size && size < ARRAYSIZE(path));
    return shared_binary_directory(std::filesystem::path(path)) / L"nskkzime_broker.exe";
}
std::wstring container_pipe_name(const ProcessIdentity& identity) {
    if (identity.container.empty()) return L"\\\\.\\pipe\\nskkzime-desktop-broker-v1-" + identity.user + L"-" + std::to_wstring(identity.session);
    return L"\\\\.\\pipe\\Sessions\\" + std::to_wstring(identity.session) + L"\\AppContainerNamedObjects\\" + identity.container +
        L"\\nskkzime-broker-v1-" + identity.user;
}
std::string container_exchange(std::string_view request, ULONGLONG deadline, HANDLE cancel) {
    const auto identity = process_identity();
    auto name = identity.container.empty() ? container_pipe_name(identity) : L"\\\\.\\pipe\\LOCAL\\nskkzime-broker-v1-" + identity.user;
    if (identity.container.empty() && environment_value(L"NSKKZIME_CONFIG")) name += config_broker_instance(dictionary_settings_path());
    return piped([&] {
        // The broker may be starting or recreating its endpoint.
        const auto pipe = open_pipe(name, deadline, cancel, true); check_peer(pipe.get(), false, identity);
        send_frame(pipe.get(), request, container_request_limit, deadline, cancel);
        auto response = receive_frame(pipe.get(), container_response_limit, deadline, cancel);
        send_receipt(pipe.get(), deadline, cancel); return response;
    });
}
void serve_container(const std::vector<std::wstring>& packages, const ContainerHandler& handler, HANDLE stop,
    std::wstring_view desktop_instance, const ContainerObserver& observer) {
    auto own = process_identity(); need(own.container.empty() && !packages.empty() && packages.size() <= 16);
    std::vector<std::wstring> sids;
    for (const auto& package : packages) {
        const auto sid = container_sid(package); if (std::find(sids.begin(), sids.end(), sid) == sids.end()) sids.push_back(sid);
    }
    // Clients can verify our token and exact executable path, but cannot inspect
    // memory, duplicate handles, write the process, or change its token.
    grant_query(GetCurrentProcess(), PROCESS_QUERY_LIMITED_INFORMATION, sids);
    HANDLE raw = nullptr; native_need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | READ_CONTROL | WRITE_DAC, &raw) != FALSE);
    NativeHandle token(raw); grant_query(token.get(), TOKEN_QUERY, sids);
    sids.push_back(L""); // Normal desktop peers: same user/session, no AppContainer.
    struct State { std::atomic<bool> active{false}; std::atomic<ContainerPhase> phase{ContainerPhase::Create}; };
    struct Listener {
        NativeHandle pipe; ProcessIdentity peer; int instance = 0; std::shared_ptr<State> state = std::make_shared<State>();
        std::mutex mutex; NativeHandle replacement; // Set by the probe, adopted by the listener between requests.
    };
    struct Endpoint {
        std::wstring name, sid, sddl; std::vector<Listener*> listeners;
        DWORD last_error = MAXDWORD, repair_delay = 5000; ULONGLONG next_report = 0, next_repair = 0;
    };
    const auto create = [&](const Endpoint& endpoint) {
        auto descriptor = piped([&] { return std::make_unique<SecurityDescriptor>(endpoint.sddl); });
        std::vector<NativeHandle> pipes;
        for (int i = 0; i < 4; ++i) {
            auto pipe = create_pipe_instance(endpoint.name, i == 0, 4, 65536, descriptor->attributes());
            const auto error = pipe.valid() ? 0 : GetLastError();
            ContainerDiagnostic event{pipe.valid() ? ContainerEvent::ListenerCreated : ContainerEvent::ListenerFailed};
            event.phase = ContainerPhase::Create; event.container = endpoint.sid; event.instance = i; event.error = error;
            if (error) event.reason = ContainerReason::Win32;
            emit(observer, event);
            if (!pipe.valid()) throw Failure(ContainerReason::Win32, error);
            pipes.push_back(std::move(pipe));
        }
        return pipes;
    };
    std::vector<std::unique_ptr<Listener>> listeners;
    std::vector<Endpoint> endpoints;
    for (const auto& sid : sids) {
        auto peer = own; peer.container = sid;
        Endpoint endpoint{container_pipe_name(peer), sid, owner_only_sddl(own.user) +
            (sid.empty() ? std::wstring{} : L"(A;;0x12019b;;;" + sid + L")S:(ML;;NW;;;LW)")};
        if (sid.empty()) endpoint.name += desktop_instance;
        auto pipes = create(endpoint);
        for (int i = 0; i < 4; ++i) {
            auto listener = std::make_unique<Listener>();
            listener->pipe = std::move(pipes[i]); listener->peer = peer; listener->instance = i;
            endpoint.listeners.push_back(listener.get()); listeners.push_back(std::move(listener));
        }
        endpoints.push_back(std::move(endpoint));
    }
    auto run = [&](Listener& listener) {
        listener.state->active = true;
        struct OnExit { Listener& listener; const ContainerObserver& observer; ~OnExit() {
            listener.state->active = false;
            ContainerDiagnostic event{ContainerEvent::ListenerExit}; event.container = listener.peer.container;
            event.instance = listener.instance; event.phase = listener.state->phase; emit(observer, event);
        } } on_exit{listener, observer};
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            {
                const std::lock_guard lock(listener.mutex);
                if (listener.replacement.valid()) listener.pipe = std::move(listener.replacement);
            }
            ContainerDiagnostic diagnostic{ContainerEvent::RequestFailed}; diagnostic.container = listener.peer.container;
            diagnostic.instance = listener.instance; diagnostic.phase = ContainerPhase::Accept;
            listener.state->phase = diagnostic.phase;
            auto started = GetTickCount64();
            const auto phase = [&](ContainerPhase value) { diagnostic.phase = value; listener.state->phase = value; };
            try {
                piped([&] { accept_pipe(listener.pipe.get(), GetTickCount64() + 60000, stop); });
                started = GetTickCount64();
                phase(ContainerPhase::Authenticate); diagnostic.client = check_peer(listener.pipe.get(), true, listener.peer);
                phase(ContainerPhase::Read);
                const auto request = piped([&] { return receive_frame(listener.pipe.get(), container_request_limit, GetTickCount64() + 2000, stop); });
                diagnostic.operation = request == "INI3" ? ContainerOperation::Init :
                    request.starts_with("CHK1") ? ContainerOperation::Check :
                    request.starts_with("LRN1") && request.size() >= 72 && request.substr(68, 4) == "WAT1" ? ContainerOperation::LearningWait :
                    request.starts_with("LRN1") ? ContainerOperation::Learning : request.starts_with("INF1") ? ContainerOperation::Inference :
                    request.starts_with("LOG2") ? ContainerOperation::Log : request.starts_with("GUI1") ? ContainerOperation::Gui : ContainerOperation::Unknown;
                const bool bootstrap = diagnostic.operation == ContainerOperation::Init;
                phase(ContainerPhase::Handle);
                if (bootstrap) { auto received = diagnostic; received.event = ContainerEvent::BootstrapReceived; emit(observer, received); }
                need(listener.peer.container.empty() || !request.starts_with("GUI1"));
                const auto deadline = GetTickCount64() + 30000;
                const auto cancelled = [&] { return GetTickCount64() >= deadline || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
                    !pipe_peer_open(listener.pipe.get()); };
                const auto response = handler(request, cancelled);
                check_deadline(deadline, stop); native_need(pipe_peer_open(listener.pipe.get()));
                phase(ContainerPhase::Write);
                piped([&] { send_frame(listener.pipe.get(), response, container_response_limit, deadline, stop); });
                phase(ContainerPhase::Acknowledge);
                piped([&] { receive_receipt(listener.pipe.get(), GetTickCount64() + 1000, stop); });
                if (bootstrap) { diagnostic.event = ContainerEvent::BootstrapComplete; diagnostic.elapsed_ms = GetTickCount64() - started; emit(observer, diagnostic); }
            } catch (const Failure& failure) {
                // Ordinary idle accept deadlines, accepts cancelled for a repair,
                // closed learning waits and shutdown are not faults.
                if (!(diagnostic.phase == ContainerPhase::Accept && failure.reason == ContainerReason::Timeout) &&
                    !(diagnostic.phase == ContainerPhase::Accept && failure.reason == ContainerReason::Win32 &&
                        failure.error == ERROR_OPERATION_ABORTED) &&
                    !(diagnostic.operation == ContainerOperation::LearningWait && failure.reason == ContainerReason::Cancelled) &&
                    WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
                    diagnostic.reason = failure.reason; diagnostic.error = failure.error;
                    diagnostic.elapsed_ms = GetTickCount64() - started; emit(observer, diagnostic);
                }
            } catch (...) {
                if (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
                    diagnostic.reason = ContainerReason::Internal; diagnostic.elapsed_ms = GetTickCount64() - started; emit(observer, diagnostic);
                }
            }
            DisconnectNamedPipe(listener.pipe.get());
        }
    };
    std::vector<std::jthread> threads;
    try {
        for (auto& listener : listeners) threads.emplace_back(run, std::ref(*listener));
        threads.emplace_back([&] {
            while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
                for (auto& endpoint : endpoints) {
                    const auto available = WaitNamedPipeW(endpoint.name.c_str(), 1);
                    const auto error = available ? 0 : GetLastError(); const auto now = GetTickCount64();
                    if (!error) endpoint.repair_delay = 5000;
                    // A container's pipe name has vanished right after sign-in while
                    // every instance stayed open and accepting. Clients then find
                    // no pipe, so publish fresh instances under the name.
                    if (error == ERROR_FILE_NOT_FOUND && now >= endpoint.next_repair) {
                        endpoint.next_repair = now + endpoint.repair_delay;
                        endpoint.repair_delay = (std::min)(endpoint.repair_delay * 2, DWORD{300000});
                        try {
                            auto pipes = create(endpoint);
                            for (std::size_t i = 0; i < pipes.size(); ++i) {
                                auto& listener = *endpoint.listeners[i];
                                const std::lock_guard lock(listener.mutex);
                                listener.replacement = std::move(pipes[i]);
                                // Only an idle accept is interrupted; a request in progress finishes first.
                                if (listener.state->phase.load() == ContainerPhase::Accept) CancelIoEx(listener.pipe.get(), nullptr);
                            }
                            ContainerDiagnostic event{ContainerEvent::EndpointRecreated}; event.phase = ContainerPhase::Probe;
                            event.container = endpoint.sid; emit(observer, event);
                        } catch (...) { /* create() reported the failure; retry after the delay. */ }
                    }
                    if (!observer || (error == endpoint.last_error && now < endpoint.next_report)) continue;
                    endpoint.last_error = error; endpoint.next_report = now + 60000;
                    ContainerDiagnostic event{ContainerEvent::EndpointState}; event.phase = ContainerPhase::Probe;
                    event.container = endpoint.sid; event.error = error; event.active = 0;
                    if (error) event.reason = ContainerReason::Win32;
                    for (const auto* listener : endpoint.listeners) if (listener->state->active.load()) {
                        ++event.active; ++event.phases[static_cast<std::size_t>(listener->state->phase.load())];
                    }
                    emit(observer, event);
                }
                if (WaitForSingleObject(stop, 5000) == WAIT_OBJECT_0) return;
            }
        });
    }
    catch (...) { SetEvent(stop); throw; }
    // Listeners live until all jthreads have joined.
}
std::wstring config_broker_instance(const std::filesystem::path& config) {
    auto key = config.lexically_normal().native();
    CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    unsigned char digest[32]{};
    need(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size() * sizeof(wchar_t)), digest, sizeof(digest)) >= 0);
    std::wstring result = L"-config-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (std::size_t i = 0; i < 16; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}
NativeHandle container_stop_event(std::wstring_view instance) { return control_object(false, instance); }
NativeHandle container_instance_mutex(std::wstring_view instance) { return control_object(true, instance); }
} // namespace nskkzime::windows
