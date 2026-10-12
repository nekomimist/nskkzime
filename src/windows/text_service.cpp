#include "module.h"
#include "tsf_edit.h"
#include "composition_session.h"
#include "inference_session.h"
#include "nskkzime/engine.h"
#include "runtime_dictionary.h"
#include "runtime_configuration.h"
#include "container_runtime.h"
#include "learning_client.h"
#include <initguid.h>
#include <inputscope.h>
#include "display_attributes.h"
#include "candidate_ui.h"
#include "input_mode.h"
#include "context_text.h"
#include "inference_client.h"
#include "romaji_settings.h"
#include "tsf_diagnostics.h"
#include "cpu_requirements.h"
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <new>
#include <optional>
#include <utility>

namespace nskkzime::windows {
namespace {
using Microsoft::WRL::ComPtr;

HRESULT activation_identity_allowed() noexcept {
    // Detect the real token rather than the immersive/XAML flag. Containers
    // use only the broker path; an unreadable identity is never a desktop fallback.
    try { (void)in_app_container(); return S_OK; } catch (...) { return E_ACCESSDENIED; }
}

// Common controls can expose their full text store through a transitory
// extension's parent document. Composing there keeps edits and selection
// notifications on the same document, without queued IMM result characters.
// A transitory flag alone is insufficient: native TSF hosts can set it too.
ComPtr<ITfContext> editing_context(ITfContext* context) {
    ComPtr<ITfContext> result(context);
    TF_STATUS flags{};
    if (!context || FAILED(context->GetStatus(&flags)) || !(flags.dwStaticFlags & TF_SS_TRANSITORY)) return result;
    ComPtr<ITfDocumentMgr> document; ComPtr<ITfCompartmentMgr> compartments;
    ComPtr<ITfCompartment> compartment;
    if (FAILED(context->GetDocumentMgr(&document)) || !document || FAILED(document.As(&compartments)) ||
        FAILED(compartments->GetCompartment(GUID_COMPARTMENT_TRANSITORYEXTENSION_PARENT, &compartment)) || !compartment) return result;
    VARIANT value; VariantInit(&value);
    if (compartment->GetValue(&value) == S_OK && value.vt == VT_UNKNOWN && value.punkVal) {
        ComPtr<ITfDocumentMgr> parent; ComPtr<ITfContext> top;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&parent))) && SUCCEEDED(parent->GetTop(&top)) && top &&
            SUCCEEDED(top->GetStatus(&flags)) && !(flags.dwStaticFlags & TF_SS_TRANSITORY)) result = top;
    }
    VariantClear(&value);
    return result;
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

enum class LearningPermission { Allowed, Protected, Unavailable };
LearningPermission learning_permission(ITfContext* context, ITfComposition* composition, TfEditCookie cookie, std::wstring& reason) {
    // Inspect the entire conversion before modifying it. A caret at its end can
    // be outside the scope. Only an explicit password/private scope suppresses
    // persistence: many hosts (Office etc.) expose no input scope at all.
    reason = L"変換範囲を取得できません";
    ComPtr<ITfRange> range;
    if (!composition || FAILED(composition->GetRange(&range)) || !range) return LearningPermission::Unavailable;
    reason = L"パスワードまたはプライベート入力欄";
    return input_permission(context, range.Get(), cookie) == InputPermission::Restricted ? LearningPermission::Protected : LearningPermission::Allowed;
}

bool finishes_before_key(WPARAM code) {
    if (GetKeyState(VK_CONTROL) & 0x8000) return code != VK_CONTROL && code != VK_LCONTROL && code != VK_RCONTROL;
    switch (code) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_DELETE: case VK_TAB: return true;
    default: return false;
    }
}
bool modifier_key(WPARAM code) {
    switch (code) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: return true;
    default: return false;
    }
}
bool plain_control_key(WPARAM code, WPARAM expected) {
    return code == expected && (GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) &&
        !(GetKeyState(VK_LWIN) & 0x8000) && !(GetKeyState(VK_RWIN) & 0x8000);
}

class TextService final : public ITfTextInputProcessorEx, public ITfKeyEventSink,
                          public ITfCompositionSink, public ITfThreadMgrEventSink,
                          public ITfTextEditSink, public ITfTextLayoutSink, public ITfDisplayAttributeProvider, public CompositionOwner {
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
        const auto permission = activation_identity_allowed();
        if (FAILED(permission)) return permission;
        if (!dictionary_loaded_) {
            // Display-attribute-only COM instances never load a dictionary.
            try {
                configuration_ = capture_runtime_configuration();
                dictionary_ = runtime_dictionary(configuration_);
                engine_ = Engine(dictionary_, true, configuration_.broker ? RomajiRules::defaults() : runtime_romaji_rules(configuration_));
                engine_.set_numeric_candidate_keys(false);
                if (!configuration_.broker) {
                    candidate_settings_ = configuration_.dictionaries.settings.candidates;
                    tool_commands_ = configuration_.tools;
                }
                dictionary_loaded_ = true;
            }
            catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
            catch (...) { return E_FAIL; }
        }
        try {
            if (host_executable_.empty()) {
                wchar_t path[32768]{};
                const auto length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
                if (length && length < ARRAYSIZE(path)) host_executable_ = std::filesystem::path(path).filename().native();
            }
            // Container settings arrive later. Do not reserve OFF keys before exclusions are known.
            off_keys_enabled_ = !configuration_.broker && !off_keys_excluded(configuration_.dictionaries.settings, host_executable_);
        } catch (...) { return E_OUTOFMEMORY; }
        if (!configuration_.broker) diagnostics_ = TsfDiagnosticLog::create(configuration_.dictionaries.settings.tsf_log, false);
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::Activate, diagnostic_state());
        try {
            candidate_actions_ = std::make_shared<CandidateActions>();
            candidate_actions_->select = [this](std::size_t index, std::uint64_t revision) { select_candidate(index, revision); };
            candidate_actions_->interact = [this](std::uint64_t revision) {
                if (engine_.revision() == revision && inference_state_.pending) cancel_inference();
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
                connecting_mode_ = true;
                input_mode_->connect(manager_.Get(), client_, mode_actions_, engine_.mode());
                connecting_mode_ = false;
                apply_startup_mode();
                for (std::size_t i = 0; i < mode_keys.size(); ++i) {
                    constexpr wchar_t description[] = L"nskkzime オン／オフ";
                    preserved_[i] = SUCCEEDED(keys_->PreserveKey(client_, mode_key_ids[i], &mode_keys[i], description, ARRAYSIZE(description) - 1));
                }
                try {
                    if (const auto config = configuration_.broker ? std::nullopt : inference_config(configuration_)) {
                        inference_state_.configure(*config, [this](InferenceCompletion result) { receive_inference(std::move(result)); });
                        set_inference_status(this,L"推論: zenzを使用");
                    }
                } catch (const UnsupportedZenzCpu&) { set_inference_status(this,L"推論: CPU/OSがzenzの要件を満たしません。設定CLIの --check-cpu で確認できます。辞書入力を続けます"); }
                catch (...) { set_inference_status(this,L"推論: 起動できません。辞書入力を続けます"); }
            } catch (...) { Deactivate(); return E_OUTOFMEMORY; }
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::Deactivate, diagnostic_state());
        startup_mode_pending_ = false;
        if (tool_launcher_) { tool_launcher_->close(); tool_launcher_.reset(); }
        mode_actions_.reset();
        auto mode = std::move(input_mode_); if (mode) mode->disconnect();
        for (std::size_t i = 0; i < mode_keys.size(); ++i) {
            if (preserved_[i] && keys_) keys_->UnpreserveKey(mode_key_ids[i], &mode_keys[i]);
            preserved_[i] = false;
        }
        candidate_actions_.reset();
        finish_composition(TsfReason::Deactivated);
        inference_state_.client.reset(); clear_inference_status(this);
        inference_state_.allow_unknown_context = false;
        if (keys_) keys_->UnadviseKeyEventSink(client_);
        if (manager_ && sink_cookie_ != TF_INVALID_COOKIE) {
            ComPtr<ITfSource> source;
            if (SUCCEEDED(manager_.As(&source))) source->UnadviseSink(sink_cookie_);
        }
        sink_cookie_ = TF_INVALID_COOKIE;
        keys_.Reset(); manager_.Reset(); client_ = TF_CLIENTID_NULL;
        eaten_keys_.reset(); reset_engine(TsfReason::Deactivated);
        cancel_quote(); quoted_keys_.reset(); quote_prefix_down_ = false;
        diagnostics_.reset();
        try { if (configuration_.broker) {
            engine_ = Engine(std::make_shared<MemoryDictionary>()); dictionary_.reset();
            dictionary_loaded_ = false; container_configured_ = container_rules_configured_ = false;
        } } catch (...) { /* Deactivation cannot unwind through COM. */ }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::KeyFocus, diagnostic_state());
        trace_tsf(TsfEvent::KeyFocus, TsfReason::None, S_OK, foreground);
        if (!foreground) { cancel_quote(); finish_composition(TsfReason::FocusLost); }
        else { apply_startup_mode(); configure_container(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM code, LPARAM parameter, BOOL* eaten) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::TestKeyDown, diagnostic_state(context));
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        configure_container();
        const auto target = editing_context(context);
        if (target.Get() != context && !editable(target.Get())) return S_OK;
        context = target.Get();
        // Request one real callback so startup policy never mutates Windows
        // state during preflight, even when broker settings have just arrived.
        if (startup_mode_pending_) { *eaten = TRUE; return S_OK; }
        update_tsf_trace(diagnostic_state(context));
        if (off_key_callback_needed(code)) { *eaten = TRUE; return S_OK; }
        if (mode_key(code, engine_.outer_mode())) { *eaten = TRUE; return S_OK; }
        const auto key = service_key(code, parameter);
        if (key) {
            if (composition_state_.context && composition_state_.context.Get() != context) {
                try { auto fresh = engine_; fresh.reset(); *eaten = fresh.would_handle(*key); }
                catch (...) { return E_OUTOFMEMORY; }
            } else *eaten = engine_.would_handle(*key);
        }
        if (!*eaten && composition_state_.composition && finishes_before_key(code)) {
            // A native host can consume the first application key if it arrives
            // while still composing. Finish during the preflight and decline
            // the key. This lifecycle cleanup is idempotent; ordinary SKK key
            // probes never advance the engine or edit the document.
            finish_composition(TsfReason::Navigation, composition_state_.context.Get() == context ? TF_ES_ASYNCDONTCARE : TF_ES_ASYNC);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM code, LPARAM parameter, BOOL* eaten) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::KeyDown, diagnostic_state(context));
        const KeyTiming timing;
        InputPhaseTiming setup(InputTimingPhase::KeySetup);
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        configure_container();
        const ComPtr<ITfContext> origin(context);
        const auto resolved = editing_context(context);
        if (resolved.Get() != context && !editable(resolved.Get())) return S_OK;
        context = resolved.Get();
        apply_startup_mode();
        // Input already started: late broker settings must not reset its mode.
        startup_mode_pending_ = false;
        update_tsf_trace(diagnostic_state(context, origin.Get()));
        if (const auto handled = handle_off_key(context, code, parameter)) {
            *eaten = *handled;
            if (*handled && code < eaten_keys_.size()) eaten_keys_.set(code);
            return S_OK;
        }
        if (const auto mode = mode_key(code, engine_.outer_mode())) {
            if (!(parameter & (static_cast<LPARAM>(1) << 30))) change_mode(*mode);
            *eaten = TRUE; if (code < eaten_keys_.size()) eaten_keys_.set(code); return S_OK;
        }
        if (composition_state_.context && composition_state_.context.Get() != context) finish_composition(TsfReason::ContextChanged);
        const auto key = service_key(code, parameter);
        if (!key || !engine_.would_handle(*key)) return S_OK;
        setup.finish();
        try {
            const ComPtr<TextService> self(this);
            const ComPtr<ITfContext> target(context);
            bool applied = false;
            InputPhaseTiming request(InputTimingPhase::EditRequest);
            const auto hr = request_edit(context, client_, TF_ES_SYNC | TF_ES_READWRITE,
                [self, target, origin, key, &applied](TfEditCookie cookie) {
                    update_tsf_trace(self->diagnostic_state(target.Get(), origin.Get()));
                    const InputPhaseTiming callback(InputTimingPhase::EditCallback);
                    for (const auto& pending : self->pending_) {
                        if (!pending->done && pending->context.Get() == target.Get()) {
                            const auto status = pending->finish(cookie);
                            if (FAILED(status)) return status;
                        }
                    }
                    std::erase_if(self->pending_, [](const auto& pending) { return pending->done; });
                    if (self->composition_state_.composition) {
                        const auto current = self->composition_state_.composition;
                        const auto session = self->composition_state_.session;
                        if (!self->composition_valid(cookie)) self->end_composition(cookie, TsfReason::InvalidBeforeKey);
                        else if (self->composition_state_.composition.Get() == current.Get() && self->composition_state_.session == session &&
                                 self->composition_state_.context.Get() == target.Get()) {
                            // A synchronous key can run before queued async edits.
                            // Valid input in this field supersedes its old null-focus notice.
                            self->cancel_focus_finish(TsfReason::FocusValidated);
                            ++self->focus_generation_;
                        }
                    }
                    return self->apply_key(target.Get(), cookie, self->candidates_.selection_key(*key, self->engine_), applied, origin.Get());
                });
            request.finish();
            update_tsf_trace(diagnostic_state(context, origin.Get()));
            trace_tsf(TsfEvent::KeyResult, FAILED(hr) ? TsfReason::EditRejected : TsfReason::None, hr, applied);
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
        *eaten = code < eaten_keys_.size() && (eaten_keys_.test(code) || quoted_keys_.test(code));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM code, LPARAM, BOOL* eaten) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::KeyUp, diagnostic_state(context));
        if (!eaten) return E_POINTER;
        *eaten = code < eaten_keys_.size() && eaten_keys_.test(code);
        if (code < eaten_keys_.size()) { eaten_keys_.reset(code); quoted_keys_.reset(code); }
        if (code == 'Q') quote_prefix_down_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID guid, BOOL* eaten) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::PreservedKey, diagnostic_state(context));
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if (!manager_ || !editable(context)) return S_OK;
        const auto target = editing_context(context);
        if (target.Get() != context && !editable(target.Get())) return S_OK;
        for (std::size_t i = 0; i < mode_keys.size(); ++i) if (preserved_[i] && guid == mode_key_ids[i]) {
            change_mode(engine_.outer_mode() == Mode::Latin ? Mode::Hiragana : Mode::Latin);
            *eaten = TRUE; break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie cookie, ITfComposition* composition) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::HostTerminated, diagnostic_state());
        trace_tsf(TsfEvent::HostTerminated, TsfReason::HostTermination, S_OK, diagnostics_ ? diagnostics_->identity(composition) : 0);
        if (composition_state_.composition.Get() != composition || composition_state_.ending) return S_OK;
        try {
            ComPtr<ITfRange> range;
            if (SUCCEEDED(composition->GetRange(&range))) {
                clear_display_attribute(composition_state_.context.Get(), cookie, range.Get());
            }
        } catch (...) { /* Do not allow a C++ exception to cross the COM boundary. */ }
        detach_composition(); reset_engine(TsfReason::HostTermination);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context, TfEditCookie cookie, ITfEditRecord*) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::EndEdit, diagnostic_state(context));
        if (composition_state_.ending || context != composition_state_.context.Get() || !composition_state_.composition) return S_OK;
        const ComPtr<TextService> self(this);
        try {
            if (!composition_valid(cookie)) finish_composition(TsfReason::InvalidAfterEdit);
            else if (!composition_state_.reading_context) {
                composition_state_.reading_context = true;
                const auto current = composition_state_.composition; const auto revision = engine_.revision();
                ComPtr<ITfRange> range;
                const auto pending = inference_state_.pending;
                const bool preview = context_preview_enabled() && (engine_.stage() == Stage::Selecting || engine_.registering());
                clear_context_preview(this);
                if ((pending || preview) && SUCCEEDED(current->GetRange(&range)) && range) {
                    auto text = surrounding_text(context, range.Get(), cookie, inference_state_.allow_unknown_context);
                    // Property callbacks may deactivate or replace the composition.
                    if (composition_state_.composition.Get() == current.Get() && composition_state_.context.Get() == context && engine_.revision() == revision) {
                        if (pending && inference_state_.pending && pending->id == inference_state_.pending->id && text != pending->text) cancel_inference();
                        if (preview) publish_context_preview(this, std::move(text));
                    }
                } else if (pending) cancel_inference();
                composition_state_.reading_context = false;
            }
        }
        catch (...) { composition_state_.reading_context = false; clear_context_preview(this); finish_composition(TsfReason::Exception); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView*) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::LayoutChange, diagnostic_state(context));
        if (context != composition_state_.context.Get() || !composition_state_.composition) return S_OK;
        candidates_.invalidate_position();
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
                    update_tsf_trace(self->diagnostic_state());
                    if (self->layout_generation_ != generation) return S_OK;
                    self->layout_pending_ = false;
                    const auto context = self->composition_state_.context;
                    const auto current = self->composition_state_.composition;
                    const auto session = self->composition_state_.session;
                    const auto revision = self->engine_.revision();
                    if (current) self->candidates_.locate(context.Get(), current.Get(), cookie, session);
                    if (self->composition_state_.context.Get() == context.Get() && self->composition_state_.composition.Get() == current.Get() &&
                        self->composition_state_.session == session && self->engine_.revision() == revision) self->cancel_unshown_editor();
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
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::DocumentUninit, diagnostic_state());
        for (const auto& context : {composition_state_.context, composition_state_.origin}) {
            if (!context) continue;
            ComPtr<ITfDocumentMgr> owner;
            if (SUCCEEDED(context->GetDocumentMgr(&owner)) && owner.Get() == document) { finish_composition(TsfReason::DocumentRemoved); break; }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* document, ITfDocumentMgr* previous) override {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::DocumentFocus, diagnostic_state());
        const ComPtr<TextService> self(this);
        const auto generation = ++focus_generation_;
        const auto quote_generation = quote_generation_;
        const auto current = composition_state_.composition;
        const auto session = composition_state_.session;
        trace_tsf(TsfEvent::DocumentFocus, TsfReason::None, S_OK,
                  diagnostics_ ? diagnostics_->identity(document) : 0, diagnostics_ ? diagnostics_->identity(previous) : 0, 0, false);
        ComPtr<ITfContext> context;
        const auto status = document ? document->GetTop(&context) : S_FALSE;
        trace_tsf(TsfEvent::DocumentTop, !context || FAILED(status) ? TsfReason::FocusUnavailable : TsfReason::None, status,
                  diagnostics_ ? diagnostics_->identity(context.Get()) : 0, diagnostics_ ? diagnostics_->identity(document) : 0, 0, document != nullptr);
        if (FAILED(status)) context.Reset();
        context = editing_context(context.Get());
        auto snapshot = diagnostic_state();
        snapshot.context = diagnostics_ ? diagnostics_->identity(context.Get()) : 0; // Preserve a genuinely unresolved target.
        update_tsf_trace(snapshot);
        // GetTop/parent resolution can re-enter focus handling, accept a key or detach this session.
        if (generation != focus_generation_ || current.Get() != composition_state_.composition.Get() || session != composition_state_.session) {
            trace_tsf(TsfEvent::FocusResolved, TsfReason::Inactive, status, 0, 0, 0, document != nullptr); return S_OK;
        }
        if (quote_generation == quote_generation_ && quote_context_.Get() != context.Get()) cancel_quote();
        trace_tsf(TsfEvent::FocusResolved, context ? TsfReason::None : TsfReason::FocusUnavailable, status, 0, 0, 0, document != nullptr);
        if (!composition_state_.context) return S_OK;
        if (!context) defer_focus_finish();
        else if (composition_state_.context.Get() != context.Get()) finish_composition(TsfReason::ContextChanged);
        else cancel_focus_finish(TsfReason::FocusRestored);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override {
        cancel_quote();
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::ContextPush, diagnostic_state(context));
        const auto parent = editing_context(context); context = parent.Get();
        update_tsf_trace(diagnostic_state(context));
        if (composition_state_.context && composition_state_.context.Get() != context) finish_composition(TsfReason::ContextChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override {
        cancel_quote();
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::ContextPop, diagnostic_state(context));
        // The parent compartment can already be gone when the origin is popped.
        if (composition_state_.context.Get() == context || composition_state_.origin.Get() == context) finish_composition(TsfReason::ContextPopped);
        return S_OK;
    }
private:
    bool off_mode() const noexcept { return !engine_.registering() && engine_.outer_mode() == Mode::Latin; }
    void cancel_quote() noexcept { ++quote_generation_; quote_context_.Reset(); }
    std::optional<Key> service_key(WPARAM code, LPARAM parameter) const {
        auto key = translate_key(code, parameter);
        if (off_mode() && !off_keys_enabled_ && key && key->code == KeyCode::Hiragana) return {};
        return key;
    }
    bool off_key_callback_needed(WPARAM code) const {
        return (code < quoted_keys_.size() && quoted_keys_.test(code)) ||
            (code == 'Q' && quote_prefix_down_) || quote_context_ ||
            (off_mode() && off_keys_enabled_ && plain_control_key(code, 'Q'));
    }
    std::optional<bool> handle_off_key(ITfContext* context, WPARAM code, LPARAM parameter) {
        const bool repeat = (parameter & (static_cast<LPARAM>(1) << 30)) != 0;
        if (code < quoted_keys_.size() && quoted_keys_.test(code)) {
            if (repeat) return false;
            quoted_keys_.reset(code); // A new physical press also recovers from a missing key-up.
        }
        if (code == 'Q' && quote_prefix_down_) {
            if (repeat) return true;
            quote_prefix_down_ = false;
            eaten_keys_.reset(code);
        }
        if (quote_context_) {
            if (quote_context_.Get() != context || !off_mode() || !off_keys_enabled_) cancel_quote();
            else if (!modifier_key(code)) {
                cancel_quote();
                if (plain_control_key(code, 'Q') || plain_control_key(code, 'J')) {
                    // Decline the actual physical key, including its repeats and key-up.
                    quoted_keys_.set(code); eaten_keys_.reset(code);
                    return false;
                }
            }
        }
        if (off_mode() && off_keys_enabled_ && plain_control_key(code, 'Q')) {
            if (repeat) return false;
            ++quote_generation_; quote_context_ = context; quote_prefix_down_ = true;
            return true;
        }
        return {};
    }
    void apply_startup_mode() noexcept {
        if (!startup_mode_pending_) return;
        const auto remote = configuration_.broker ? container_configuration(dictionary_) : nullptr;
        if (configuration_.broker && !remote) return;
        const auto& settings = remote ? remote->configuration.settings : configuration_.dictionaries.settings;
        startup_mode_pending_ = false;
        if (settings.startup_ime_off) change_mode(Mode::Latin);
    }
    void configure_container() noexcept {
        try {
            if (!configuration_.broker) return;
            const auto remote = container_configuration(dictionary_);
            if (!remote || !dictionary_ || !dictionary_->ready()) return;
            if (!container_rules_configured_ && !composition_state_.composition && engine_.preedit().empty()) {
                const auto mode = engine_.outer_mode();
                engine_ = Engine(dictionary_, true, remote->romaji); engine_.set_mode(mode); engine_.set_numeric_candidate_keys(false);
                container_rules_configured_ = true;
            }
            if (container_configured_) return;
            off_keys_enabled_ = !off_keys_excluded(remote->configuration.settings, host_executable_);
            diagnostics_ = TsfDiagnosticLog::create(remote->configuration.settings.tsf_log, in_app_container());
            candidate_settings_ = remote->configuration.settings.candidates;
            if (const auto config = inference_config(*remote)) {
                inference_state_.configure(*config, [this](InferenceCompletion result) { receive_inference(std::move(result)); });
                set_inference_status(this, L"推論: 補助プロセス経由でzenzを使用");
            }
            if (!in_app_container()) {
                tool_commands_ = tool_commands(remote->configuration.file,
                    remote->configuration.file.parent_path() / L"user-dictionary.nskkdb", remote->generation);
                tool_launcher_ = std::make_shared<ToolLauncher>(tool_commands_);
                if (mode_actions_) {
                    mode_actions_->tools_available = {tool_launcher_->available(ImeTool::Settings), tool_launcher_->available(ImeTool::Dictionary)};
                    mode_actions_->open_tool = [launcher = tool_launcher_](ImeTool tool) { return launcher->open(tool); };
                }
            }
            container_configured_ = true;
        } catch (...) { set_inference_status(this, L"推論: AppContainer設定を適用できません"); }
    }

    void cancel_inference() noexcept {
        if (inference_state_.cancel()) set_inference_status(this,L"推論: 入力状態が変わったため応答を破棄");
    }
    bool matches(const PendingInference& pending) const {
        return manager_ && inference_state_.matches(pending, engine_.revision(), composition_state_.session,
            composition_state_.context.Get(), composition_state_.composition.Get());
    }
    void receive_inference(InferenceCompletion result) noexcept {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::InferenceResult, diagnostic_state());
        if (!inference_state_.pending || result.id != inference_state_.pending->id) return;
        const auto id = result.id;
        try {
            const ComPtr<TextService> self(this); const auto pending = *inference_state_.pending;
            const auto hr = request_edit(pending.context.Get(),client_,TF_ES_ASYNC | TF_ES_READWRITE,
                [self,pending,result=std::move(result)](TfEditCookie cookie) {
                    update_tsf_trace(self->diagnostic_state(pending.context.Get()));
                    if (!self->matches(pending)) return S_OK;
                    if (!editable(pending.context.Get()) || !self->composition_valid(cookie)) { self->cancel_inference(); return S_OK; }
                    ComPtr<ITfRange> range;
                    if (FAILED(pending.composition->GetRange(&range)) || !range) { self->cancel_inference(); return S_OK; }
                    const auto text = surrounding_text(pending.context.Get(),range.Get(),cookie,self->inference_state_.allow_unknown_context);
                    if (!self->matches(pending)) return S_OK;
                    if (text != pending.text || text.permission == InputPermission::Restricted) { self->cancel_inference(); return S_OK; }
                    auto next = self->engine_; bool adjusted = false;
                    const bool timed_out = GetTickCount64() >= pending.deadline || result.status == InferenceStatus::Timeout;
                    if (!timed_out && result.status == InferenceStatus::Ok) {
                        adjusted = apply_inference_result(next,pending.plan,result.response,self->inference_state_.policy);
                    }
                    self->cancel_inference();
                    const bool fallback = pending.plan.purpose == InferencePurpose::Fallback ||
                        (pending.plan.purpose == InferencePurpose::LongReading && next.candidates().empty());
                    const bool changed = adjusted || (fallback && next.resume_registration());
                    if (changed && next.registering()) {
                        self->candidates_.locate(pending.context.Get(), pending.composition.Get(), cookie, pending.session, true);
                        if (!self->manager_ || self->engine_.revision() != pending.revision ||
                            self->composition_state_.composition.Get() != pending.composition.Get() ||
                            self->composition_state_.context.Get() != pending.context.Get() || self->composition_state_.session != pending.session) return S_OK;
                    }
                    if (changed && FAILED(self->apply(pending.context.Get(),cookie,U"",next))) {
                        set_inference_status(self.Get(),L"推論: 応答を反映できません。現在の入力を維持します");
                        return S_OK;
                    }
                    if (changed) { self->engine_ = std::move(next); self->refresh_candidates(cookie); }
                    try { if (self->inference_state_.client && self->inference_state_.client->input_log_enabled() && !pending.text.contains_unknown_scope && self->composition_state_.context.Get() == pending.context.Get()) {
                        // Applying text/UI can re-enter the host. Recheck privacy
                        // and identity after it, before queuing any text for disk.
                        const auto current = self->composition_state_.composition;
                        const auto revision = self->engine_.revision();
                        const auto generation = self->inference_state_.generation;
                        ComPtr<ITfRange> active;
                        if (current && SUCCEEDED(current->GetRange(&active)) && active) {
                            const auto allowed = surrounding_text(pending.context.Get(), active.Get(), cookie);
                            if (self->inference_state_.client && self->composition_state_.composition.Get() == current.Get() && self->engine_.revision() == revision && self->inference_state_.generation == generation &&
                                allowed.permission == InputPermission::Allowed && allowed.before == pending.text.before && allowed.after == pending.text.after)
                                self->inference_state_.client->record_input(pending.plan.request,
                                    !timed_out && result.status == InferenceStatus::Ok ? &result.response : nullptr,
                                    timed_out ? InferenceStatus::Timeout : result.status);
                        }
                    } } catch (...) { /* Diagnostics must not fail an already applied edit. */ }
                    set_inference_status(self.Get(),adjusted ? (pending.plan.purpose == InferencePurpose::Rerank ? L"推論: 最良の辞書候補を先頭へ移動" : L"推論: 生成候補を反映") :
                        timed_out ? L"推論: 時間切れ。通常のSKK操作へ復帰" : L"推論: 現在の辞書候補または読みで継続");
                    return S_OK;
                });
            if (FAILED(hr) && matches(pending)) cancel_inference();
        } catch (...) { if (inference_state_.pending && inference_state_.pending->id == id) cancel_inference(); }
    }
    void change_mode(Mode mode) noexcept {
        // Initial compartment synchronization is not an explicit mode choice.
        if (!connecting_mode_) startup_mode_pending_ = false;
        cancel_quote();
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::ModeChange, diagnostic_state());
        const ComPtr<TextService> self(this);
        if (!manager_) return;
        if (engine_.mode() != mode || engine_.registering()) {
            trace_tsf(TsfEvent::ModeChange, TsfReason::ModeChanged);
            finish_composition(TsfReason::ModeChanged); engine_.set_mode(mode);
            update_tsf_trace(diagnostic_state());
        }
        const auto binding = input_mode_; if (binding) binding->publish(engine_.mode());
    }
    HRESULT apply_key(ITfContext* target, TfEditCookie cookie, Key key, bool& applied, ITfContext* origin = nullptr) {
        const auto before_notice = composition_state_.composition;
        const auto notice_revision = engine_.revision(), notice_session = composition_state_.session;
        candidates_.dismiss_notice();
        if (!manager_ || composition_state_.composition.Get() != before_notice.Get() ||
            composition_state_.session != notice_session || engine_.revision() != notice_revision) return S_OK;
        // A destroyed or clipped view must not keep capturing registration
        // input while its own editor is hidden.
        if ((engine_.registering() || engine_.forgetting()) && !candidates_.presentation_available()) {
            const auto current = composition_state_.composition;
            const auto revision = engine_.revision(), session = composition_state_.session;
            candidates_.locate(target, current.Get(), cookie, session, true);
            if (composition_state_.composition.Get() != current.Get() || composition_state_.session != session || engine_.revision() != revision) return S_OK;
            cancel_unshown_editor();
        }
        InputPhaseTiming core(InputTimingPhase::Core);
        auto next = engine_;
        const auto result = next.handle(key);
        core.finish();
        if (!result.consumed) return S_OK;
        const auto revision = engine_.revision();
        const auto current = composition_state_.composition;
        const auto session = composition_state_.session;
        const bool was_pending = inference_state_.pending.has_value();
        std::wstring learning_reason;
        const bool starting_registration = next.registration_depth() > engine_.registration_depth();
        const bool starting_forget = next.forgetting() && !engine_.forgetting() && next.forget_target().has_value();
        const auto permission = (result.learning || result.forgetting || starting_registration || starting_forget) ?
            learning_permission(target, current.Get(), cookie, learning_reason) : LearningPermission::Unavailable;
        const bool persistence_allowed = permission == LearningPermission::Allowed;
        const bool protected_registration = starting_registration && permission == LearningPermission::Protected;
        // Property callbacks can end or replace the composition while answering.
        if (engine_.revision() != revision || composition_state_.composition.Get() != current.Get()) return S_OK;
        if (starting_registration && !persistence_allowed) next.abandon_registration();
        if (starting_forget && !persistence_allowed) next.cancel_forgetting();
        std::optional<SurroundingText> inference_text;
        auto plan = inference_state_.client && !was_pending ? plan_inference(engine_,next,result,inference_state_.policy) : std::nullopt;
        if (plan) {
            ComPtr<ITfRange> range;
            if (current && SUCCEEDED(current->GetRange(&range)) && range) {
                auto text = surrounding_text(target,range.Get(),cookie,inference_state_.allow_unknown_context);
                // Some hosts (including Office) expose no usable input scope.
                // Context requires a separate opt-in for unknown scopes.
                // Actual permission remains Unknown, so local inference never
                // grants learning, registration or input-log permission.
                if (text.permission != InputPermission::Restricted) {
                    inference_text = std::move(text); next.defer_registration();
                    if (plan->purpose != InferencePurpose::Rerank) next.note_generation_attempt();
                }
                else set_inference_status(this,L"推論: 保護された入力欄のため送信しません");
            }
            if (engine_.revision() != revision || composition_state_.composition.Get() != current.Get()) return S_OK;
        }
        // Like skk-zenz, score before the first candidate is shown so the list
        // never reorders under the user. Too slow: fall back to the async path.
        std::optional<std::pair<InferenceRequest, InferenceResponse>> scored;
        bool promoted = false, log_scored = false;
        if (plan && inference_text) {
            if (auto response = inference_state_.score_now(*plan, *inference_text)) {
                promoted = apply_inference_result(next, *plan, *response, inference_state_.policy);
                log_scored = inference_text->permission == InputPermission::Allowed && !inference_text->contains_unknown_scope;
                scored.emplace(std::move(plan->request), std::move(*response));
                plan.reset(); inference_text.reset();
            }
        }
        auto visibility = candidate_visibility_;
        if (next.registering() || next.forgetting() || protected_registration) {
            // Capture layout before changing a candidate back into the reading.
            // This also works with a delayed candidate list or a dictionary miss.
            candidates_.locate(target, current.Get(), cookie, session, true);
            if (!manager_ || engine_.revision() != revision || composition_state_.composition.Get() != current.Get() ||
                composition_state_.context.Get() != target || composition_state_.session != session) return S_OK;
        }
        const auto status = apply(target, cookie, result.committed, next);
        if (SUCCEEDED(status)) {
            cancel_inference();
            visibility.advance(key, result, next);
            candidate_visibility_ = visibility;
            engine_ = std::move(next); applied = true;
            if (origin && composition_state_.composition && composition_state_.context.Get() == target) composition_state_.origin = origin;
            update_tsf_trace(diagnostic_state(target, origin));
            trace_tsf(TsfEvent::StateAccepted);
            if (result.forgetting && persistence_allowed && publish_runtime_forgetting(dictionary_, *result.forgetting))
                engine_.forget_registration(*result.forgetting);
            if (result.learning && persistence_allowed) {
                publish_runtime_learning(dictionary_, *result.learning);
                if (result.registration) engine_.remember_registration(*result.learning);
            } else if (result.learning || ((result.forgetting || starting_registration || starting_forget) && !persistence_allowed)) {
                skip_runtime_learning(dictionary_, std::move(learning_reason));
            }
            refresh_candidates(cookie, protected_registration ? L"保護された入力欄のため、単語登録を行いません。" : L"");
            if (scored) {
                set_inference_status(this, promoted ? L"推論: 表示前の採点で最良の辞書候補を先頭へ移動" : L"推論: 表示前に採点し、辞書候補の順位を維持");
                try { if (log_scored && inference_state_.client && inference_state_.client->input_log_enabled())
                    inference_state_.client->record_input(scored->first, &scored->second, InferenceStatus::Ok); } catch (...) {}
            }
            // Inner registration input has its own modes. Keep Windows open
            // until the outer editor is completed or explicitly abandoned.
            const auto binding = input_mode_;
            if (binding && !engine_.registering()) {
                const InputPhaseTiming mode(InputTimingPhase::ModeSync);
                binding->publish(engine_.mode());
            }
            if (inference_text && inference_state_.client && composition_state_.composition && !engine_.registering()) {
                try {
                    inference_state_.submit(std::move(*plan), std::move(*inference_text), engine_.revision(), composition_state_.session,
                        composition_state_.context.Get(), composition_state_.composition.Get());
                    set_inference_status(this,inference_state_.pending->text.before.empty() && inference_state_.pending->text.after.empty()
                        ? L"推論: 応答待ち（周辺本文なし）" : L"推論: 応答待ち（周辺本文あり）");
                } catch (...) { set_inference_status(this,L"推論: 要求を開始できません。Spaceで通常のSKK操作へ復帰"); }
            }
        }
        return status;
    }
    void select_candidate(std::size_t index, std::uint64_t revision) noexcept {
        TsfDiagnosticScope diagnostic(diagnostics_, TsfEvent::CandidateSelect, diagnostic_state());
        if (selection_pending_ || !manager_ || !composition_state_.composition || !composition_state_.context ||
            engine_.revision() != revision || engine_.forgetting() || engine_.input_stage() != Stage::Selecting || index >= engine_.candidates().size()) return;
        cancel_inference();
        const auto request = ++selection_request_;
        selection_pending_ = true;
        try {
            const ComPtr<TextService> self(this);
            const auto target = composition_state_.context;
            const auto current = composition_state_.composition;
            const auto status = request_edit(target.Get(), client_, TF_ES_ASYNC | TF_ES_READWRITE,
                [self, target, current, index, revision, request](TfEditCookie cookie) {
                    update_tsf_trace(self->diagnostic_state(target.Get()));
                    if (self->selection_request_ != request) return S_OK;
                    self->selection_pending_ = false;
                    if (!self->manager_ || self->composition_state_.context.Get() != target.Get() ||
                        self->composition_state_.composition.Get() != current.Get() || self->engine_.revision() != revision ||
                        self->engine_.input_stage() != Stage::Selecting || !editable(target.Get())) return S_OK;
                    if (!self->composition_valid(cookie)) { self->end_composition(cookie, TsfReason::InvalidCandidateSelection); return S_OK; }
                    bool applied = false;
                    return self->apply_key(target.Get(), cookie, {KeyCode::SelectCandidate, 0, index}, applied);
                });
            if (FAILED(status) && selection_request_ == request) selection_pending_ = false;
        } catch (...) { if (selection_request_ == request) selection_pending_ = false; }
    }
    void refresh_candidates(TfEditCookie cookie, std::wstring_view notice = {}) noexcept {
        const InputPhaseTiming timing(InputTimingPhase::Candidates);
        const auto context = composition_state_.context;
        const auto current = composition_state_.composition;
        const auto revision = engine_.revision(), session = composition_state_.session;
        const auto unchanged = [&] {
            return manager_ && composition_state_.context.Get() == context.Get() && composition_state_.composition.Get() == current.Get() &&
                composition_state_.session == session && engine_.revision() == revision;
        };
        // Common EDIT's full parent permits direct editing but does not paint
        // display attributes. Present its reading separately from document text.
        const bool show_reading = composition_state_.origin && composition_state_.origin.Get() != composition_state_.context.Get();
        candidates_.update(manager_.Get(), context.Get(), engine_, candidate_actions_, show_reading,
            candidate_visibility_.visible(candidate_settings_), candidate_settings_.keys, notice);
        if (!unchanged()) return;
        candidates_.locate(context.Get(), current.Get(), cookie, session);
        if (unchanged()) cancel_unshown_editor();
    }
    void cancel_unshown_editor() noexcept {
        if (engine_.forgetting() && !candidates_.presentation_available()) {
            engine_.cancel_forgetting(); candidates_.close();
        }
        if (engine_.registering() && !candidates_.presentation_available()) {
            // The host text is still the outer reading. Never capture more keys
            // in an editor the user cannot see, even when popup creation fails.
            engine_.abandon_registration(); candidates_.close();
        }
    }
    bool composition_valid(TfEditCookie cookie) const { return composition_state_.valid(cookie); }
    void cancel_focus_finish(TsfReason reason) noexcept {
        if (!focus_pending_) return;
        trace_tsf(TsfEvent::FocusCancelled, reason, S_OK, focus_request_);
        ++focus_request_; focus_pending_ = false;
    }
    bool focus_finish_current(std::uint64_t request, ITfContext* target, ITfComposition* composition, std::uint64_t session) const noexcept {
        return focus_pending_ && focus_request_ == request && manager_ && composition_state_.context.Get() == target &&
            composition_state_.composition.Get() == composition && composition_state_.session == session;
    }
    void defer_focus_finish() noexcept {
        if (focus_pending_ || !manager_ || !composition_state_.context || !composition_state_.composition) return;
        const auto target = composition_state_.context;
        const auto current = composition_state_.composition;
        const auto session = composition_state_.session;
        const auto request = ++focus_request_; focus_pending_ = true;
        trace_tsf(TsfEvent::FocusDeferred, TsfReason::FocusUnavailable, S_OK, request, 0);
        try {
            const ComPtr<TextService> self(this);
            const auto status = request_edit(target.Get(), client_, TF_ES_ASYNC | TF_ES_READWRITE,
                [self, target, current, session, request](TfEditCookie) {
                    update_tsf_trace(self->diagnostic_state(target.Get()));
                    if (!self->focus_finish_current(request, target.Get(), current.Get(), session)) {
                        trace_tsf(TsfEvent::FocusDeferred, TsfReason::Inactive, S_OK, request, 3); return S_OK;
                    }
                    self->focus_pending_ = false;
                    trace_tsf(TsfEvent::FocusDeferred, TsfReason::FocusUnavailable, S_OK, request, 1);
                    // Decide at this async opportunity, then use the existing detached
                    // cleanup so host re-entry cannot redirect it into a new field.
                    self->finish_composition(TsfReason::FocusUnavailable);
                    return S_OK;
                });
            if (FAILED(status) && focus_finish_current(request, target.Get(), current.Get(), session)) {
                trace_tsf(TsfEvent::FocusDeferred, TsfReason::EditRejected, status, request, 2);
                finish_composition(TsfReason::FocusUnavailable);
            }
        } catch (...) {
            if (focus_finish_current(request, target.Get(), current.Get(), session)) finish_composition(TsfReason::Exception);
        }
    }
    void detach_composition() noexcept override {
        trace_tsf(TsfEvent::DetachComposition);
        cancel_focus_finish(TsfReason::Inactive); ++focus_generation_;
        candidate_visibility_.reset();
        ++composition_state_.session; cancel_inference();
        clear_context_preview(this);
        ++layout_generation_; layout_pending_ = false;
        ++selection_request_; selection_pending_ = false;
        candidates_.close();
        composition_state_.release_context();
        update_tsf_trace(diagnostic_state());
    }
    void end_composition(TfEditCookie cookie, TsfReason reason) noexcept override {
        composition_state_.end(cookie);
        detach_composition(); reset_engine(reason);
    }
    HRESULT apply(ITfContext* context, TfEditCookie cookie, std::u32string_view committed, Engine& next) {
        return composition_state_.apply(context, cookie, committed, next, *this,
            {this, this, this}, input_atom_, converted_atom_);
    }
    void finish_composition(TsfReason reason, DWORD scheduling = TF_ES_ASYNC) noexcept {
        trace_tsf(TsfEvent::FinishComposition, reason);
        cancel_focus_finish(reason);
        try {
            if (!composition_state_.composition || !composition_state_.context) { reset_engine(reason); return; }
            auto pending = std::make_shared<PendingComposition>();
            pending->composition = composition_state_.composition; pending->context = composition_state_.context;
            pending->session = composition_state_.session;
            std::erase_if(pending_, [](const auto& item) { return item->done; });
            pending_.push_back(pending);
            // Detach before scheduling: this session must never operate on a new input field.
            detach_composition(); reset_engine(reason);
            const auto status = request_edit(pending->context.Get(), client_, scheduling | TF_ES_READWRITE,
                [pending](TfEditCookie cookie) { return pending->finish(cookie); });
            if (FAILED(status)) std::erase(pending_, pending);
        } catch (...) { detach_composition(); reset_engine(TsfReason::Exception); }
    }
    TsfTraceState diagnostic_state(ITfContext* target = nullptr, ITfContext* origin = nullptr) noexcept {
        if (!diagnostics_) return {};
        TsfTraceState state;
        state.context = diagnostics_->identity(target ? target : composition_state_.context.Get());
        state.origin = diagnostics_->identity(origin ? origin : composition_state_.origin.Get());
        state.owner_context = diagnostics_->identity(composition_state_.context.Get());
        state.composition = diagnostics_->identity(composition_state_.composition.Get());
        state.session = composition_state_.session; state.revision = engine_.revision();
        state.stage = static_cast<unsigned>(engine_.stage()); state.mode = static_cast<unsigned>(engine_.mode());
        state.pending = engine_.pending_romaji_length(); state.reading = engine_.reading_length(); state.okuri = engine_.okuri_length();
        state.expected = composition_state_.expected_text.size(); return state;
    }
    void reset_engine(TsfReason reason) noexcept {
        update_tsf_trace(diagnostic_state()); trace_tsf(TsfEvent::EngineReset, reason, S_OK, 0);
        engine_.reset();
        update_tsf_trace(diagnostic_state()); trace_tsf(TsfEvent::EngineReset, reason, S_OK, 1);
    }
    std::atomic<ULONG> refs_{1};
    ComPtr<ITfThreadMgr> manager_;
    ComPtr<ITfKeystrokeMgr> keys_;
    TfClientId client_ = TF_CLIENTID_NULL;
    DWORD sink_cookie_ = TF_INVALID_COOKIE;
    CompositionSession composition_state_;
    InferenceSession inference_state_;
    std::uint64_t composition_revision() const override { return engine_.revision(); }
    Engine engine_;
    std::shared_ptr<const Dictionary> dictionary_;
    RuntimeConfiguration configuration_;
    std::shared_ptr<TsfDiagnosticLog> diagnostics_;
    std::vector<std::shared_ptr<PendingComposition>> pending_;
    CandidatePresenter candidates_;
    CandidateSettings candidate_settings_;
    CandidateVisibility candidate_visibility_;
    ComPtr<InputMode> input_mode_;
    bool startup_mode_pending_ = true, connecting_mode_ = false;
    std::shared_ptr<ModeActions> mode_actions_;
    std::array<ToolCommand, 2> tool_commands_;
    std::shared_ptr<ToolLauncher> tool_launcher_;
    std::array<bool, 2> preserved_{};
    std::shared_ptr<CandidateActions> candidate_actions_;
    std::uint64_t selection_request_ = 0;
    bool selection_pending_ = false;
    std::uint64_t layout_generation_ = 0;
    bool layout_pending_ = false;
    std::uint64_t focus_request_ = 0, focus_generation_ = 0;
    bool focus_pending_ = false;
    TfGuidAtom input_atom_ = TF_INVALID_GUIDATOM;
    TfGuidAtom converted_atom_ = TF_INVALID_GUIDATOM;
    std::bitset<256> eaten_keys_;
    std::bitset<256> quoted_keys_;
    ComPtr<ITfContext> quote_context_;
    std::uint64_t quote_generation_ = 0;
    std::wstring host_executable_;
    bool quote_prefix_down_ = false, off_keys_enabled_ = true;
    bool dictionary_loaded_ = false;
    bool container_configured_ = false;
    bool container_rules_configured_ = false;
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
