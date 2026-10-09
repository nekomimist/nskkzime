#include "tsf_diagnostics.h"
#include "tsf_edit.h"
#include "diagnostic_file.h"
#include "learning_ipc.h"
#include "module.h"
#include "runtime_dictionary.h"
#include "dictionary_wait.h"
#include "isolated_settings.h"
#include "tsf_test_host.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

namespace nskkzime::windows {
HINSTANCE module_instance = nullptr;
std::atomic<long> module_objects{0}, server_locks{0};
}
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void wait(F predicate) {
    const auto end = GetTickCount64() + 5000;
    while (!predicate()) { check(GetTickCount64() < end, "diagnostic worker did not complete"); Sleep(1); }
}
std::vector<nlohmann::json> records(const std::filesystem::path& directory) {
    std::vector<nlohmann::json> result;
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        const auto filename = item.path().filename().native();
        if (!filename.starts_with(L"nskkzime-tsf-") || !filename.ends_with(L".jsonl")) continue;
        std::ifstream stream(item.path()); std::string line;
        while (std::getline(stream, line)) {
            auto json = nlohmann::json::parse(line, nullptr, false);
            if (!json.is_discarded()) result.push_back(std::move(json));
        }
    }
    return result;
}
bool contains(const std::vector<nlohmann::json>& rows, std::string_view event, std::string_view reason = "none") {
    return std::any_of(rows.begin(), rows.end(), [&](const nlohmann::json& row) {
        return row.at("event").get_ref<const std::string&>() == event && row.at("reason").get_ref<const std::string&>() == reason;
    });
}
struct Session {
    ComPtr<ITfTextInputProcessorEx> service;
    ComPtr<ITfKeyEventSink> keys;
    ComPtr<test_host::Manager> host;
    ComPtr<test_host::Context> context;
    Session(const std::filesystem::path& root, bool enabled) {
        std::filesystem::create_directories(root);
        use_isolated_settings(root, "learning=off\ntsf-log=" + std::string(enabled ? "on" : "off") +
            "\ntsf-log-directory=" + settings_path(root) + "\n");
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "create service"); service.As(&keys);
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, 0)), "activate service");
        wait_for_dictionary([] { return runtime_dictionary_ready(); });
    }
    ~Session() { service->Deactivate(); context->pump(); }
    bool key(WPARAM code, bool shift = false) {
        BYTE keyboard[256]{}; if (shift) keyboard[VK_SHIFT] = keyboard[VK_LSHIFT] = 0x80; SetKeyboardState(keyboard);
        const auto parameter = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
        BOOL eaten = FALSE; keys->OnTestKeyDown(context.Get(), code, parameter, &eaten);
        if (eaten) keys->OnKeyDown(context.Get(), code, parameter, &eaten);
        BOOL up = FALSE; keys->OnKeyUp(context.Get(), code, parameter, &up);
        check(eaten == up, "key-up changed");
        // These cases inspect every lifecycle reason. Yield between keys so
        // the dedicated overflow case below is the only deliberate queue flood.
        Sleep(1); return eaten != FALSE;
    }
};
struct Outcome { std::wstring text; unsigned reads, writes, selections, selection_writes, layouts; bool operator==(const Outcome&) const = default; };
Outcome scenario(const std::filesystem::path& root, bool enabled) {
    Session s(root, enabled);
    check(s.key('K', true) && s.key('U') && s.key(VK_RETURN), "normal romaji failed");
    check(s.context->doc->text == L"く", "normal romaji changed");
    check(s.key('H', true), "pending key failed");
    s.context->doc->reject_read = true; s.context->notify();
    s.context->doc->reject_read = false; s.context->pump();
    check(s.key('U') && s.context->doc->text == L"くhう", "existing reset behavior changed");
    check(s.key('J', true), "termination key failed"); s.context->composition->terminate();
    check(s.key('A') && s.context->doc->text == L"くhうjあ", "host termination behavior changed");
    check(s.key('K', true), "focus key failed"); s.keys->OnSetFocus(FALSE); s.context->pump();
    check(s.key('K', true), "navigation key failed"); check(!s.key(VK_LEFT), "navigation consumed"); s.context->pump();
    s.context->reject_session = true; check(!s.key('K', true), "refused session consumed"); s.context->reject_session = false;
    s.context->doc->move_selection_on_write = false; s.context->reject_selection_write = true;
    check(s.key('K', true), "successful write was replayed on caret failure");
    s.context->reject_selection_write = false; s.context->doc->move_selection_on_write = true;
    const auto tail = static_cast<LONG>(s.context->doc->text.size()); s.context->move_selection(tail, tail);
    for (const bool restore : {true, false}) {
        const auto before = s.context->doc->text;
        check(s.key('H', true), "transient focus key failed");
        s.host->events->OnSetFocus(nullptr, s.context->document_manager.Get());
        if (restore) s.host->events->OnSetFocus(s.context->document_manager.Get(), nullptr);
        else check(s.key('U'), "next key before deferred check failed");
        s.context->pump(); if (restore) check(s.key('U'), "restored input failed");
        check(s.context->doc->text == before + L"ふ", "transient focus changed kana");
        check(s.key(VK_RETURN), "restored composition did not commit");
    }
    check(s.key('H', true), "persistent focus key failed");
    s.host->events->OnSetFocus(nullptr, s.context->document_manager.Get()); s.context->pump(); s.context->pump();
    check(s.context->composition->ended && !s.context->edits, "persistent focus did not end");
    check(s.key('H', true), "refused check key failed");
    const auto old = s.context->composition;
    s.context->reject_session = true; s.host->events->OnSetFocus(nullptr, s.context->document_manager.Get());
    s.context->reject_session = false; old->terminate();
    return {s.context->doc->text, s.context->doc->reads, s.context->doc->writes, s.context->selection_reads, s.context->selection_writes, s.context->layout_queries};
}
void checks_and_scopes(const std::filesystem::path& root) {
    auto log = TsfDiagnosticLog::create({true, root}, false); check(log != nullptr, "log unavailable");
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    context->doc->text = L"private text"; context->doc->caret = 12;
    ComPtr<test_host::Range> range; range.Attach(new test_host::Range(context->doc, 0, 12));
    {
        TsfDiagnosticScope callback(log, TsfEvent::KeyDown, {});
        { TsfDiagnosticScope disabled(nullptr, TsfEvent::EndEdit, {}); trace_tsf(TsfEvent::EngineReset, TsfReason::Exception); }
        check(!range_matches(range.Get(), 1, L"different secret"), "mismatch accepted");
        context->reject_selection_read = true; check(!selection_at_end(context.Get(), 1, range.Get()), "unavailable selection accepted"); context->reject_selection_read = false;
        context->interim_selection = true; check(!selection_at_end(context.Get(), 1, range.Get()), "interim selection accepted"); context->interim_selection = false;
        context->doc->caret = 0; context->doc->selection_end = 1;
        check(!selection_at_end(context.Get(), 1, range.Get()), "noncollapsed selection accepted");
        context->doc->selection_end = -1;
        check(!selection_at_end(context.Get(), 1, range.Get()), "wrong caret accepted");
        context->doc->read_query = [&] {
            context->doc->read_query = nullptr;
            TsfDiagnosticScope notification(log, TsfEvent::EndEdit, {});
        };
        check(range_matches(range.Get(), 1, L"private text"), "nested callback changed read");
        check(SUCCEEDED(request_edit(context.Get(), 1, TF_ES_ASYNC | TF_ES_READWRITE, [](TfEditCookie) { return S_OK; })), "async request failed");
        for (const bool allocation : {false, true}) {
            const auto fail = [allocation](TfEditCookie) -> HRESULT {
                if (allocation) throw std::bad_alloc();
                throw std::runtime_error("private exception");
            };
            check(request_edit(context.Get(), 1, TF_ES_SYNC | TF_ES_READWRITE, fail) ==
                (allocation ? E_OUTOFMEMORY : E_FAIL), "sync exception result changed");
            check(SUCCEEDED(request_edit(context.Get(), 1, TF_ES_ASYNC | TF_ES_READWRITE, fail)), "async exception request failed");
        }
        {
            TsfDiagnosticScope disabled(nullptr, TsfEvent::KeyDown, {});
            check(SUCCEEDED(request_edit(context.Get(), 1, TF_ES_ASYNC | TF_ES_READWRITE, [](TfEditCookie) {
                trace_tsf(TsfEvent::EngineReset, TsfReason::Exception); return S_OK;
            })), "disabled async request failed");
        }
    }
    context->pump(); // The queued callbacks have request IDs, but no active parent callback.
    // An unlogged deferred callback must also mask an unrelated active logger.
    check(SUCCEEDED(request_edit(context.Get(), 1, TF_ES_ASYNC | TF_ES_READWRITE, [](TfEditCookie) {
        trace_tsf(TsfEvent::EngineReset, TsfReason::Exception); return S_OK;
    })), "unlogged deferred request failed");
    {
        TsfDiagnosticScope other(log, TsfEvent::KeyDown, {});
        context->pump();
        trace_tsf(TsfEvent::StateAccepted);
    }
    wait([&] { return contains(records(root), "state_accepted"); });
    const auto rows = records(root);
    check(!contains(rows, "engine_reset", "exception"), "disabled callback inherited another service's log");
    for (const auto reason : {"text_mismatch", "selection_unavailable", "interim_selection", "selection_not_collapsed", "caret_mismatch"})
        check(contains(rows, std::string_view(reason) == "text_mismatch" ? "range_check" : "selection_check", reason), "missing validity reason");
    check(std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.at("event") == "end_edit" && row.at("depth") == 2; }), "nested depth missing");
    check(std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.at("event") == "edit_callback" && row.at("request") != 0 && row.at("depth") == 1 && row.at("parent_call") == 0; }), "async request correlation missing");
    for (const auto status : {"0x80004005", "0x8007000E"}) {
        for (const unsigned depth : {1u, 2u}) {
            check(std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
                return row.at("event") == "edit_callback" && row.at("phase") == "exit" &&
                    row.at("reason") == "exception" && row.at("hr") == status &&
                    row.at("result_known") == true && row.at("depth") == depth;
            }), "exception result missing from callback exit");
        }
    }
    check(std::any_of(rows.begin(), rows.end(), [](const auto& row) {
        return row.at("event") == "edit_callback" && row.at("state_source") == "request";
    }), "request-time snapshot not labeled");
    for (const auto& row : rows) {
        const auto encoded = row.dump();
        check(encoded.find("private text") == std::string::npos && encoded.find("different secret") == std::string::npos, "input text leaked");
        check(row.at("format") == "nskkzime-tsf-diagnostics" && row.contains("seq") && row.contains("hr") && row.contains("app"), "missing schema");
    }
}
void shutdown_drain(const std::filesystem::path& root) {
    auto log = TsfDiagnosticLog::create({true, root}, false);
    check(log != nullptr, "shutdown log unavailable");
    {
        TsfDiagnosticScope scope(log, TsfEvent::Deactivate, {});
        for (unsigned i = 0; i < 100; ++i) trace_tsf(TsfEvent::EngineReset, TsfReason::Deactivated, S_OK, i);
    }
    log.reset(); // No joining or waiting on the TSF thread; the worker owns its drain.
    wait([&] { return records(root).size() == 102; });
    const auto rows = records(root);
    check(rows.back().at("event") == "deactivate" && rows.back().at("phase") == "exit", "final callback record lost on shutdown");
}
void limits(const std::filesystem::path& root) {
    check(!TsfDiagnosticLog::create({}, false), "default enabled log");
    auto unsupported = TsfDiagnosticLog::create({true, root}, true);
    check(unsupported && unsupported->status() == TsfLogStatus::Unsupported && std::filesystem::is_empty(root), "AppContainer writer started");
    unsupported.reset();
    const auto full = root / L"full"; std::filesystem::create_directory(full);
    for (std::size_t i = 0; i < DiagnosticFile::directory_files; ++i) std::ofstream(full / ("nskkzime-tsf-fixture-" + std::to_string(i) + ".jsonl")) << "preserve";
    auto log = TsfDiagnosticLog::create({true, full}, false); log->record({});
    wait([&] { return log->status() == TsfLogStatus::Full; }); log.reset();
    auto missing = TsfDiagnosticLog::create({true, root / L"missing"}, false); missing->record({});
    wait([&] { return missing->status() == TsfLogStatus::Unavailable; }); missing.reset();
    const auto locked = root / L"locked"; std::filesystem::create_directory(locked);
    NativeHandle lock(CreateFileW((locked / L"nskkzime-tsf-log.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(lock.valid(), "lock fixture");
    auto busy = TsfDiagnosticLog::create({true, locked}, false);
    for (unsigned i = 0; i < 10000; ++i) busy->record({});
    check(busy->dropped() > 0, "queue did not report overflow");
    wait([&] { return busy->status() == TsfLogStatus::Unavailable; });
}
}
int main() {
    BYTE keyboard[256]{}; GetKeyboardState(keyboard); CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    module_instance = GetModuleHandleW(nullptr);
    const auto root = std::filesystem::current_path() / ("tsf-diagnostics-" + learning_client_id());
    std::filesystem::create_directory(root); int result = 1;
    try {
        const auto quiet = scenario(root / L"off", false);
        const auto logged = scenario(root / L"on", true);
        check(quiet == logged && records(root / L"off").empty(), "logging changed host operations or input");
        wait([&] { return contains(records(root / L"on"), "deactivate"); });
        const auto rows = records(root / L"on");
        for (const auto reason : {"invalid_after_edit", "host_termination", "focus_lost", "navigation", "selection_write_failed", "focus_unavailable"})
            check(contains(rows, "engine_reset", reason), "missing reset reason");
        check(contains(rows, "range_check", "range_read_failed") && contains(rows, "edit_request", "edit_rejected"), "missing API failure");
        check(contains(rows, "focus_cancelled", "focus_restored") && contains(rows, "focus_cancelled", "focus_validated") &&
              contains(rows, "focus_deferred", "edit_rejected"), "missing deferred-focus diagnostics");
        check(std::any_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.at("event") == "focus_resolved" && row.at("reason") == "focus_unavailable" &&
                row.at("context") == 0 && row.at("owner_context") != 0;
        }), "unresolved focus was replaced by the owned context in diagnostics");
        for (const auto& row : rows) check(row.dump().find("く") == std::string::npos && !row.contains("key_code"), "typed content logged");
        const auto checks = root / L"checks"; std::filesystem::create_directory(checks); checks_and_scopes(checks);
        const auto shutdown = root / L"shutdown"; std::filesystem::create_directory(shutdown); shutdown_drain(shutdown);
        const auto caps = root / L"limits"; std::filesystem::create_directory(caps); limits(caps);
        std::cout << "PASS TSF logging neutrality, reset/API reasons, nesting, async correlation, privacy, opt-in, queue/file caps and unavailable status\n";
        result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    CoUninitialize(); SetKeyboardState(keyboard);
    // Workers drain asynchronously; preserve failed fixtures for diagnosis.
    if (!result && root.is_absolute() && root.parent_path() == std::filesystem::current_path() &&
        root.filename().native().starts_with(L"tsf-diagnostics-")) {
        std::error_code error;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            std::filesystem::remove_all(root, error); if (!error) break; Sleep(10); error.clear();
        }
    }
    return result;
}
