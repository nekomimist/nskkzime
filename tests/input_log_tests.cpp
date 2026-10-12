#include "input_log.h"
#include "learning_ipc.h"
#include "inference_client.h"
#include <aclapi.h>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::filesystem::path> logs(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> result;
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().extension() == L".jsonl") result.push_back(item.path());
    return result;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void check_acl(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    check(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                               nullptr, &descriptor) == ERROR_SUCCESS, "cannot inspect log ACL");
    bool valid = false;
    if (dacl && dacl->AceCount == 1) {
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        void* raw = nullptr;
        if (GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED) && GetAce(dacl, 0, &raw)) {
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            HANDLE process_token = nullptr;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
                NativeHandle token(process_token); DWORD size = 0;
                GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
                std::vector<unsigned char> bytes(size);
                valid = GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size) &&
                    ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && !(ace->Header.AceFlags & INHERITED_ACE) &&
                    EqualSid(const_cast<DWORD*>(&ace->SidStart), reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
            }
        }
    }
    LocalFree(descriptor);
    check(valid, "log inherited access or was granted to another SID");
}
}
int main() {
    const auto root = std::filesystem::current_path() / ("input-log-test-" + learning_client_id());
    std::filesystem::create_directory(root);
    int result = 1;
    try {
        const InferenceRequest request{1, U"かな", U"前\n\"\\😀", U"後", 2};
        const InferenceResponse response{1, {U"仮名", U"かな😀"}};
        const InputLogRecord record{request, response, InferenceStatus::Ok};
        { InputLog disabled({}); check(disabled.write(record) == InputLogStatus::Off, "default logger wrote"); }
        check(std::filesystem::is_empty(root), "disabled logger created files");
        const auto normal = root / L"ログ 日本語 space";
        std::filesystem::create_directory(normal);
        const auto sentinel = normal / L"settings.ini";
        { std::ofstream file(sentinel); file << "keep user data"; }
        {
            InputLog writer(normal);
            check(logs(normal).empty(), "constructor performed I/O");
            check(writer.write(record) == InputLogStatus::Written, "cannot write diagnostic record");
            const auto files = logs(normal); check(files.size() == 1, "missing log");
            const auto line = contents(files[0]);
            check(std::count(line.begin(), line.end(), '\n') == 1, "text injected extra JSON lines");
            const auto json = nlohmann::json::parse(line);
            check(json.at("format") == "nskkzime-input-log" && json.at("version") == 1 && json.at("status") == "ok", "missing schema/status");
            check(decode_request(json.at("request").dump()).left == request.left &&
                  decode_response(json.at("response").dump(), request).candidates == response.candidates, "text or candidates changed");
            check_acl(files[0]);
            const InputLogRecord timeout{request, std::nullopt, InferenceStatus::Timeout};
            check(writer.write(timeout) == InputLogStatus::Written, "cannot write timeout metadata");
            const auto bytes = contents(files[0]);
            const auto second = nlohmann::json::parse(bytes.substr(bytes.find('\n') + 1));
            check(second.at("response").is_null() && second.at("status") == "timeout", "expired result recorded");
            InputLogStatus status = InputLogStatus::Written;
            for (int i = 0; i < 10000 && status == InputLogStatus::Written; ++i) status = writer.write(record);
            check(status == InputLogStatus::Full, "file size cap not reached");
            const auto size = std::filesystem::file_size(files[0]);
            check(size <= InputLog::file_limit && size > InputLog::file_limit / 2, "incorrect size cap");
            check(writer.write(record) == InputLogStatus::Full && std::filesystem::file_size(files[0]) == size, "full file kept growing");
        }
        check(contents(sentinel) == "keep user data", "existing file changed");
        {
            InputLog writer(normal);
            check(writer.write(record) == InputLogStatus::Written && logs(normal).size() == 2, "new activation reused/truncated old log");
        }
        const auto full = root / L"full"; std::filesystem::create_directory(full);
        for (std::size_t i = 0; i < InputLog::directory_files; ++i) {
            std::ofstream file(full / ("nskkzime-input-fixture-" + std::to_string(i) + ".jsonl")); file << "preserve";
        }
        { InputLog writer(full); check(writer.write(record) == InputLogStatus::Full, "directory cap ignored"); }
        check(logs(full).size() == InputLog::directory_files && contents(logs(full)[0]) == "preserve", "directory cap overwrote a log");
        { InputLog writer(root / L"missing"); check(writer.write(record) == InputLogStatus::Unavailable, "missing directory accepted"); }
        check(!std::filesystem::exists(root / L"missing"), "created unrequested directory");
        const auto locked = root / L"locked"; std::filesystem::create_directory(locked);
        {
            NativeHandle lock(CreateFileW((locked / L"nskkzime-input-log.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                          0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            check(lock.valid(), "cannot create lock fixture");
            InputLog writer(locked);
            check(writer.write(record) == InputLogStatus::Unavailable && logs(locked).empty(), "contended destination wrote text");
        }
        const auto client_logs = root / L"client"; std::filesystem::create_directory(client_logs);
        InferenceConfig config; config.log_directory = client_logs;
        { InferenceClient disabled(config, {}); disabled.record_input(request, &response, InferenceStatus::Ok); Sleep(30); }
        check(std::filesystem::is_empty(client_logs), "directory setting alone enabled input logging");
        config.log_input = true;
        {
            InferenceClient client(config, {});
            check(logs(client_logs).empty(), "enabled constructor created a log before validation");
            const auto start = GetTickCount64();
            client.record_input(request, &response, InferenceStatus::Ok);
            check(GetTickCount64() - start < 100, "record submission waited on I/O");
            const auto end = GetTickCount64() + 3000;
            while (client.input_log_status().find(L"保存中") == std::wstring::npos && GetTickCount64() < end) Sleep(5);
            check(client.input_log_status().find(L"保存中") != std::wstring::npos && logs(client_logs).size() == 1, "worker did not persist queued record");
        }
        std::cout << "PASS opt-in only, escaped JSON, user-only ACL, file/directory limits, no overwrite, missing path and asynchronous queue\n";
        result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    // Only our absolute, uniquely named workspace fixture is eligible for cleanup.
    if (root.parent_path() == std::filesystem::current_path() && root.filename().native().starts_with(L"input-log-test-")) {
        const auto end = GetTickCount64() + 3000;
        std::error_code error;
        do { std::filesystem::remove_all(root, error); if (!error) break; Sleep(5); } while (GetTickCount64() < end);
        if (error) { std::cerr << "FAIL input log worker did not release fixture\n"; result = 1; }
    }
    return result;
}
