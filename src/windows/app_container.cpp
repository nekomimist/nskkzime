#include "app_container.h"
#include <sddl.h>
#include <aclapi.h>
#include <userenv.h>
#include <algorithm>
#include <array>
#include <thread>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
int anchor;
void need(bool ok) { if (!ok) throw std::runtime_error("AppContainer broker unavailable"); }
std::wstring sid_text(PSID sid) {
    LPWSTR raw = nullptr; need(ConvertSidToStringSidW(sid, &raw) != FALSE);
    try { std::wstring result(raw); LocalFree(raw); return result; }
    catch (...) { LocalFree(raw); throw; }
}
std::vector<unsigned char> token_info(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0; GetTokenInformation(token, kind, nullptr, 0, &size);
    need(size && size <= 65536); std::vector<unsigned char> bytes(size);
    need(GetTokenInformation(token, kind, bytes.data(), size, &size) != FALSE); return bytes;
}
struct Descriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    explicit Descriptor(const std::wstring& sddl) {
        need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &value, nullptr) != FALSE);
    }
    ~Descriptor() { LocalFree(value); }
};
void grant_query(HANDLE object, DWORD rights, const std::vector<std::wstring>& sids) {
    PACL previous = nullptr, next = nullptr; PSECURITY_DESCRIPTOR raw = nullptr;
    need(GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &previous, nullptr, &raw) == ERROR_SUCCESS);
    try {
        std::vector<EXPLICIT_ACCESSW> entries(sids.size()); std::vector<PSID> parsed(sids.size());
        try {
            for (std::size_t i = 0; i < sids.size(); ++i) {
                need(ConvertStringSidToSidW(sids[i].c_str(), &parsed[i]) != FALSE);
                auto& e = entries[i]; e.grfAccessPermissions = rights; e.grfAccessMode = GRANT_ACCESS;
                e.Trustee.TrusteeForm = TRUSTEE_IS_SID; e.Trustee.ptstrName = static_cast<LPWSTR>(parsed[i]);
            }
            need(SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), previous, &next) == ERROR_SUCCESS);
        } catch (...) { for (auto p : parsed) LocalFree(p); throw; }
        for (auto p : parsed) LocalFree(p);
        need(SetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, next, nullptr) == ERROR_SUCCESS);
    } catch (...) { LocalFree(next); LocalFree(raw); throw; }
    LocalFree(next); LocalFree(raw);
}
void check_peer(HANDLE pipe, bool server, const ProcessIdentity& expected) {
    ULONG pid = 0; need((server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)) != FALSE);
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); need(process.valid());
    const auto identity = process_identity(process.get());
    need(identity.user == expected.user && identity.session == expected.session);
    if (server) need(identity.container == expected.container);
    else {
        need(identity.container.empty());
        std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
        need(QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) != FALSE); path.resize(size);
        const auto wanted = container_broker_path().native();
        need(CompareStringOrdinal(path.c_str(), -1, wanted.c_str(), -1, TRUE) == CSTR_EQUAL);
    }
}
DWORD finish(HANDLE pipe, OVERLAPPED& operation, BOOL started, ULONGLONG deadline, HANDLE cancel) {
    const auto error = started ? ERROR_SUCCESS : GetLastError(); need(started || error == ERROR_IO_PENDING);
    DWORD count = 0;
    if (!started) {
        const HANDLE events[]{cancel, operation.hEvent}; const auto now = GetTickCount64();
        const auto waited = WaitForMultipleObjects(2, events, FALSE, now < deadline ? static_cast<DWORD>(deadline - now) : 0);
        if (waited != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE); need(false);
        }
    }
    need(GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE);
    need(WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0 && GetTickCount64() < deadline); return count;
}
void transfer(HANDLE pipe, char* bytes, std::size_t size, bool write, ULONGLONG deadline, HANDLE cancel) {
    while (size) {
        need(GetTickCount64() < deadline && WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0);
        NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); need(event.valid());
        OVERLAPPED operation{}; operation.hEvent = event.get();
        const auto amount = static_cast<DWORD>((std::min)(size, std::size_t(65536)));
        const auto started = write ? WriteFile(pipe, bytes, amount, nullptr, &operation) : ReadFile(pipe, bytes, amount, nullptr, &operation);
        const auto count = finish(pipe, operation, started, deadline, cancel); need(count && count <= amount);
        bytes += count; size -= count;
    }
}
void send(HANDLE pipe, std::string_view bytes, std::size_t limit, ULONGLONG deadline, HANDLE cancel) {
    need(!bytes.empty() && bytes.size() <= limit); auto size = static_cast<std::uint32_t>(bytes.size());
    transfer(pipe, reinterpret_cast<char*>(&size), sizeof(size), true, deadline, cancel);
    transfer(pipe, const_cast<char*>(bytes.data()), bytes.size(), true, deadline, cancel);
}
std::string receive(HANDLE pipe, std::size_t limit, ULONGLONG deadline, HANDLE cancel) {
    std::uint32_t size = 0; transfer(pipe, reinterpret_cast<char*>(&size), sizeof(size), false, deadline, cancel);
    need(size && size <= limit); std::string bytes(size, '\0'); transfer(pipe, bytes.data(), bytes.size(), false, deadline, cancel); return bytes;
}
NativeHandle control_object(bool mutex, std::wstring_view instance) {
    need(instance.size() <= 192 && instance.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == instance.npos);
    const auto identity = process_identity(); need(identity.container.empty());
    Descriptor descriptor(L"D:P(A;;GA;;;" + identity.user + L")");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
    const auto name = L"Local\\nskkzime-container-v1-" + identity.user + std::wstring(instance) + (mutex ? L"-instance" : L"-stop");
    NativeHandle result(mutex ? CreateMutexW(&attributes, FALSE, name.c_str()) : CreateEventW(&attributes, TRUE, FALSE, name.c_str()));
    need(result.valid()); return result;
}
}
ProcessIdentity process_identity(HANDLE process) {
    HANDLE raw = nullptr; need(OpenProcessToken(process, TOKEN_QUERY, &raw) != FALSE); NativeHandle token(raw);
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
    return std::filesystem::path(path).parent_path() / L"nskkzime_broker.exe";
}
std::wstring container_pipe_name(const ProcessIdentity& identity) {
    need(!identity.container.empty());
    return L"\\\\.\\pipe\\Sessions\\" + std::to_wstring(identity.session) + L"\\AppContainerNamedObjects\\" + identity.container +
        L"\\nskkzime-broker-v1-" + identity.user;
}
std::string container_exchange(std::string_view request, ULONGLONG deadline, HANDLE cancel) {
    const auto identity = process_identity(); need(!identity.container.empty());
    const auto name = L"\\\\.\\pipe\\LOCAL\\nskkzime-broker-v1-" + identity.user;
    HANDLE raw = INVALID_HANDLE_VALUE;
    for (;;) {
        need(GetTickCount64() < deadline && WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0);
        raw = CreateFileW(name.c_str(), (FILE_GENERIC_READ | FILE_GENERIC_WRITE) & ~FILE_CREATE_PIPE_INSTANCE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
        if (raw != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError(); need(error == ERROR_PIPE_BUSY || error == ERROR_FILE_NOT_FOUND);
        WaitForSingleObject(cancel, 20);
    }
    NativeHandle pipe(raw); check_peer(pipe.get(), false, identity);
    send(pipe.get(), request, container_request_limit, deadline, cancel);
    auto response = receive(pipe.get(), container_response_limit, deadline, cancel);
    char ack = 1; transfer(pipe.get(), &ack, 1, true, deadline, cancel); return response;
}
void serve_container(const std::vector<std::wstring>& packages, const ContainerHandler& handler, HANDLE stop) {
    auto own = process_identity(); need(own.container.empty() && !packages.empty() && packages.size() <= 16);
    std::vector<std::wstring> sids;
    for (const auto& package : packages) {
        const auto sid = container_sid(package); if (std::find(sids.begin(), sids.end(), sid) == sids.end()) sids.push_back(sid);
    }
    // Clients can verify our token and exact executable path, but cannot inspect
    // memory, duplicate handles, write the process, or change its token.
    grant_query(GetCurrentProcess(), PROCESS_QUERY_LIMITED_INFORMATION, sids);
    HANDLE raw = nullptr; need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | READ_CONTROL | WRITE_DAC, &raw) != FALSE);
    NativeHandle token(raw); grant_query(token.get(), TOKEN_QUERY, sids);
    struct Listener { NativeHandle pipe; ProcessIdentity peer; };
    std::vector<Listener> listeners;
    for (const auto& sid : sids) {
        auto peer = own; peer.container = sid; const auto name = container_pipe_name(peer);
        Descriptor descriptor(L"D:P(A;;GA;;;" + own.user + L")(A;;0x12019b;;;" + sid + L")S:(ML;;NW;;;LW)");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
        for (int i = 0; i < 4; ++i) {
            NativeHandle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (i ? 0 : FILE_FLAG_FIRST_PIPE_INSTANCE),
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4, 65536, 65536, 0, &attributes));
            need(pipe.valid()); listeners.push_back({std::move(pipe), peer});
        }
    }
    auto run = [&](Listener& listener) {
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event.valid()) return;
            OVERLAPPED operation{}; operation.hEvent = event.get();
            const auto connected = ConnectNamedPipe(listener.pipe.get(), &operation); const auto error = connected ? ERROR_SUCCESS : GetLastError();
            try {
                if (!connected && error != ERROR_PIPE_CONNECTED) finish(listener.pipe.get(), operation, connected, GetTickCount64() + 60000, stop);
                check_peer(listener.pipe.get(), true, listener.peer);
                const auto request = receive(listener.pipe.get(), container_request_limit, GetTickCount64() + 2000, stop);
                const auto deadline = GetTickCount64() + 30000;
                const auto cancelled = [&] { return GetTickCount64() >= deadline || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
                    !PeekNamedPipe(listener.pipe.get(), nullptr, 0, nullptr, nullptr, nullptr); };
                const auto response = handler(request, cancelled); need(!cancelled());
                send(listener.pipe.get(), response, container_response_limit, deadline, stop);
                char ack = 0; transfer(listener.pipe.get(), &ack, 1, false, GetTickCount64() + 1000, stop);
            } catch (...) { /* Reject malformed, disconnected or unauthorized requests without logging text. */ }
            DisconnectNamedPipe(listener.pipe.get());
        }
    };
    std::vector<std::jthread> threads;
    try { for (auto& listener : listeners) threads.emplace_back(run, std::ref(listener)); }
    catch (...) { SetEvent(stop); throw; }
    // Listeners live until all jthreads have joined.
}
NativeHandle container_stop_event(std::wstring_view instance) { return control_object(false, instance); }
NativeHandle container_instance_mutex(std::wstring_view instance) { return control_object(true, instance); }
} // namespace nskkzime::windows
