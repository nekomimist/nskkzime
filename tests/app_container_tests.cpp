#include "app_container.h"
#include "container_runtime.h"
#include "dictionary_wait.h"
#include "tsf_test_host.h"
#include "candidate_ui.h"
#include "inference_client.h"
#include <userenv.h>
#include <aclapi.h>
#include <fstream>
#include <future>
#include <iostream>

namespace nskkzime::windows { HINSTANCE module_instance = nullptr; std::atomic<long> module_objects{0}, server_locks{0}; }
using namespace nskkzime;
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void messages() { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
struct Profile {
    std::wstring name; PSID sid = nullptr;
    explicit Profile(std::wstring value) : name(std::move(value)) {
        check(SUCCEEDED(CreateAppContainerProfile(name.c_str(), L"nskkzime test", L"Isolated IME test", nullptr, 0, &sid)), "create profile");
    }
    ~Profile() { FreeSid(sid); DeleteAppContainerProfile(name.c_str()); }
};
struct FileGrant {
    std::wstring path; PSECURITY_DESCRIPTOR descriptor = nullptr; PACL old = nullptr;
    FileGrant(std::wstring file, PSID sid) : path(std::move(file)) {
        check(GetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old, nullptr, &descriptor) == ERROR_SUCCESS, "read fixture ACL");
        EXPLICIT_ACCESSW entry{}; entry.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE; entry.grfAccessMode = GRANT_ACCESS;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID; entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL next = nullptr; check(SetEntriesInAclW(1, &entry, old, &next) == ERROR_SUCCESS, "fixture ACL");
        const auto result = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, next, nullptr);
        LocalFree(next); check(result == ERROR_SUCCESS, "grant fixture execute");
    }
    ~FileGrant() { SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, old, nullptr); LocalFree(descriptor); }
};
NativeHandle launch(const std::filesystem::path& exe, std::wstring args, PSID container = nullptr) {
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    HANDLE output = nullptr;
    check(DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(), &output, 0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE, "child output");
    NativeHandle output_handle(output);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output;
    SIZE_T size = 0; std::vector<unsigned char> attributes;
    SECURITY_CAPABILITIES caps{}; caps.AppContainerSid = container;
    {
        InitializeProcThreadAttributeList(nullptr, container ? 2 : 1, 0, &size); attributes.resize(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        check(InitializeProcThreadAttributeList(startup.lpAttributeList, container ? 2 : 1, 0, &size) != FALSE, "initialize attributes");
        check(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &output, sizeof(output), nullptr, nullptr) != FALSE, "output attribute");
        if (container) check(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr) != FALSE, "container attribute");
    }
    PROCESS_INFORMATION child{}; auto command = L"\"" + exe.native() + L"\" " + args;
    const auto created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    check(created != FALSE, "launch test child"); CloseHandle(child.hThread); return NativeHandle(child.hProcess);
}
DWORD wait_child(HANDLE process) {
    if (WaitForSingleObject(process, 45000) != WAIT_OBJECT_0) { TerminateProcess(process, 99); WaitForSingleObject(process, 1000); return 99; }
    DWORD code = 0; check(GetExitCodeProcess(process, &code) != FALSE, "read child exit"); return code;
}
struct Session {
    ComPtr<ITfTextInputProcessorEx> service; ComPtr<ITfKeyEventSink> keys;
    ComPtr<test_host::Manager> host; ComPtr<test_host::Context> context;
    Session() {
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "create service"); service.As(&keys);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, TF_TMF_IMMERSIVEMODE)), "container activation");
        wait_for_dictionary(runtime_dictionary_ready);
        check(container_configuration() != nullptr, "container bootstrap");
    }
    ~Session() { service->Deactivate(); context->pump(); }
    void key(WPARAM code, bool shift = false) {
        BYTE state[256]{}; if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80; SetKeyboardState(state);
        const auto scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
        BOOL eaten = FALSE; keys->OnTestKeyDown(context.Get(), code, scan, &eaten); check(eaten, "key preflight");
        const auto started = GetTickCount64(); keys->OnKeyDown(context.Get(), code, scan, &eaten);
        check(eaten && GetTickCount64() - started < 150, "blocked container key"); keys->OnKeyUp(context.Get(), code, scan, &eaten);
    }
    void type(std::string_view text) { for (char ch : text) { const auto vk = VkKeyScanExW(ch, GetKeyboardLayout(0)); key(LOBYTE(vk), (HIBYTE(vk) & 1) != 0); } }
    void wait(DWORD ms = 400) { const auto end = GetTickCount64() + ms; do { messages(); context->pump(); Sleep(5); } while (GetTickCount64() < end); }
    void complete() {
        const auto end = GetTickCount64() + 5000;
        while (context->queued.empty() && GetTickCount64() < end) { messages(); Sleep(5); }
        check(!context->queued.empty(), "missing container inference result"); context->pump();
    }
};
int container_test(bool denied, bool real_model) {
    check(in_app_container(), "child is not AppContainer");
    NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (denied) {
        bool rejected = false; try { container_exchange("INIT", GetTickCount64() + 1500, cancel.get()); } catch (...) { rejected = true; }
        check(rejected, "unauthorized endpoint accepted"); return 0;
    }
    const auto path = environment_value(L"NSKKZIME_CONFIG"); check(path.has_value(), "missing fixture path");
    NativeHandle file(CreateFileW(path->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    check(!file.valid(), "container unexpectedly reads private settings");
    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization");
    if (real_model) {
        {
            Session session;
            session.context->doc->text = L"試験問題の"; session.context->doc->caret = static_cast<LONG>(session.context->doc->text.size());
            session.type("Kaitou"); session.key(VK_SPACE); session.complete();
            check(session.context->doc->text == L"試験問題の解答", "real broker context rerank"); session.key(VK_RETURN);
        }
        {
            Session session; session.type("Mondainasasou"); session.key(VK_SPACE); session.complete();
            check(session.context->doc->text == L"問題なさそう", "real broker generation"); session.key(VK_RETURN); session.wait(300);
        }
        CoUninitialize(); return 0;
    }
    {
        Session session;
        session.type("Kanji"); session.key(VK_SPACE); check(session.context->doc->text == L"漢字" && !session.host->ui, "first dictionary candidate");
        session.key(VK_SPACE); check(!session.host->ui, "candidate delay"); session.key(VK_SPACE); check(session.host->ui != nullptr, "candidate list");
        session.type("s"); check(session.context->doc->text == L"感じ", "ASDF shortcut"); session.wait(500);
    }
    {
        Session session; session.type("Kaitou"); session.key(VK_SPACE); session.complete();
        check(session.context->doc->text == L"解答", "broker rerank"); session.key(VK_RETURN);
        session.type("Mikan"); session.key(VK_SPACE); session.complete(); check(session.context->doc->text == L"解答蜜柑", "broker generation"); session.key(VK_RETURN);
        session.wait(300);
    }
    {
        Session session; session.type("Mikan"); session.key(VK_SPACE); session.key(VK_ESCAPE); session.wait();
        check(session.context->doc->text.empty(), "late response replaced cancelled reading");
    }
    {
        Session session; session.context->input_scope = IS_PASSWORD; session.type("Himitsu"); session.key(VK_SPACE); session.wait();
        check(session.context->queued.empty(), "protected field sent inference");
    }
    {
        bool rejected = false;
        try { container_exchange("INF1" + std::string(64, '0') + "{}", GetTickCount64() + 1500, cancel.get()); } catch (...) { rejected = true; }
        check(rejected, "stale settings accepted");
        rejected = false;
        try { container_operation("FILE", "C:\\private.txt", GetTickCount64() + 1500, cancel.get()); } catch (...) { rejected = true; }
        check(rejected, "arbitrary file operation accepted");
    }
    CoUninitialize(); return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2) return container_test(std::wstring_view(argv[1]) == L"--denied", std::wstring_view(argv[1]) == L"--real-client");
        const bool real_model = argc == 3 && std::wstring_view(argv[1]) == L"--model";
        check(argc == 1 || real_model, "invalid test arguments");
        if (real_model && !std::filesystem::exists(argv[2])) return 77;
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        const auto id = learning_client_id(); const std::wstring token(id.begin(), id.end());
        Profile allowed(L"nskkzime-test-" + token), denied(L"nskkzime-denied-" + token);
        FileGrant allow_binary(executable, allowed.sid), deny_binary(executable, denied.sid);
        const auto root = std::filesystem::path(executable).parent_path() / ("container-test-" + id); std::filesystem::create_directory(root);
        const auto config = root / L"settings.ini", dict = root / L"fixture.skk", user = root / L"user-dictionary.nskkdb";
        { std::ofstream out(dict, std::ios::binary); out << "かんじ /漢字/感じ/幹事/\nかいとう /回答/解答/解凍/\n"; }
        DictionarySettings settings{{dict}, true}; settings.candidates = {3, CandidateKeys::Asdfjkl};
        settings.zenz.enabled = true; settings.zenz.model = root / L"fixture.gguf";
        settings.zenz.server = std::filesystem::path(executable).parent_path() / L"nkkz-ime-server.exe";
        settings.zenz.timeout_ms = 3000; settings.zenz.policy.rerank = true; settings.zenz.policy.score_timeout_ms = 3000;
        settings.zenz.log_input = true; settings.zenz.log_directory = root / L"logs";
        std::filesystem::create_directory(settings.zenz.log_directory);
        if (real_model) settings.zenz.model = argv[2];
        save_dictionary_settings(config, settings);
        SetEnvironmentVariableW(L"NSKKZIME_CONFIG", config.c_str()); SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", nullptr);
        SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", user.c_str()); SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TOKEN", token.c_str());
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY", nullptr);
        std::atomic<bool> private_seen = false;
        std::future<int> inference;
        if (!real_model) inference = std::async(std::launch::async, [&] { return serve_inference(token, [&](const InferenceRequest& request, const auto& cancelled) {
            if (request.reading == U"ひみつ") private_seen = true;
            const auto end = GetTickCount64() + 160; while (!cancelled() && GetTickCount64() < end) Sleep(5);
            InferenceResponse response; response.id = request.id; response.operation = request.operation;
            if (request.operation == InferenceOperation::Score) { response.scores.assign(request.candidates.size(), -5); if (response.scores.size() > 1) response.scores[1] = 0; }
            else response.candidates = {U"蜜柑", U"みかん"}; return response;
        }, "fixture", 10000, 5000); });
        const auto instance = container_sid(allowed.name); const auto stop = container_stop_event(instance);
        const auto broker = launch(container_broker_path(), L"--config \"" + config.native() + L"\" --test-package " + allowed.name);
        int result = 1;
        try {
            auto client = launch(executable, real_model ? L"--real-client" : L"--client", allowed.sid); const auto code = wait_child(client.get());
            std::cout << "allowed child=" << code << std::endl; check(code == 0, "container client failed");
            check(!private_seen.load(), "broker received protected text");
            if (!real_model) {
                const auto learned = read_user_dictionary(user).lookup({U"かんじ", 0});
                check(!learned.empty() && learned.front().text == U"感じ", "broker learning did not persist");
            }
            bool logged = false; for (const auto& item : std::filesystem::directory_iterator(root / L"logs")) if (item.path().extension() == L".jsonl") logged = true;
            check(logged, "broker input log missing");
            auto blocked = launch(executable, L"--denied", denied.sid); check(wait_child(blocked.get()) == 0, "unlisted container connected");
            result = 0;
        } catch (...) { SetEvent(stop.get()); WaitForSingleObject(broker.get(), 35000); throw; }
        SetEvent(stop.get()); check(WaitForSingleObject(broker.get(), 35000) == WAIT_OBJECT_0, "broker did not stop");
        ResetEvent(stop.get()); std::atomic<bool> spoof_received = false;
        auto spoof = std::async(std::launch::async, [&] { serve_container({allowed.name}, [&](auto, const auto&) { spoof_received = true; return std::string("wrong server"); }, stop.get()); });
        auto client = launch(executable, L"--denied", allowed.sid); const auto code = wait_child(client.get()); SetEvent(stop.get()); spoof.get();
        check(code == 0 && !spoof_received.load(), "client sent data to another executable");
        if (inference.valid()) inference.get();
        if (real_model) {
            // This randomly named endpoint belongs only to this fixture.
            NativeHandle pipe(CreateFileW(inference_pipe_name(token).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
            ULONG pid = 0; check(pipe.valid() && GetNamedPipeServerProcessId(pipe.get(), &pid), "fixture server identity");
            NativeHandle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
            check(process.valid() && TerminateProcess(process.get(), 0), "stop fixture model"); WaitForSingleObject(process.get(), 5000);
        }
        std::cout << "AppContainer broker, TSF, learning, inference, cancellation and peer checks passed\n";
        // Keep only this test's fixture directory for diagnosing cross-process failures.
        return result;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
