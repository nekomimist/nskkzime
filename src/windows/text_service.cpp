#include "module.h"
#include "nskkzime/engine.h"
#include "runtime_dictionary.h"
#include "learning_client.h"
#include <initguid.h>
#include <inputscope.h>
#include "display_attributes.h"
#include "candidate_ui.h"
#include "input_mode.h"
#include "context_text.h"
#include "inference_client.h"
#include "romaji_settings.h"
#include <wrl/client.h>
#include <algorithm>
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
std::optional<Mode> mode_key(WPARAM code, Mode current) {
    if (GetKeyState(VK_MENU) & 0x8000 || GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000 || GetKeyState(VK_SHIFT) & 0x8000) return {};
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if ((code == VK_SPACE && control) || (code == VK_KANJI && !control))
        return current == Mode::Latin ? Mode::Hiragana : Mode::Latin;
    if (!control && code == VK_IME_ON) return Mode::Hiragana;
    if (!control && code == VK_IME_OFF) return Mode::Latin;
    return {};
}

bool learning_allowed(ITfContext* context, ITfComposition* composition, TfEditCookie cookie, std::wstring& reason) {
    // Inspect the entire conversion before modifying it. A caret at its end can
    // be outside the scope. Mixed/absent/failed values suppress persistence.
    reason = L"変換範囲を取得できません";
    ComPtr<ITfRange> range;
    if (!composition || FAILED(composition->GetRange(&range)) || !range) return false;
    const auto permission = input_permission(context, range.Get(), cookie);
    reason = L"入力属性が不明または範囲内で異なる入力欄";
    if (permission == InputPermission::Restricted) reason = L"パスワードまたはプライベート入力欄";
    return permission == InputPermission::Allowed;
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
                engine_ = Engine(dictionary_, true, runtime_romaji_rules());
                tool_commands_ = current_tool_commands();
                dictionary_loaded_ = true;
            }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            catch (...) { return E_FAIL; }
        }
        try {
            candidate_actions_ = std::make_shared<CandidateActions>();
            candidate_actions_->select = [this](std::size_t index, std::uint64_t revision) { select_candidate(index, revision); };
            candidate_actions_->interact = [this](std::uint64_t revision) {
                if (engine_.revision() == revision && inference_pending_) cancel_inference();
            };
        } catch (...) { candidate_actions_.reset(); return E_OUTOFMEMORY; }
        manager_ = manager; client_ = client;
        auto hr = manager_.As(&keys_);
        if (SUCCEEDED(hr)) hr = keys_->AdviseKeyEventSink(client_, this, TRUE);
        if (FAILED(hr)) { candidate_actions_.reset(); keys_.Reset(); manager_.Reset(); client_ = TF_CLIENTID_NULL; return hr; }
        ComPtr<ITfSource> source;
        hr = manager_.As(&source);
        if (SUCCEEDED(hr)) hr = source->AdviseSink(IID_ITfThreadMgrEventSink,
            static_cast<ITfThreadMgrEventSink*>(this), &sink_cookie_);
        if (FAILED(hr)) Deactivate();
        if (SUCCEEDED(hr)) {
            display_attribute_atoms(input_atom_, converted_atom_);
            try {
                mode_actions_ = std::make_shared<ModeActions>();
                mode_actions_->change = [this](Mode mode) { change_mode(mode); };
                tool_launcher_ = std::make_shared<ToolLauncher>(tool_commands_);
                mode_actions_->tools_available = {tool_launcher_->available(ImeTool::Settings), tool_launcher_->available(ImeTool::Dictionary)};
                mode_actions_->open_tool = [launcher = tool_launcher_](ImeTool tool) { return launcher->open(tool); };
                input_mode_.Attach(new InputMode);
                input_mode_->connect(manager_.Get(), client_, mode_actions_, engine_.mode());
                for (std::size_t i = 0; i < mode_keys.size(); ++i) {
                    constexpr wchar_t description[] = L"nskkzime オン／オフ";
                    preserved_[i] = SUCCEEDED(keys_->PreserveKey(client_, mode_key_ids[i], &mode_keys[i], description, ARRAYSIZE(description) - 1));
                }
                try {
                    if (const auto config = current_inference_config()) {
                        inference_dummy_ = config->model.empty();
                        inference_policy_ = config->policy;
                        inference_ = std::make_unique<InferenceClient>(*config,[this](InferenceCompletion result) { receive_inference(std::move(result)); });
                        set_inference_status(this,inference_dummy_ ? L"推論: ダミーサーバーを使用（辞書候補の枯渇時）" : L"推論: zenzを使用");
                    }
                } catch (...) { set_inference_status(this,L"推論: 起動できません。辞書入力を続けます"); }
            } catch (...) { Deactivate(); return E_OUTOFMEMORY; }
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        if (tool_launcher_) { tool_launcher_->close(); tool_launcher_.reset(); }
        mode_actions_.reset();
        auto mode = std::move(input_mode_); if (mode) mode->disconnect();
        for (std::size_t i = 0; i < mode_keys.size(); ++i) {
            if (preserved_[i] && keys_) keys_->UnpreserveKey(mode_key_ids[i], &mode_keys[i]);
            preserved_[i] = false;
        }
        candidate_actions_.reset();
        finish_composition();
        inference_.reset(); clear_inference_status(this);
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
        if (mode_key(code, engine_.outer_mode())) { *eaten = TRUE; return S_OK; }
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
        const KeyTiming timing;
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        if (const auto mode = mode_key(code, engine_.outer_mode())) {
            if (!(parameter & (static_cast<LPARAM>(1) << 30))) change_mode(*mode);
            *eaten = TRUE; if (code < eaten_keys_.size()) eaten_keys_.set(code); return S_OK;
        }
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
                    return self->apply_key(target.Get(), cookie, self->candidates_.selection_key(*key, self->engine_), applied);
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
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID guid, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        for (std::size_t i = 0; i < mode_keys.size(); ++i) if (preserved_[i] && guid == mode_key_ids[i]) {
            change_mode(engine_.outer_mode() == Mode::Latin ? Mode::Hiragana : Mode::Latin);
            *eaten = TRUE; break;
        }
        return S_OK;
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
        const ComPtr<TextService> self(this);
        try {
            if (!composition_valid(cookie)) finish_composition();
            else if (!reading_context_) {
                reading_context_ = true;
                const auto current = composition_; const auto revision = engine_.revision();
                ComPtr<ITfRange> range;
                const auto pending = inference_pending_;
                const bool preview = context_preview_enabled() && (engine_.stage() == Stage::Selecting || engine_.registering());
                clear_context_preview(this);
                if ((pending || preview) && SUCCEEDED(current->GetRange(&range)) && range) {
                    auto text = surrounding_text(context, range.Get(), cookie);
                    // Property callbacks may deactivate or replace the composition.
                    if (composition_.Get() == current.Get() && composition_context_.Get() == context && engine_.revision() == revision) {
                        if (pending && inference_pending_ && pending->id == inference_pending_->id && text != pending->text) cancel_inference();
                        if (preview) publish_context_preview(this, std::move(text));
                    }
                } else if (pending) cancel_inference();
                reading_context_ = false;
            }
        }
        catch (...) { reading_context_ = false; clear_context_preview(this); finish_composition(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView*) override {
        if (context != composition_context_.Get() || !composition_) return S_OK;
        if (code == TF_LC_DESTROY) {
            cancel_inference();
            ++layout_generation_; layout_pending_ = false;
            ++selection_request_; selection_pending_ = false;
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
    struct PendingInference {
        std::uint64_t id, revision, session, generation;
        ULONGLONG deadline;
        ComPtr<ITfContext> context;
        ComPtr<ITfComposition> composition;
        SurroundingText text;
        PlannedInference plan;
    };
    void cancel_inference() noexcept {
        ++context_generation_;
        if (inference_pending_) set_inference_status(this,L"推論: 入力状態が変わったため応答を破棄");
        inference_pending_.reset(); if (inference_) inference_->cancel();
    }
    bool matches(const PendingInference& pending) const {
        return manager_ && inference_pending_ && inference_pending_->id == pending.id && engine_.revision() == pending.revision &&
            composition_session_ == pending.session && context_generation_ == pending.generation &&
            composition_context_.Get() == pending.context.Get() && composition_.Get() == pending.composition.Get();
    }
    void receive_inference(InferenceCompletion result) noexcept {
        if (!inference_pending_ || result.id != inference_pending_->id) return;
        const auto id = result.id;
        try {
            const ComPtr<TextService> self(this); const auto pending = *inference_pending_;
            const auto hr = request_edit(pending.context.Get(),client_,TF_ES_ASYNC | TF_ES_READWRITE,
                [self,pending,result=std::move(result)](TfEditCookie cookie) {
                    if (!self->matches(pending)) return S_OK;
                    if (!editable(pending.context.Get()) || !self->composition_valid(cookie)) { self->cancel_inference(); return S_OK; }
                    ComPtr<ITfRange> range;
                    if (FAILED(pending.composition->GetRange(&range)) || !range) { self->cancel_inference(); return S_OK; }
                    const auto text = surrounding_text(pending.context.Get(),range.Get(),cookie);
                    if (!self->matches(pending)) return S_OK;
                    if (text != pending.text || text.permission != InputPermission::Allowed) { self->cancel_inference(); return S_OK; }
                    auto next = self->engine_; bool adjusted = false;
                    const bool timed_out = GetTickCount64() >= pending.deadline || result.status == InferenceStatus::Timeout;
                    if (!timed_out && result.status == InferenceStatus::Ok) {
                        adjusted = apply_inference_result(next,pending.plan,result.response,self->inference_policy_,self->inference_dummy_);
                    }
                    self->cancel_inference();
                    const bool fallback = pending.plan.purpose == InferencePurpose::Fallback ||
                        (pending.plan.purpose == InferencePurpose::LongReading && next.candidates().empty());
                    const bool changed = adjusted || (fallback && next.resume_registration());
                    if (changed && FAILED(self->apply(pending.context.Get(),cookie,U"",next))) {
                        set_inference_status(self.Get(),L"推論: 応答を反映できません。現在の入力を維持します");
                        return S_OK;
                    }
                    if (changed) { self->engine_ = std::move(next); self->refresh_candidates(cookie); }
                    try { if (self->inference_ && self->inference_->input_log_enabled() && self->composition_context_.Get() == pending.context.Get()) {
                        // Applying text/UI can re-enter the host. Recheck privacy
                        // and identity after it, before queuing any text for disk.
                        const auto current = self->composition_;
                        const auto revision = self->engine_.revision();
                        const auto generation = self->context_generation_;
                        ComPtr<ITfRange> active;
                        if (current && SUCCEEDED(current->GetRange(&active)) && active) {
                            const auto allowed = surrounding_text(pending.context.Get(), active.Get(), cookie);
                            if (self->inference_ && self->composition_.Get() == current.Get() && self->engine_.revision() == revision && self->context_generation_ == generation &&
                                allowed.permission == InputPermission::Allowed && allowed.before == pending.text.before && allowed.after == pending.text.after)
                                self->inference_->record_input(pending.plan.request,
                                    !timed_out && result.status == InferenceStatus::Ok ? &result.response : nullptr,
                                    timed_out ? InferenceStatus::Timeout : result.status);
                        }
                    } } catch (...) { /* Diagnostics must not fail an already applied edit. */ }
                    set_inference_status(self.Get(),adjusted ? (pending.plan.purpose == InferencePurpose::Rerank ? L"推論: 最良の辞書候補を先頭へ移動" : L"推論: 生成候補を反映") :
                        timed_out ? L"推論: 時間切れ。通常のSKK操作へ復帰" : L"推論: 現在の辞書候補または読みで継続");
                    return S_OK;
                });
            if (FAILED(hr) && matches(pending)) cancel_inference();
        } catch (...) { if (inference_pending_ && inference_pending_->id == id) cancel_inference(); }
    }
    void change_mode(Mode mode) noexcept {
        const ComPtr<TextService> self(this);
        if (!manager_) return;
        if (engine_.mode() != mode || engine_.registering()) {
            finish_composition(); engine_.set_mode(mode);
        }
        const auto binding = input_mode_; if (binding) binding->publish(engine_.mode());
    }
    HRESULT apply_key(ITfContext* target, TfEditCookie cookie, Key key, bool& applied) {
        auto next = engine_;
        const auto result = next.handle(key);
        if (!result.consumed) return S_OK;
        const auto revision = engine_.revision();
        const auto current = composition_;
        const bool was_pending = inference_pending_.has_value();
        std::wstring learning_reason;
        const bool starting_registration = next.registration_depth() > engine_.registration_depth();
        const bool starting_forget = next.forgetting() && !engine_.forgetting() && next.forget_target().has_value();
        const bool persistence_allowed = (result.learning || result.forgetting || starting_registration || starting_forget) &&
            learning_allowed(target, current.Get(), cookie, learning_reason);
        // Property callbacks can end or replace the composition while answering.
        if (engine_.revision() != revision || composition_.Get() != current.Get()) return S_OK;
        if (starting_registration && !persistence_allowed) next.abandon_registration();
        if (starting_forget && !persistence_allowed) next.cancel_forgetting();
        std::optional<SurroundingText> inference_text;
        auto plan = inference_ && !was_pending ? plan_inference(engine_,next,result,inference_policy_) : std::nullopt;
        if (plan) {
            ComPtr<ITfRange> range;
            if (current && SUCCEEDED(current->GetRange(&range)) && range) {
                auto text = surrounding_text(target,range.Get(),cookie);
                if (text.permission == InputPermission::Allowed) {
                    inference_text = std::move(text); next.defer_registration();
                    if (plan->purpose != InferencePurpose::Rerank) next.note_generation_attempt();
                }
                else set_inference_status(this,L"推論: 保護された、または属性不明の入力欄のため送信しません");
            }
            if (engine_.revision() != revision || composition_.Get() != current.Get()) return S_OK;
        }
        const auto status = apply(target, cookie, result.committed, next);
        if (SUCCEEDED(status)) {
            cancel_inference();
            engine_ = std::move(next); applied = true;
            if (result.forgetting && persistence_allowed && publish_runtime_forgetting(dictionary_, *result.forgetting))
                engine_.forget_registration(*result.forgetting);
            if (result.learning && persistence_allowed) {
                publish_runtime_learning(dictionary_, *result.learning);
                if (result.registration) engine_.remember_registration(*result.learning);
            } else if (result.learning || ((result.forgetting || starting_registration || starting_forget) && !persistence_allowed)) {
                skip_runtime_learning(dictionary_, std::move(learning_reason));
            }
            refresh_candidates(cookie);
            // Inner registration input has its own modes. Keep Windows open
            // until the outer editor is completed or explicitly abandoned.
            const auto binding = input_mode_;
            if (binding && !engine_.registering()) binding->publish(engine_.mode());
            if (inference_text && inference_ && composition_ && !engine_.registering()) {
                try {
                    const auto id = ++inference_sequence_;
                    plan->request.id = id; plan->request.left = inference_text->before; plan->request.right = inference_text->after;
                    const auto deadline = inference_->submit(plan->request,plan->purpose == InferencePurpose::Rerank ? inference_policy_.score_timeout_ms : 0);
                    inference_pending_ = PendingInference{id,engine_.revision(),composition_session_,context_generation_,deadline,composition_context_,composition_,std::move(*inference_text),std::move(*plan)};
                    set_inference_status(this,L"推論: 応答待ち（入力は継続できます）");
                } catch (...) { set_inference_status(this,L"推論: 要求を開始できません。Spaceで通常のSKK操作へ復帰"); }
            }
        }
        return status;
    }
    void select_candidate(std::size_t index, std::uint64_t revision) noexcept {
        if (selection_pending_ || !manager_ || !composition_ || !composition_context_ ||
            engine_.revision() != revision || engine_.forgetting() || engine_.input_stage() != Stage::Selecting || index >= engine_.candidates().size()) return;
        cancel_inference();
        const auto request = ++selection_request_;
        selection_pending_ = true;
        try {
            const ComPtr<TextService> self(this);
            const auto target = composition_context_;
            const auto current = composition_;
            const auto status = request_edit(target.Get(), client_, TF_ES_ASYNC | TF_ES_READWRITE,
                [self, target, current, index, revision, request](TfEditCookie cookie) {
                    if (self->selection_request_ != request) return S_OK;
                    self->selection_pending_ = false;
                    if (!self->manager_ || self->composition_context_.Get() != target.Get() ||
                        self->composition_.Get() != current.Get() || self->engine_.revision() != revision ||
                        self->engine_.input_stage() != Stage::Selecting || !editable(target.Get())) return S_OK;
                    if (!self->composition_valid(cookie)) { self->end_composition(cookie); return S_OK; }
                    bool applied = false;
                    return self->apply_key(target.Get(), cookie, {KeyCode::SelectCandidate, 0, index}, applied);
                });
            if (FAILED(status) && selection_request_ == request) selection_pending_ = false;
        } catch (...) { if (selection_request_ == request) selection_pending_ = false; }
    }
    void refresh_candidates(TfEditCookie cookie) noexcept {
        candidates_.update(manager_.Get(), composition_context_.Get(), engine_, candidate_actions_);
        candidates_.locate(composition_context_.Get(), composition_.Get(), cookie);
        if (engine_.forgetting() && !candidates_.presentation_available()) {
            engine_.cancel_forgetting(); candidates_.close();
        }
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
        ++composition_session_; cancel_inference();
        clear_context_preview(this);
        ++layout_generation_; layout_pending_ = false;
        ++selection_request_; selection_pending_ = false;
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
    ComPtr<InputMode> input_mode_;
    std::shared_ptr<ModeActions> mode_actions_;
    std::array<ToolCommand, 2> tool_commands_;
    std::shared_ptr<ToolLauncher> tool_launcher_;
    std::array<bool, 2> preserved_{};
    std::shared_ptr<CandidateActions> candidate_actions_;
    std::uint64_t selection_request_ = 0;
    bool selection_pending_ = false;
    std::uint64_t layout_generation_ = 0;
    bool layout_pending_ = false;
    TfGuidAtom input_atom_ = TF_INVALID_GUIDATOM;
    TfGuidAtom converted_atom_ = TF_INVALID_GUIDATOM;
    std::bitset<256> eaten_keys_;
    bool ending_ = false;
    bool reading_context_ = false;
    bool dictionary_loaded_ = false;
    std::unique_ptr<InferenceClient> inference_;
    bool inference_dummy_ = false;
    InferencePolicy inference_policy_;
    std::optional<PendingInference> inference_pending_;
    std::uint64_t inference_sequence_ = 0, composition_session_ = 0, context_generation_ = 0;
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
