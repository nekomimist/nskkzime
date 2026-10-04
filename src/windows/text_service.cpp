#include "module.h"
#include "nskkzime/engine.h"
#include "runtime_dictionary.h"
#include <wrl/client.h>
#include <atomic>
#include <bitset>
#include <functional>
#include <new>
#include <optional>
#include <utility>

namespace nskkzime::windows {
namespace {
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

std::optional<Key> translate_key(WPARAM code, LPARAM parameter) {
    if (GetKeyState(VK_MENU) & 0x8000 || GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000) return {};
    if (GetKeyState(VK_CONTROL) & 0x8000) {
        if (code == 'J') return Key{KeyCode::Hiragana};
        if (code == 'G') return Key{KeyCode::Cancel};
        return {};
    }
    switch (code) {
    case VK_SPACE: return Key{KeyCode::Space};
    case VK_RETURN: return Key{KeyCode::Enter};
    case VK_BACK: return Key{KeyCode::Backspace};
    case VK_ESCAPE: return Key{KeyCode::Cancel};
    default: break;
    }
    BYTE state[256]{};
    if (!GetKeyboardState(state)) return {};
    wchar_t text[4]{};
    // Flag 4 leaves the dead-key state unchanged, including during OnTestKeyDown.
    const int length = ToUnicodeEx(static_cast<UINT>(code), static_cast<UINT>((parameter >> 16) & 0xff),
                                  state, text, ARRAYSIZE(text), 4, GetKeyboardLayout(0));
    if (length != 1 || text[0] < 0x20 || text[0] > 0x7e) return {};
    return Key{KeyCode::Character, static_cast<char>(text[0])};
}

bool editable(ITfContext* context) {
    if (!context) return false;
    TF_STATUS status{};
    if (FAILED(context->GetStatus(&status)) || (status.dwDynamicFlags & TF_SD_READONLY)) return false;
    ComPtr<ITfCompartmentMgr> compartments;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&compartments)))) {
        for (const auto* id : {&GUID_COMPARTMENT_KEYBOARD_DISABLED, &GUID_COMPARTMENT_EMPTYCONTEXT}) {
            ComPtr<ITfCompartment> compartment;
            if (SUCCEEDED(compartments->GetCompartment(*id, &compartment))) {
                VARIANT value; VariantInit(&value);
                const auto hr = compartment->GetValue(&value);
                const bool disabled = SUCCEEDED(hr) && value.vt == VT_I4 && value.lVal != 0;
                VariantClear(&value);
                if (disabled) return false;
            }
        }
    }
    const auto focus = GetFocus();
    if (focus) {
        wchar_t name[32]{};
        GetClassNameW(focus, name, ARRAYSIZE(name));
        if (_wcsicmp(name, L"Edit") == 0 && (GetWindowLongPtrW(focus, GWL_STYLE) & ES_PASSWORD)) return false;
    }
    return true;
}

HRESULT select_end(ITfContext* context, TfEditCookie cookie, ITfRange* range) {
    ComPtr<ITfRange> caret;
    auto hr = range->Clone(&caret);
    if (FAILED(hr)) return hr;
    hr = caret->Collapse(cookie, TF_ANCHOR_END);
    if (FAILED(hr)) return hr;
    TF_SELECTION selection{caret.Get(), {TF_AE_NONE, FALSE}};
    return context->SetSelection(cookie, 1, &selection);
}

class TextService final : public ITfTextInputProcessorEx, public ITfKeyEventSink,
                          public ITfCompositionSink, public ITfThreadMgrEventSink {
public:
    TextService() : engine_(runtime_dictionary()) { ++module_objects; }
    ~TextService() { --module_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfTextInputProcessor || iid == IID_ITfTextInputProcessorEx)
            *output = static_cast<ITfTextInputProcessorEx*>(this);
        else if (iid == IID_ITfKeyEventSink) *output = static_cast<ITfKeyEventSink*>(this);
        else if (iid == IID_ITfCompositionSink) *output = static_cast<ITfCompositionSink*>(this);
        else if (iid == IID_ITfThreadMgrEventSink) *output = static_cast<ITfThreadMgrEventSink*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* manager, TfClientId client) override { return ActivateEx(manager, client, 0); }
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* manager, TfClientId client, DWORD) override {
        if (!manager || client == TF_CLIENTID_NULL) return E_INVALIDARG;
        if (manager_) return E_UNEXPECTED;
        manager_ = manager; client_ = client;
        auto hr = manager_.As(&keys_);
        if (SUCCEEDED(hr)) hr = keys_->AdviseKeyEventSink(client_, this, TRUE);
        if (FAILED(hr)) { keys_.Reset(); manager_.Reset(); client_ = TF_CLIENTID_NULL; return hr; }
        ComPtr<ITfSource> source;
        hr = manager_.As(&source);
        if (SUCCEEDED(hr)) hr = source->AdviseSink(IID_ITfThreadMgrEventSink,
            static_cast<ITfThreadMgrEventSink*>(this), &sink_cookie_);
        if (FAILED(hr)) Deactivate();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        finish_composition();
        if (keys_) keys_->UnadviseKeyEventSink(client_);
        if (manager_ && sink_cookie_ != TF_INVALID_COOKIE) {
            ComPtr<ITfSource> source;
            if (SUCCEEDED(manager_.As(&source))) source->UnadviseSink(sink_cookie_);
        }
        sink_cookie_ = TF_INVALID_COOKIE;
        keys_.Reset(); manager_.Reset(); client_ = TF_CLIENTID_NULL;
        eaten_keys_.reset(); engine_.reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override {
        if (!foreground) finish_composition();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM code, LPARAM parameter, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        const auto key = translate_key(code, parameter);
        if (key) *eaten = engine_.would_handle(*key);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM code, LPARAM parameter, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        const auto key = translate_key(code, parameter);
        if (!key || !engine_.would_handle(*key)) return S_OK;
        if (composition_context_ && composition_context_.Get() != context) finish_composition();
        try {
            const ComPtr<TextService> self(this);
            const ComPtr<ITfContext> target(context);
            bool applied = false;
            const auto hr = request_edit(context, client_, TF_ES_SYNC | TF_ES_READWRITE,
                [self, target, key, &applied](TfEditCookie cookie) {
                    auto next = self->engine_;
                    const auto result = next.handle(*key);
                    const auto status = self->apply(target.Get(), cookie, result.committed, next);
                    if (SUCCEEDED(status)) { self->engine_ = std::move(next); applied = true; }
                    return status;
                });
            // A refused synchronous session leaves both the key and engine untouched.
            if (SUCCEEDED(hr) && applied) {
                *eaten = TRUE;
                if (code < eaten_keys_.size()) eaten_keys_.set(code);
            }
        } catch (...) { return E_FAIL; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext*, WPARAM code, LPARAM, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = code < eaten_keys_.size() && eaten_keys_.test(code);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM code, LPARAM parameter, BOOL* eaten) override {
        const auto hr = OnTestKeyUp(context, code, parameter, eaten);
        if (code < eaten_keys_.size()) eaten_keys_.reset(code);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = FALSE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie cookie, ITfComposition* composition) override {
        if (composition_.Get() != composition || ending_) return S_OK;
        try {
            ComPtr<ITfRange> range;
            if (SUCCEEDED(composition->GetRange(&range))) {
                const auto text = utf16(engine_.literal_text());
                range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
            }
        } catch (...) { /* Do not allow a C++ exception to cross the COM boundary. */ }
        composition_.Reset(); composition_context_.Reset(); engine_.reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* document) override {
        if (composition_context_) {
            ComPtr<ITfDocumentMgr> owner;
            if (SUCCEEDED(composition_context_->GetDocumentMgr(&owner)) && owner.Get() == document) finish_composition();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* document, ITfDocumentMgr*) override {
        ComPtr<ITfContext> context;
        if (document) document->GetTop(&context);
        if (composition_context_ && composition_context_.Get() != context.Get()) finish_composition();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override {
        if (composition_context_ && composition_context_.Get() != context) finish_composition();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override {
        if (composition_context_.Get() == context) finish_composition();
        return S_OK;
    }
private:
    HRESULT apply(ITfContext* context, TfEditCookie cookie, std::u32string_view committed, Engine& next) {
        const auto preedit = next.preedit();
        const auto text = utf16(std::u32string(committed) + std::u32string(preedit));
        ComPtr<ITfRange> range;
        bool created = false;
        HRESULT hr;
        if (composition_) {
            hr = composition_->GetRange(&range);
            if (FAILED(hr)) return hr;
        } else {
            if (text.empty()) return S_OK;
            if (preedit.empty()) {
                ComPtr<ITfInsertAtSelection> insert;
                hr = context->QueryInterface(IID_PPV_ARGS(&insert));
                if (FAILED(hr)) return hr;
                hr = insert->InsertTextAtSelection(cookie, 0, text.c_str(), static_cast<LONG>(text.size()), &range);
                if (FAILED(hr)) return hr;
                // The insertion succeeded; do not pass this key through on caret failure.
                select_end(context, cookie, range.Get());
                return S_OK;
            }
            TF_SELECTION selection{};
            ULONG count = 0;
            hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count);
            if (FAILED(hr)) return hr;
            if (count != 1 || !selection.range) return E_FAIL;
            range.Attach(selection.range);
            ComPtr<ITfContextComposition> compositions;
            hr = context->QueryInterface(IID_PPV_ARGS(&compositions));
            if (FAILED(hr)) return hr;
            hr = compositions->StartComposition(cookie, range.Get(), this, &composition_);
            if (FAILED(hr)) return hr;
            if (!composition_) return E_FAIL; // Host rejected composition (S_OK + null).
            composition_context_ = context;
            created = true;
        }
        hr = range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
        if (FAILED(hr)) {
            if (created) {
                ending_ = true; composition_->EndComposition(cookie); ending_ = false;
                composition_.Reset(); composition_context_.Reset();
            }
            return hr;
        }
        HRESULT post_status = S_OK;
        if (preedit.empty()) {
            ending_ = true;
            post_status = composition_->EndComposition(cookie);
            ending_ = false;
            if (SUCCEEDED(post_status)) { composition_.Reset(); composition_context_.Reset(); }
        } else if (!committed.empty()) {
            ComPtr<ITfRange> start;
            post_status = range->Clone(&start);
            if (SUCCEEDED(post_status)) {
                LONG shifted = 0;
                const auto length = static_cast<LONG>(utf16(committed).size());
                post_status = start->ShiftStart(cookie, length, &shifted, nullptr);
                if (SUCCEEDED(post_status) && shifted != length) post_status = E_FAIL;
                if (SUCCEEDED(post_status)) post_status = composition_->ShiftStart(cookie, start.Get());
            }
        }
        const auto caret_status = select_end(context, cookie, range.Get());
        if (FAILED(post_status) || FAILED(caret_status)) {
            // Preserve the text as plain text and abandon this composition if the
            // host cannot maintain its range. Never overwrite committed text on
            // the next key, and never replay a key whose SetText already succeeded.
            const auto plain = utf16(std::u32string(committed) + next.literal_text());
            range->SetText(cookie, 0, plain.c_str(), static_cast<LONG>(plain.size()));
            if (composition_) {
                ending_ = true; composition_->EndComposition(cookie); ending_ = false;
            }
            composition_.Reset(); composition_context_.Reset(); next.reset();
        }
        // Once SetText succeeds, consume the key even if a subsequent host operation fails.
        return S_OK;
    }
    void finish_composition() noexcept {
        try {
            if (!composition_ || !composition_context_) { engine_.reset(); return; }
            auto composition = composition_;
            auto context = composition_context_;
            auto text = utf16(engine_.literal_text());
            // Detach before scheduling: this session must never operate on a new input field.
            composition_.Reset(); composition_context_.Reset(); engine_.reset();
            request_edit(context.Get(), client_, TF_ES_ASYNC | TF_ES_READWRITE,
                [composition, context, text = std::move(text)](TfEditCookie cookie) {
                    ComPtr<ITfRange> range;
                    auto hr = composition->GetRange(&range);
                    if (SUCCEEDED(hr)) hr = range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
                    const auto end_status = composition->EndComposition(cookie);
                    return FAILED(hr) ? hr : end_status;
                });
        } catch (...) { composition_.Reset(); composition_context_.Reset(); engine_.reset(); }
    }
    std::atomic<ULONG> refs_{1};
    ComPtr<ITfThreadMgr> manager_;
    ComPtr<ITfKeystrokeMgr> keys_;
    TfClientId client_ = TF_CLIENTID_NULL;
    DWORD sink_cookie_ = TF_INVALID_COOKIE;
    ComPtr<ITfComposition> composition_;
    ComPtr<ITfContext> composition_context_;
    Engine engine_;
    std::bitset<256> eaten_keys_;
    bool ending_ = false;
};
}
HRESULT create_text_service(REFIID iid, void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    try {
        auto* service = new TextService;
        const auto hr = service->QueryInterface(iid, output);
        service->Release(); return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
} // namespace nskkzime::windows
