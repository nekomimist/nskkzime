#include "tsf_edit.h"
#include "module.h"
#include "input_timing.h"
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
    explicit EditSession(std::function<HRESULT(TfEditCookie)> function) : function_(std::move(function)) { ++module_objects; }
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
        try { return function_(cookie); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
private:
    std::atomic<ULONG> refs_{1};
    std::function<HRESULT(TfEditCookie)> function_;
};

HRESULT request_edit(ITfContext* context, TfClientId client, DWORD flags,
                     std::function<HRESULT(TfEditCookie)> function) {
    if (!context) return E_INVALIDARG;
    ComPtr<ITfEditSession> edit;
    edit.Attach(new EditSession(std::move(function)));
    HRESULT session_result = E_FAIL;
    const auto hr = context->RequestEditSession(client, edit.Get(), flags, &session_result);
    return FAILED(hr) ? hr : session_result;
}

bool range_matches(ITfRange* range, TfEditCookie cookie, std::wstring_view expected) {
    ComPtr<ITfRange> cursor;
    if (FAILED(range->Clone(&cursor)) || !cursor) return false;
    std::size_t offset = 0;
    for (;;) {
        wchar_t buffer[256]{};
        ULONG count = 0;
        if (FAILED(cursor->GetText(cookie, TF_TF_MOVESTART, buffer, ARRAYSIZE(buffer), &count)) || count > ARRAYSIZE(buffer)) return false;
        if (!count) return offset == expected.size();
        if (count > expected.size() - offset || expected.substr(offset, count) != std::wstring_view(buffer, count)) return false;
        offset += count;
    }
}

bool selection_at_end(ITfContext* context, TfEditCookie cookie, ITfRange* range) {
    TF_SELECTION selection{};
    ULONG count = 0;
    const auto hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count);
    ComPtr<ITfRange> selected; selected.Attach(selection.range);
    if (FAILED(hr) || count != 1 || !selected || selection.style.fInterimChar) return false;
    BOOL empty = FALSE;
    LONG order = 0;
    return SUCCEEDED(selected->IsEmpty(cookie, &empty)) && empty &&
        SUCCEEDED(selected->CompareStart(cookie, range, TF_ANCHOR_END, &order)) && order == 0;
}

HRESULT select_end(ITfContext* context, TfEditCookie cookie, ITfRange* range) {
    const InputPhaseTiming timing(InputTimingPhase::Selection);
    // Some hosts move the collapsed selection with SetText/InsertText already.
    // Avoid another host edit/layout notification only when it is exactly right.
    // An unknown, nonempty or interim selection still takes the normal path.
    if (selection_at_end(context, cookie, range)) return S_OK;
    ComPtr<ITfRange> caret;
    auto hr = range->Clone(&caret);
    if (FAILED(hr)) return hr;
    hr = caret->Collapse(cookie, TF_ANCHOR_END);
    if (FAILED(hr)) return hr;
    TF_SELECTION selection{caret.Get(), {TF_AE_NONE, FALSE}};
    return context->SetSelection(cookie, 1, &selection);
}
} // namespace nskkzime::windows
