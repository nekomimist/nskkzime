#include "module.h"
#include "nskkzime/engine.h"
#include "runtime_dictionary.h"
#include "learning_client.h"
#include <initguid.h>
#include <inputscope.h>
#include "display_attributes.h"
#include "candidate_ui.h"
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
    case VK_NEXT: case VK_PRIOR:
        if (GetKeyState(VK_SHIFT) & 0x8000) return {}; // Preserve application range-selection shortcuts.
        return Key{code == VK_NEXT ? KeyCode::PageDown : KeyCode::PageUp};
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

bool learning_allowed(ITfContext* context, ITfComposition* composition, TfEditCookie cookie, std::wstring& reason) {
    // Inspect the entire conversion before modifying it. A caret at its end can
    // be outside the scope. Mixed/absent/failed values suppress persistence.
    reason = L"変換範囲を取得できません";
    ComPtr<ITfRange> range;
    if (!composition || FAILED(composition->GetRange(&range)) || !range) return false;
    ComPtr<ITfReadOnlyProperty> property;
    const auto property_hr = context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property);
    reason = L"入力属性を取得できない入力欄";
    if (FAILED(property_hr) || !property) return false;
    VARIANT value; VariantInit(&value);
    const auto hr = property->GetValue(cookie, range.Get(), &value);
    reason = L"入力属性が不明または範囲内で異なる入力欄";
    bool allowed = false;
    if (hr == S_OK && value.vt == VT_UNKNOWN && value.punkVal) {
        ComPtr<ITfInputScope> scope;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&scope)))) {
            InputScope* values = nullptr; UINT count = 0;
            if (scope->GetInputScopes(&values, &count) == S_OK && count > 0 && values) {
                allowed = true;
                for (UINT i = 0; i < count; ++i) {
                    if (values[i] == IS_PASSWORD || values[i] == IS_PRIVATE || values[i] == IS_NUMERIC_PASSWORD ||
                        values[i] == IS_NUMERIC_PIN || values[i] == IS_ALPHANUMERIC_PIN || values[i] == IS_ALPHANUMERIC_PIN_SET)
                        { allowed = false; reason = L"パスワードまたはプライベート入力欄"; }
                }
            }
            CoTaskMemFree(values);
        }
    }
    VariantClear(&value); return allowed;
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
bool finishes_before_key(WPARAM code) {
    if (GetKeyState(VK_CONTROL) & 0x8000) return code != VK_CONTROL && code != VK_LCONTROL && code != VK_RCONTROL;
    switch (code) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_DELETE: case VK_TAB: return true;
    default: return false;
    }
}
struct PendingComposition {
    ComPtr<ITfComposition> composition;
    ComPtr<ITfContext> context;
    bool done = false;
    HRESULT finish(TfEditCookie cookie) noexcept {
        if (done) return S_OK;
        ComPtr<ITfRange> range;
        if (SUCCEEDED(composition->GetRange(&range))) clear_display_attribute(context.Get(), cookie, range.Get());
        const auto hr = composition->EndComposition(cookie);
        if (SUCCEEDED(hr)) { done = true; composition.Reset(); }
        return hr;
    }
};

class TextService final : public ITfTextInputProcessorEx, public ITfKeyEventSink,
                          public ITfCompositionSink, public ITfThreadMgrEventSink,
                          public ITfTextEditSink, public ITfTextLayoutSink, public ITfDisplayAttributeProvider {
public:
    TextService() : engine_(std::make_shared<MemoryDictionary>()) { ++module_objects; }
    ~TextService() { --module_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfTextInputProcessor || iid == IID_ITfTextInputProcessorEx)
            *output = static_cast<ITfTextInputProcessorEx*>(this);
        else if (iid == IID_ITfKeyEventSink) *output = static_cast<ITfKeyEventSink*>(this);
        else if (iid == IID_ITfCompositionSink) *output = static_cast<ITfCompositionSink*>(this);
        else if (iid == IID_ITfThreadMgrEventSink) *output = static_cast<ITfThreadMgrEventSink*>(this);
        else if (iid == IID_ITfTextEditSink) *output = static_cast<ITfTextEditSink*>(this);
        else if (iid == IID_ITfTextLayoutSink) *output = static_cast<ITfTextLayoutSink*>(this);
        else if (iid == IID_ITfDisplayAttributeProvider) *output = static_cast<ITfDisplayAttributeProvider*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* manager, TfClientId client) override { return ActivateEx(manager, client, 0); }
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* manager, TfClientId client, DWORD) override {
        if (!manager || client == TF_CLIENTID_NULL) return E_INVALIDARG;
        if (manager_) return E_UNEXPECTED;
        if (!dictionary_loaded_) {
            // Display-attribute-only COM instances never load a dictionary.
            try {
                dictionary_ = runtime_dictionary();
                engine_ = Engine(dictionary_, dynamic_cast<const LearningDictionary*>(dictionary_.get()) != nullptr);
                dictionary_loaded_ = true;
            }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            catch (...) { return E_FAIL; }
        }
        manager_ = manager; client_ = client;
        auto hr = manager_.As(&keys_);
        if (SUCCEEDED(hr)) hr = keys_->AdviseKeyEventSink(client_, this, TRUE);
        if (FAILED(hr)) { keys_.Reset(); manager_.Reset(); client_ = TF_CLIENTID_NULL; return hr; }
        ComPtr<ITfSource> source;
        hr = manager_.As(&source);
        if (SUCCEEDED(hr)) hr = source->AdviseSink(IID_ITfThreadMgrEventSink,
            static_cast<ITfThreadMgrEventSink*>(this), &sink_cookie_);
        if (FAILED(hr)) Deactivate();
        if (SUCCEEDED(hr)) display_attribute_atoms(input_atom_, converted_atom_);
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
        if (key) {
            if (composition_context_ && composition_context_.Get() != context) {
                try { auto fresh = engine_; fresh.reset(); *eaten = fresh.would_handle(*key); }
                catch (...) { return E_OUTOFMEMORY; }
            } else *eaten = engine_.would_handle(*key);
        }
        if (!*eaten && composition_ && finishes_before_key(code)) {
            // A native host can consume the first application key if it arrives
            // while still composing. Finish during the preflight and decline
            // the key. This lifecycle cleanup is idempotent; ordinary SKK key
            // probes never advance the engine or edit the document.
            finish_composition(composition_context_.Get() == context ? TF_ES_ASYNCDONTCARE : TF_ES_ASYNC);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM code, LPARAM parameter, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        if (composition_context_ && composition_context_.Get() != context) finish_composition();
        const auto key = translate_key(code, parameter);
        if (!key || !engine_.would_handle(*key)) return S_OK;
        try {
            const ComPtr<TextService> self(this);
            const ComPtr<ITfContext> target(context);
            bool applied = false;
            const auto hr = request_edit(context, client_, TF_ES_SYNC | TF_ES_READWRITE,
                [self, target, key, &applied](TfEditCookie cookie) {
                    for (const auto& pending : self->pending_) {
                        if (!pending->done && pending->context.Get() == target.Get()) {
                            const auto status = pending->finish(cookie);
                            if (FAILED(status)) return status;
                        }
                    }
                    std::erase_if(self->pending_, [](const auto& pending) { return pending->done; });
                    if (self->composition_ && !self->composition_valid(cookie)) {
                        self->end_composition(cookie);
                    }
                    auto next = self->engine_;
                    const auto result = next.handle(*key);
                    if (!result.consumed) return S_OK;
                    std::wstring learning_reason;
                    const bool starting_registration = next.registration_depth() > self->engine_.registration_depth();
                    const bool persistence_allowed = (result.learning || starting_registration) &&
                        learning_allowed(target.Get(), self->composition_.Get(), cookie, learning_reason);
                    if (starting_registration && !persistence_allowed) next.abandon_registration();
                    const auto status = self->apply(target.Get(), cookie, result.committed, next);
                    if (SUCCEEDED(status)) {
                        self->engine_ = std::move(next); applied = true;
                        if (result.learning && persistence_allowed) {
                            publish_learning(self->dictionary_, *result.learning);
                            if (result.registration) self->engine_.remember_registration(*result.learning);
                        }
                        else if (result.learning || (starting_registration && !persistence_allowed)) {
                            if (const auto* learning = dynamic_cast<const LearningDictionary*>(self->dictionary_.get())) learning->skipped(std::move(learning_reason));
                        }
                        self->refresh_candidates(cookie);
                    }
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
                clear_display_attribute(composition_context_.Get(), cookie, range.Get());
            }
        } catch (...) { /* Do not allow a C++ exception to cross the COM boundary. */ }
        detach_composition(); engine_.reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context, TfEditCookie cookie, ITfEditRecord*) override {
        if (ending_ || context != composition_context_.Get() || !composition_) return S_OK;
        try { if (!composition_valid(cookie)) finish_composition(); }
        catch (...) { finish_composition(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView*) override {
        if (context != composition_context_.Get() || !composition_) return S_OK;
        if (code == TF_LC_DESTROY) {
            ++layout_generation_; layout_pending_ = false;
            candidates_.hide(); return S_OK;
        }
        if (layout_pending_) return S_OK;
        try {
            const auto generation = layout_generation_;
            const ComPtr<TextService> self(this);
            layout_pending_ = true;
            const auto status = request_edit(context, client_, TF_ES_ASYNC | TF_ES_READ,
                [self, generation](TfEditCookie cookie) {
                    if (self->layout_generation_ != generation) return S_OK;
                    self->layout_pending_ = false;
                    if (self->composition_ && (self->engine_.stage() == Stage::Selecting || self->engine_.registering()))
                        self->candidates_.locate(self->composition_context_.Get(), self->composition_.Get(), cookie);
                    return S_OK;
                });
            if (FAILED(status)) layout_pending_ = false;
        } catch (...) { layout_pending_ = false; candidates_.hide(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** output) override {
        return enum_display_attributes(output);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** output) override {
        return get_display_attribute(guid, output);
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
    void refresh_candidates(TfEditCookie cookie) noexcept {
        candidates_.update(manager_.Get(), composition_context_.Get(), engine_);
        candidates_.locate(composition_context_.Get(), composition_.Get(), cookie);
        if (engine_.registering() && !candidates_.presentation_available()) {
            // The host text is still the outer reading. Never capture more keys
            // in an editor the user cannot see, even when popup creation fails.
            engine_.abandon_registration(); candidates_.close();
        }
    }
    bool composition_valid(TfEditCookie cookie) const {
        ComPtr<ITfRange> range;
        return composition_ && SUCCEEDED(composition_->GetRange(&range)) && range &&
            range_matches(range.Get(), cookie, expected_text_) && selection_at_end(composition_context_.Get(), cookie, range.Get());
    }
    void detach_composition() noexcept {
        ++layout_generation_; layout_pending_ = false;
        candidates_.close();
        if (composition_context_ && layout_sink_cookie_ != TF_INVALID_COOKIE) {
            ComPtr<ITfSource> source;
            if (SUCCEEDED(composition_context_.As(&source))) source->UnadviseSink(layout_sink_cookie_);
        }
        layout_sink_cookie_ = TF_INVALID_COOKIE;
        if (composition_context_ && edit_sink_cookie_ != TF_INVALID_COOKIE) {
            ComPtr<ITfSource> source;
            if (SUCCEEDED(composition_context_.As(&source))) source->UnadviseSink(edit_sink_cookie_);
        }
        edit_sink_cookie_ = TF_INVALID_COOKIE;
        composition_.Reset(); composition_context_.Reset(); expected_text_.clear();
    }
    void end_composition(TfEditCookie cookie) noexcept {
        if (composition_) {
            ComPtr<ITfRange> range;
            if (SUCCEEDED(composition_->GetRange(&range))) clear_display_attribute(composition_context_.Get(), cookie, range.Get());
            ending_ = true; composition_->EndComposition(cookie); ending_ = false;
        }
        detach_composition(); engine_.reset();
    }
    HRESULT apply(ITfContext* context, TfEditCookie cookie, std::u32string_view committed, Engine& next) {
        const auto preedit = next.composition_text();
        // An empty reading (notably just after '/') still owns an input range.
        // Track it so moving the caret or focus cannot carry its mode elsewhere.
        const bool needs_composition = next.stage() != Stage::Direct || !preedit.empty();
        auto expected = utf16(preedit);
        const auto text = utf16(std::u32string(committed) + std::u32string(preedit));
        ComPtr<ITfRange> range;
        bool created = false;
        HRESULT hr;
        if (composition_) {
            hr = composition_->GetRange(&range);
            if (FAILED(hr)) return hr;
        } else {
            if (text.empty() && !needs_composition) return S_OK;
            if (!needs_composition) {
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
            range.Attach(selection.range);
            if (FAILED(hr)) return hr;
            if (count != 1 || !range) return E_FAIL;
            ComPtr<ITfContextComposition> compositions;
            hr = context->QueryInterface(IID_PPV_ARGS(&compositions));
            if (FAILED(hr)) return hr;
            ComPtr<ITfSource> source;
            hr = context->QueryInterface(IID_PPV_ARGS(&source));
            if (SUCCEEDED(hr)) hr = source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &edit_sink_cookie_);
            if (FAILED(hr)) return hr;
            composition_context_ = context;
            // Layout notifications are optional: input still works if the host
            // cannot supply positions, and the candidate UI then stays hidden.
            source->AdviseSink(IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink*>(this), &layout_sink_cookie_);
            hr = compositions->StartComposition(cookie, range.Get(), this, &composition_);
            if (FAILED(hr) || !composition_) { detach_composition(); return FAILED(hr) ? hr : E_FAIL; }
            created = true;
        }
        hr = range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
        if (FAILED(hr)) {
            if (created) {
                ending_ = true; composition_->EndComposition(cookie); ending_ = false;
                detach_composition();
            }
            return hr;
        }
        HRESULT post_status = S_OK;
        clear_display_attribute(context, cookie, range.Get());
        if (!needs_composition) {
            ending_ = true;
            post_status = composition_->EndComposition(cookie);
            ending_ = false;
            if (SUCCEEDED(post_status)) detach_composition();
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
        expected_text_ = std::move(expected);
        if (composition_ && SUCCEEDED(post_status)) {
            ComPtr<ITfRange> composing;
            if (SUCCEEDED(composition_->GetRange(&composing)))
                set_display_attribute(context, cookie, composing.Get(), next.stage() == Stage::Selecting ? converted_atom_ : input_atom_);
        }
        const auto caret_status = select_end(context, cookie, range.Get());
        if (FAILED(post_status) || FAILED(caret_status)) {
            // Preserve the text as plain text and abandon this composition if the
            // host cannot maintain its range. Never overwrite committed text on
            // the next key, and never replay a key whose SetText already succeeded.
            end_composition(cookie); next.reset();
        }
        // Once SetText succeeds, consume the key even if a subsequent host operation fails.
        return S_OK;
    }
    void finish_composition(DWORD scheduling = TF_ES_ASYNC) noexcept {
        try {
            if (!composition_ || !composition_context_) { engine_.reset(); return; }
            auto pending = std::make_shared<PendingComposition>();
            pending->composition = composition_; pending->context = composition_context_;
            std::erase_if(pending_, [](const auto& item) { return item->done; });
            pending_.push_back(pending);
            // Detach before scheduling: this session must never operate on a new input field.
            detach_composition(); engine_.reset();
            const auto status = request_edit(pending->context.Get(), client_, scheduling | TF_ES_READWRITE,
                [pending](TfEditCookie cookie) { return pending->finish(cookie); });
            if (FAILED(status)) std::erase(pending_, pending);
        } catch (...) { detach_composition(); engine_.reset(); }
    }
    std::atomic<ULONG> refs_{1};
    ComPtr<ITfThreadMgr> manager_;
    ComPtr<ITfKeystrokeMgr> keys_;
    TfClientId client_ = TF_CLIENTID_NULL;
    DWORD sink_cookie_ = TF_INVALID_COOKIE;
    DWORD edit_sink_cookie_ = TF_INVALID_COOKIE;
    DWORD layout_sink_cookie_ = TF_INVALID_COOKIE;
    ComPtr<ITfComposition> composition_;
    ComPtr<ITfContext> composition_context_;
    Engine engine_;
    std::shared_ptr<const Dictionary> dictionary_;
    std::wstring expected_text_;
    std::vector<std::shared_ptr<PendingComposition>> pending_;
    CandidatePresenter candidates_;
    std::uint64_t layout_generation_ = 0;
    bool layout_pending_ = false;
    TfGuidAtom input_atom_ = TF_INVALID_GUIDATOM;
    TfGuidAtom converted_atom_ = TF_INVALID_GUIDATOM;
    std::bitset<256> eaten_keys_;
    bool ending_ = false;
    bool dictionary_loaded_ = false;
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
