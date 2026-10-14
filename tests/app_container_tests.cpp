#include "app_container.h"
#include "broker_snapshot.h"
#include "container_runtime.h"
#include "dictionary_wait.h"
#include "tsf_mode_host.h"
#include "candidate_ui.h"
#include "inference_client.h"
#include <userenv.h>
#include <aclapi.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>

namespace nskkzime::windows { HINSTANCE module_instance = nullptr; std::atomic<long> module_objects{0}, server_locks{0}; }
using namespace nskkzime;
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// One production bootstrap attempt allows 30 seconds. Give initialization and
// retry scheduling some margin; this is a test budget, not a latency guarantee.
constexpr DWORD dictionary_ready_ms = 35000;
constexpr DWORD startup_control_ms = 45000;
nlohmann::json trace_record(const char* phase, nlohmann::json fields = {}) {
    fields["format"] = "nskkzime-startup-test";
    fields["phase"] = phase;
    fields["pid"] = GetCurrentProcessId();
    fields["uptime_ms"] = GetTickCount64();
    return fields;
}
void child_trace(const char* phase, nlohmann::json fields = {}) {
    std::cerr << trace_record(phase, std::move(fields)).dump() << '\n';
}
const char* reason_name(ContainerReason reason) {
    switch (reason) {
    case ContainerReason::None: return "none";
    case ContainerReason::Win32: return "win32";
    case ContainerReason::Timeout: return "timeout";
    case ContainerReason::Cancelled: return "cancelled";
    case ContainerReason::Validation: return "validation";
    case ContainerReason::Internal: return "internal";
    }
    return "unknown";
}
struct StartupTrace {
    std::ofstream output;
    explicit StartupTrace(const std::filesystem::path& root) : output(root / L"startup.jsonl", std::ios::binary) {
        check(output.good(), "create startup diagnostic log");
    }
    void record(const char* phase, nlohmann::json fields = {}) noexcept {
        try { output << trace_record(phase, std::move(fields)).dump() << '\n'; output.flush(); }
        catch (...) { std::cerr << "startup diagnostic write failed\n"; }
    }
};
nlohmann::json process_state(const NativeHandle& process) {
    if (!process.valid()) return {{"created", false}, {"running", false}};
    const auto waited = WaitForSingleObject(process.get(), 0);
    const auto wait_error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    DWORD code = 0;
    const bool read = GetExitCodeProcess(process.get(), &code) != FALSE;
    const auto code_error = read ? ERROR_SUCCESS : GetLastError();
    return {{"created", true}, {"pid", GetProcessId(process.get())}, {"running", waited == WAIT_TIMEOUT},
        {"wait_result", waited}, {"wait_error", wait_error}, {"exit_code", code}, {"exit_error", code_error}};
}
DWORD remaining(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}
void stop_owned_child(const NativeHandle& process) {
    if (process.valid() && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
        check(TerminateProcess(process.get(), 99) != FALSE, "terminate owned test child");
        check(WaitForSingleObject(process.get(), 1000) == WAIT_OBJECT_0, "reap owned test child");
    }
}
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
NativeHandle launch(const std::filesystem::path& exe, std::wstring args, PSID container = nullptr,
    HANDLE startup_gate = nullptr, HANDLE broker_created = nullptr, HANDLE diagnostic_output = nullptr) {
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    HANDLE output = nullptr;
    check(DuplicateHandle(GetCurrentProcess(), diagnostic_output ? diagnostic_output : GetStdHandle(STD_ERROR_HANDLE),
        GetCurrentProcess(), &output, 0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE, "child output");
    NativeHandle output_handle(output);
    HANDLE inherited_gate = nullptr;
    if (startup_gate) {
        check(DuplicateHandle(GetCurrentProcess(), startup_gate, GetCurrentProcess(), &inherited_gate, EVENT_MODIFY_STATE, TRUE,
            0) != FALSE, "startup gate inheritance");
        args += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited_gate));
    }
    NativeHandle gate_handle(inherited_gate);
    HANDLE inherited_broker = nullptr;
    if (broker_created) {
        check(DuplicateHandle(GetCurrentProcess(), broker_created, GetCurrentProcess(), &inherited_broker,
            SYNCHRONIZE, TRUE, 0) != FALSE, "broker notification inheritance");
        args += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited_broker));
    }
    NativeHandle broker_handle(inherited_broker);
    std::vector<HANDLE> inherited{output};
    if (startup_gate) inherited.push_back(inherited_gate);
    if (broker_created) inherited.push_back(inherited_broker);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output;
    SIZE_T size = 0; std::vector<unsigned char> attributes;
    SECURITY_CAPABILITIES caps{}; caps.AppContainerSid = container;
    {
        InitializeProcThreadAttributeList(nullptr, container ? 2 : 1, 0, &size); attributes.resize(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        check(InitializeProcThreadAttributeList(startup.lpAttributeList, container ? 2 : 1, 0, &size) != FALSE, "initialize attributes");
        check(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
            inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE, "output attribute");
        if (container) check(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr) != FALSE, "container attribute");
    }
    PROCESS_INFORMATION child{}; auto command = L"\"" + exe.native() + L"\" " + args;
    const auto created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
    const auto error = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) throw std::runtime_error("launch test child: Win32 error " + std::to_string(error));
    CloseHandle(child.hThread); return NativeHandle(child.hProcess);
}
DWORD wait_child(HANDLE process) {
    if (WaitForSingleObject(process, 45000) != WAIT_OBJECT_0) { TerminateProcess(process, 99); WaitForSingleObject(process, 1000); return 99; }
    DWORD code = 0; check(GetExitCodeProcess(process, &code) != FALSE, "read child exit"); return code;
}
NativeHandle start_early_children(const std::filesystem::path& executable, const Profile& allowed, const Profile& denied,
    const std::filesystem::path& root, const std::filesystem::path& config, HANDLE stop, std::wstring_view scenario) {
    StartupTrace trace(root);
    const auto started = GetTickCount64();
    // Reserve 30 seconds of the 120-second CTest watchdog for final cleanup.
    const auto deadline = started + 90000;
    NativeHandle on_gate(CreateEventW(nullptr, TRUE, FALSE, nullptr)), input_gate(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    NativeHandle broker_created(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    check(on_gate.valid() && input_gate.valid() && broker_created.valid(), "create startup notification events");
    NativeHandle on_output(CreateFileW((root / L"startup-on.log").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    NativeHandle input_output(CreateFileW((root / L"startup-input.log").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(on_output.valid() && input_output.valid(), "create startup child diagnostic files");
    NativeHandle early_on, early_input, broker;
    const auto children = [&] { return nlohmann::json{{"on", process_state(early_on)}, {"input", process_state(early_input)}}; };
    const char* phase = "launch_children";
    trace.record("startup_started", {{"scenario", utf8_message(scenario)}});
    try {
        early_on = launch(executable, scenario == L"child-failure" ? L"--startup-early-fail" : L"--startup-early-on",
            allowed.sid, on_gate.get(), broker_created.get(), on_output.get());
        early_input = launch(executable, L"--startup-early-input", allowed.sid, input_gate.get(), broker_created.get(), input_output.get());
        trace.record("children_created", {{"children", children()}});
        phase = "choices_wait";
        const auto choices_deadline = (std::min)(deadline, GetTickCount64() + 30000);
        bool on_chosen = false, input_chosen = false;
        while (!on_chosen || !input_chosen) {
            const auto on = process_state(early_on), input = process_state(early_input);
            check(on.at("running") && input.at("running"), "a startup child exited before broker creation");
            on_chosen = WaitForSingleObject(on_gate.get(), 0) == WAIT_OBJECT_0;
            input_chosen = WaitForSingleObject(input_gate.get(), 0) == WAIT_OBJECT_0;
            if (on_chosen && input_chosen) break;
            std::vector<HANDLE> watched{early_on.get(), early_input.get()};
            if (!on_chosen) watched.push_back(on_gate.get());
            if (!input_chosen) watched.push_back(input_gate.get());
            const auto waited = WaitForMultipleObjects(static_cast<DWORD>(watched.size()), watched.data(), FALSE, remaining(choices_deadline));
            const auto error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            if (waited == WAIT_FAILED || waited == WAIT_TIMEOUT) {
                trace.record("choices_wait_failed", {{"wait_result", waited}, {"win32_error", error}});
                throw std::runtime_error("startup choices did not reach both gates");
            }
        }
        trace.record("choices_completed", {{"elapsed_ms", GetTickCount64() - started}});
        if (scenario == L"delayed-broker") {
            Sleep(6000); // Regression: exceed the former pre-broker 5-second wait.
            const auto states = children();
            trace.record("deliberate_delay_completed", {{"delay_ms", 6000}, {"children", states}});
            check(states.at("on").at("running") && states.at("input").at("running"), "startup children exited during the deliberate broker delay");
        }
        phase = "broker_start";
        trace.record("broker_start_requested");
        const auto broker_path = scenario == L"broker-failure" ? root / L"missing-broker.exe" : container_broker_path();
        const auto& package = scenario == L"readiness-timeout" ? denied.name : allowed.name;
        broker = launch(broker_path, L"--config \"" + config.native() + L"\" --test-package " + package);
        trace.record("broker_process_created", {{"broker", process_state(broker)}});
        check(SetEvent(broker_created.get()) != FALSE, "notify broker process creation");
        // This notification means process creation succeeded, not IPC readiness.
        trace.record("broker_creation_notified");
        phase = "children_wait";
        const auto children_deadline = (std::min)(deadline, GetTickCount64() + startup_control_ms);
        bool on_done = false, input_done = false;
        while (!on_done || !input_done) {
            std::vector<HANDLE> watched;
            if (!on_done) watched.push_back(early_on.get());
            if (!input_done) watched.push_back(early_input.get());
            const auto waited = WaitForMultipleObjects(static_cast<DWORD>(watched.size()), watched.data(), FALSE, remaining(children_deadline));
            const auto error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            if (waited == WAIT_FAILED || waited == WAIT_TIMEOUT) {
                trace.record("children_wait_failed", {{"wait_result", waited}, {"win32_error", error}});
                throw std::runtime_error("startup children did not finish within their shared deadline");
            }
            const auto on = process_state(early_on), input = process_state(early_input);
            if (!on_done && !on.at("running")) { on_done = true; trace.record("child_exited", {{"child", "on"}, {"state", on}}); }
            if (!input_done && !input.at("running")) { input_done = true; trace.record("child_exited", {{"child", "input"}, {"state", input}}); }
        }
        const auto on = process_state(early_on), input = process_state(early_input);
        check(on.at("exit_error") == 0 && input.at("exit_error") == 0 &&
            on.at("exit_code") == 0 && input.at("exit_code") == 0, "early startup child failed; inspect its diagnostic log");
        trace.record("startup_passed", {{"elapsed_ms", GetTickCount64() - started}, {"children", children()}});
        return broker;
    } catch (const std::exception& error) {
        trace.record("failure", {{"at_phase", phase}, {"error", error.what()}, {"children", children()}, {"broker", process_state(broker)}});
        for (const auto* child : {&early_on, &early_input}) {
            try { stop_owned_child(*child); }
            catch (const std::exception& cleanup) { trace.record("cleanup_failed", {{"error", cleanup.what()}}); }
        }
        if (broker.valid()) {
            const auto signalled = SetEvent(stop);
            const auto stop_error = signalled ? ERROR_SUCCESS : GetLastError();
            trace.record("broker_stop_requested", {{"signalled", signalled != FALSE}, {"win32_error", stop_error}});
            const auto waited = WaitForSingleObject(broker.get(), (std::min)(DWORD{35000}, remaining(deadline)));
            trace.record("broker_shutdown", {{"wait_result", waited}, {"win32_error", waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS}});
            try { stop_owned_child(broker); }
            catch (const std::exception& cleanup) { trace.record("cleanup_failed", {{"error", cleanup.what()}}); }
        }
        trace.record("cleanup_completed", {{"elapsed_ms", GetTickCount64() - started}, {"children", children()}, {"broker", process_state(broker)}});
        throw;
    }
}
struct Session {
    ComPtr<ITfTextInputProcessorEx> service; ComPtr<ITfKeyEventSink> keys;
    ComPtr<test_host::Manager> host; ComPtr<test_host::Context> context;
    Session() {
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "create service"); service.As(&keys);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, TF_TMF_IMMERSIVEMODE)), "container activation");
        wait_for_dictionary(runtime_dictionary_ready, dictionary_ready_ms);
        check(container_configuration() != nullptr, "container bootstrap");
    }
    ~Session() { service->Deactivate(); context->pump(); }
    void key(WPARAM code, bool shift = false) {
        BYTE state[256]{}; if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80; SetKeyboardState(state);
        const auto scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
        BOOL eaten = FALSE; keys->OnTestKeyDown(context.Get(), code, scan, &eaten); check(eaten, "key preflight");
        const auto started = GetTickCount64();
        const auto result = keys->OnKeyDown(context.Get(), code, scan, &eaten);
        const auto elapsed = GetTickCount64() - started;
        if (!eaten || elapsed >= 150) child_trace("key_dispatch_failed",
            {{"accepted", eaten != FALSE}, {"elapsed_ms", elapsed}, {"hresult", result}});
        check(eaten, "container key was not consumed");
        check(elapsed < 150, "container key exceeded latency bound");
        keys->OnKeyUp(context.Get(), code, scan, &eaten);
    }
    void type(std::string_view text) { for (char ch : text) { const auto vk = VkKeyScanExW(ch, GetKeyboardLayout(0)); key(LOBYTE(vk), (HIBYTE(vk) & 1) != 0); } }
    void wait(DWORD ms = 400) { const auto end = GetTickCount64() + ms; do { messages(); context->pump(); Sleep(5); } while (GetTickCount64() < end); }
    void complete() {
        const auto end = GetTickCount64() + 5000;
        while (context->queued.empty() && GetTickCount64() < end) { messages(); Sleep(5); }
        check(!context->queued.empty(), "missing container inference result"); context->pump();
    }
};
int startup_mode_test(HANDLE gate = nullptr, HANDLE broker_created = nullptr, bool early_input = false, bool fail_choice = false) {
    const auto started = GetTickCount64();
    child_trace("child_started", {{"action", early_input ? "early_input" : "manual_on"}});
    check(in_app_container(), "startup child is not AppContainer");
    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "startup COM initialization");
    ComPtr<test_host::ModeManager> host; host.Attach(new test_host::ModeManager);
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    host->open->value = 1; host->conversion->value = TF_CONVERSIONMODE_NATIVE;
    ComPtr<ITfTextInputProcessorEx> service;
    check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "startup create service");
    check(SUCCEEDED(service->ActivateEx(host.Get(), 1, TF_TMF_IMMERSIVEMODE)), "startup activate");
    ComPtr<ITfKeyEventSink> keys; service.As(&keys);
    BYTE state[256]{}; SetKeyboardState(state);
    BOOL eaten = FALSE, again = FALSE;
    if (gate) {
        check(!container_configuration(), "startup settings arrived before gate");
        const auto code = early_input ? static_cast<WPARAM>('K') : static_cast<WPARAM>(VK_IME_ON);
        keys->OnTestKeyDown(context.Get(), code, 0, &eaten);
        check(eaten && host->open->value == 1 && context->doc->text.empty(), "early startup preflight changed mode/text");
        keys->OnKeyDown(context.Get(), code, 0, &eaten);
        check(eaten && host->open->value == 1, "early startup input failed");
        if (fail_choice) { child_trace("injected_choice_failure"); throw std::runtime_error("injected startup child failure"); }
        check(SetEvent(gate) != FALSE, "signal startup gate");
        child_trace("choice_completed", {{"elapsed_ms", GetTickCount64() - started}});
        check(broker_created != nullptr, "missing broker notification handle");
        const auto waited = WaitForSingleObject(broker_created, startup_control_ms);
        const auto error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        child_trace("broker_notification", {{"wait_result", waited}, {"win32_error", error},
            {"elapsed_ms", GetTickCount64() - started}});
        check(waited == WAIT_OBJECT_0, "broker creation was not notified");
    }
    const auto waiting = GetTickCount64();
    child_trace("dictionary_wait_started", {{"timeout_ms", dictionary_ready_ms}});
    try { wait_for_dictionary(runtime_dictionary_ready, dictionary_ready_ms); }
    catch (const std::exception& error) {
        child_trace("dictionary_wait_failed", {{"error", error.what()}, {"elapsed_ms", GetTickCount64() - waiting},
            {"ready", runtime_dictionary_ready()}});
        // A bounded, separately labelled probe exposes a transport failure even
        // when the background loader retries it internally. Never log payloads.
        NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        try {
            (void)container_exchange("INI3", GetTickCount64() + 1500, cancel.get());
            child_trace("bootstrap_probe", {{"result", "responded"}});
        } catch (const ContainerFailure& probe) {
            child_trace("bootstrap_probe", {{"error", probe.what()}, {"reason", reason_name(probe.reason)}, {"win32_error", probe.error}});
        } catch (const std::exception& probe) { child_trace("bootstrap_probe", {{"error", probe.what()}}); }
        throw;
    }
    child_trace("dictionary_ready", {{"elapsed_ms", GetTickCount64() - waiting}});
    check(container_configuration()->configuration.settings.startup_ime_off, "broker startup setting missing");
    if (gate) {
        keys->OnSetFocus(TRUE);
        check(host->open->value == 1 && context->doc->text == (early_input ? L"k" : L""),
            "late startup settings overwrote manual mode/input");
        child_trace("state_preserved");
        service->Deactivate(); context->pump(); return 0;
    }
    const auto before = host->open->value;
    keys->OnTestKeyDown(context.Get(), 'A', 0, &eaten);
    keys->OnTestKeyDown(context.Get(), 'A', 0, &again);
    check(host->open->value == before && eaten == again && context->doc->text.empty(), "startup preflight mutated mode/text");
    keys->OnKeyDown(context.Get(), 'A', 0, &eaten);
    check(!eaten && host->open->value == 0 && host->conversion->value == 0 && context->doc->text.empty(),
        "broker startup OFF did not pass through first key");
    keys->OnKeyDown(context.Get(), VK_IME_ON, 0, &eaten);
    check(eaten && host->open->value == 1, "broker startup prevented manual ON");
    keys->OnSetFocus(FALSE); keys->OnSetFocus(TRUE);
    check(host->open->value == 1, "broker focus reapplied startup OFF");
    service->Deactivate();
    check(SUCCEEDED(service->ActivateEx(host.Get(), 1, TF_TMF_IMMERSIVEMODE)), "startup reactivate");
    wait_for_dictionary(runtime_dictionary_ready, dictionary_ready_ms); keys->OnSetFocus(TRUE);
    check(host->open->value == 1, "broker reactivation reapplied startup OFF");
    service->Deactivate(); context->pump();
    return 0;
}
int container_test(bool denied, bool real_model) {
    check(in_app_container(), "child is not AppContainer");
    NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (denied) {
        bool rejected = false; try { container_exchange("INI3", GetTickCount64() + 1500, cancel.get()); } catch (...) { rejected = true; }
        check(rejected, "unauthorized endpoint accepted"); return 0;
    }
    const auto path = environment_value(L"NSKKZIME_CONFIG"); check(path.has_value(), "missing fixture path");
    NativeHandle file(CreateFileW(path->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    check(!file.valid(), "container unexpectedly reads private settings");
    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization");
    {
        // Exercise the actual popup in the restricted process, beyond the
        // fake TSF host which deliberately suppresses its own candidate UI.
        Engine engine(demo_dictionary());
        for (char ch : std::string_view("Kanji")) engine.handle({KeyCode::Character, ch});
        engine.handle({KeyCode::Space});
        ComPtr<CandidateList> popup; popup.Attach(new CandidateList);
        popup->update(engine, nullptr);
        RECT anchor{100, 100, 110, 120}; popup->locate(nullptr, &anchor);
        popup->Show(TRUE); messages();
        BOOL visible = FALSE; popup->IsShown(&visible);
        check(visible != FALSE, "AppContainer candidate popup not visible");
        popup->Show(FALSE); popup->IsShown(&visible);
        check(visible == FALSE, "AppContainer candidate popup not hidden");
        popup->close();
    }
    {
        Session session; session.key(VK_IME_OFF);
        BYTE state[256]{}; state[VK_CONTROL] = state[VK_LCONTROL] = 0x80; SetKeyboardState(state);
        for (const auto code : {'Q', 'J'}) {
            BOOL eaten = TRUE; session.keys->OnTestKeyDown(session.context.Get(), code, 0, &eaten);
            check(!eaten, "container OFF key exclusion not applied");
            session.keys->OnKeyDown(session.context.Get(), code, 0, &eaten);
            check(!eaten, "container OFF key captured after preflight");
            session.keys->OnTestKeyUp(session.context.Get(), code, 0, &eaten);
            check(!eaten, "excluded container key-up captured");
        }
        session.key(VK_IME_ON); session.type("a");
        check(session.context->doc->text == L"あ", "container exclusion disabled explicit IME ON");
    }
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
        // Reactivation only confirms the settings; another writer's save then
        // arrives through the broker's wait, without polling.
        std::shared_ptr<const ContainerConfiguration> first;
        { Session session; first = container_configuration(); }
        Session session; const auto config = container_configuration();
        check(config == first, "reactivation copied the dictionary again");
        LearningRequest request{learning_client_id(), 1, {{U"ちょうき", 0}, {}, {}}, DictionaryAction::Learn};
        request.event.candidate.text = U"長期";
        check(container_operation(config->generation, "LRN1", encode_learning(request), GetTickCount64() + 1500, cancel.get()) == "OKAY",
            "external container write");
        const auto dictionary = container_runtime_dictionary(); const auto end = GetTickCount64() + 3000; bool seen = false;
        while (!seen && GetTickCount64() < end) { seen = !dictionary->lookup({U"ちょうき", 0}).empty(); Sleep(10); }
        check(seen, "AppContainer missed another writer's save");
    }
    {
        Session session; session.type("/99"); session.key(VK_SPACE); session.key(VK_SPACE); session.key(VK_SPACE);
        check(session.context->doc->text == L"九九" && session.host->ui, "broker numeric candidates did not expand");
        session.type("d"); session.wait(500);
        check(container_runtime_dictionary()->lookup({U"#"})[0].text == U"#2", "broker saved expanded numeric spelling");
    }
    {
        Session session; session.type("/98"); session.key(VK_SPACE);
        check(session.context->doc->text == L"九八", "broker numeric preference did not transfer");
        session.type("Xyes"); session.key(VK_RETURN); session.key(VK_SPACE);
        check(session.context->doc->text == L"９８", "broker numeric deletion retained learned format");
        session.key(VK_ESCAPE); session.key(VK_ESCAPE); session.wait(500);
    }
    {
        Session session; session.type("Kaitou"); session.key(VK_SPACE);
        check(session.context->doc->text == L"解答", "broker rerank before display"); session.key(VK_RETURN);
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
        Session session;
        const auto config = container_configuration();
        check(config != nullptr, "security checks require a live bootstrap");
        bool rejected = false;
        try { container_exchange("INF1" + std::string(64, '0') + "{}", GetTickCount64() + 1500, cancel.get()); } catch (...) { rejected = true; }
        check(rejected, "stale settings accepted");
        rejected = false;
        try { container_operation(config->generation, "FILE", "C:\\private.txt", GetTickCount64() + 1500, cancel.get()); } catch (...) { rejected = true; }
        check(rejected, "arbitrary file operation accepted");
        rejected = false;
        try { container_operation(config->generation, "GUI1", "settings", GetTickCount64() + 1500, cancel.get()); } catch (...) { rejected = true; }
        check(rejected, "AppContainer launched desktop GUI");
    }
    CoUninitialize(); return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    std::filesystem::path fixture;
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--startup") return startup_mode_test();
        if (argc == 4 && (std::wstring_view(argv[1]) == L"--startup-early-on" || std::wstring_view(argv[1]) == L"--startup-early-input" ||
            std::wstring_view(argv[1]) == L"--startup-early-fail"))
            return startup_mode_test(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(argv[2]))),
                reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(argv[3]))),
                std::wstring_view(argv[1]) == L"--startup-early-input", std::wstring_view(argv[1]) == L"--startup-early-fail");
        if (argc == 2 && std::wstring_view(argv[1]) == L"--updated") {
            const auto dictionary = container_runtime_dictionary();
            wait_for_dictionary([&] { return dictionary->ready(); }, dictionary_ready_ms);
            const auto words = dictionary->lookup({U"かんじ", 0});
            check(!words.empty() && words.front().text == U"新字", "broker retained same-size/time dictionary");
            const auto config = container_configuration();
            check(config && config->romaji->find("wi")->output == U"ヰ", "broker retained same-time rules");
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--degraded") {
            const auto dictionary = container_runtime_dictionary();
            wait_for_dictionary([&] { return dictionary->ready(); }, dictionary_ready_ms);
            check(!dictionary->lookup({U"かんじ", 0}).empty(), "degraded broker lost valid dictionary");
            check(!dictionary->registration_allowed(), "degraded broker enabled learning");
            const auto config = container_configuration();
            check(config && config->romaji->find("wi")->output == U"うぃ", "degraded broker lost built-in rules");
            const auto runtime = std::dynamic_pointer_cast<const RuntimeDictionary>(dictionary);
            check(runtime->status().find(L"読み込み失敗") != std::wstring::npos &&
                runtime->status().find(L"内蔵ルールで継続") != std::wstring::npos, "degraded broker lost diagnostics");
            NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            bool rejected = false;
            try { (void)container_operation(config->generation, "LRN1", snapshot_request(0), GetTickCount64() + 1500, cancel.get()); }
            catch (...) { rejected = true; }
            check(rejected, "broker accepted learning from a degraded snapshot");
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--desktop") {
            check(!in_app_container(), "desktop client unexpectedly restricted");
            auto dictionary = container_runtime_dictionary();
            wait_for_dictionary([&] { return dictionary->ready(); }, dictionary_ready_ms);
            const auto config = container_configuration();
            check(config && config->configuration.settings.candidates.show_after == 3 &&
                config->configuration.settings.candidates.keys == CandidateKeys::Asdfjkl, "desktop broker settings");
            check(off_keys_excluded(config->configuration.settings, L"APP_CONTAINER_TESTS.EXE"), "desktop broker OFF key exclusions");
            check(!dictionary->lookup({U"かんじ", 0}).empty(), "desktop broker dictionary");
            // A released snapshot is reused until its settings change.
            const auto reactivate = [&] {
                dictionary.reset(); dictionary = container_runtime_dictionary();
                wait_for_dictionary([&] { return dictionary->ready(); }, dictionary_ready_ms);
                return container_configuration();
            };
            check(reactivate() == config, "desktop broker reactivation copied the dictionary again");
            auto changed = config->configuration.settings; changed.candidates.show_after = 4;
            save_dictionary_settings(*environment_value(L"NSKKZIME_CONFIG"), changed); // The parent restores it.
            const auto updated = reactivate();
            check(updated != config && updated->configuration.settings.candidates.show_after == 4, "desktop broker reused stale settings");
            return 0;
        }
        if (argc == 2) return container_test(std::wstring_view(argv[1]) == L"--denied", std::wstring_view(argv[1]) == L"--real-client");
        const bool real_model = argc == 3 && std::wstring_view(argv[1]) == L"--model";
        const bool startup_only = argc == 3 && std::wstring_view(argv[1]) == L"--startup-scenario";
        const std::wstring_view scenario = startup_only ? argv[2] : L"";
        check(argc == 1 || real_model || startup_only, "invalid test arguments");
        check(!startup_only || scenario == L"delayed-broker" || scenario == L"child-failure" ||
            scenario == L"broker-failure" || scenario == L"readiness-timeout", "invalid startup scenario");
        if (real_model && !std::filesystem::exists(argv[2])) return 77;
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        const auto id = learning_client_id(); const std::wstring token(id.begin(), id.end());
        Profile allowed(L"nskkzime-test-" + token), denied(L"nskkzime-denied-" + token);
        FileGrant allow_binary(executable, allowed.sid), deny_binary(executable, denied.sid);
        const auto root = std::filesystem::path(executable).parent_path() / ("container-test-" + id); std::filesystem::create_directory(root);
        fixture = root;
        const auto config = root / L"settings.ini", dict = root / L"fixture.skk", user = root / L"user-dictionary.nskkdb";
        { std::ofstream out(dict, std::ios::binary); out << "# /#1/#3/#2/\nかんじ /漢字/感じ/幹事/\nかいとう /回答/解答/解凍/\n"; }
        DictionarySettings settings{{dict}, true}; settings.candidates = {3, CandidateKeys::Asdfjkl};
        settings.off_key_exclusions = {L"app_container_tests.exe"};
        settings.zenz.enabled = true; settings.zenz.model = root / L"fixture.gguf";
        settings.zenz.server = std::filesystem::path(executable).parent_path() / L"nkkz-ime-server.exe";
        settings.zenz.timeout_ms = 3000; settings.zenz.policy.rerank = true; settings.zenz.policy.score_timeout_ms = 3000;
        // The fixture scores in 5 ms: wait for it through the broker. The real
        // model may need a cold start, so that case keeps the asynchronous path.
        settings.zenz.policy.rerank_wait_ms = real_model ? 0 : 1000;
        settings.zenz.idle_seconds = 10; // Broker-launched servers must not outlive the test for long.
        settings.zenz.log_input = true; settings.zenz.log_directory = root / L"logs";
        std::filesystem::create_directory(settings.zenz.log_directory);
        if (real_model) settings.zenz.model = argv[2];
        save_dictionary_settings(config, settings);
        // Children inherit this file; its broker endpoint and inference endpoint are private to the test.
        SetEnvironmentVariableW(L"NSKKZIME_CONFIG", config.c_str());
        const auto inference_token = current_inference_config()->token;
        std::atomic<bool> private_seen = false;
        std::future<int> inference;
        if (!real_model && !startup_only) inference = std::async(std::launch::async, [&] { return serve_inference(inference_token, [&](const InferenceRequest& request, const auto& cancelled) {
            if (request.reading == U"ひみつ") private_seen = true;
            // Scores are waited for before display and must still fit the per-key bound; generation stays asynchronous.
            const auto end = GetTickCount64() + (request.operation == InferenceOperation::Score ? 5 : 160);
            while (!cancelled() && GetTickCount64() < end) Sleep(5);
            InferenceResponse response; response.id = request.id; response.operation = request.operation;
            if (request.operation == InferenceOperation::Score) {
                // Only the rerank case promotes; other readings keep their dictionary order.
                response.scores.assign(request.candidates.size(), -5);
                if (response.scores.size() > 1 && request.reading == U"かいとう") response.scores[1] = 0;
            }
            else response.candidates = {U"蜜柑", U"みかん"}; return response;
        }, 10000, 5000); });
        const auto instance = config_broker_instance(config); const auto stop = container_stop_event(instance);
        // Withhold the broker until both children have made their first choice.
        // This makes late-settings protection independent of bootstrap timing.
        auto early_settings = settings; early_settings.startup_ime_off = true;
        save_dictionary_settings(config, early_settings);
        const auto broker = start_early_children(executable, allowed, denied, root, config, stop.get(), scenario);
        if (startup_only) {
            SetEvent(stop.get());
            const auto stopped = WaitForSingleObject(broker.get(), 35000);
            if (stopped != WAIT_OBJECT_0) stop_owned_child(broker);
            check(stopped == WAIT_OBJECT_0, "startup-only broker did not stop");
            std::filesystem::remove_all(root);
            std::cout << "Delayed broker startup preserved both manual choices\n";
            return 0;
        }
        int result = 1;
        try {
            save_dictionary_settings(config, settings);
            auto desktop = launch(executable, L"--desktop"); check(wait_child(desktop.get()) == 0, "desktop client failed");
            save_dictionary_settings(config, settings);
            auto client = launch(executable, real_model ? L"--real-client" : L"--client", allowed.sid); const auto code = wait_child(client.get());
            std::cout << "allowed child=" << code << std::endl; check(code == 0, "container client failed");
            check(!private_seen.load(), "broker received protected text");
            if (!real_model) {
                // The fixture answers generation after 160 ms; this client stops waiting first.
                NativeHandle abandon(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                const auto generation = decode_broker_bootstrap(container_exchange("INI3", GetTickCount64() + 5000, abandon.get())).generation;
                bool abandoned = false;
                try { (void)container_operation(generation, "INF1", encode_request({1, U"とりけし", U"", U"", 1}), GetTickCount64() + 40, abandon.get()); }
                catch (...) { abandoned = true; }
                check(abandoned, "abandoned inference was answered");
                Sleep(300); // The broker notices the closed client and ends the request.
                const auto learned = read_user_dictionary(user).lookup({U"かんじ", 0});
                check(!learned.empty() && learned.front().text == U"感じ", "broker learning did not persist");
            }
            bool logged = false; for (const auto& item : std::filesystem::directory_iterator(root / L"logs")) if (item.path().extension() == L".jsonl") logged = true;
            check(logged, "broker input log missing");
            auto startup_settings = settings; startup_settings.startup_ime_off = true;
            save_dictionary_settings(config, startup_settings);
            NativeHandle startup_output(CreateFileW((root / L"startup-later.log").c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            check(startup_output.valid(), "create later startup diagnostic file");
            auto startup_client = launch(executable, L"--startup", allowed.sid, nullptr, nullptr, startup_output.get());
            check(wait_child(startup_client.get()) == 0, "AppContainer startup mode failed");
            save_dictionary_settings(config, settings);
            auto degraded = settings;
            degraded.dictionaries.push_back(root / L"missing.skk");
            degraded.romaji_rules = root / L"missing-romaji.tsv";
            save_dictionary_settings(config, degraded);
            auto partial_desktop = launch(executable, L"--degraded");
            check(wait_child(partial_desktop.get()) == 0, "degraded desktop broker client failed");
            auto partial_container = launch(executable, L"--degraded", allowed.sid);
            check(wait_child(partial_container.get()) == 0, "degraded AppContainer client failed");
            save_dictionary_settings(config, settings);
            auto updated = settings; updated.learning = false;
            updated.romaji_rules = root / L"romaji.tsv";
            { std::ofstream out(updated.romaji_rules, std::ios::binary); out << "version=1\nwi\tゐ\n"; }
            save_dictionary_settings(config, updated);
            NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            (void)container_exchange("INI3", GetTickCount64() + 5000, cancel.get());
            const auto dictionary_time = std::filesystem::last_write_time(dict);
            const auto dictionary_size = std::filesystem::file_size(dict);
            { std::ofstream out(dict, std::ios::binary); out << "# /#1/#3/#2/\nかんじ /新字/感じ/幹事/\nかいとう /回答/解答/解凍/\n"; }
            std::filesystem::last_write_time(dict, dictionary_time);
            const auto romaji_time = std::filesystem::last_write_time(updated.romaji_rules);
            { std::ofstream out(updated.romaji_rules, std::ios::binary); out << "version=1\nwi\tヰ\n"; }
            std::filesystem::last_write_time(updated.romaji_rules, romaji_time);
            check(std::filesystem::file_size(dict) == dictionary_size, "fixture size changed");
            auto refreshed = launch(executable, L"--updated");
            check(wait_child(refreshed.get()) == 0, "updated broker client failed");
            save_dictionary_settings(config, settings);
            auto blocked = launch(executable, L"--denied", denied.sid); check(wait_child(blocked.get()) == 0, "unlisted container connected");
            result = 0;
        } catch (...) { SetEvent(stop.get()); WaitForSingleObject(broker.get(), 35000); throw; }
        SetEvent(stop.get()); check(WaitForSingleObject(broker.get(), 35000) == WAIT_OBJECT_0, "broker did not stop");
        {
            std::ifstream log(root / L"diagnostics" / L"broker.jsonl", std::ios::binary); std::string line;
            bool created = false, probe = false, received = false, acked = false, failed = false, degraded = false;
            while (std::getline(log, line)) {
                const auto event = nlohmann::json::parse(line); const auto kind = event.at("event").get<std::string>();
                created |= kind == "listener_created";
                probe |= kind == "endpoint_state" && event.at("listeners_expected") == 4;
                received |= kind == "bootstrap_received" && event.at("operation") == "INI3";
                acked |= kind == "bootstrap_response_acked" && event.at("operation") == "INI3";
                failed |= kind == "request_failed" && event.at("phase") == "handle";
                degraded |= kind == "snapshot_ready" && event.at("failed_sources") == 1;
                check(!event.contains("request") && !event.contains("response"), "diagnostics included input payload");
                check(!(kind == "request_failed" && event.at("operation") == "LRN1/WAT1"), "closed learning wait logged as a failure");
                check(!(kind == "request_failed" && event.at("operation") == "INF1" && event.at("client_pid") == GetCurrentProcessId()),
                    "abandoned inference logged as a failure");
            }
            check(created && probe && received && acked && failed && degraded, "broker diagnostic stages missing");
        }
        // A broken log destination must not prevent real broker bootstrap.
        std::filesystem::rename(root / L"diagnostics", root / L"saved-diagnostics");
        { std::ofstream log(root / L"diagnostics"); log << "unwritable directory fixture"; }
        ResetEvent(stop.get());
        const auto no_log_broker = launch(container_broker_path(), L"--config \"" + config.native() + L"\" --test-package " + allowed.name);
        try {
            auto desktop = launch(executable, L"--desktop"); check(wait_child(desktop.get()) == 0, "logging failure prevented bootstrap");
        } catch (...) { SetEvent(stop.get()); WaitForSingleObject(no_log_broker.get(), 35000); throw; }
        SetEvent(stop.get()); check(WaitForSingleObject(no_log_broker.get(), 35000) == WAIT_OBJECT_0, "logging failure prevented stop");
        ResetEvent(stop.get()); std::atomic<bool> spoof_received = false;
        auto spoof = std::async(std::launch::async, [&] { serve_container({allowed.name}, [&](auto, const auto&) { spoof_received = true; return std::string("wrong server"); }, stop.get(), instance); });
        auto client = launch(executable, L"--denied", allowed.sid); const auto code = wait_child(client.get()); SetEvent(stop.get()); spoof.get();
        check(code == 0 && !spoof_received.load(), "client sent data to another executable");
        if (inference.valid()) inference.get();
        if (real_model) {
            // This randomly named endpoint belongs only to this fixture.
            NativeHandle pipe(CreateFileW(inference_pipe_name(inference_token).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
            ULONG pid = 0; check(pipe.valid() && GetNamedPipeServerProcessId(pipe.get(), &pid), "fixture server identity");
            NativeHandle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
            check(process.valid() && TerminateProcess(process.get(), 0), "stop fixture model"); WaitForSingleObject(process.get(), 5000);
        }
        // Retain fixtures only when a check fails.
        std::filesystem::remove_all(root);
        std::cout << "AppContainer broker, TSF, learning, inference, cancellation and peer checks passed\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!fixture.empty()) {
            std::cerr << "AppContainer diagnostics: " << utf8_message(fixture.native()) << '\n';
            for (const auto* name : {L"startup.jsonl", L"startup-on.log", L"startup-input.log", L"startup-later.log"}) {
                std::ifstream log(fixture / name, std::ios::binary);
                std::string line; std::size_t bytes = 0;
                while (bytes < 32768 && std::getline(log, line)) { std::cerr << line << '\n'; bytes += line.size() + 1; }
            }
        }
        return 1;
    }
}
