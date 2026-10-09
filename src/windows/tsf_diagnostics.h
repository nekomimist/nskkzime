#pragma once
#include "dictionary_settings.h"
#include <windows.h>
#include <array>
#include <memory>

namespace nskkzime::windows {
enum class TsfEvent {
    Activate, Deactivate, KeyFocus, TestKeyDown, KeyDown, KeyUp, PreservedKey,
    HostTerminated, EndEdit, LayoutChange, DocumentUninit, DocumentFocus, ContextPush, ContextPop,
    EditRequest, EditCallback, RangeCheck, SelectionCheck, CompositionCheck, StartComposition,
    EndComposition, SetText, InsertText, SetSelection, ShiftRange, AdviseEditSink,
    FinishComposition, DetachComposition, EngineReset, StateAccepted, ModeChange, KeyResult,
    CandidateSelect, InferenceResult, DocumentTop, FocusResolved, FocusDeferred, FocusCancelled
};
enum class TsfReason {
    None, FocusLost, ContextChanged, ContextPopped, DocumentRemoved, Navigation, ModeChanged,
    Deactivated, HostTermination, InvalidBeforeKey, InvalidAfterEdit, InvalidCandidateSelection,
    Exception, MissingComposition, MissingRange, RangeCloneFailed, RangeReadFailed, TextMismatch,
    SelectionUnavailable, InterimSelection, SelectionNotCollapsed, SelectionCompareFailed, CaretMismatch,
    StartRejected, TextWriteFailed, SelectionWriteFailed, RangeShiftFailed, Commit, EditRejected, Inactive,
    FocusUnavailable, FocusRestored, FocusValidated
};
enum class TsfLogStatus { Ready, Written, Full, Unavailable, Unsupported };
struct TsfTraceState {
    std::uint64_t context = 0, origin = 0, owner_context = 0, composition = 0, session = 0, revision = 0;
    std::size_t pending = 0, reading = 0, okuri = 0, expected = 0;
    unsigned stage = 0, mode = 0;
    bool request_state = false;
};
struct TsfTraceRecord {
    TsfEvent event = TsfEvent::Activate;
    TsfReason reason = TsfReason::None;
    TsfTraceState state;
    HRESULT status = S_OK;
    std::int64_t detail = 0, detail2 = 0;
    std::uint64_t sequence = 0, call = 0, parent_call = 0, request = 0, ticks = 0;
    FILETIME utc{};
    unsigned depth = 0, phase = 0; // 0 point, 1 callback entry, 2 callback exit.
    bool result_known = false;
};
// Record/identity/scope calls belong to the creating TSF thread. The queue
// contains primitives only; no host objects or input text reach the worker.
class TsfDiagnosticLog {
public:
    static std::shared_ptr<TsfDiagnosticLog> create(const DictionarySettings::TsfLogSettings&, bool app_container) noexcept;
    ~TsfDiagnosticLog();
    void record(TsfTraceRecord) noexcept;
    std::uint64_t identity(const void*) noexcept; // Bounded, thread-owned interface-pointer IDs, not COM identity.
    std::uint64_t next_call() noexcept { return ++call_; }
    TsfLogStatus status() const noexcept;
    std::uint64_t dropped() const noexcept;
    static constexpr std::size_t queue_limit = 512;
private:
    struct State;
    explicit TsfDiagnosticLog(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
    struct Identity { const void* pointer = nullptr; std::uint64_t id = 0; };
    std::array<Identity, 128> identities_{};
    std::uint64_t identity_ = 0, call_ = 0, sequence_ = 0;
};
class TsfDiagnosticScope {
public:
    TsfDiagnosticScope(std::shared_ptr<TsfDiagnosticLog>, TsfEvent, TsfTraceState, std::uint64_t request = 0) noexcept;
    ~TsfDiagnosticScope();
    TsfDiagnosticScope(const TsfDiagnosticScope&) = delete;
    TsfDiagnosticScope& operator=(const TsfDiagnosticScope&) = delete;
    std::shared_ptr<TsfDiagnosticLog> log;
    TsfTraceRecord record;
private:
    TsfDiagnosticScope* previous_ = nullptr;
};
void trace_tsf(TsfEvent, TsfReason = TsfReason::None, HRESULT = S_OK, std::int64_t detail = 0,
               std::int64_t detail2 = 0, std::uint64_t request = 0, bool result_known = true) noexcept;
void update_tsf_trace(TsfTraceState) noexcept;
void update_tsf_trace_context(const void* context, const void* composition, std::uint64_t session) noexcept;
std::shared_ptr<TsfDiagnosticLog> current_tsf_log() noexcept;
TsfTraceState current_tsf_trace_state() noexcept;
std::wstring tsf_diagnostics_status();
} // namespace nskkzime::windows
