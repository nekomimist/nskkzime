#include "tsf_diagnostics.h"
#include "diagnostic_file.h"
#include "learning_ipc.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdio>

namespace nskkzime::windows {
namespace {
thread_local TsfDiagnosticScope* active = nullptr;
thread_local std::weak_ptr<TsfDiagnosticLog> latest;
thread_local bool startup_failed = false;
int module_anchor = 0;
const char* name(TsfEvent value) {
    constexpr const char* names[]{"activate", "deactivate", "key_focus", "test_key_down", "key_down", "key_up", "preserved_key",
        "host_terminated", "end_edit", "layout_change", "document_uninit", "document_focus", "context_push", "context_pop",
        "edit_request", "edit_callback", "range_check", "selection_check", "composition_check", "start_composition",
        "end_composition", "set_text", "insert_text", "set_selection", "shift_range", "advise_edit_sink",
        "finish_composition", "detach_composition", "engine_reset", "state_accepted", "mode_change", "key_result",
        "candidate_select", "inference_result", "document_top", "focus_resolved", "focus_deferred", "focus_cancelled"};
    return names[static_cast<unsigned>(value)];
}
const char* name(TsfReason value) {
    constexpr const char* names[]{"none", "focus_lost", "context_changed", "context_popped", "document_removed", "navigation", "mode_changed",
        "deactivated", "host_termination", "invalid_before_key", "invalid_after_edit", "invalid_candidate_selection",
        "exception", "missing_composition", "missing_range", "range_clone_failed", "range_read_failed", "text_mismatch",
        "selection_unavailable", "interim_selection", "selection_not_collapsed", "selection_compare_failed", "caret_mismatch",
        "start_rejected", "text_write_failed", "selection_write_failed", "range_shift_failed", "commit", "edit_rejected", "inactive",
        "focus_unavailable", "focus_restored", "focus_validated"};
    return names[static_cast<unsigned>(value)];
}
}
struct TsfDiagnosticLog::State {
    NativeHandle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::array<TsfTraceRecord, queue_limit> queue;
    // One TSF apartment produces, one worker consumes. Publishing the indices
    // transfers slot ownership; neither side waits on the other's I/O or lock.
    std::atomic<std::uint64_t> begin{0}, end{0};
    std::atomic<bool> stopping{false};
    std::atomic<TsfLogStatus> status{TsfLogStatus::Ready};
    std::atomic<std::uint64_t> dropped{0};
    std::filesystem::path directory;
    DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
    std::string app;
    struct Work { std::shared_ptr<State> state; HMODULE module; };
    static void CALLBACK run(PTP_CALLBACK_INSTANCE instance, void* raw) noexcept {
        std::unique_ptr<Work> work(static_cast<Work*>(raw));
        FreeLibraryWhenCallbackReturns(instance, work->module);
        CallbackMayRunLong(instance);
        auto& state = *work->state;
        try {
            DiagnosticFile file(state.directory, L"nskkzime-tsf-", L"nskkzime-tsf-log.lock");
            for (;;) {
                TsfTraceRecord event;
                const auto begin = state.begin.load(std::memory_order_relaxed);
                if (begin == state.end.load(std::memory_order_acquire)) {
                    if (state.stopping.load(std::memory_order_acquire)) {
                        // The final publication can occur after the first empty
                        // observation. Stop orders a second read after that publication.
                        if (begin == state.end.load(std::memory_order_acquire)) return;
                        continue;
                    }
                    if (WaitForSingleObject(state.wake.get(), INFINITE) != WAIT_OBJECT_0) throw std::runtime_error("diagnostic wake failed");
                    continue;
                }
                event = state.queue[begin % queue_limit];
                state.begin.store(begin + 1, std::memory_order_release);
                SYSTEMTIME utc{}; FileTimeToSystemTime(&event.utc, &utc);
                char timestamp[32]{}, status[12]{};
                sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
                          utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
                sprintf_s(status, "0x%08lX", static_cast<unsigned long>(event.status));
                const auto& snapshot = event.state;
                const nlohmann::json record{{"format", "nskkzime-tsf-diagnostics"}, {"version", 1}, {"utc", timestamp},
                    {"ticks_ms", event.ticks}, {"pid", state.pid}, {"tid", state.tid}, {"app", state.app},
                    {"seq", event.sequence}, {"dropped", state.dropped.load()}, {"call", event.call}, {"parent_call", event.parent_call},
                    {"request", event.request}, {"depth", event.depth}, {"phase", event.phase == 1 ? "enter" : event.phase == 2 ? "exit" : "point"},
                    {"event", name(event.event)}, {"reason", name(event.reason)}, {"hr", status}, {"result_known", event.result_known}, {"detail", event.detail}, {"detail2", event.detail2},
                    {"context", snapshot.context}, {"origin", snapshot.origin}, {"owner_context", snapshot.owner_context}, {"composition", snapshot.composition},
                    {"session", snapshot.session}, {"revision", snapshot.revision}, {"stage", snapshot.stage}, {"mode", snapshot.mode},
                    {"pending_length", snapshot.pending}, {"reading_length", snapshot.reading}, {"okuri_length", snapshot.okuri}, {"expected_length", snapshot.expected},
                    {"state_source", snapshot.request_state ? "request" : "current"}};
                switch (file.append(record.dump() + '\n')) {
                case DiagnosticFileStatus::Written: state.status = TsfLogStatus::Written; break;
                case DiagnosticFileStatus::Full: state.status = TsfLogStatus::Full; return;
                default: state.status = TsfLogStatus::Unavailable; return;
                }
            }
        } catch (...) { state.status = TsfLogStatus::Unavailable; }
    }
};
std::shared_ptr<TsfDiagnosticLog> TsfDiagnosticLog::create(const DictionarySettings::TsfLogSettings& settings, bool app_container) noexcept {
    startup_failed = false;
    if (!settings.enabled) { latest.reset(); return {}; }
    try {
        auto state = std::make_shared<State>(); state->directory = settings.directory;
        auto log = std::shared_ptr<TsfDiagnosticLog>(new TsfDiagnosticLog(state)); latest = log;
        if (app_container) { state->status = TsfLogStatus::Unsupported; return log; }
        if (!state->wake.valid()) { state->status = TsfLogStatus::Unavailable; return log; }
        wchar_t path[32768]{}; const auto length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (length && length < ARRAYSIZE(path)) state->app = utf8_message(std::filesystem::path(path).filename().native());
        auto work = std::make_unique<State::Work>(); work->state = state;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&module_anchor), &work->module)) {
            state->status = TsfLogStatus::Unavailable; return log;
        }
        if (!TrySubmitThreadpoolCallback(State::run, work.get(), nullptr)) {
            FreeLibrary(work->module); state->status = TsfLogStatus::Unavailable; return log;
        }
        work.release(); return log;
    } catch (...) { startup_failed = true; latest.reset(); return {}; }
}
TsfDiagnosticLog::~TsfDiagnosticLog() {
    state_->stopping = true;
    if (state_->wake.valid()) SetEvent(state_->wake.get()); // Worker drains its state and DLL pin. Never join here.
}
TsfLogStatus TsfDiagnosticLog::status() const noexcept { return state_->status.load(); }
std::uint64_t TsfDiagnosticLog::dropped() const noexcept { return state_->dropped.load(); }
void TsfDiagnosticLog::record(TsfTraceRecord event) noexcept {
    event.sequence = ++sequence_;
    event.ticks = GetTickCount64(); GetSystemTimeAsFileTime(&event.utc);
    const auto status = state_->status.load();
    if (status != TsfLogStatus::Ready && status != TsfLogStatus::Written) return;
    const auto end = state_->end.load(std::memory_order_relaxed);
    if (end - state_->begin.load(std::memory_order_acquire) == queue_limit) { ++state_->dropped; return; }
    if (state_->stopping.load()) return;
    state_->queue[end % queue_limit] = event;
    state_->end.store(end + 1, std::memory_order_release);
    SetEvent(state_->wake.get());
}
std::uint64_t TsfDiagnosticLog::identity(const void* pointer) noexcept {
    if (!pointer) return 0;
    for (const auto& item : identities_) if (item.pointer == pointer) return item.id;
    const auto id = ++identity_; identities_[(id - 1) % identities_.size()] = {pointer, id}; return id;
}
TsfDiagnosticScope::TsfDiagnosticScope(std::shared_ptr<TsfDiagnosticLog> logger, TsfEvent event, TsfTraceState snapshot, std::uint64_t request) noexcept
    : log(std::move(logger)) {
    previous_ = active;
    if (!log) { active = nullptr; return; }
    record.event = event; record.state = snapshot; record.call = log->next_call(); record.request = request;
    record.depth = previous_ && previous_->log == log ? previous_->record.depth + 1 : 1;
    record.parent_call = previous_ && previous_->log == log ? previous_->record.call : 0;
    record.phase = 1; active = this; log->record(record);
}
TsfDiagnosticScope::~TsfDiagnosticScope() {
    if (log) { record.phase = 2; log->record(record); }
    active = previous_;
}
void trace_tsf(TsfEvent event, TsfReason reason, HRESULT status, std::int64_t detail, std::int64_t detail2,
               std::uint64_t request, bool result_known) noexcept {
    if (!active) return;
    auto record = active->record; record.event = event; record.reason = reason; record.status = status;
    record.detail = detail; record.detail2 = detail2; record.phase = 0; record.result_known = result_known;
    if (request) record.request = request;
    active->log->record(record);
}
void update_tsf_trace(TsfTraceState snapshot) noexcept { if (active) active->record.state = snapshot; }
void update_tsf_trace_context(const void* context, const void* composition, std::uint64_t session) noexcept {
    if (!active) return;
    active->record.state.context = active->log->identity(context);
    active->record.state.composition = active->log->identity(composition); active->record.state.session = session;
    active->record.state.owner_context = composition ? active->record.state.context : 0;
}
std::shared_ptr<TsfDiagnosticLog> current_tsf_log() noexcept { return active ? active->log : nullptr; }
TsfTraceState current_tsf_trace_state() noexcept { return active ? active->record.state : TsfTraceState{}; }
std::wstring tsf_diagnostics_status() {
    const auto log = latest.lock(); if (!log) return startup_failed ? L"TSF診断ログ: 初期化できないため停止" : L"TSF診断ログ: オフ";
    const wchar_t* status = L"有効・記録待ち";
    switch (log->status()) {
    case TsfLogStatus::Written: status = L"保存中"; break;
    case TsfLogStatus::Full: status = L"上限に達したため停止"; break;
    case TsfLogStatus::Unavailable: status = L"保存できないため停止"; break;
    case TsfLogStatus::Unsupported: status = L"AppContainerでの保存は未対応"; break;
    default: break;
    }
    return L"TSF診断ログ: " + std::wstring(status) + L" / 記録の欠落: " + std::to_wstring(log->dropped());
}
} // namespace nskkzime::windows
