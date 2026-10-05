#include "candidate_ui.h"
#include "learning_ipc.h"
#include "tsf_test_host.h"
#include <iostream>
#include <stdexcept>

namespace nskkzime::windows {
HINSTANCE module_instance = nullptr;
std::atomic<long> module_objects{0};
std::atomic<long> server_locks{0};
}
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Session {
    ComPtr<ITfTextInputProcessorEx> service;
    ComPtr<ITfKeyEventSink> keys;
    ComPtr<test_host::Manager> host;
    ComPtr<test_host::Context> context;
    Session() {
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))), "create service failed");
        service.As(&keys); host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        check(SUCCEEDED(service->ActivateEx(host.Get(), 1, 0)), "activation failed");
    }
    ~Session() { service->Deactivate(); context->pump(); }
    void key(WPARAM code, bool shift = false) {
        BYTE state[256]{}; if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80; SetKeyboardState(state);
        const auto scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16;
        BOOL eaten = FALSE; keys->OnTestKeyDown(context.Get(), code, scan, &eaten);
        if (eaten) keys->OnKeyDown(context.Get(), code, scan, &eaten);
        check(eaten != FALSE, "test key was not consumed"); keys->OnKeyUp(context.Get(), code, scan, &eaten);
    }
    void type(std::string_view text) {
        for (const char ch : text) {
            const auto vk = VkKeyScanExW(static_cast<wchar_t>(ch), GetKeyboardLayout(0));
            check(vk != -1 && !(HIBYTE(vk) & 6), "test character unavailable");
            key(LOBYTE(vk), (HIBYTE(vk) & 1) != 0);
        }
    }
    ComPtr<CandidateList> snapshot() {
        check(host->ui != nullptr, "candidate snapshot missing");
        // This test links the concrete adapter, so both normal/registration UI
        // snapshots have a known type without exposing a test COM interface.
        return ComPtr<CandidateList>(static_cast<CandidateList*>(host->ui.Get()));
    }
    void convert() { type("Kanji"); key(VK_SPACE); }
};
template<class F> void until(F predicate) {
    const auto end = GetTickCount64() + 7000;
    do { try { if (predicate()) return; } catch (...) {} Sleep(25); } while (GetTickCount64() < end);
    throw std::runtime_error("click learning did not reach the helper");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::current_path() / ("tsf-click-" + learning_client_id());
    std::filesystem::create_directory(root); const auto file = root / L"user.nskkdb";
    SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", argv[1]);
    SetEnvironmentVariableW(L"NSKKZIME_LEARNING", L"1"); SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", file.c_str());
    BYTE previous[256]{}; GetKeyboardState(previous); CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        {
            Session s; s.convert(); auto list = s.snapshot();
            list->request_selection(99); check(s.context->queued.empty(), "invalid click queued an edit");
            list->request_selection(1); list->request_selection(1);
            check(s.context->queued.size() == 1 && s.context->doc->text == L"漢字", "click edited early or queued twice");
            s.key(VK_SPACE); s.context->pump();
            check(s.context->doc->text == L"感じ" && !s.context->composition->ended, "stale click committed after key input");
        }
        {
            Session s; s.convert(); s.snapshot()->request_selection(1);
            s.key(VK_ESCAPE); s.key(VK_ESCAPE); s.context->pump();
            check(s.context->doc->text.empty() && !s.host->ui, "canceled click reappeared");
        }
        {
            Session s; s.convert(); auto old = s.context; auto list = s.snapshot(); list->request_selection(1);
            s.context.Attach(new test_host::Context); s.type("KaKu"); old->pump();
            check(old->doc->text == L"漢字" && s.context->doc->text == L"書く" && !s.context->composition->ended,
                "delayed click changed another field");
            list->request_selection(0); check(old->queued.empty(), "closed snapshot queued work");
        }
        {
            Session s; s.convert(); s.snapshot()->request_selection(1);
            s.context->doc->replace(0, 2, L"外部編集"); s.context->pump();
            check(s.context->doc->text == L"外部編集" && !s.host->ui, "click overwrote external edit");
        }
        {
            Session s; s.convert(); s.snapshot()->request_selection(1); s.context->readonly = true; s.context->pump();
            check(s.context->doc->text == L"漢字" && !s.context->composition->ended, "click edited a readonly field");
        }
        {
            Session s; s.convert(); auto list = s.snapshot(); list->request_selection(1);
            s.context->layout->OnLayoutChange(s.context.Get(), TF_LC_DESTROY, nullptr); s.context->pump();
            check(s.context->doc->text == L"漢字" && !s.context->composition->ended, "destroyed layout accepted old click");
        }
        {
            Session s; s.convert(); auto list = s.snapshot(); list->request_selection(1);
            s.service->Deactivate(); s.context->pump(); list->request_selection(1);
            check(s.context->doc->text == L"漢字" && !s.host->ui && s.context->queued.empty(), "deactivated service received click");
            check(SUCCEEDED(s.service->ActivateEx(s.host.Get(), 1, 0)), "reactivation failed");
            s.convert(); list->request_selection(1); check(s.context->queued.empty(), "old endpoint affected new activation");
        }
        {
            Session s; s.convert(); auto list = s.snapshot();
            s.context->reject_session = true; list->request_selection(1); check(s.context->queued.empty(), "refused click queued");
            s.context->reject_session = false; s.context->doc->reject_write = true;
            list->request_selection(1); s.context->pump();
            check(s.context->doc->text == L"漢字" && !s.context->composition->ended, "failed click changed selection");
            s.context->doc->reject_write = false; s.context->input_scope = IS_PRIVATE;
            list->request_selection(1); s.context->pump();
            check(s.context->doc->text == L"感じ" && s.context->composition->ended && !s.host->ui, "click retry failed");
        }
        {
            Session s; s.type("/draft"); s.key(VK_SPACE); s.type("Nihon"); s.key(VK_SPACE);
            s.snapshot()->request_selection(0); s.context->pump();
            BSTR text = nullptr; s.host->ui->GetDescription(&text);
            const bool settled = text && std::wstring_view(text).find(L"日本") != std::wstring_view::npos &&
                std::wstring_view(text).find(L"▼") == std::wstring_view::npos;
            SysFreeString(text);
            check(settled && s.context->doc->text == L"draft", "inner click committed registration or leaked draft");
            s.type("a"); s.key(VK_ESCAPE); s.key(VK_ESCAPE);
        }
        Sleep(600);
        check(!std::filesystem::exists(file) || read_user_dictionary(file).revision() == 0, "failed/private/canceled click leaked learning");
        {
            Session s; s.convert(); s.snapshot()->request_selection(1); s.context->pump();
            check(s.context->doc->text == L"感じ" && !s.host->ui, "click did not confirm");
            until([&] { return read_user_dictionary(file).revision() == 1; });
            check(read_user_dictionary(file).lookup({U"かんじ"})[0].text == U"感じ", "wrong click learned");
        }
        check(module_objects == 0, "click sessions or UI retained the service");
        std::cout << "PASS deferred clicks, stale requests, retries, private scopes, registration draft, persistence and lifetime\n";
        result = 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; }
    SetKeyboardState(previous); CoUninitialize();
    Sleep(12000); // Drain the worker and let the helper release these owned test files.
    for (const auto& path : {file, std::filesystem::path(file.native() + L".lock")}) {
        std::error_code ignored; std::filesystem::remove(path, ignored);
    }
    std::error_code ignored; std::filesystem::remove(root, ignored);
    return result;
}
