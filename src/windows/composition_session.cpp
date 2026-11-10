#include "composition_session.h"
#include "tsf_edit.h"
#include "display_attributes.h"
#include "input_timing.h"

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
HRESULT PendingComposition::finish(TfEditCookie cookie) noexcept {
    if (done) return S_OK;
    update_tsf_trace_context(context.Get(), composition.Get(), session);
    ComPtr<ITfRange> range;
    if (SUCCEEDED(composition->GetRange(&range))) clear_display_attribute(context.Get(), cookie, range.Get());
    const auto hr = composition->EndComposition(cookie);
    trace_tsf(TsfEvent::EndComposition, TsfReason::None, hr);
    if (SUCCEEDED(hr)) { done = true; composition.Reset(); }
    return hr;
}

bool CompositionSession::valid(TfEditCookie cookie) const {
    const InputPhaseTiming timing(InputTimingPhase::CompositionCheck);
    ComPtr<ITfRange> range;
    update_tsf_trace_context(context.Get(), composition.Get(), session);
    if (!composition) { trace_tsf(TsfEvent::CompositionCheck, TsfReason::MissingComposition); return false; }
    const auto hr = composition->GetRange(&range);
    if (FAILED(hr) || !range) { trace_tsf(TsfEvent::CompositionCheck, TsfReason::MissingRange, hr); return false; }
    const bool matches = range_matches(range.Get(), cookie, expected_text) && selection_at_end(context.Get(), cookie, range.Get());
    trace_tsf(TsfEvent::CompositionCheck, TsfReason::None, S_OK, matches);
    return matches;
}

void CompositionSession::release_context() noexcept {
    if (context && layout_sink_cookie != TF_INVALID_COOKIE) {
        ComPtr<ITfSource> source;
        if (SUCCEEDED(context.As(&source))) source->UnadviseSink(layout_sink_cookie);
    }
    layout_sink_cookie = TF_INVALID_COOKIE;
    if (context && edit_sink_cookie != TF_INVALID_COOKIE) {
        ComPtr<ITfSource> source;
        if (SUCCEEDED(context.As(&source))) source->UnadviseSink(edit_sink_cookie);
    }
    edit_sink_cookie = TF_INVALID_COOKIE;
    composition.Reset(); context.Reset(); origin.Reset(); expected_text.clear();
}

void CompositionSession::end(TfEditCookie cookie) noexcept {
    if (composition) {
        ComPtr<ITfRange> range;
        if (SUCCEEDED(composition->GetRange(&range))) clear_display_attribute(context.Get(), cookie, range.Get());
        ending = true; const auto hr = composition->EndComposition(cookie); ending = false;
        trace_tsf(TsfEvent::EndComposition, TsfReason::None, hr);
    }
}

namespace {
HRESULT insert_text(ITfContext* target, TfEditCookie cookie, std::wstring_view text) {
    if (text.empty()) return S_OK;
    ComPtr<ITfRange> range;
    ComPtr<ITfInsertAtSelection> insert;
    auto hr = target->QueryInterface(IID_PPV_ARGS(&insert));
    if (FAILED(hr)) return hr;
    {
        const InputPhaseTiming write(InputTimingPhase::SetText);
        hr = insert->InsertTextAtSelection(cookie, 0, text.data(), static_cast<LONG>(text.size()), &range);
        trace_tsf(TsfEvent::InsertText, FAILED(hr) ? TsfReason::TextWriteFailed : TsfReason::None, hr, text.size());
    }
    if (FAILED(hr)) return hr;
    // The insertion succeeded; do not pass this key through on caret failure.
    select_end(target, cookie, range.Get());
    return S_OK;
}
}
HRESULT CompositionSession::start_composition(ITfContext* target, TfEditCookie cookie, CompositionOwner& owner,
    const CompositionSinks& sinks, ComPtr<ITfRange>& range) {
    TF_SELECTION selection{};
    ULONG count = 0;
    auto hr = target->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count);
    trace_tsf(TsfEvent::SelectionCheck, FAILED(hr) || count != 1 || !selection.range ? TsfReason::SelectionUnavailable : TsfReason::None, hr, count);
    range.Attach(selection.range);
    if (FAILED(hr)) return hr;
    if (count != 1 || !range) return E_FAIL;
    ComPtr<ITfContextComposition> compositions;
    hr = target->QueryInterface(IID_PPV_ARGS(&compositions));
    if (FAILED(hr)) return hr;
    ComPtr<ITfSource> source;
    hr = target->QueryInterface(IID_PPV_ARGS(&source));
    if (SUCCEEDED(hr)) hr = source->AdviseSink(IID_ITfTextEditSink, sinks.edit, &edit_sink_cookie);
    trace_tsf(TsfEvent::AdviseEditSink, TsfReason::None, hr);
    if (FAILED(hr)) return hr;
    context = target;
    // Layout notifications are optional: input still works if the host
    // cannot supply positions, and the candidate UI then stays hidden.
    source->AdviseSink(IID_ITfTextLayoutSink, sinks.layout, &layout_sink_cookie);
    hr = compositions->StartComposition(cookie, range.Get(), sinks.composition, &composition);
    update_tsf_trace_context(context.Get(), composition.Get(), session);
    trace_tsf(TsfEvent::StartComposition, FAILED(hr) || !composition ? TsfReason::StartRejected : TsfReason::None, hr);
    if (FAILED(hr) || !composition) { owner.detach_composition(); return FAILED(hr) ? hr : E_FAIL; }
    return hr;
}
HRESULT CompositionSession::write_text(TfEditCookie cookie, ITfRange* range, std::wstring_view text,
    bool created, CompositionOwner& owner) {
    HRESULT hr;
    {
        const InputPhaseTiming write(InputTimingPhase::SetText);
        hr = range->SetText(cookie, 0, text.data(), static_cast<LONG>(text.size()));
        trace_tsf(TsfEvent::SetText, FAILED(hr) ? TsfReason::TextWriteFailed : TsfReason::None, hr, text.size());
    }
    if (FAILED(hr) && created) {
        ending = true; const auto end_status = composition->EndComposition(cookie); ending = false;
        trace_tsf(TsfEvent::EndComposition, TsfReason::TextWriteFailed, end_status);
        owner.detach_composition();
    }
    return hr;
}
HRESULT CompositionSession::update_composition_range(TfEditCookie cookie, ITfRange* range, std::u32string_view committed,
    bool needs_composition, CompositionOwner& owner) {
    if (!needs_composition) {
        ending = true;
        const auto hr = composition->EndComposition(cookie);
        trace_tsf(TsfEvent::EndComposition, TsfReason::Commit, hr);
        ending = false;
        if (SUCCEEDED(hr)) owner.detach_composition();
        return hr;
    }
    if (committed.empty()) return S_OK;
    ComPtr<ITfRange> start;
    auto hr = range->Clone(&start);
    if (FAILED(hr)) return hr;
    LONG shifted = 0;
    const auto length = static_cast<LONG>(utf16(committed).size());
    hr = start->ShiftStart(cookie, length, &shifted, nullptr);
    if (SUCCEEDED(hr) && shifted != length) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = composition->ShiftStart(cookie, start.Get());
    trace_tsf(TsfEvent::ShiftRange, FAILED(hr) ? TsfReason::RangeShiftFailed : TsfReason::None, hr, shifted, length);
    return hr;
}
void CompositionSession::update_display_attribute(ITfContext* target, TfEditCookie cookie, TfGuidAtom attribute) {
    if (!composition) return;
    ComPtr<ITfRange> composing;
    if (SUCCEEDED(composition->GetRange(&composing))) set_display_attribute(target, cookie, composing.Get(), attribute);
}
HRESULT CompositionSession::apply(ITfContext* target, TfEditCookie cookie, std::u32string_view committed, Engine& next,
    CompositionOwner& owner, const CompositionSinks& sinks, TfGuidAtom input, TfGuidAtom converted) {
    const InputPhaseTiming timing(InputTimingPhase::TextEdit);
    const auto preedit = next.composition_text();
    // An empty reading (notably just after '/') still owns an input range.
    // Track it so moving the caret or focus cannot carry its mode elsewhere.
    const bool needs_composition = next.stage() != Stage::Direct || !preedit.empty();
    auto expected = utf16(preedit);
    const auto text = utf16(std::u32string(committed) + std::u32string(preedit));
    if (!composition && !needs_composition) return insert_text(target, cookie, text);
    ComPtr<ITfRange> range;
    bool created = false;
    HRESULT hr;
    if (composition) {
        hr = composition->GetRange(&range);
        if (FAILED(hr)) trace_tsf(TsfEvent::CompositionCheck, TsfReason::MissingRange, hr);
    } else {
        hr = start_composition(target, cookie, owner, sinks, range);
        created = SUCCEEDED(hr);
    }
    if (FAILED(hr)) return hr;
    // Confirming the displayed candidate or editing only the registration
    // popup often leaves the document unchanged. Avoid another host text
    // mutation (layout/accessibility/undo), but still finish/shift the
    // composition and update its attributes and selection below.
    const auto current = composition;
    const auto revision = owner.composition_revision();
    const bool unchanged = !created && text == expected_text;
    if (unchanged && !range_matches(range.Get(), cookie, text)) return E_UNEXPECTED;
    // Reading the host range may reenter and end or replace the composition.
    if (composition.Get() != current.Get() || context.Get() != target || owner.composition_revision() != revision) return E_UNEXPECTED;
    if (!unchanged) {
        hr = write_text(cookie, range.Get(), text, created, owner);
        if (FAILED(hr)) return hr;
    }
    // The text is now applied or verified. Failures below abandon the
    // composition but must never replay the key or roll back its text.
    clear_display_attribute(target, cookie, range.Get());
    const auto post_status = update_composition_range(cookie, range.Get(), committed, needs_composition, owner);
    expected_text = std::move(expected);
    if (SUCCEEDED(post_status)) update_display_attribute(target, cookie, next.stage() == Stage::Selecting ? converted : input);
    const auto caret_status = select_end(target, cookie, range.Get());
    if (FAILED(post_status) || FAILED(caret_status)) {
        // Preserve the text as plain text and abandon this composition if the
        // host cannot maintain its range. Never overwrite committed text on
        // the next key, and never replay a key whose SetText already succeeded.
        owner.end_composition(cookie, FAILED(post_status) ? TsfReason::RangeShiftFailed : TsfReason::SelectionWriteFailed); next.reset();
    }
    return S_OK;
}
} // namespace nskkzime::windows
