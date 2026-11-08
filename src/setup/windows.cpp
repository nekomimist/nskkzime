#include "platform.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include <memory>

namespace nskkzime::setup {
namespace {
struct LocalMemory {
    void* value = nullptr;
    ~LocalMemory() { if (value) LocalFree(value); }
};
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
void status_check(LSTATUS error, const std::string& operation) {
    if (error != ERROR_SUCCESS) { SetLastError(static_cast<DWORD>(error)); win_check(false, operation); }
}
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    require(SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)), "Find Windows folder");
    const fs::path result(path);
    CoTaskMemFree(path);
    return result;
}
std::vector<unsigned char> token_information(TOKEN_INFORMATION_CLASS kind) {
    HANDLE raw = nullptr;
    win_check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "Open process token");
    Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.value, kind, nullptr, 0, &size);
    require(size > 0 && size <= 64 * 1024, "Invalid token information size");
    std::vector<unsigned char> bytes(size);
    win_check(GetTokenInformation(token.value, kind, bytes.data(), size, &size), "Read process token");
    return bytes;
}
const wchar_t* class_key = L"Software\\Classes\\CLSID\\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}";
fs::path registration(bool x86) {
    const REGSAM view = x86 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
    Key shadow;
    auto error = RegOpenKeyExW(HKEY_CURRENT_USER, class_key, 0, KEY_READ | view, &shadow.value);
    require(error == ERROR_FILE_NOT_FOUND, "Cannot use a shadowed or inaccessible HKCU CLSID");
    Key key;
    error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key, 0, KEY_READ | view, &key.value);
    if (error == ERROR_FILE_NOT_FOUND) return {};
    status_check(error, "Read IME CLSID");
    Key server;
    status_check(RegOpenKeyExW(key.value, L"InprocServer32", 0, KEY_READ | view, &server.value), "Read IME server");
    DWORD kind = 0, size = 0;
    status_check(RegQueryValueExW(server.value, nullptr, nullptr, &kind, nullptr, &size), "Read IME DLL size");
    require(kind == REG_SZ && size >= sizeof(wchar_t) && size <= 65536 && size % sizeof(wchar_t) == 0, "Invalid IME registry value");
    std::wstring value(size / sizeof(wchar_t), L'\0');
    status_check(RegQueryValueExW(server.value, nullptr, nullptr, &kind, reinterpret_cast<BYTE*>(value.data()), &size), "Read IME DLL");
    require(kind == REG_SZ && value.back() == L'\0', "Unterminated IME path");
    value.pop_back();
    require(value.find(L'\0') == std::wstring::npos && fs::path(value).is_absolute(), "Invalid registered IME path");
    return value;
}
}
fs::path program_root() { return known_folder(FOLDERID_ProgramFiles) / L"nskkzime"; }
fs::path executable_path() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    win_check(size > 0 && size < path.size(), "Find setup executable");
    path.resize(size); return path;
}
std::wstring user_sid() {
    const auto bytes = token_information(TokenUser);
    LPWSTR text = nullptr;
    win_check(ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid, &text), "Read user SID");
    LocalMemory memory{text};
    return text;
}
bool elevated() {
    const auto bytes = token_information(TokenElevation);
    return reinterpret_cast<const TOKEN_ELEVATION*>(bytes.data())->TokenIsElevated != 0;
}
void assert_native_x64() {
    USHORT process = 0, native = 0;
    win_check(IsWow64Process2(GetCurrentProcess(), &process, &native), "Read process architecture");
    require(native == IMAGE_FILE_MACHINE_AMD64 && process == IMAGE_FILE_MACHINE_UNKNOWN && sizeof(void*) == 8,
        "Native setup requires Windows x64; ARM64 experimental installations use their separate tools");
}
void assert_install_context() {
    UINT32 size = 0;
    require(GetCurrentPackageFullName(&size, nullptr) == APPMODEL_ERROR_NO_PACKAGE, "Packaged execution context detected. Launch setup from Explorer");
    const auto path = known_folder(FOLDERID_LocalAppData) / L"nskkzime/settings.ini";
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "Cannot check installation context");
        return;
    }
    std::wstring actual(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(file.value, actual.data(), static_cast<DWORD>(actual.size()), FILE_NAME_NORMALIZED);
    win_check(length > 0 && length < actual.size(), "Check installation context");
    actual.resize(length);
    if (actual.starts_with(L"\\\\?\\")) actual.erase(0, 4);
    require(same_path(actual, path), "Data virtualization detected. Launch setup from Explorer");
}
void protected_path(const fs::path& path) {
    no_reparse(path);
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    status_check(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor), "Read installation ACL");
    LocalMemory memory{descriptor};
    require(owner && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid)), "Installation owner must be SYSTEM or Administrators");
    require(acl != nullptr, "Unprotected installation DACL");
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw = nullptr;
        win_check(GetAce(acl, index, &raw), "Read installation ACE");
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        require(header->AceType == ACCESS_ALLOWED_ACE_TYPE, "Unsupported installation ACE");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        auto sid = const_cast<DWORD*>(&ace->SidStart);
        const DWORD allowed = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        require(IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || !(ace->Mask & ~allowed), "Installation is writable by a non-administrator");
    }
}
void protected_tree(const fs::path& root) {
    protected_path(root);
    for (const auto& entry : fs::recursive_directory_iterator(root)) protected_path(entry.path());
}
void ensure_appcontainer_access(const fs::path& root) {
    protected_tree(root);
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    status_check(GetNamedSecurityInfoW(root.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &acl, nullptr, &descriptor), "Read AppContainer access");
    LocalMemory memory{descriptor};
    BYTE sid[SECURITY_MAX_SID_SIZE]{}; DWORD sid_size = sizeof(sid);
    win_check(CreateWellKnownSid(WinBuiltinAnyPackageSid, nullptr, sid, &sid_size), "Create AppContainer SID");
    const DWORD rights = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw = nullptr; win_check(GetAce(acl, index, &raw), "Read AppContainer ACE");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(const_cast<DWORD*>(&ace->SidStart), sid) &&
            (ace->Mask & rights) == rights && (ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) &&
            !(ace->Header.AceFlags & INHERIT_ONLY_ACE)) return;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = rights; access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID; access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    PACL updated = nullptr;
    status_check(SetEntriesInAclW(1, &access, acl, &updated), "Build AppContainer read access");
    LocalMemory updated_memory{updated};
    status_check(SetNamedSecurityInfoW(const_cast<LPWSTR>(root.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr), "Grant AppContainer read access");
    protected_tree(root);
}
void assert_root(const fs::path& root) {
    protected_path(root);
    const auto marker = root / L"install-root.json";
    protected_path(marker);
    const auto record = parse_json(read_file(marker, 16 * 1024));
    require(record.at("format_version").is_number_integer() && record.at("format_version") == 1 && record.at("product") == "nskkzime", "Invalid installation root marker");
}
void initialize_root(const fs::path& root) {
    no_reparse(root);
    if (fs::exists(root)) { assert_root(root); return; }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    win_check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)", SDDL_REVISION_1, &descriptor, nullptr), "Build installation ACL");
    LocalMemory memory{descriptor};
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    win_check(CreateDirectoryW(root.c_str(), &security), "Create protected installation root");
    protected_path(root);
    write_new(root / L"install-root.json", "{\"format_version\":1,\"product\":\"nskkzime\"}\n");
    assert_root(root);
}
void assert_no_registrar() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    win_check(snapshot.value != INVALID_HANDLE_VALUE, "Inspect running registrars");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    win_check(Process32FirstW(snapshot.value, &entry), "Read process list");
    do { require(_wcsicmp(entry.szExeFile, L"regsvr32.exe") != 0, "regsvr32 is still running; wait before installing or recovering"); }
    while (Process32NextW(snapshot.value, &entry));
    require(GetLastError() == ERROR_NO_MORE_FILES, "Cannot finish reading process list");
}
std::wstring quote(const std::wstring& argument) {
    require(argument.find(L'\0') == std::wstring::npos, "NUL in process argument");
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (auto c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'\"';
    return result;
}
DWORD run_process(const fs::path& executable, const std::vector<std::wstring>& arguments, bool wait) {
    require(executable.is_absolute(), "Executable must be an absolute path: " + utf8(executable.native()));
    auto command = quote(executable.native());
    for (const auto& argument : arguments) command += L" " + quote(argument);
    STARTUPINFOW start{}; start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    win_check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        executable.parent_path().c_str(), &start, &process), "Start " + utf8(executable.native()));
    Handle child(process.hProcess), thread(process.hThread);
    if (!wait) return 0;
    win_check(WaitForSingleObject(child.value, INFINITE) == WAIT_OBJECT_0, "Wait for child process");
    DWORD code = 0;
    win_check(GetExitCodeProcess(child.value, &code), "Read child result");
    return code;
}
DWORD elevate(const std::vector<std::wstring>& arguments) {
    const auto executable = executable_path();
    std::wstring parameters;
    for (const auto& argument : arguments) { if (!parameters.empty()) parameters += L' '; parameters += quote(argument); }
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas"; info.lpFile = executable.c_str(); info.lpParameters = parameters.c_str(); info.nShow = SW_SHOWNORMAL;
    win_check(ShellExecuteExW(&info), "Start setup with UAC approval by the same user");
    Handle child(info.hProcess);
    require(child.value != nullptr, "No elevated setup process");
    win_check(WaitForSingleObject(child.value, INFINITE) == WAIT_OBJECT_0, "Wait for elevated setup");
    DWORD code = 0;
    win_check(GetExitCodeProcess(child.value, &code), "Read elevated setup result");
    return code;
}
fs::path registrar_path(bool x86) {
    // WOW64's API can return a required length without filling a 32768-WCHAR
    // buffer. Start with MAX_PATH and grow only if the API requires it.
    std::wstring system(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto count = x86 ? GetSystemWow64DirectoryW(system.data(), static_cast<UINT>(system.size())) :
            GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        win_check(count > 0, "Find Windows registrar");
        if (count >= system.size()) {
            require(count < 32767, "Windows system directory is too long");
            system.assign(static_cast<size_t>(count) + 1, L'\0');
            continue;
        }
        require(system.find(L'\0') == count, "Windows returned an invalid system directory");
        system.resize(count);
        const auto result = fs::path(system) / L"regsvr32.exe";
        require(result.is_absolute(), "Windows returned a non-absolute registrar path");
        return result;
    }
    throw std::runtime_error("Windows system directory changed while reading it");
}
Backend windows_backend() {
    return {registration,
        [](const fs::path& dll, bool remove) {
            protected_path(dll);
            std::vector<std::wstring> args{L"/s"}; if (remove) args.emplace_back(L"/u"); args.push_back(dll.native());
            const auto code = run_process(registrar_path(same_path(dll.parent_path().filename(), L"x86")), args);
            require(code == 0, std::string(remove ? "Unregister " : "Register ") + utf8(dll.native()) + " failed (regsvr32 exit " + std::to_string(code) + ")");
        },
        [](const fs::path& broker) {
            if (!broker.empty()) protected_path(broker);
            Key run;
            status_check(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &run.value, nullptr), "Open broker startup key");
            if (broker.empty()) {
                const auto error = RegDeleteValueW(run.value, L"nskkzime broker");
                if (error != ERROR_FILE_NOT_FOUND) status_check(error, "Remove broker startup");
            } else {
                const auto command = quote(broker.native()) + L" --replace";
                status_check(RegSetValueExW(run.value, L"nskkzime broker", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))), "Set broker startup");
            }
        }};
}
void append_log(const fs::path& root, const std::string& message) {
    assert_root(root);
    const auto path = root / L"setup.log";
    no_reparse(path);
    if (fs::exists(path)) protected_path(path);
    Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    win_check(file.value != INVALID_HANDLE_VALUE, "Open setup log");
    LARGE_INTEGER size{};
    win_check(GetFileSizeEx(file.value, &size), "Read setup log size");
    if (size.QuadPart > 4 * 1024 * 1024) return;
    SYSTEMTIME time{}; GetSystemTime(&time);
    const auto bytes = std::to_string(time.wYear) + "-" + std::to_string(time.wMonth) + "-" + std::to_string(time.wDay) + "T" +
        std::to_string(time.wHour) + ":" + std::to_string(time.wMinute) + ":" + std::to_string(time.wSecond) + "Z " + message + "\n";
    DWORD written = 0;
    win_check(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "Write setup log");
    win_check(FlushFileBuffers(file.value), "Flush setup log");
}
std::string read_setup_log(const fs::path& root) {
    no_reparse(root);
    if (!fs::exists(root)) return {};
    assert_root(root);
    const auto path = root / L"setup.log";
    no_reparse(path);
    if (!fs::exists(path)) return {};
    protected_path(path);
    return read_file(path, 8 * 1024 * 1024);
}
std::string appended_log(const std::string& before, const std::string& after) {
    if (!after.starts_with(before)) return {};
    constexpr size_t limit = 16 * 1024;
    const auto count = after.size() - before.size();
    if (count <= limit) return after.substr(before.size());
    return "[Earlier log records omitted]\n" + after.substr(after.size() - limit);
}
}
