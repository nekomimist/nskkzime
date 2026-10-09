#include "composition_session.h"
#include "tsf_edit.h"
#include "display_attributes.h"
#include "input_timing.h"

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;
HRESULT PendingComposition::finish(TfEditCookie cookie) noexcept {
    if (done) return S_OK;
    ComPtr<ITfRange> range;
    if (SUCCEEDED(composition->GetRange(&range))) clear_display_attribute(context.Get(), cookie, range.Get());
    const auto hr = composition->EndComposition(cookie);
    if (SUCCEEDED(hr)) { done = true; composition.Reset(); }
    return hr;
}

bool CompositionSession::valid(TfEditCookie cookie) const {
    const InputPhaseTiming timing(InputTimingPhase::CompositionCheck);
    ComPtr<ITfRange> range;
    return composition && SUCCEEDED(composition->GetRange(&range)) && range &&
        range_matches(range.Get(), cookie, expected_text) && selection_at_end(context.Get(), cookie, range.Get());
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
        ending = true; composition->EndComposition(cookie); ending = false;
    }
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
    ComPtr<ITfRange> range;
    bool created = false;
    HRESULT hr;
    if (composition) {
        hr = composition->GetRange(&range);
        if (FAILED(hr)) return hr;
    } else {
        if (text.empty() && !needs_composition) return S_OK;
        if (!needs_composition) {
            ComPtr<ITfInsertAtSelection> insert;
            hr = target->QueryInterface(IID_PPV_ARGS(&insert));
            if (FAILED(hr)) return hr;
            {
                const InputPhaseTiming write(InputTimingPhase::SetText);
                hr = insert->InsertTextAtSelection(cookie, 0, text.c_str(), static_cast<LONG>(text.size()), &range);
            }
            if (FAILED(hr)) return hr;
            // The insertion succeeded; do not pass this key through on caret failure.
            select_end(target, cookie, range.Get());
            return S_OK;
        }
        TF_SELECTION selection{};
        ULONG count = 0;
        hr = target->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count);
        range.Attach(selection.range);
        if (FAILED(hr)) return hr;
        if (count != 1 || !range) return E_FAIL;
        ComPtr<ITfContextComposition> compositions;
        hr = target->QueryInterface(IID_PPV_ARGS(&compositions));
        if (FAILED(hr)) return hr;
        ComPtr<ITfSource> source;
        hr = target->QueryInterface(IID_PPV_ARGS(&source));
        if (SUCCEEDED(hr)) hr = source->AdviseSink(IID_ITfTextEditSink, sinks.edit, &edit_sink_cookie);
        if (FAILED(hr)) return hr;
        context = target;
        // Layout notifications are optional: input still works if the host
        // cannot supply positions, and the candidate UI then stays hidden.
        source->AdviseSink(IID_ITfTextLayoutSink, sinks.layout, &layout_sink_cookie);
        hr = compositions->StartComposition(cookie, range.Get(), sinks.composition, &composition);
        if (FAILED(hr) || !composition) { owner.detach_composition(); return FAILED(hr) ? hr : E_FAIL; }
        created = true;
    }
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
        const InputPhaseTiming write(InputTimingPhase::SetText);
        hr = range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
    } else hr = S_OK;
    if (FAILED(hr)) {
        if (created) {
            ending = true; composition->EndComposition(cookie); ending = false;
            owner.detach_composition();
        }
        return hr;
    }
    HRESULT post_status = S_OK;
    clear_display_attribute(target, cookie, range.Get());
    if (!needs_composition) {
        ending = true;
        post_status = composition->EndComposition(cookie);
        ending = false;
        if (SUCCEEDED(post_status)) owner.detach_composition();
    } else if (!committed.empty()) {
        ComPtr<ITfRange> start;
        post_status = range->Clone(&start);
        if (SUCCEEDED(post_status)) {
            LONG shifted = 0;
            const auto length = static_cast<LONG>(utf16(committed).size());
            post_status = start->ShiftStart(cookie, length, &shifted, nullptr);
            if (SUCCEEDED(post_status) && shifted != length) post_status = E_FAIL;
            if (SUCCEEDED(post_status)) post_status = composition->ShiftStart(cookie, start.Get());
        }
    }
    expected_text = std::move(expected);
    if (composition && SUCCEEDED(post_status)) {
        ComPtr<ITfRange> composing;
        if (SUCCEEDED(composition->GetRange(&composing)))
            set_display_attribute(target, cookie, composing.Get(), next.stage() == Stage::Selecting ? converted : input);
    }
    const auto caret_status = select_end(target, cookie, range.Get());
    if (FAILED(post_status) || FAILED(caret_status)) {
        // Preserve the text as plain text and abandon this composition if the
        // host cannot maintain its range. Never overwrite committed text on
        // the next key, and never replay a key whose SetText already succeeded.
        owner.end_composition(cookie); next.reset();
    }
    // Once text is applied or verified, consume the key even if a later host operation fails.
    return S_OK;
}
} // namespace nskkzime::windows
