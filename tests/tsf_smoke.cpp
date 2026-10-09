#include "dictionary_wait.h"
#include "module.h"
#include "display_attributes.h"
#include "candidate_ui.h"
#include "tsf_test_host.h"
#include "isolated_settings.h"
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void succeeded(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::cerr << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << '\n'; throw std::runtime_error(message); }
}
struct KeyboardState {
    BYTE saved[256]{};
    KeyboardState() { GetKeyboardState(saved); }
    ~KeyboardState() { SetKeyboardState(saved); }
};
bool key(ITfKeyEventSink* sink, ITfContext* context, WPARAM code, bool shift = false, bool control = false) {
    BYTE state[256]{};
    if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (control) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    SetKeyboardState(state); // Thread-local state only; no SendInput/global keystrokes.
    const auto parameter = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
    BOOL probe = FALSE;
    succeeded(sink->OnTestKeyDown(context, code, parameter, &probe), "key probe failed");
    BOOL eaten = FALSE;
    // A host need not call OnKeyDown when the probe declines the key.
    if (probe) succeeded(sink->OnKeyDown(context, code, parameter, &eaten), "key handling failed");
    BOOL up = FALSE;
    succeeded(sink->OnTestKeyUp(context, code, parameter, &up), "key-up probe failed");
    check(up == eaten, "key-up disagrees with key-down");
    succeeded(sink->OnKeyUp(context, code, parameter, &up), "key-up failed");
    return eaten != FALSE;
}
void type(ITfKeyEventSink* sink, ITfContext* context, std::string_view text) {
    for (const char ch : text) {
        const auto translated = VkKeyScanExW(static_cast<wchar_t>(ch), GetKeyboardLayout(0));
        check(translated != -1 && !(HIBYTE(translated) & 6), "test character unavailable on this keyboard layout");
        check(key(sink, context, LOBYTE(translated), (HIBYTE(translated) & 1) != 0), "character not consumed");
    }
}
void adapter_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys, bool file_dictionary) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    host->reject_sink = true;
    check(FAILED(service->ActivateEx(host.Get(), 1, 0)), "failed sink subscription accepted");
    check(!host->keys && !host->events, "partial activation leaked a sink");
    host->reject_sink = false;
    for (int i = 0; i < 3; ++i) {
        succeeded(service->ActivateEx(host.Get(), 1, 0), "test-host activate failed"); wait_for_dictionary();
        check(service->ActivateEx(host.Get(), 1, 0) == E_UNEXPECTED, "double activation accepted");
        succeeded(service->Deactivate(), "test-host deactivate failed");
        check(!host->keys && !host->events, "deactivation leaked a sink");
    }
    succeeded(service->ActivateEx(host.Get(), 1, TF_TMF_IMMERSIVEMODE), "desktop immersive activate failed"); wait_for_dictionary();
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    type(keys, context.Get(), "Kanji");
    check(context->doc->text == L"かんじ", "reading was not composed");
    key(keys, context.Get(), VK_SPACE);
    check(context->doc->text == L"漢字", "candidate not displayed");
    type(keys, context.Get(), "desu");
    check(context->doc->text == L"漢字です", "committed prefix lost while continuing input");
    check(context->composition->ended, "direct input left a composition open");
    type(keys, context.Get(), "KaKu");
    check(context->doc->text == L"漢字です書く", "okuri composition incorrect");
    key(keys, context.Get(), VK_SPACE);
    key(keys, context.Get(), VK_RETURN);
    check(context->doc->text == L"漢字です描く", "okuri commit incorrect");
    type(keys, context.Get(), "Kanji");
    key(keys, context.Get(), VK_SPACE);
    key(keys, context.Get(), VK_ESCAPE);
    key(keys, context.Get(), VK_ESCAPE);
    check(context->doc->text == L"漢字です描く", "cancel changed committed prefix");
    context->reject_session = true;
    check(!key(keys, context.Get(), 'K', true), "refused edit ate a key");
    context->reject_session = false;
    context->reject_composition = true;
    check(!key(keys, context.Get(), 'K', true), "null composition was accepted");
    context->reject_composition = false;
    type(keys, context.Get(), "a");
    check(context->doc->text == L"漢字です描くあ", "rejected key mutated the engine");
    type(keys, context.Get(), "Kanji");
    context->doc->reject_write = true;
    check(!key(keys, context.Get(), VK_SPACE), "failed SetText ate a key");
    context->doc->reject_write = false;
    key(keys, context.Get(), VK_SPACE);
    check(context->doc->text == L"漢字です描くあ漢字", "failed SetText destroyed the old state");
    context->doc->reject_shift = true;
    type(keys, context.Get(), "d");
    check(context->doc->text == L"漢字です描くあ漢字d", "range failure lost committed prefix or left markers");
    context->doc->reject_shift = false;
    // Delayed cleanup for the previous field must not modify the new field.
    type(keys, context.Get(), "Kanji");
    keys->OnSetFocus(FALSE);
    check(context->queued.size() == 1, "focus cleanup was not queued");
    ComPtr<test_host::Context> next; next.Attach(new test_host::Context);
    type(keys, next.Get(), "KaKu");
    context->pump();
    check(next->doc->text == L"書く", "old cleanup modified the new field");
    check(context->doc->text == L"漢字です描くあ漢字dかんじ", "old composition retained a marker");
    next->composition->terminate();
    check(next->doc->text == L"書く", "host termination did not remove markers");
    type(keys, next.Get(), "a");
    check(next->doc->text == L"書くあ", "terminated composition reused old state");
    next->readonly = true;
    check(!key(keys, next.Get(), 'A'), "read-only context consumed input");
    {
        ComPtr<test_host::Context> shifted; shifted.Attach(new test_host::Context);
        type(keys, shifted.Get(), "KA");
        check(shifted->doc->text == L"か", "held Shift split the first romaji syllable");
        type(keys, shifted.Get(), "nji");
        key(keys, shifted.Get(), VK_SPACE); key(keys, shifted.Get(), VK_RETURN);
        type(keys, shifted.Get(), "KAKU");
        check(shifted->doc->text == L"漢字書く", "held Shift broke dictionary conversion or okuri");
        key(keys, shifted.Get(), VK_RETURN);
        check(shifted->composition->ended, "shifted input left a composition open");
    }
    if (file_dictionary) {
        ComPtr<test_host::Context> dictionary_context; dictionary_context.Attach(new test_host::Context);
        type(keys, dictionary_context.Get(), "Jisho");
        key(keys, dictionary_context.Get(), VK_SPACE);
        key(keys, dictionary_context.Get(), VK_RETURN);
        check(dictionary_context->doc->text == L"辞書", "TSF did not use the configured dictionary file");
        type(keys, dictionary_context.Get(), "/skk"); key(keys, dictionary_context.Get(), VK_SPACE);
        check(dictionary_context->doc->text == L"辞書かな漢字変換" && host->ui, "abbrev did not use the dictionary or candidate UI");
        key(keys, dictionary_context.Get(), VK_SPACE); key(keys, dictionary_context.Get(), VK_RETURN);
        type(keys, dictionary_context.Get(), "a");
        check(dictionary_context->doc->text == L"辞書日本語入力あ", "abbrev commit did not return to kana input");
    }
    succeeded(service->Deactivate(), "deactivation failed");
    context->pump(); next->pump();
    check(!host->keys && !host->events, "adapter test leaked a sink");
}
void focus_transition_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    succeeded(service->ActivateEx(host.Get(), 1, 0), "focus-transition activate failed"); wait_for_dictionary();
    auto fresh = [] { ComPtr<test_host::Context> value; value.Attach(new test_host::Context); return value; };
    for (const bool transitory : {false, true}) {
        for (int missing = 0; missing != 3; ++missing) {
            for (const auto& syllable : {std::pair{"Ku", L"く"}, std::pair{"Hu", L"ふ"}, std::pair{"Ja", L"じゃ"}}) {
                auto context = fresh(); auto origin = transitory ? test_host::transitory_context(context.Get()) : context;
                auto* document = static_cast<test_host::DocumentManager*>(origin->document_manager.Get());
                type(keys, origin.Get(), std::string_view(syllable.first, 1));
                // Excel briefly has no resolved focus after the first SetText.
                if (missing == 1) document->top_status = E_FAIL;
                if (missing == 2) document->top = nullptr;
                for (int notice = 0; notice != 2; ++notice)
                    host->events->OnSetFocus(missing == 0 ? nullptr : document, document);
                check(context->queued.size() == 1 && context->edits, "unresolved focus was not deferred once");
                document->top_status = S_OK; document->top = origin.Get();
                host->events->OnSetFocus(document, nullptr);
                context->pump(); type(keys, origin.Get(), syllable.first + 1);
                check(context->doc->text == syllable.second, "transient document focus split the first romaji syllable");
                key(keys, origin.Get(), VK_RETURN);
            }
        }
    }
    {
        auto context = fresh(); type(keys, context.Get(), "K");
        host->events->OnSetFocus(nullptr, context->document_manager.Get());
        // Synchronous input may precede the queued focus check, even if a
        // restoring focus notification has not arrived yet.
        type(keys, context.Get(), "u"); context->pump();
        check(context->doc->text == L"く" && !context->composition->ended, "deferred check ended validated input");
        key(keys, context.Get(), VK_RETURN);
    }
    {
        auto context = fresh(); type(keys, context.Get(), "K");
        host->events->OnSetFocus(nullptr, context->document_manager.Get()); keys->OnSetFocus(FALSE);
        type(keys, context.Get(), "H"); context->pump(); context->pump();
        type(keys, context.Get(), "u"); key(keys, context.Get(), VK_RETURN);
        check(context->doc->text == L"kふ", "old deferred check ended a new session in the same store");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "K");
        host->events->OnSetFocus(nullptr, context->document_manager.Get());
        context->pump(); context->pump();
        check(context->composition->ended && !context->edits && !host->ui, "persistent focus loss retained composition");
        type(keys, context.Get(), "u"); check(context->doc->text == L"kう", "persistent focus loss retained romaji");
    }
    for (int departure = 0; departure != 7; ++departure) {
        auto first = fresh(), second = fresh(); type(keys, first.Get(), "K");
        host->events->OnSetFocus(nullptr, first->document_manager.Get());
        switch (departure) {
        case 0: host->events->OnSetFocus(second->document_manager.Get(), nullptr); break;
        case 1: host->events->OnPopContext(first.Get()); break;
        case 2: host->events->OnUninitDocumentMgr(first->document_manager.Get()); break;
        case 3: keys->OnSetFocus(FALSE); break;
        case 4: first->composition->terminate(); break;
        case 5:
            succeeded(service->Deactivate(), "pending-focus deactivate failed");
            succeeded(service->ActivateEx(host.Get(), 1, 0), "pending-focus reactivate failed"); break;
        case 6: check(!key(keys, first.Get(), VK_LEFT), "pending-focus navigation consumed"); break;
        }
        check(!first->edits, "known departure did not detach immediately");
        type(keys, second.Get(), "H"); first->pump(); first->pump();
        check(second->doc->text == L"h" && !second->composition->ended, "old deferred check ended a new composition");
        type(keys, second.Get(), "u"); key(keys, second.Get(), VK_RETURN);
        check(first->doc->text == L"k" && first->composition->ended && second->doc->text == L"ふ", "departure carried input across fields");
    }
    {
        auto first = fresh(); type(keys, first.Get(), "K");
        host->events->OnSetFocus(nullptr, first->document_manager.Get());
        // Moving inside the same text store must still invalidate its range/caret.
        first->move_selection(0, 0); type(keys, first.Get(), "u");
        first->pump(); first->pump();
        check(first->doc->text == L"うk", "same-store caret movement retained pending romaji");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "K");
        const auto old = context->composition;
        context->reject_session = true; host->events->OnSetFocus(nullptr, context->document_manager.Get());
        check(!context->edits && context->queued.empty(), "refused focus check retained its state");
        context->reject_session = false; old->terminate();
        type(keys, context.Get(), "u"); check(context->doc->text == L"kう", "refused check retained pending romaji");
    }
    for (int reentry = 0; reentry != 3; ++reentry) {
        auto first = fresh(), second = fresh(); type(keys, first.Get(), "K");
        auto* document = static_cast<test_host::DocumentManager*>(second->document_manager.Get());
        document->top_query = [&] {
            if (reentry == 1) {
                host->events->OnSetFocus(second->document_manager.Get(), first->document_manager.Get());
                type(keys, second.Get(), "H");
            } else if (reentry == 2) {
                type(keys, first.Get(), "u");
            } else {
                host->events->OnSetFocus(nullptr, first->document_manager.Get());
                host->events->OnSetFocus(first->document_manager.Get(), nullptr);
            }
        };
        host->events->OnSetFocus(second->document_manager.Get(), first->document_manager.Get());
        first->pump(); first->pump();
        auto target = reentry == 1 ? second : first;
        check(target->composition && target->edits, "stale focus resolution ended the reentrant session");
        if (reentry != 2) type(keys, target.Get(), "u");
        key(keys, target.Get(), VK_RETURN);
        check(target->doc->text == (reentry == 1 ? L"ふ" : L"く"), "focus reentry split pending romaji");
    }
    succeeded(service->Deactivate(), "focus-transition deactivate failed");
}
void transitory_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    succeeded(service->ActivateEx(host.Get(), 1, 0), "transitory activate failed"); wait_for_dictionary();
    auto fresh = [] { ComPtr<test_host::Context> value; value.Attach(new test_host::Context); return value; };
    auto reading = [&]() {
        GUID guid{};
        return host->ui && SUCCEEDED(host->ui->GetGUID(&guid)) && guid == nskkzime::windows::reading_ui_guid;
    };
    auto description = [&]() {
        BSTR value = nullptr;
        check(host->ui && SUCCEEDED(host->ui->GetDescription(&value)), "reading description missing");
        const std::wstring result(value, SysStringLen(value)); SysFreeString(value); return result;
    };
    {
        auto parent = fresh(); auto origin = test_host::transitory_context(parent.Get());
        type(keys, origin.Get(), "K");
        check(reading() && description().find(L"▽k") != std::wstring::npos && parent->doc->text == L"k",
              "first redirected key did not present its reading outside the document");
        const auto retained = host->ui; const auto begins = host->ui_begins;
        BOOL shown = TRUE; retained->IsShown(&shown); check(!shown, "ignored host UI suppression");
        type(keys, origin.Get(), "anji");
        check(host->ui_begins == begins && description().find(L"▽かんじ") != std::wstring::npos,
              "reading updates reopened or failed to refresh UI");
        key(keys, origin.Get(), VK_SPACE);
        check(host->ui && !reading() && parent->doc->text == L"漢字", "reading did not transition to candidates");
        retained->Show(TRUE); retained->IsShown(&shown); check(!shown, "old reading element reopened after transition");
        key(keys, origin.Get(), VK_ESCAPE);
        check(reading() && parent->doc->text == L"かんじ", "candidate cancel did not restore reading UI");
        const auto queries = parent->layout_queries;
        parent->layout->OnLayoutChange(parent.Get(), TF_LC_CHANGE, nullptr); parent->pump();
        check(parent->layout_queries > queries, "reading layout was not refreshed");
        key(keys, origin.Get(), VK_ESCAPE);
        check(!host->ui && parent->doc->text.empty(), "reading cancel retained UI or markers");
        type(keys, origin.Get(), "k"); check(reading(), "pending direct romaji missing reading UI");
        type(keys, origin.Get(), "a"); check(!host->ui && parent->doc->text == L"か", "direct kana retained reading UI");
        type(keys, origin.Get(), "/"); check(reading(), "empty abbrev lost reading state");
        key(keys, origin.Get(), VK_ESCAPE); check(!host->ui, "abbrev cancel retained reading UI");
        host->reject_ui = true;
        type(keys, origin.Get(), "Ka"); key(keys, origin.Get(), VK_RETURN);
        check(!host->ui && parent->doc->text == L"かか", "reading presentation failure broke text input");
        host->reject_ui = false;
    }
    {
        auto parent = fresh(); auto origin = test_host::transitory_context(parent.Get());
        parent->doc->text = L"前後"; parent->doc->caret = 1;
        type(keys, origin.Get(), "Ka");
        check(parent->doc->text == L"前か後" && origin->doc->text.empty() && parent->edits && !origin->edits,
              "transitory input did not use the full parent's text and notifications");
        host->events->OnSetFocus(origin->document_manager.Get(), nullptr);
        host->events->OnPushContext(origin.Get());
        check(parent->queued.empty() && !parent->composition->ended, "origin focus prematurely ended parent composition");
        check(!key(keys, origin.Get(), VK_LEFT) && parent->composition->ended && parent->queued.empty(),
              "navigation did not end parent composition before returning the key");
        parent->move_selection(1, 1); // Host processes the declined Left on the already inserted text.
        type(keys, origin.Get(), "aKanji"); key(keys, origin.Get(), VK_SPACE);
        check(parent->doc->text == L"前あ漢字か後" && host->ui, "parent candidate conversion failed");
        parent->move_selection(0, 0);
        check(!host->ui && parent->queued.size() == 1, "parent selection change did not detach composition");
        parent->pump(); type(keys, origin.Get(), "a");
        check(parent->doc->text == L"あ前あ漢字か後" && origin->doc->text.empty(), "click moved or overwrote old conversion");
        type(keys, origin.Get(), "Ka");
        // Disconnecting the extension can clear its parent link before OnPopContext.
        static_cast<test_host::DocumentManager*>(origin->document_manager.Get())->transitory_parent.Reset();
        host->events->OnPopContext(origin.Get()); parent->pump();
        check(parent->composition->ended && !parent->edits && !parent->layout && !host->ui, "origin pop retained parent composition or UI");
    }
    for (const bool source_document : {false, true}) {
        auto parent = fresh(); auto origin = test_host::transitory_context(parent.Get());
        type(keys, origin.Get(), "Ka");
        host->events->OnUninitDocumentMgr(source_document ? origin->document_manager.Get() : parent->document_manager.Get());
        parent->pump(); check(parent->composition->ended, "document teardown retained parent composition");
    }
    {
        auto first = fresh(), second = fresh(); auto origin = test_host::transitory_context(first.Get());
        type(keys, origin.Get(), "Ka");
        auto* manager = static_cast<test_host::DocumentManager*>(origin->document_manager.Get());
        static_cast<test_host::ParentCompartment*>(manager->transitory_parent.Get())->parent = second->document_manager;
        host->events->OnSetFocus(origin->document_manager.Get(), nullptr);
        type(keys, origin.Get(), "a"); first->pump();
        check(first->doc->text == L"か" && first->composition->ended && second->doc->text == L"あ",
              "reused transitory document mixed fields or delayed cleanup");
    }
    for (const bool source_readonly : {false, true}) {
        auto parent = fresh(); auto origin = test_host::transitory_context(parent.Get());
        (source_readonly ? origin : parent)->readonly = true;
        check(!key(keys, origin.Get(), 'A') && parent->doc->text.empty() && origin->doc->text.empty(),
              "context redirection bypassed read-only input");
    }
    for (int failure = 0; failure != 9; ++failure) {
        auto parent = fresh(); auto origin = test_host::transitory_context(parent.Get());
        auto* source = static_cast<test_host::DocumentManager*>(origin->document_manager.Get());
        auto* owner = static_cast<test_host::DocumentManager*>(parent->document_manager.Get());
        auto* link = static_cast<test_host::ParentCompartment*>(source->transitory_parent.Get());
        switch (failure) {
        case 0: source->transitory_parent.Reset(); break;
        case 1: link->type = VT_I4; break;
        case 2: link->parent.Reset(); break;
        case 3: link->result = E_FAIL; break;
        case 4: link->result = S_FALSE; break;
        case 5: owner->top_status = E_FAIL; break;
        case 6: owner->top = nullptr; break;
        case 7: parent->status_result = E_FAIL; break;
        case 8: parent->static_flags = TF_SS_TRANSITORY; break;
        }
        type(keys, origin.Get(), "Ka"); key(keys, origin.Get(), VK_RETURN);
        check(origin->doc->text == L"か" && parent->doc->text.empty(), "unavailable parent broke original context input");
    }
    {
        auto parent = fresh(); auto native = test_host::transitory_context(parent.Get()); native->static_flags = 0;
        type(keys, native.Get(), "Ka"); check(!host->ui, "native text store unnecessarily showed reading popup");
        key(keys, native.Get(), VK_RETURN);
        check(native->doc->text == L"か" && parent->doc->text.empty(), "nontransitory native context was redirected");
    }
    succeeded(service->Deactivate(), "transitory deactivate failed");
    check(!host->keys && !host->events && !host->ui, "transitory test leaked subscriptions");
}
void selection_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    succeeded(service->ActivateEx(host.Get(), 1, 0), "selection-test activate failed"); wait_for_dictionary();
    auto fresh = [] { ComPtr<test_host::Context> value; value.Attach(new test_host::Context); return value; };
    for (const bool parent_context : {false, true}) {
        auto context = fresh(); auto origin = parent_context ? test_host::transitory_context(context.Get()) : context;
        context->doc->text = L"前後"; context->doc->caret = 1;
        type(keys, origin.Get(), "Kanji"); key(keys, origin.Get(), VK_SPACE);
        const auto writes = context->doc->writes;
        // The current candidate is already in the text store. Committing it is
        // a composition lifecycle change, not another text insertion.
        context->doc->reject_write = true;
        check(key(keys, origin.Get(), VK_RETURN) && context->doc->writes == writes && context->composition->ended &&
              context->doc->text == L"前漢字後" && context->doc->caret == 3 && !host->ui,
              "unchanged candidate commit rewrote text or failed to finish");
        context->doc->reject_write = false;
        type(keys, origin.Get(), "Ka"); const auto reading_writes = context->doc->writes;
        key(keys, origin.Get(), VK_RETURN);
        check(context->doc->writes == reading_writes && context->doc->text == L"前漢字か後", "literal confirmation rewrote its reading");
        type(keys, origin.Get(), "Kanji"); key(keys, origin.Get(), VK_SPACE);
        const auto first_writes = context->doc->writes;
        key(keys, origin.Get(), VK_PRIOR);
        check(context->doc->writes == first_writes, "page edge rewrote unchanged candidate");
        type(keys, origin.Get(), "a");
        check(context->doc->writes == first_writes + 1 && context->doc->text == L"前漢字か漢字あ後",
              "commit and additional kana skipped a necessary edit");
    }
    for (const bool reenter : {false, true}) {
        auto context = fresh(); type(keys, context.Get(), "Ka");
        const auto writes = context->doc->writes;
        unsigned reads = 0;
        context->doc->read_query = [&] {
            // Two reads check the existing composition at key entry. Change
            // the host on the later no-op comparison, without prior notice.
            if (++reads == 3) {
                if (reenter) context->composition->terminate();
                else context->doc->replace(0, 1, L"改");
            }
        };
        check(!key(keys, context.Get(), VK_RETURN) && context->doc->writes == writes && context->doc->text == (reenter ? L"か" : L"改"),
              "no-op comparison overwrote a host edit or adopted reentrant composition state");
        context->doc->read_query = {}; context->pump();
        type(keys, context.Get(), "a");
        check(context->doc->text == (reenter ? L"かあ" : L"改あ"), "comparison invalidation leaked old reading into next input");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "aKa"); key(keys, context.Get(), VK_RETURN);
        check(context->doc->text == L"あか" && context->doc->caret == 2 && context->selection_writes == 0,
              "already-correct host selection was rewritten");
    }
    {
        auto context = fresh(); context->doc->move_selection_on_write = false;
        type(keys, context.Get(), "aKa"); key(keys, context.Get(), VK_RETURN);
        check(context->doc->text == L"あか" && context->doc->caret == 2 && context->selection_writes == 2,
              "host without automatic caret movement was not repaired");
    }
    for (const bool unreadable : {false, true}) {
        auto context = fresh(); context->reject_selection_read = unreadable;
        context->interim_selection = !unreadable;
        type(keys, context.Get(), "a");
        check(context->doc->text == L"あ" && context->doc->caret == 1 && context->selection_writes == 1 && !context->interim_selection,
              "unknown or interim selection skipped the normal update");
    }
    {
        auto context = fresh(); context->doc->text = L"前後";
        context->doc->selection_end = 2; context->doc->move_selection_on_write = false;
        type(keys, context.Get(), "a");
        check(context->doc->text == L"あ" && context->doc->caret == 1 && context->doc->selection_end == 1 && context->selection_writes == 1,
              "nonempty selection was not collapsed after replacement");
    }
    {
        auto context = fresh(); context->doc->text = L"前後"; context->doc->caret = 1;
        type(keys, context.Get(), "/");
        check(context->doc->text == L"前後" && context->composition && !context->composition->ended,
            "empty abbrev must track its range without adding a slash");
        type(keys, context.Get(), "SKKql");
        check(context->doc->text == L"前SKKql後", "abbrev lost ASCII spelling or damaged surrounding text");
        key(keys, context.Get(), VK_ESCAPE); type(keys, context.Get(), "a");
        check(context->doc->text == L"前あ後", "abbrev cancel did not restore kana input");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "/a"); key(keys, context.Get(), VK_BACK);
        check(context->doc->text.empty() && !context->composition->ended, "empty abbrev lost its composition after deletion");
        type(keys, context.Get(), "q"); check(context->doc->text == L"q", "empty abbrev stopped accepting ASCII");
        key(keys, context.Get(), VK_ESCAPE);
        type(keys, context.Get(), "/"); key(keys, context.Get(), VK_BACK); type(keys, context.Get(), "a");
        check(context->doc->text == L"あ", "backspace on empty abbrev did not exit");
    }
    {
        auto context = fresh(); context->doc->text = L"前後"; context->doc->caret = 1;
        type(keys, context.Get(), "/"); context->move_selection(0, 0);
        type(keys, context.Get(), "a"); context->pump();
        check(context->doc->text == L"あ前後", "empty abbrev followed a caret move");
        type(keys, context.Get(), "/"); keys->OnSetFocus(FALSE);
        auto next = fresh(); type(keys, next.Get(), "a"); context->pump();
        check(next->doc->text == L"あ", "empty abbrev leaked to the next input field");
    }
    {
        auto context = fresh(); context->doc->text = L"前後"; context->doc->caret = 1;
        type(keys, context.Get(), "Kanji");
        context->move_selection(0, 0);
        check(context->queued.size() == 1 && !context->edits, "selection change did not detach and schedule completion");
        type(keys, context.Get(), "a"); // A key can arrive before the queued completion.
        context->pump();
        check(context->doc->text == L"あ前かんじ後" && context->doc->caret == 1, "delayed completion changed text or moved the new caret");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->move_selection(1, 2); context->pump();
        type(keys, context.Get(), "a");
        check(context->doc->text == L"かあじ", "new input ignored the user's range selection");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        auto old = context->composition;
        context->move_selection(0, 0);
        type(keys, context.Get(), "KaKu");
        check(old->ended && !context->composition->ended, "new composition raced with pending completion in the same field");
        context->pump();
        check(context->doc->text == L"書くかんじ" && !context->composition->ended, "queued old completion ended new composition");
        key(keys, context.Get(), VK_RETURN);
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->host_replace(1, 2, L"改");
        check(!key(keys, context.Get(), VK_SPACE), "external edit retained the old conversion state");
        context->pump();
        check(context->doc->text == L"か改じ", "completion reverted an application's edit");
        type(keys, context.Get(), "a");
        check(context->doc->text == L"か改じあ", "input after external edit reused stale reading");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->doc->caret = context->doc->selection_end = 0; // Missing/late notification.
        type(keys, context.Get(), "a");
        check(context->doc->text == L"あかんじ" && context->composition->ended, "write-time selection validation failed");
    }
    {
        auto context = fresh(); context->doc->text = L"前"; context->doc->caret = 1;
        type(keys, context.Get(), "Kanji");
        context->host_replace(0, 1, L"接頭");
        check(context->queued.empty(), "edit outside the composition cancelled input");
        key(keys, context.Get(), VK_SPACE); key(keys, context.Get(), VK_RETURN);
        check(context->doc->text == L"接頭漢字", "range tracking lost the composition after a prefix edit");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        BYTE state[256]{}; SetKeyboardState(state);
        for (int attempt = 0; attempt < 2; ++attempt) {
            BOOL eaten = FALSE;
            succeeded(keys->OnTestKeyDown(context.Get(), 'A', 0, &eaten), "SKK key probe failed");
            check(eaten && context->doc->text == L"かんじ" && !context->composition->ended, "SKK key probe mutated state");
        }
        for (const auto code : {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_DELETE, VK_TAB}) {
            BOOL eaten = TRUE;
            succeeded(keys->OnTestKeyDown(context.Get(), code, 0, &eaten), "navigation probe failed");
            check(!eaten, "navigation probe claimed an application key");
            check(context->composition->ended && context->doc->text == L"かんじ", "navigation preflight changed text or did not finish");
            context = fresh(); type(keys, context.Get(), "Kanji");
        }
        check(!key(keys, context.Get(), VK_LEFT), "navigation key was consumed");
        check(context->composition->ended && context->queued.empty(), "navigation preflight did not finish the composition");
        context->move_selection(2, 2); // The application receives the first Left.
        type(keys, context.Get(), "a"); context->pump();
        check(context->composition->ended && !context->edits && context->doc->text == L"かんあじ", "input did not use the application's new caret");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        BYTE state[256]{}; state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
        for (const auto code : {'A', 'C', 'V', 'X', 'Z'}) {
            SetKeyboardState(state);
            BOOL eaten = TRUE;
            succeeded(keys->OnTestKeyDown(context.Get(), code, 0, &eaten), "shortcut probe failed");
            check(!eaten, "shortcut probe claimed an application key");
            check(context->composition->ended && context->doc->text == L"かんじ", "shortcut preflight changed text or did not finish");
            context = fresh(); type(keys, context.Get(), "Kanji");
        }
        check(!key(keys, context.Get(), 'V', false, true), "paste shortcut was consumed");
        check(context->composition->ended && context->queued.empty(), "shortcut preflight did not finish the composition");
        context->host_replace(0, 3, L"貼り付け");
        type(keys, context.Get(), "a"); context->pump();
        check(context->doc->text == L"貼り付けあ", "input after paste restored stale text");
    }
    {
        auto context = fresh(); context->reject_edit_sink = true;
        check(!key(keys, context.Get(), 'K', true), "unmonitored composition consumed input");
        check(context->doc->text.empty() && !context->composition, "failed edit sink changed the document");
        context->reject_edit_sink = false; type(keys, context.Get(), "a");
        check(context->doc->text == L"あ", "failed edit sink changed the engine");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        auto previous = context->composition;
        context->defer_optional_session = true;
        check(!key(keys, context.Get(), VK_LEFT), "deferred cleanup claimed navigation");
        check(!key(keys, context.Get(), VK_LEFT), "repeated navigation probe claimed the key");
        check(context->queued.size() == 1 && !context->edits, "repeated probe duplicated deferred cleanup");
        context->move_selection(0, 0);
        type(keys, context.Get(), "KaKu"); context->pump();
        check(previous->ended && !context->composition->ended && context->doc->text == L"書くかんじ",
            "deferred preflight cleanup changed the new composition");
        key(keys, context.Get(), VK_RETURN);
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->doc->replace(0, 3, L"置換済み");
        context->composition->terminate();
        check(context->doc->text == L"置換済み" && !context->edits, "host termination overwrote the current document");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "Kanji");
        context->doc->reject_read = true; type(keys, context.Get(), "a");
        context->doc->reject_read = false;
        check(context->doc->text == L"かんじあ" && context->composition->ended, "failed validation read overwrote unverified text");
    }
    {
        auto context = fresh(); type(keys, context.Get(), "K" + std::string(300, 'a'));
        check(context->queued.empty() && context->edits, "long composition failed chunked content validation");
        key(keys, context.Get(), VK_RETURN);
        check(context->doc->text.size() == 300 && context->composition->ended, "long composition did not commit");
    }
    succeeded(service->Deactivate(), "selection-test deactivation failed");
    check(!host->keys && !host->events, "selection test leaked a sink");
}
void attribute_tests(ITfTextInputProcessorEx* service) {
    ComPtr<ITfDisplayAttributeProvider> provider;
    succeeded(service->QueryInterface(IID_PPV_ARGS(&provider)), "display attribute provider missing");
    ComPtr<IEnumTfDisplayAttributeInfo> items;
    succeeded(provider->EnumDisplayAttributeInfo(&items), "attribute enumeration failed");
    ComPtr<ITfDisplayAttributeInfo> reading;
    check(items->Next(1, &reading, nullptr) == S_OK, "reading attribute missing");
    TF_DISPLAYATTRIBUTE attribute{};
    succeeded(reading->GetAttributeInfo(&attribute), "reading attribute not readable");
    check(attribute.bAttr == TF_ATTR_INPUT && attribute.lsStyle == TF_LS_DOT, "reading underline incorrect");
    ComPtr<IEnumTfDisplayAttributeInfo> copy;
    succeeded(items->Clone(&copy), "attribute enumeration clone failed");
    ComPtr<ITfDisplayAttributeInfo> converted;
    check(copy->Next(1, &converted, nullptr) == S_OK, "cloned enumeration lost its position");
    succeeded(converted->GetAttributeInfo(&attribute), "converted attribute not readable");
    check(attribute.bAttr == TF_ATTR_TARGET_CONVERTED && attribute.fBoldLine, "candidate underline incorrect");
    GUID id{}; succeeded(converted->GetGUID(&id), "attribute GUID missing");
    check(id == nskkzime::windows::converted_attribute_guid, "wrong stable attribute GUID");
    ComPtr<ITfDisplayAttributeInfo> by_guid;
    succeeded(provider->GetDisplayAttributeInfo(id, &by_guid), "attribute lookup failed");
    attribute.lsStyle = TF_LS_NONE;
    succeeded(by_guid->SetAttributeInfo(&attribute), "attribute customization failed");
    succeeded(by_guid->Reset(), "attribute reset failed");
    succeeded(by_guid->GetAttributeInfo(&attribute), "reset attribute not readable");
    check(attribute.lsStyle == TF_LS_SOLID, "reset did not restore defaults");
    ComPtr<ITfDisplayAttributeInfo> end;
    check(copy->Next(1, &end, nullptr) == S_FALSE, "attribute enumeration did not end");
    check(provider->GetDisplayAttributeInfo(GUID_NULL, &end) == E_INVALIDARG, "unknown attribute GUID accepted");
    check(items->Next(2, &end, nullptr) == E_POINTER, "enumerator accepted missing count");
}
void missing_dictionary_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    succeeded(service->ActivateEx(host.Get(), 1, 0), "test-host activate failed"); wait_for_dictionary();
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    type(keys, context.Get(), "Kanji");
    key(keys, context.Get(), VK_SPACE);
    check(context->doc->text == L"かんじ", "missing dictionary used demo candidates or lost the reading");
    key(keys, context.Get(), VK_RETURN);
    type(keys, context.Get(), "desu");
    check(context->doc->text == L"かんじです", "kana input failed without a dictionary");
    succeeded(service->Deactivate(), "deactivation failed");
    context->pump();
    check(!host->keys && !host->events, "missing-dictionary test leaked a sink");
}
void candidate_ui_tests(ITfTextInputProcessorEx* service, ITfKeyEventSink* keys) {
    KeyboardState keyboard;
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    succeeded(service->ActivateEx(host.Get(), 1, 0), "candidate UI activation failed"); wait_for_dictionary();
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    type(keys, context.Get(), "Kanji");
    check(!host->ui, "reading showed candidate UI");
    key(keys, context.Get(), VK_SPACE);
    check(host->ui && host->ui_begins == 1, "selection did not begin candidate UI");
    check(host->ui_updates == 1, "UILess host did not receive the initial candidate contents");
    ComPtr<ITfCandidateListUIElement> list; succeeded(host->ui.As(&list), "candidate list interface missing");
    UINT count = 0, selected = 99;
    list->GetCount(&count); check(count == 2, "candidate UI count incorrect");
    BOOL shown = TRUE; list->IsShown(&shown); check(!shown, "host did not suppress native UI");
    BSTR text = nullptr; list->GetString(0, &text);
    const bool correct = text && std::wstring_view(text) == L"漢字"; SysFreeString(text);
    check(correct, "candidate UI text or annotation incorrect");
    key(keys, context.Get(), VK_SPACE);
    list->GetSelection(&selected); check(selected == 1 && host->ui_updates == 2, "candidate UI selection not updated");
    // Page probes stay pure; an actual page edit adopts the selection only on success.
    for (int i = 0; i < 3; ++i) {
        BOOL eaten = FALSE; succeeded(keys->OnTestKeyDown(context.Get(), VK_PRIOR, 0, &eaten), "page probe failed");
        list->GetSelection(&selected);
        check(eaten && selected == 1 && context->doc->text == L"感じ" && !context->composition->ended, "page probe changed selection or committed");
    }
    check(key(keys, context.Get(), VK_PRIOR), "page up not consumed");
    list->GetSelection(&selected); check(selected == 0 && context->doc->text == L"漢字", "page up did not select first");
    context->doc->reject_write = true;
    check(!key(keys, context.Get(), VK_NEXT), "failed page edit consumed key");
    list->GetSelection(&selected); check(selected == 0 && context->doc->text == L"漢字", "failed page edit changed selection");
    context->doc->reject_write = false;
    check(key(keys, context.Get(), VK_NEXT) && key(keys, context.Get(), VK_NEXT), "page down or edge not consumed");
    list->GetSelection(&selected);
    check(selected == 1 && context->doc->text == L"感じ" && !context->composition->ended, "page edge committed or exhausted");
    UINT custom_pages[]{0, 1}; succeeded(list->SetPageIndex(custom_pages, 2), "custom selection pages rejected");
    check(key(keys, context.Get(), '2') && context->doc->text == L"感じ" && !context->composition->ended,
        "missing shortcut row committed or inserted number");
    for (int i = 0; i < 3; ++i) {
        BOOL eaten = FALSE; succeeded(keys->OnTestKeyDown(context.Get(), '1', 0, &eaten), "numeric probe failed");
        check(eaten && context->doc->text == L"感じ" && !context->composition->ended, "numeric probe changed composition");
    }
    context->reject_session = true;
    check(!key(keys, context.Get(), '1'), "refused numeric commit consumed shortcut");
    list->GetSelection(&selected); check(selected == 1 && context->doc->text == L"感じ" && !context->composition->ended,
        "failed numeric edit changed selection");
    context->reject_session = false;
    check(context->layout != nullptr, "composition did not subscribe to layout changes");
    auto layout = context->layout;
    // Destruction invalidates older layout requests; later changes may resume.
    const auto queries = context->layout_queries;
    layout->OnLayoutChange(context.Get(), TF_LC_CHANGE, nullptr);
    layout->OnLayoutChange(context.Get(), TF_LC_CHANGE, nullptr);
    check(context->queued.size() == 1, "layout notifications were not coalesced");
    layout->OnLayoutChange(context.Get(), TF_LC_DESTROY, nullptr); context->pump();
    check(context->layout_queries == queries, "destroyed view was queried by an old layout request");
    layout->OnLayoutChange(context.Get(), TF_LC_CHANGE, nullptr); context->pump();
    check(context->layout_queries == queries + 1, "layout queries did not resume for a new view");
    // Queue a layout update and end before it runs. It must not resurrect UI.
    layout->OnLayoutChange(context.Get(), TF_LC_CHANGE, nullptr);
    check(key(keys, context.Get(), '1') && context->doc->text == L"感じ", "numeric selection did not use host page"); context->pump();
    check(!host->ui && host->ui_ends == 1 && !context->layout, "commit retained UI or layout sink");
    list->Show(TRUE); list->IsShown(&shown); check(!shown, "retained UI reopened after commit");
    type(keys, context.Get(), "KaKu");
    check(host->ui != nullptr, "okuri selection did not begin UI");
    keys->OnSetFocus(FALSE);
    check(!host->ui, "focus loss left candidate UI visible"); context->pump();
    host->reject_ui = true;
    type(keys, context.Get(), "Kanji"); key(keys, context.Get(), VK_SPACE); key(keys, context.Get(), VK_RETURN);
    check(context->doc->text == L"感じ書く漢字", "candidate UI failure broke conversion");
    succeeded(service->Deactivate(), "candidate UI deactivation failed");
    check(!host->ui && !host->keys && !host->events, "candidate UI leaked subscriptions");
    succeeded(service->ActivateEx(host.Get(), 1, 0), "modified page activation failed"); wait_for_dictionary();
    ComPtr<test_host::Context> modified; modified.Attach(new test_host::Context);
    type(keys, modified.Get(), "Kanji"); key(keys, modified.Get(), VK_SPACE);
    check(!key(keys, modified.Get(), VK_NEXT, true) && modified->composition->ended && modified->doc->text == L"漢字", "shift-page did not preserve application navigation");
    host->reject_ui = false;
    ComPtr<test_host::Context> replaced; replaced.Attach(new test_host::Context);
    type(keys, replaced.Get(), "Kanji"); key(keys, replaced.Get(), VK_SPACE);
    replaced->doc->replace(0, 2, L"置換済み"); // Host edit notification has not arrived yet.
    check(key(keys, replaced.Get(), '2') && replaced->doc->text == L"置換済み2" && !host->ui,
        "numeric shortcut reused a stale composition");
    ComPtr<test_host::Context> punctuation; punctuation.Attach(new test_host::Context);
    type(keys, punctuation.Get(), "Kanji"); key(keys, punctuation.Get(), VK_SPACE); type(keys, punctuation.Get(), "!");
    check(punctuation->doc->text == L"漢字!" && !host->ui, "shifted punctuation selected a numeric row");
    type(keys, punctuation.Get(), "Kanji"); key(keys, punctuation.Get(), VK_SPACE);
    check(!key(keys, punctuation.Get(), '1', false, true) && punctuation->composition->ended && !host->ui,
        "Ctrl-number shortcut was consumed");
    succeeded(service->Deactivate(), "modified page deactivation failed");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::wstring_view(argv[2]) != L"--empty")) return 2;
    // --empty: an unreadable settings file must leave kana input working.
    const bool empty = argc == 3;
    const auto config = use_isolated_settings(std::filesystem::current_path() / (empty ? L"tsf-smoke-empty" : L"tsf-smoke"),
        empty ? std::string{} : "learning=off\ndictionary=" + settings_path(NSKKZIME_TEST_DICTIONARY) + "\n");
    if (empty) std::ofstream(config, std::ios::binary | std::ios::trunc) << "version=unsupported\n";
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    const auto library = LoadLibraryW(argv[1]);
    if (!library) { CoUninitialize(); return 1; }
    int result = 0;
    try {
        using GetFactory = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        using CanUnload = HRESULT(WINAPI*)();
        using GetStatus = HRESULT(WINAPI*)(BSTR*);
        using EnableTiming = HRESULT(WINAPI*)(BOOL);
        // GetProcAddress is the documented Windows pattern for these COM exports.
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto get_factory = reinterpret_cast<GetFactory>(GetProcAddress(library, "DllGetClassObject"));
        const auto can_unload = reinterpret_cast<CanUnload>(GetProcAddress(library, "DllCanUnloadNow"));
        const auto get_status = reinterpret_cast<GetStatus>(GetProcAddress(library, "NskkzimeGetDictionaryStatus"));
        const auto get_timing = reinterpret_cast<GetStatus>(GetProcAddress(library, "NskkzimeGetInputTiming"));
        const auto enable_timing = reinterpret_cast<EnableTiming>(GetProcAddress(library, "NskkzimeEnableInputTiming"));
#pragma warning(pop)
        check(get_factory && can_unload, "missing COM exports");
        check(get_status && get_status(nullptr) == E_POINTER, "invalid diagnostic export contract");
        check(get_timing && enable_timing && get_timing(nullptr) == E_POINTER, "invalid timing exports");
        const auto timing_text = [&] {
            BSTR value = nullptr; succeeded(get_timing(&value), "timing status failed");
            const std::wstring text(value, SysStringLen(value)); SysFreeString(value); return text;
        };
        check(timing_text().find(L"オフ") != std::wstring::npos, "timing enabled by default");
        succeeded(enable_timing(TRUE), "timing enable failed");
        check(timing_text().find(L"TSF OnKeyDown: n=0") != std::wstring::npos, "new timing included old keys");
        const auto status_text = [&] {
            BSTR value = nullptr; succeeded(get_status(&value), "dictionary status failed");
            const std::wstring text(value, SysStringLen(value)); SysFreeString(value); return text;
        };
        check(status_text().find(L"まだ読み込んでいません") != std::wstring::npos, "status triggered dictionary loading");
        check(GetProcAddress(library, "DllRegisterServer") && GetProcAddress(library, "DllUnregisterServer"), "missing registration exports");
        check(can_unload() == S_OK, "fresh DLL cannot unload");
        {
            ComPtr<IClassFactory> factory;
            check(get_factory(CLSID_NULL, IID_PPV_ARGS(&factory)) == CLASS_E_CLASSNOTAVAILABLE, "unknown CLSID accepted");
            succeeded(get_factory(nskkzime::windows::service_clsid, IID_PPV_ARGS(&factory)), "factory failed");
            check(can_unload() == S_FALSE, "factory not counted");
            succeeded(factory->LockServer(TRUE), "lock failed");
            succeeded(factory->LockServer(FALSE), "unlock failed");
            check(factory->LockServer(FALSE) == E_UNEXPECTED, "unbalanced unlock accepted");
            ComPtr<ITfTextInputProcessorEx> service;
            succeeded(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service)), "create service failed");
            check(service->ActivateEx(nullptr, TF_CLIENTID_NULL, 0) == E_INVALIDARG, "null manager accepted");
            ComPtr<IUnknown> identity;
            succeeded(service.As(&identity), "IUnknown missing");
            ComPtr<ITfKeyEventSink> keys;
            succeeded(service.As(&keys), "key sink missing");
            ComPtr<IUnknown> key_identity;
            succeeded(keys.As(&key_identity), "key sink identity missing");
            check(identity.Get() == key_identity.Get(), "COM identity differs across interfaces");
            BOOL eaten = TRUE;
            succeeded(keys->OnTestKeyDown(nullptr, 'A', 0, &eaten), "null-context key probe failed");
            check(!eaten, "null-context key consumed");
            check(keys->OnTestKeyDown(nullptr, 'A', 0, nullptr) == E_POINTER, "null output accepted");
            ComPtr<ITfThreadMgr> manager;
            succeeded(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager)), "real TSF manager unavailable");
            TfClientId client = TF_CLIENTID_NULL;
            succeeded(manager->Activate(&client), "TSF manager activate failed");
            succeeded(manager->Deactivate(), "TSF manager deactivate failed");
            attribute_tests(service.Get());
            if (empty) missing_dictionary_tests(service.Get(), keys.Get());
            else { adapter_tests(service.Get(), keys.Get(), true); focus_transition_tests(service.Get(), keys.Get()); transitory_tests(service.Get(), keys.Get()); selection_tests(service.Get(), keys.Get()); candidate_ui_tests(service.Get(), keys.Get()); }
            const auto status = status_text();
            check(status.find(L"まだ読み込んでいません") == status.npos, "activation failed to update status");
            if (!empty) check(status.find(L"読み込み済み") != status.npos && status.find(config.native()) != status.npos, "loaded configuration invisible");
        }
        check(can_unload() == S_OK, "COM object or subscription leaked");
        check(timing_text().find(L"TSF OnKeyDown: n=0") == std::wstring::npos, "DLL key timing did not collect callbacks");
        check(timing_text().find(L"TSF本文編集: n=0") == std::wstring::npos &&
              timing_text().find(L"入力コア: n=0") == std::wstring::npos,
              "DLL operation timing did not collect synchronous edits");
        succeeded(enable_timing(FALSE), "timing disable failed");
        check(timing_text().find(L"オフ") != std::wstring::npos, "timing disable did not clear state");
        std::cout << "PASS COM exports/identity/unload, real TSF manager startup, "
                  << (empty ? "kana fallback without dictionary" : "test-host activation/composition/cancel/failure/focus isolation") << '\n';
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; result = 1; }
    // Unload only after the tested COM objects and sinks have been released.
    if (!result) FreeLibrary(library); // A failed assertion may leave a subscription alive until process exit.
    CoUninitialize();
    return result;
}
