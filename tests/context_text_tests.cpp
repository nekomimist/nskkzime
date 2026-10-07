#include "dictionary_wait.h"
#include "runtime_dictionary.h"
#include "tsf_test_host.h"
#include "context_text.h"
#include "module.h"
#include <iostream>
#include <stdexcept>

namespace nskkzime::windows {
HINSTANCE module_instance = nullptr;
std::atomic<long> module_objects{0}, server_locks{0};
}
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Field {
    ComPtr<test_host::Context> context;
    ComPtr<test_host::Range> target;
    Field(std::wstring text, LONG start, LONG end) {
        context.Attach(new test_host::Context); context->doc->text = std::move(text);
        target.Attach(new test_host::Range(context->doc, start, end));
    }
    SurroundingText capture(bool allow_unknown = false) { return surrounding_text(context.Get(), target.Get(), 1, allow_unknown); }
};
void capture_tests() {
    {
        Field f(L"前の文漢字後の文", 3, 5); const auto value = f.capture();
        check(value.permission == InputPermission::Allowed && value.available && value.before == U"前の文" && value.after == U"後の文", "surrounding text includes conversion");
        check(f.target->start == 3 && f.target->end == 5 && f.context->doc->text == L"前の文漢字後の文", "capture changed target/document");
    }
    {
        Field f(L"", 0, 0); const auto value = f.capture();
        check(value.available && value.before.empty() && value.after.empty() && !f.context->doc->reads, "empty document");
        Field first(L"語末", 0, 1); check(first.capture().after == U"末" && first.capture().before.empty(), "start boundary");
        Field last(L"先語", 1, 2); check(last.capture().before == U"先" && last.capture().after.empty(), "end boundary");
    }
    {
        std::wstring left, right;
        for (int i = 0; i < 50; ++i) { left += L"😀"; right += L"🍣"; }
        Field f(left + L"対象" + right, 100, 102); const auto value = f.capture();
        check(value.available && value.before == std::u32string(40, U'😀') && value.after == std::u32string(40, U'🍣'), "surrogate clipping / scalar limit");
        Field mixed(std::wstring(80, L'a') + L"前語後" + std::wstring(80, L'b'), 81, 82);
        const auto v = mixed.capture();
        check(v.available && v.before == std::u32string(39, U'a') + U"前" && v.after == U"後" + std::u32string(39, U'b'), "nearest 40 scalars");
    }
    for (const auto scope : {IS_PASSWORD, IS_PRIVATE, IS_NUMERIC_PASSWORD, IS_NUMERIC_PIN, IS_ALPHANUMERIC_PIN, IS_ALPHANUMERIC_PIN_SET}) {
        Field f(L"秘密対象秘密", 2, 4); f.context->input_scope = scope; const auto v = f.capture();
        check(v.permission == InputPermission::Restricted && !v.available && !f.context->doc->reads, "protected scope read text");
    }
    for (const auto hr : {S_FALSE, E_FAIL}) {
        Field f(L"前語後", 1, 2); f.context->scope_value_status = hr; const auto v = f.capture();
        check(v.permission == InputPermission::Unknown && !v.available && !f.context->doc->reads, "unknown/mixed scope read text");
    }
    {
        Field f(L"前語後", 1, 2); f.context->unknown_scope = true;
        check(!f.capture().available && !f.context->doc->reads, "missing scope read text");
    }
    for (const bool before : {true, false}) {
        Field f(L"前語後", 1, 2);
        f.context->scope_query = [before](ITfRange* range, InputScope& scope) {
            const auto r = static_cast<test_host::Range*>(range);
            if (before ? r->start == 0 : r->end == 3) scope = IS_PRIVATE;
            return S_OK;
        };
        const auto v = f.capture();
        check(v.permission == InputPermission::Allowed && !v.available && !f.context->doc->reads, "surrounding protected scope read text");
    }
    {
        Field f(L"前語後", 1, 2); f.context->doc->reject_shift = true;
        check(!f.capture().available && !f.context->doc->reads, "failed range expansion read text");
        f.context->doc->reject_shift = false; f.context->doc->reject_read = true;
        check(!f.capture().available, "failed read accepted");
        f.context->doc->reject_read = false; f.context->doc->read_limit = 0;
        const auto v = f.capture(); check(!v.available && v.before.empty() && v.after.empty(), "partial read accepted");
    }
    for (const auto bad : {wchar_t(0xd800), wchar_t(0xdc00), wchar_t(0), wchar_t(0xfffc)}) {
        Field f(std::wstring(L"前語") + bad, 1, 2); const auto v = f.capture();
        check(!v.available && v.before.empty() && v.after.empty(), "invalid text exposed partial context");
    }
    check(surrounding_text(nullptr, nullptr, 0).permission == InputPermission::Unknown, "missing target");
    check(!surrounding_text(nullptr,nullptr,0,true).available,"opt-in accepted a missing target");
    for (const auto status : {S_OK,S_FALSE,E_FAIL}) {
        Field f(L"前😀対象後🍣",3,5);
        f.context->unknown_scope=status==S_OK;f.context->scope_value_status=status;
        const auto value=f.capture(true);
        check(value.available&&value.permission==InputPermission::Unknown&&value.contains_unknown_scope&&
              value.before==U"前😀"&&value.after==U"後🍣","opt-in failed to preserve actual scope or exclude the target");
        f.context->doc->reads=0;
        check(!f.capture().available&&!f.context->doc->reads,"context opt-in persisted outside its request");
    }
    for (const bool target_unknown : {false,true}) {
        for (const auto scope : {IS_PASSWORD,IS_PRIVATE,IS_NUMERIC_PASSWORD,IS_NUMERIC_PIN,IS_ALPHANUMERIC_PIN,IS_ALPHANUMERIC_PIN_SET}) {
            Field f(L"前語後",1,2);
            f.context->scope_query=[=](ITfRange* range,InputScope& value) {
                const auto r=static_cast<test_host::Range*>(range);
                if (target_unknown && r->start==1 && r->end==2) return E_FAIL;
                value=scope;return S_OK;
            };
            check(!f.capture(true).available&&!f.context->doc->reads,"context opt-in read an explicitly protected range");
        }
    }
    for (const bool before : {false,true}) {
        Field f(L"前語後",1,2);
        f.context->scope_query=[=](ITfRange* range,InputScope&) {
            const auto r=static_cast<test_host::Range*>(range);
            return (before ? r->start==0 : r->end==3) ? S_FALSE : S_OK;
        };
        check(!f.capture().available&&!f.context->doc->reads,"unknown adjacent scope read without opt-in");
        const auto value=f.capture(true);
        check(value.available&&value.permission==InputPermission::Allowed&&value.contains_unknown_scope&&
              value.before==U"前"&&value.after==U"後","unknown adjacent scope lost persistence restriction");
    }
    {
        Field f(std::wstring(90,L'前')+L"語"+std::wstring(90,L'後'),90,91);f.context->unknown_scope=true;
        const auto value=f.capture(true);
        check(value.before.size()==40&&value.after.size()==40,"unknown context exceeded bounds");
        f.context->doc->reject_read=true;
        const auto rejected=f.capture(true);
        check(!rejected.available&&rejected.before.empty()&&rejected.after.empty(),"opt-in bypassed text validation");
    }
}
struct Session {
    ComPtr<test_host::Manager> host;
    ComPtr<test_host::Context> context;
    ComPtr<ITfTextInputProcessorEx> service;
    ComPtr<ITfKeyEventSink> keys;
    Session() {
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        context->doc->text = L"前後"; context->doc->caret = 1;
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "create service"); service.As(&keys);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, 0)), "activate"); wait_for_dictionary(nskkzime::windows::runtime_dictionary_ready);
    }
    ~Session() { service->Deactivate(); context->pump(); }
    void key(WPARAM code, bool shift = false) {
        BYTE state[256]{}; if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80; SetKeyboardState(state);
        const auto parameter = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
        BOOL eaten = FALSE; keys->OnTestKeyDown(context.Get(), code, parameter, &eaten); check(eaten, "key preflight");
        keys->OnKeyDown(context.Get(), code, parameter, &eaten); check(eaten, "key dispatch"); keys->OnKeyUp(context.Get(), code, parameter, &eaten);
    }
    void convert() { key('K', true); key('A'); key('N'); key('J'); key('I'); key(VK_SPACE); }
};
bool status_contains(const wchar_t* text) { return context_preview_status().find(text) != std::wstring::npos; }
void integration_tests() {
    check(!context_preview_enabled(), "preview enabled by default");
    {
        Session s; s.convert(); check(status_contains(L"オフ"), "preview collected without opt-in");
        enable_context_preview(true); s.context->notify();
        check(status_contains(L"前: 「前」") && status_contains(L"後: 「後」"), "adapter did not exclude composition");
        s.context->host_replace(0, 1, L"別"); check(status_contains(L"前: 「別」"), "external surrounding edit left stale preview");
        s.context->input_scope = IS_PRIVATE; s.context->notify(); check(status_contains(L"保護"), "protected field retained preview");
        s.context->input_scope = IS_TEXT; s.context->unknown_scope = true; s.context->notify(); check(status_contains(L"不明"), "unknown field retained preview");
        s.context->unknown_scope = false; s.context->notify(); check(status_contains(L"前: 「別」"), "preview did not recover");
        s.key(VK_ESCAPE); check(status_contains(L"Space変換"), "cancel retained preview");
        s.key(VK_SPACE); check(status_contains(L"後: 「後」"), "reconversion missing preview");
        s.key(VK_RETURN); check(status_contains(L"Space変換"), "commit retained preview");
    }
    {
        Session s; s.context->scope_query = [&](ITfRange*, InputScope&) { s.service->Deactivate(); return S_OK; };
        s.convert(); check(status_contains(L"Space変換"), "reentrant deactivation published stale context");
    }
    {
        Session s; s.convert(); enable_context_preview(false);
        check(status_contains(L"オフ") && !status_contains(L"前: "), "disable retained preview");
    }
    check(module_objects == 0, "preview leaked service");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", argv[1]); SetEnvironmentVariableW(L"NSKKZIME_LEARNING", L"0");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); BYTE previous[256]{}; GetKeyboardState(previous);
    int result = 1;
    try { capture_tests(); integration_tests(); std::cout << "PASS bounded context, scopes, UTF-16, failures, TSF preview and lifetime\n"; result = 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    enable_context_preview(false); SetKeyboardState(previous); CoUninitialize(); return result;
}
