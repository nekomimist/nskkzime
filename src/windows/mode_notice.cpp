#include "mode_notice.h"
#include "tsf_edit.h"

namespace nskkzime::windows {
using Microsoft::WRL::ComPtr;

void ModeNotice::dismiss() noexcept {
    ++request_;
    publishing_context_.Reset();
    auto indicator = std::move(indicator_);
    if (indicator) indicator->close();
}
void ModeNotice::clear_focus() noexcept {
    focus_document_.Reset();
    focus_context_.Reset();
}
void ModeNotice::remove_document(ITfDocumentMgr* document) noexcept {
    if (focus_document_.Get() == document) clear_focus();
}
void ModeNotice::observe_focus(ITfDocumentMgr* document, ITfContext* origin, ITfContext* resolved) noexcept {
    const bool same = resolved && ((indicator_ && indicator_->matches_focus(document, origin)) ||
        publishing_context_.Get() == resolved ||
        (focus_document_.Get() == document && focus_context_.Get() == origin));
    if (!same) dismiss();
}
void ModeNotice::publish(InputMode* mode, Mode value, ITfContext* target) noexcept {
    const auto request = request_;
    const auto previous = publishing_context_;
    // IME open/close can refresh focus synchronously without moving the caret.
    publishing_context_ = target;
    const ComPtr<InputMode> binding(mode);
    if (binding) binding->publish(value);
    if (request == request_) publishing_context_ = previous;
}
void ModeNotice::show_focus(const ModeNoticeOwner& owner) noexcept {
    const auto initial = owner.mode_notice_state();
    if (!initial.settings.show_input_mode || !initial.thread || !initial.foreground || initial.connecting || resolving_focus_) return;
    // Parent resolution in a file dialog can call OnPushContext synchronously.
    resolving_focus_ = true;
    struct Reset { bool& value; ~Reset() { value = false; } } reset{resolving_focus_};
    const auto request = request_;
    try {
        ComPtr<ITfDocumentMgr> document;
        ComPtr<ITfContext> origin;
        if (FAILED(initial.thread->GetFocus(&document)) || !document || FAILED(document->GetTop(&origin))) return;
        const auto context = editing_context(origin.Get());
        const auto current = owner.mode_notice_state();
        if (request != request_ || initial.focus_generation != current.focus_generation || !current.thread || !current.foreground) return;
        if (focus_document_ == document && focus_context_ == origin) return;
        // Retain the field beyond the notice timer, avoiding repeated notices.
        focus_document_ = document;
        focus_context_ = origin;
        if (!context || publishing_context_ || (indicator_ && indicator_->matches_focus(document.Get(), origin.Get()))) return;
        show(owner, context.Get());
    } catch (...) { /* A focus notification must never fail input. */ }
}
void ModeNotice::show(const ModeNoticeOwner& owner, ITfContext* target) noexcept {
    const auto initial = owner.mode_notice_state();
    if (!initial.settings.show_input_mode || !initial.thread || !initial.foreground || !initial.idle) return;
    dismiss();
    const auto request = request_;
    // dismiss() may reenter. Match the same capture point as the caller's edit.
    const auto before = owner.mode_notice_state();
    try {
        if (!before.thread) return;
        ComPtr<ITfDocumentMgr> document;
        ComPtr<ITfContext> origin;
        if (FAILED(before.thread->GetFocus(&document)) || !document || FAILED(document->GetTop(&origin))) return;
        const auto context = editing_context(origin.Get());
        if (target && context.Get() != target) return;
        if (!context || !editable(context.Get())) return;
        const auto current = owner.mode_notice_state();
        if (request != request_ || !current.thread || before.revision != current.revision) return;
        ComPtr<ModeIndicator> indicator;
        indicator.Attach(new ModeIndicator(current.mode, current.settings));
        indicator_ = indicator;
        indicator->start(current.thread.Get(), context.Get(), current.client, origin.Get());
    } catch (...) { if (request == request_) dismiss(); }
}
} // namespace nskkzime::windows
