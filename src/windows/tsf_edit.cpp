#include "tsf_edit.h"
#include "module.h"
#include "input_timing.h"
#include "tsf_diagnostics.h"
#include <wrl/client.h>
#include <new>

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
std::wstring utf16(std::u32string_view text) {
    std::wstring result;
    for (auto ch : text) {
        if (ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) ch = 0xfffd;
        if (ch <= 0xffff) result += static_cast<wchar_t>(ch);
        else {
            ch -= 0x10000;
            result += static_cast<wchar_t>(0xd800 + (ch >> 10));
            result += static_cast<wchar_t>(0xdc00 + (ch & 0x3ff));
        }
    }
    return result;
}

class EditSession final : public ITfEditSession {
public:
    EditSession(std::function<HRESULT(TfEditCookie)> function, std::shared_ptr<TsfDiagnosticLog> log,
                TsfTraceState snapshot, std::uint64_t request)
        : function_(std::move(function)), log_(std::move(log)), snapshot_(snapshot), request_(request) { ++module_objects; }
    ~EditSession() { --module_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
        *output = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        TsfDiagnosticScope diagnostic(log_, TsfEvent::EditCallback, snapshot_, request_);
        HRESULT status; TsfReason reason = TsfReason::None;
        try { status = function_(cookie); }
        catch (const std::bad_alloc&) { status = E_OUTOFMEMORY; reason = TsfReason::Exception; }
        catch (...) { status = E_FAIL; reason = TsfReason::Exception; }
        trace_tsf(TsfEvent::EditCallback, reason, status);
        diagnostic.record.status = status; diagnostic.record.reason = reason; diagnostic.record.result_known = true;
        return status;
    }
private:
    std::atomic<ULONG> refs_{1};
    std::function<HRESULT(TfEditCookie)> function_;
    std::shared_ptr<TsfDiagnosticLog> log_;
    TsfTraceState snapshot_;
    std::uint64_t request_;
};

HRESULT request_edit(ITfContext* context, TfClientId client, DWORD flags,
                     std::function<HRESULT(TfEditCookie)> function) {
    if (!context) return E_INVALIDARG;
    const auto log = current_tsf_log();
    const auto request = log ? log->next_call() : 0;
    auto snapshot = current_tsf_trace_state(); snapshot.request_state = true;
    if (log) {
        snapshot.context = log->identity(context);
        trace_tsf(TsfEvent::EditRequest, TsfReason::None, S_OK, 0, flags, request, false);
    }
    ComPtr<ITfEditSession> edit;
    edit.Attach(new EditSession(std::move(function), log, snapshot, request));
    HRESULT session_result = E_FAIL;
    const auto hr = context->RequestEditSession(client, edit.Get(), flags, &session_result);
    trace_tsf(TsfEvent::EditRequest, FAILED(hr) || FAILED(session_result) ? TsfReason::EditRejected : TsfReason::None,
              hr, session_result, flags, request);
    return FAILED(hr) ? hr : session_result;
}

bool range_matches(ITfRange* range, TfEditCookie cookie, std::wstring_view expected) {
    ComPtr<ITfRange> cursor;
    const auto clone_status = range->Clone(&cursor);
    if (FAILED(clone_status) || !cursor) {
        trace_tsf(TsfEvent::RangeCheck, TsfReason::RangeCloneFailed, clone_status); return false;
    }
    std::size_t offset = 0;
    for (;;) {
        wchar_t buffer[256]{};
        ULONG count = 0;
        const auto hr = cursor->GetText(cookie, TF_TF_MOVESTART, buffer, ARRAYSIZE(buffer), &count);
        if (FAILED(hr) || count > ARRAYSIZE(buffer)) {
            trace_tsf(TsfEvent::RangeCheck, TsfReason::RangeReadFailed, hr, count); return false;
        }
        if (!count) {
            const bool same = offset == expected.size();
            trace_tsf(TsfEvent::RangeCheck, same ? TsfReason::None : TsfReason::TextMismatch, hr, offset, expected.size());
            return same;
        }
        if (count > expected.size() - offset || expected.substr(offset, count) != std::wstring_view(buffer, count)) {
            trace_tsf(TsfEvent::RangeCheck, TsfReason::TextMismatch, hr, offset, expected.size()); return false;
        }
        offset += count;
    }
}

bool selection_at_end(ITfContext* context, TfEditCookie cookie, ITfRange* range) {
    TF_SELECTION selection{};
    ULONG count = 0;
    const auto hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count);
    ComPtr<ITfRange> selected; selected.Attach(selection.range);
    if (FAILED(hr) || count != 1 || !selected) {
        trace_tsf(TsfEvent::SelectionCheck, TsfReason::SelectionUnavailable, hr, count); return false;
    }
    if (selection.style.fInterimChar) {
        trace_tsf(TsfEvent::SelectionCheck, TsfReason::InterimSelection, hr); return false;
    }
    BOOL empty = FALSE;
    LONG order = 0;
    const auto empty_status = selected->IsEmpty(cookie, &empty);
    if (FAILED(empty_status) || !empty) {
        trace_tsf(TsfEvent::SelectionCheck, FAILED(empty_status) ? TsfReason::SelectionUnavailable : TsfReason::SelectionNotCollapsed, empty_status); return false;
    }
    const auto compare_status = selected->CompareStart(cookie, range, TF_ANCHOR_END, &order);
    const bool at_end = SUCCEEDED(compare_status) && order == 0;
    trace_tsf(TsfEvent::SelectionCheck, FAILED(compare_status) ? TsfReason::SelectionCompareFailed :
              at_end ? TsfReason::None : TsfReason::CaretMismatch, compare_status, order);
    return at_end;
}

HRESULT select_end(ITfContext* context, TfEditCookie cookie, ITfRange* range) {
    const InputPhaseTiming timing(InputTimingPhase::Selection);
    // Some hosts move the collapsed selection with SetText/InsertText already.
    // Avoid another host edit/layout notification only when it is exactly right.
    // An unknown, nonempty or interim selection still takes the normal path.
    if (selection_at_end(context, cookie, range)) return S_OK;
    ComPtr<ITfRange> caret;
    auto hr = range->Clone(&caret);
    if (FAILED(hr)) { trace_tsf(TsfEvent::SetSelection, TsfReason::RangeCloneFailed, hr); return hr; }
    hr = caret->Collapse(cookie, TF_ANCHOR_END);
    if (FAILED(hr)) { trace_tsf(TsfEvent::SetSelection, TsfReason::SelectionWriteFailed, hr); return hr; }
    TF_SELECTION selection{caret.Get(), {TF_AE_NONE, FALSE}};
    hr = context->SetSelection(cookie, 1, &selection);
    trace_tsf(TsfEvent::SetSelection, FAILED(hr) ? TsfReason::SelectionWriteFailed : TsfReason::None, hr);
    return hr;
}
} // namespace nskkzime::windows
