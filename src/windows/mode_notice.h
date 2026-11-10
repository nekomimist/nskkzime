#pragma once
#include "mode_indicator.h"
#include "input_mode.h"

namespace nskkzime::windows {
struct ModeNoticeState {
    Microsoft::WRL::ComPtr<ITfThreadMgr> thread;
    TfClientId client;
    const UiSettings& settings;
    Mode mode;
    std::uint64_t revision, focus_generation;
    bool foreground, connecting, idle;
};
class ModeNoticeOwner {
public:
    virtual ModeNoticeState mode_notice_state() const noexcept = 0;
protected:
    ~ModeNoticeOwner() = default;
};

// Owns notice lifetime, focus deduplication and invalidation during reentrant
// host calls. The caller keeps the owner alive for show/show_focus/publish.
class ModeNotice {
public:
    std::uint64_t request() const noexcept { return request_; }
    void dismiss() noexcept;
    void clear_focus() noexcept;
    void remove_document(ITfDocumentMgr*) noexcept;
    void observe_focus(ITfDocumentMgr*, ITfContext* origin, ITfContext* resolved) noexcept;
    void publish(InputMode*, Mode, ITfContext* target) noexcept;
    void show_focus(const ModeNoticeOwner&) noexcept;
    void show(const ModeNoticeOwner&, ITfContext* target = nullptr) noexcept;
private:
    Microsoft::WRL::ComPtr<ModeIndicator> indicator_;
    Microsoft::WRL::ComPtr<ITfContext> publishing_context_, focus_context_;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> focus_document_;
    std::uint64_t request_ = 0;
    bool resolving_focus_ = false;
};
} // namespace nskkzime::windows
