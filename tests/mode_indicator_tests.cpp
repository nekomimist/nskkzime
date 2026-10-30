#include "mode_indicator.h"
#include "tsf_test_host.h"
#include <iostream>
#include <stdexcept>

namespace nskkzime::windows {
HINSTANCE module_instance = GetModuleHandleW(nullptr);
std::atomic<long> module_objects{0};
std::atomic<long> server_locks{0};
}
using namespace nskkzime;
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void drain_events() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
bool shown(ITfUIElement* element) {
    BOOL value = FALSE;
    check(element->IsShown(&value) == S_OK, "shown state unavailable");
    return value != FALSE;
}
struct Fixture {
    ComPtr<test_host::Manager> host;
    ComPtr<test_host::Context> context;
    ComPtr<test_host::ContextView> view;
    ComPtr<ModeIndicator> indicator;
    Fixture() {
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        view.Attach(new test_host::ContextView); context->active_view = view;
        host->focused_document = context->document_manager;
    }
    ~Fixture() { if (indicator) indicator->close(); context->pump(); drain_events(); }
    void start(unsigned duration = 3000, Mode mode = Mode::Hiragana) {
        UiSettings settings; settings.show_input_mode = true; settings.font = L"Meiryo";
        settings.input_mode_duration_ms = duration;
        indicator.Attach(new ModeIndicator(mode, settings));
        indicator->start(host.Get(), context.Get(), 1);
    }
    HWND window() const {
        wchar_t name[96]{};
        swprintf_s(name, L"nskkzime.InputMode.%p", static_cast<void*>(indicator.Get()));
        return FindWindowW(name, nullptr);
    }
    void closed() {
        check(!host->ui && !context->edits && !context->layout && !window(), "closed indicator retained UI, sinks or window");
        check(host->ui_begins == host->ui_ends, "unbalanced UI lifetime");
    }
};
void ui_contract() {
    Fixture f;
    const auto focus = GetFocus(), active = GetActiveWindow();
    f.start();
    check(f.host->ui && f.host->ui_updates == 1 && !shown(f.indicator.Get()), "host-suppressed element missing or visible");
    GUID id{}; f.host->ui->GetGUID(&id);
    check(id == mode_indicator_ui_guid, "mode element identity incorrect");
    ComPtr<ITfCandidateListUIElement> candidates;
    check(f.host->ui.As(&candidates) == E_NOINTERFACE, "mode element exposed candidate selection");
    BSTR description = nullptr;
    check(f.host->ui->GetDescription(&description) == S_OK, "description missing");
    const std::wstring text(description, SysStringLen(description)); SysFreeString(description);
    check(text.find(L"ひらがな") != text.npos, "mode unavailable to UI host");
    f.context->pump();
    check(!shown(f.indicator.Get()), "async layout overrode host suppression");
    f.indicator->Show(TRUE);
    check(shown(f.indicator.Get()) && f.window(), "empty field did not show mode");
    check((GetClassLongPtrW(f.window(), GCL_STYLE) & CS_IME) &&
        (GetWindowLongPtrW(f.window(), GWL_EXSTYLE) & WS_EX_NOACTIVATE), "immersive/nonactivating styles missing");
    check(GetFocus() == focus && GetActiveWindow() == active, "indicator stole focus");
    check(f.context->doc->text.empty() && f.context->selection_writes == 0, "indicator changed document or selection");
    f.indicator->Show(FALSE); check(!shown(f.indicator.Get()), "host hide ignored");
    f.indicator->close(); f.closed();
    f.indicator->Show(TRUE); check(!shown(f.indicator.Get()) && !f.window(), "retained element reopened");
}
void selection_and_layout() {
    Fixture f;
    f.context->doc->text = L"abcdefgh";
    f.context->doc->caret = 2; f.context->doc->selection_end = 6;
    f.context->selection_active_end = TF_AE_START;
    LONG anchor = -1;
    f.view->text_query = [&](ITfRange* range) {
        const auto* selected = dynamic_cast<test_host::Range*>(range);
        check(selected && selected->start == selected->end, "queried whole selection instead of caret");
        anchor = selected->start; return S_OK;
    };
    f.host->show_ui = TRUE;
    f.start(); f.context->pump();
    check(anchor == 2 && shown(f.indicator.Get()), "reverse selection used wrong endpoint");
    f.context->selection_active_end = TF_AE_END;
    f.context->notify();
    check(anchor == 6, "selection change did not follow active endpoint");
    check(f.context->doc->caret == 2 && f.context->doc->selection_end == 6 && !f.context->selection_writes,
        "position query changed selection");
    f.view->text_status = TF_E_NOLAYOUT;
    f.view->text_query = {};
    f.context->layout->OnLayoutChange(f.context.Get(), TF_LC_CHANGE, f.view.Get()); f.context->pump();
    check(!shown(f.indicator.Get()), "unavailable layout reused old position");
    f.view->text_status = S_OK; f.view->clipped = TRUE;
    f.context->notify(); check(!shown(f.indicator.Get()), "clipped range shown");
    f.view->clipped = FALSE; f.context->notify(); check(shown(f.indicator.Get()), "layout recovery did not show marker");
    f.context->interim_selection = true; f.context->notify();
    check(!shown(f.indicator.Get()), "interim selection shown");
    f.indicator->close(); f.closed();
}
void delayed_layout() {
    Fixture f; f.host->show_ui = TRUE; f.context->notify_after_session = false;
    f.view->text_query = [&](ITfRange*) { return f.view->queries <= 2 ? TF_E_NOLAYOUT : S_OK; };
    f.start(); f.context->pump();
    check(!shown(f.indicator.Get()), "unavailable initial layout shown");
    const auto deadline = GetTickCount64() + 1000;
    // A mode-only key may produce neither an edit nor a layout notification.
    while (!shown(f.indicator.Get()) && GetTickCount64() < deadline) {
        drain_events(); f.context->pump(); Sleep(5);
    }
    check(shown(f.indicator.Get()), "mode-only notification never retried unavailable layout");
    check(f.context->selection_writes == 0 && f.context->doc->text.empty(), "layout retries edited document");
    f.indicator->close(); f.closed();
}
void bounded_retries() {
    const auto pump_for = [](Fixture& f, ULONGLONG milliseconds) {
        const auto deadline = GetTickCount64() + milliseconds;
        do { drain_events(); f.context->pump(); Sleep(5); } while (GetTickCount64() < deadline);
    };
    {
        Fixture f; f.host->show_ui = TRUE; f.context->notify_after_session = false;
        f.view->text_status = E_FAIL;
        f.start(); f.context->pump();
        pump_for(f, 650);
        const auto queries = f.view->queries;
        check(queries > 1 && queries <= 5 && !shown(f.indicator.Get()), "layout retries were absent or unbounded");
        pump_for(f, 75);
        check(f.view->queries == queries, "retry polling continued beyond initial layout window");
        // An actual host notification can still recover after the retry budget.
        f.view->text_status = S_OK; f.context->notify();
        check(shown(f.indicator.Get()), "retry limit prevented later host layout recovery");
    }
    {
        Fixture f; f.host->show_ui = TRUE; f.context->notify_after_session = false;
        f.context->async_session_error = TF_E_LOCKED;
        f.start();
        f.context->async_session_error = S_OK;
        pump_for(f, 75);
        check(shown(f.indicator.Get()), "temporary edit lock prevented mode display");
    }
    {
        Fixture f; f.host->show_ui = TRUE; f.context->notify_after_session = false;
        f.view->text_status = TF_E_NOLAYOUT;
        f.start(); f.context->pump();
        const auto queries = f.view->queries;
        f.indicator->close(); pump_for(f, 75);
        check(f.view->queries == queries, "closing marker left a retry timer running");
        f.closed();
    }
    {
        Fixture f; f.host->show_ui = TRUE; f.context->notify_after_session = false;
        f.view->text_status = TF_E_NOLAYOUT;
        f.start(); f.context->pump();
        const auto queries = f.view->queries;
        f.indicator->Show(FALSE); pump_for(f, 75);
        check(!shown(f.indicator.Get()) && f.view->queries == queries, "host suppression did not stop position retries");
    }
    {
        Fixture f; f.host->show_ui = TRUE; f.context->notify_after_session = false;
        f.view->text_status = TF_E_NOLAYOUT;
        f.start(); f.context->pump();
        const auto queries = f.view->queries;
        ComPtr<test_host::Context> other; other.Attach(new test_host::Context);
        f.host->focused_document = other->document_manager;
        pump_for(f, 75);
        check(f.view->queries == queries, "position retry queried the previous input field");
        f.closed();
    }
}
void cancellation_and_expiry() {
    {
        Fixture f; f.host->show_ui = TRUE; f.start();
        check(!f.context->queued.empty(), "position read was not deferred");
        const auto queries = f.view->queries;
        f.indicator->close(); f.context->pump();
        check(f.view->queries == queries && !shown(f.indicator.Get()), "canceled layout read still queried or showed UI");
        f.closed();
    }
    {
        Fixture f; f.start();
        ComPtr<test_host::Context> other; other.Attach(new test_host::Context);
        f.host->focused_document = other->document_manager;
        f.context->pump(); f.closed();
    }
    {
        Fixture f; f.host->show_ui = TRUE; f.start(1);
        Sleep(20); // Expire before the host services the queued read.
        f.context->pump(); f.closed();
    }
    {
        Fixture f; f.start(10);
        Sleep(30); drain_events();
        f.closed(); // Timer also expires when no edit callback ever arrives.
        f.context->pump(); check(!shown(f.indicator.Get()), "expired queued request reopened UI");
    }
    {
        Fixture f; f.start(); f.context->pump();
        const auto sink = f.context->layout;
        sink->OnLayoutChange(f.context.Get(), TF_LC_DESTROY, f.view.Get()); f.closed();
    }
    {
        Fixture f; f.start(); f.context->pump();
        DestroyWindow(f.window()); f.closed();
    }
}
void reentry_and_failures() {
    {
        Fixture f;
        f.host->ui_begin_query = [&] { f.indicator->close(); };
        f.start(); f.closed();
    }
    {
        Fixture f; f.host->show_ui = TRUE;
        f.view->text_query = [&](ITfRange*) { f.indicator->close(); return S_OK; };
        f.start(); f.context->pump(); f.closed();
    }
    {
        Fixture f; f.host->show_ui = TRUE;
        f.view->text_query = [&](ITfRange*) {
            auto* replacement = new test_host::ContextView;
            replacement->text_status = TF_E_NOLAYOUT;
            f.context->active_view.Attach(replacement); return S_OK;
        };
        f.start(); f.context->pump();
        // GetTextExt may replace the view through a host callback. Its old
        // answer must never be painted in the new view.
        check(!shown(f.indicator.Get()), "replaced view used stale coordinates");
    }
    {
        Fixture f; f.host->reject_ui = true;
        f.start(); f.closed();
    }
    {
        Fixture f; f.context->reject_edit_sink = true;
        f.start(); f.closed();
    }
    {
        Fixture f; f.context->reject_session = true;
        f.start(); f.closed();
    }
}
}
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        ui_contract(); selection_and_layout(); delayed_layout(); bounded_retries(); cancellation_and_expiry(); reentry_and_failures();
        check(module_objects == 0, "mode indicator leaked COM objects");
        std::cout << "PASS mode UI visibility, read-only caret layout, cancellation, expiry and reentry\n";
        result = 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    CoUninitialize(); return result;
}
