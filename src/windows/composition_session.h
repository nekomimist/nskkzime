#pragma once
#include "nskkzime/engine.h"
#include "input_checkpoint.h"
#include "tsf_diagnostics.h"
#include <msctf.h>
#include <wrl/client.h>

namespace nskkzime::windows {
class CompositionOwner {
public:
    virtual std::uint64_t composition_revision() const = 0;
    virtual void detach_composition() noexcept = 0;
    virtual void end_composition(TfEditCookie, TsfReason) noexcept = 0;
protected:
    ~CompositionOwner() = default;
};
struct CompositionSinks {
    ITfCompositionSink* composition;
    ITfTextEditSink* edit;
    ITfTextLayoutSink* layout;
};
struct PendingComposition {
    Microsoft::WRL::ComPtr<ITfComposition> composition;
    Microsoft::WRL::ComPtr<ITfContext> context;
    bool done = false;
    std::uint64_t session = 0;
    HRESULT finish(TfEditCookie) noexcept;
};
// One host composition. The adapter invalidates inference/UI before releasing
// this context; the session number remains monotonic across detach/reuse.
class CompositionSession {
public:
    Microsoft::WRL::ComPtr<ITfComposition> composition;
    Microsoft::WRL::ComPtr<ITfContext> context, origin;
    std::wstring expected_text;
    DWORD edit_sink_cookie = TF_INVALID_COOKIE, layout_sink_cookie = TF_INVALID_COOKIE;
    bool ending = false, reading_context = false;
    std::uint64_t session = 0;
    InputCheckpoint checkpoint(std::uint64_t revision) const { return {context, composition, session, revision}; }
    bool valid(TfEditCookie) const;
    void release_context() noexcept;
    void end(TfEditCookie) noexcept;
    HRESULT apply(ITfContext*, TfEditCookie, std::u32string_view committed, Engine& next,
        CompositionOwner&, const CompositionSinks&, TfGuidAtom input, TfGuidAtom converted);
private:
    HRESULT start_composition(ITfContext*, TfEditCookie, CompositionOwner&, const CompositionSinks&,
        Microsoft::WRL::ComPtr<ITfRange>&);
    HRESULT write_text(TfEditCookie, ITfRange*, std::wstring_view, bool created, CompositionOwner&);
    HRESULT update_composition_range(TfEditCookie, ITfRange*, std::u32string_view committed,
        bool needs_composition, CompositionOwner&);
    void update_display_attribute(ITfContext*, TfEditCookie, TfGuidAtom);
};
} // namespace nskkzime::windows
