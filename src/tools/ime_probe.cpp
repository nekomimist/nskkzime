#include "module.h"
#include <wrl/client.h>
#include <richedit.h>
#include <inputscope.h>
#include <ctfutb.h>
#include <ctffunc.h>
#include <filesystem>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
constexpr int activate_id = 101, restore_id = 102, refresh_id = 103, mode_id = 104, preview_id = 105, timing_id = 106;
#ifdef NSKKZIME_PROBE_ROMAJI
constexpr auto window_title = L"nskkzime 入力検証 (ローマ字ルール・学習なし)";
#elif defined(NSKKZIME_PROBE_DUMMY)
constexpr auto window_title = L"nskkzime 入力検証 (ダミーIPC・隔離データ)";
#elif defined(NSKKZIME_PROBE_DICTIONARY)
constexpr auto window_title = L"nskkzime 入力検証 (隔離データ)";
#elif defined(NSKKZIME_LOCAL_PROBE)
constexpr auto window_title = L"nskkzime 入力検証 (ビルド版)";
#else
constexpr auto window_title = L"nskkzime 入力検証";
#endif
struct Probe {
    ComPtr<ITfThreadMgr> manager;
    ComPtr<ITfInputProcessorProfileMgr> profiles;
    TF_INPUTPROCESSORPROFILE previous{};
    bool previous_valid = false;
    bool switched = false;
    HWND plain = nullptr, rich = nullptr, status = nullptr, last_edit = nullptr, mode = nullptr;
    HWND preview_check = nullptr, preview_text = nullptr;
    HWND timing_check = nullptr, timing_text = nullptr;
    std::wstring last_preview, last_timing;
    HFONT font = nullptr, status_font = nullptr;
    void enable_timing(bool enable) {
        using Enable = HRESULT (__stdcall*)(BOOL);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto function = reinterpret_cast<Enable>(GetProcAddress(GetModuleHandleW(L"nskkzime_tsf.dll"), "NskkzimeEnableInputTiming"));
#pragma warning(pop)
        if (function) function(enable);
    }
    void update_timing() {
        using GetTiming = HRESULT (__stdcall*)(BSTR*);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto function = reinterpret_cast<GetTiming>(GetProcAddress(GetModuleHandleW(L"nskkzime_tsf.dll"), "NskkzimeGetInputTiming"));
#pragma warning(pop)
        EnableWindow(timing_check, function != nullptr);
        BSTR value = nullptr;
        std::wstring text = L"遅延計測: このDLLでは利用できません。";
        if (function && SUCCEEDED(function(&value)) && value) text = value;
        SysFreeString(value);
        if (text != last_timing) { SetWindowTextW(timing_text, text.c_str()); last_timing = std::move(text); }
    }
    void enable_preview(bool enable) {
        using Enable = HRESULT (__stdcall*)(BOOL);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto function = reinterpret_cast<Enable>(GetProcAddress(GetModuleHandleW(L"nskkzime_tsf.dll"), "NskkzimeEnableContextPreview"));
#pragma warning(pop)
        if (function) function(enable);
    }
    void update_preview() {
        using GetPreview = HRESULT (__stdcall*)(BSTR*);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto function = reinterpret_cast<GetPreview>(GetProcAddress(GetModuleHandleW(L"nskkzime_tsf.dll"), "NskkzimeGetContextPreview"));
        const auto inference = reinterpret_cast<GetPreview>(GetProcAddress(GetModuleHandleW(L"nskkzime_tsf.dll"), "NskkzimeGetInferenceStatus"));
#pragma warning(pop)
        EnableWindow(preview_check, function != nullptr);
        BSTR value = nullptr;
        std::wstring text = L"文脈確認: このDLLでは利用できません。";
        if (function && SUCCEEDED(function(&value)) && value) text = value;
        SysFreeString(value);
        value = nullptr;
        if (inference && SUCCEEDED(inference(&value)) && value) text = std::wstring(value) + L"\r\n" + text;
        SysFreeString(value);
        if (text != last_preview) { SetWindowTextW(preview_text, text.c_str()); last_preview = std::move(text); }
    }
    ComPtr<ITfLangBarItemButton> mode_button() {
        ComPtr<ITfLangBarItemMgr> bar; ComPtr<ITfLangBarItem> item; ComPtr<ITfLangBarItemButton> button;
        TF_INPUTPROCESSORPROFILE active{};
        if (profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active) == S_OK && active.clsid == nskkzime::windows::service_clsid &&
            SUCCEEDED(manager.As(&bar)) && SUCCEEDED(bar->GetItem(GUID_LBI_INPUTMODE, &item)) && item) item.As(&button);
        return button;
    }
    void update_mode() {
        const auto button = mode_button(); BSTR value = nullptr;
        const bool available = button && SUCCEEDED(button->GetText(&value)) && value;
        SetWindowTextW(mode, available ? value : L"モード"); EnableWindow(mode, available); SysFreeString(value);
    }
    HRESULT set_input_scopes(InputScope scope) {
        // This host owns ordinary text fields. Publish their scope explicitly;
        // RichEdit otherwise returns E_FAIL for GUID_PROP_INPUTSCOPE on some OS versions.
        using SetScope = HRESULT (WINAPI*)(HWND, InputScope);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto set_scope = reinterpret_cast<SetScope>(GetProcAddress(GetModuleHandleW(L"msctf.dll"), "SetInputScope"));
#pragma warning(pop)
        if (!set_scope) return E_NOTIMPL;
        const auto first = set_scope(plain, scope), second = set_scope(rich, scope);
        return FAILED(first) ? first : second;
    }
    void report(const wchar_t* operation, HRESULT result) {
        wchar_t code[32]{};
        swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(result));
        TF_INPUTPROCESSORPROFILE active{};
        const auto current = profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
        const bool ours = current == S_OK && active.clsid == nskkzime::windows::service_clsid && active.guidProfile == nskkzime::windows::profile_guid;
        std::wstring text = std::wstring(operation) + L": " + code + L"\r\n現在のIME: " + (ours ? L"nskkzime" : L"別のIME／キーボード");
        if (const auto button = mode_button()) {
            BSTR label = nullptr;
            if (SUCCEEDED(button->GetTooltipString(&label)) && label) text += std::wstring(L"\r\n入力モード: ") + label;
            SysFreeString(label);
        }
        if (result == S_FALSE) text += L"\r\nプロファイルが無効です。開発用登録手順を確認してください。";
        if (FAILED(result)) text += L"\r\nDLLの登録と、日本語入力言語の選択を確認してください。";
        if (const auto module = GetModuleHandleW(L"nskkzime_tsf.dll")) {
            wchar_t path[32768]{};
            const auto size = GetModuleFileNameW(module, path, ARRAYSIZE(path));
            if (size && size < ARRAYSIZE(path)) text += std::wstring(L"\r\nDLL: ") + path;
            using GetStatus = HRESULT (__stdcall*)(BSTR*);
#pragma warning(push)
#pragma warning(disable: 4191) // The diagnostic export is resolved from this loaded DLL.
            const auto get_status = reinterpret_cast<GetStatus>(GetProcAddress(module, "NskkzimeGetDictionaryStatus"));
#pragma warning(pop)
            BSTR dictionary_status = nullptr;
            if (get_status && SUCCEEDED(get_status(&dictionary_status)) && dictionary_status)
                text += std::wstring(L"\r\n") + dictionary_status;
            else text += L"\r\n辞書診断: このDLLでは取得できません。";
            SysFreeString(dictionary_status);
        }
        SetWindowTextW(status, text.c_str());
    }
    HRESULT restore() {
        if (!switched || !previous_valid) return S_FALSE;
        const auto result = profiles->ActivateProfile(previous.dwProfileType, previous.langid, previous.clsid,
            previous.guidProfile, previous.hkl, TF_IPPMF_FORPROCESS);
        if (result == S_OK) switched = false;
        return result;
    }
};
HWND control(HWND parent, const wchar_t* kind, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, HFONT font) {
    const auto window = CreateWindowExW((style & WS_TABSTOP) && _wcsicmp(kind, L"BUTTON") ? WS_EX_CLIENTEDGE : 0,
        kind, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (window) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return window;
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        probe = static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(probe));
    }
    if (!probe) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        probe->font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        probe->status_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        control(window, L"STATIC", L"本文の診断ログ・候補の学習はIMEの保存設定に従います。", 0, 16, 12, 680, 30, 0, probe->font);
        control(window, L"BUTTON", L"nskkzimeを選ぶ", WS_TABSTOP, 16, 48, 190, 36, activate_id, probe->font);
        control(window, L"BUTTON", L"元のIMEに戻す", WS_TABSTOP, 218, 48, 180, 36, restore_id, probe->font);
        control(window, L"BUTTON", L"状態を更新", WS_TABSTOP, 410, 48, 150, 36, refresh_id, probe->font);
        probe->mode = control(window, L"BUTTON", L"モード", WS_TABSTOP, 570, 48, 106, 36, mode_id, probe->font);
        control(window, L"STATIC", L"標準 EDIT", 0, 16, 100, 300, 26, 0, probe->font);
        probe->preview_check = control(window, L"BUTTON", L"文脈確認（本文を表示）", WS_TABSTOP | BS_AUTOCHECKBOX,
            425, 100, 260, 26, preview_id, probe->font);
        probe->plain = control(window, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
            16, 132, 660, 105, 201, probe->font);
        control(window, L"STATIC", L"RichEdit (TSF)", 0, 16, 250, 600, 26, 0, probe->font);
        probe->timing_check = control(window, L"BUTTON", L"遅延計測（本文なし）", WS_TABSTOP | BS_AUTOCHECKBOX,
            425, 250, 260, 26, timing_id, probe->font);
        probe->rich = control(window, MSFTEDIT_CLASS, L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
            16, 282, 660, 105, 202, probe->font);
        probe->preview_text = control(window, L"STATIC", L"", 0, 16, 400, 660, 78, 0, probe->status_font);
        probe->timing_text = control(window, L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            16, 480, 660, 76, 204, probe->status_font);
        probe->status = control(window, L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            16, 562, 660, 150, 203, probe->status_font);
        if (!probe->plain || !probe->rich || !probe->status) return -1;
        // RichEdit defaults to IMM compatibility input. Explicitly enable its
        // native TSF text store so the two controls exercise different hosts.
        SendMessageW(probe->rich, EM_SETEDITSTYLE, SES_USECTF, SES_USECTF);
        if (FAILED(probe->set_input_scopes(IS_TEXT))) return -1;
        probe->last_edit = probe->plain;
        probe->report(L"起動", S_OK);
        probe->update_mode(); probe->update_preview(); probe->update_timing(); SetTimer(window, mode_id, 300, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wparam == mode_id) { probe->update_mode(); probe->update_preview(); probe->update_timing(); return 0; } break;
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_SETFOCUS && (LOWORD(wparam) == 201 || LOWORD(wparam) == 202)) {
            probe->last_edit = reinterpret_cast<HWND>(lparam); return 0;
        }
        if (LOWORD(wparam) == activate_id) {
            if (!probe->previous_valid) probe->previous_valid = probe->profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &probe->previous) == S_OK;
            // Never change desktop/session defaults; no ENABLEPROFILE registry flag.
            const auto result = probe->previous_valid ? probe->profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR,
                nskkzime::windows::language_id, nskkzime::windows::service_clsid, nskkzime::windows::profile_guid, nullptr, TF_IPPMF_FORPROCESS) : E_FAIL;
            if (result == S_OK) probe->switched = true;
            probe->report(L"IME切替", result); SetFocus(probe->last_edit); return 0;
        }
        if (LOWORD(wparam) == restore_id) { probe->report(L"IME復帰", probe->restore()); SetFocus(probe->last_edit); return 0; }
        if (LOWORD(wparam) == refresh_id) { probe->report(L"状態確認", S_OK); SetFocus(probe->last_edit); return 0; }
        if (LOWORD(wparam) == mode_id) {
            if (const auto button = probe->mode_button()) button->OnClick(TF_LBI_CLK_LEFT, {}, nullptr);
            probe->update_mode(); SetFocus(probe->last_edit); return 0;
        }
        if (LOWORD(wparam) == preview_id) {
            probe->enable_preview(SendMessageW(probe->preview_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            probe->update_preview(); SetFocus(probe->last_edit); return 0;
        }
        if (LOWORD(wparam) == timing_id) {
            probe->enable_timing(SendMessageW(probe->timing_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            probe->update_timing(); SetFocus(probe->last_edit); return 0;
        }
        break;
    case WM_CLOSE:
        probe->restore();
        DestroyWindow(window); return 0;
    case WM_DESTROY:
        probe->enable_timing(false);
        probe->enable_preview(false);
        probe->restore();
        probe->set_input_scopes(IS_DEFAULT); // Remove associations before the child HWNDs are destroyed.
        if (probe->font) DeleteObject(probe->font);
        if (probe->status_font) DeleteObject(probe->status_font);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
#ifdef NSKKZIME_PROBE_DICTIONARY
    // Interactive development fixture; never load or write the user's settings/store.
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length == ARRAYSIZE(executable)) return 2;
#ifdef NSKKZIME_PROBE_DUMMY
    const auto file = std::filesystem::path(executable).parent_path() / L"probe-async-user-dictionary.nskkdb";
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY",L"1");
    // Leave enough time to inspect and cancel a pending request manually.
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DELAY_MS",L"8000");
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TIMEOUT_MS",L"10000");
#else
    const auto file = std::filesystem::path(executable).parent_path() / L"probe-user-dictionary.nskkdb";
#endif
    if (!SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY", NSKKZIME_PROBE_DICTIONARY) ||
        !SetEnvironmentVariableW(L"NSKKZIME_LEARNING", L"1") || !SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY", file.c_str())) return 2;
#ifdef NSKKZIME_PROBE_ROMAJI
    if (!SetEnvironmentVariableW(L"NSKKZIME_ROMAJI",NSKKZIME_PROBE_ROMAJI) || !SetEnvironmentVariableW(L"NSKKZIME_LEARNING",L"0") ||
        !SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY",L"0")) return 2;
#else
    if (!SetEnvironmentVariableW(L"NSKKZIME_ROMAJI",L"builtin")) return 2;
#endif
#endif
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    const auto rich_module = LoadLibraryW(L"msftedit.dll");
    int result = 1;
    {
        Probe probe;
        auto hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&probe.manager));
        TfClientId client = TF_CLIENTID_NULL;
        if (SUCCEEDED(hr)) hr = probe.manager->Activate(&client);
        const bool activated = SUCCEEDED(hr);
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&probe.profiles));
        if (SUCCEEDED(hr) && rich_module) {
            WNDCLASSW klass{}; klass.lpfnWndProc = window_proc; klass.hInstance = instance;
            klass.hCursor = LoadCursorW(nullptr, IDC_IBEAM); klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            klass.lpszClassName = L"nskkzime-input-probe";
            if (RegisterClassW(&klass)) {
                const auto window = CreateWindowExW(0, klass.lpszClassName, window_title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, 720, 770, nullptr, nullptr, instance, &probe);
                if (window) {
                    ShowWindow(window, show); UpdateWindow(window); SetFocus(probe.plain);
                    ComPtr<ITfMessagePump> pump; probe.manager.As(&pump);
                    ComPtr<ITfKeystrokeMgr> keys; probe.manager.As(&keys);
                    MSG message{};
                    while (true) {
                        BOOL available = FALSE;
                        if (pump) { if (FAILED(pump->GetMessageW(&message, nullptr, 0, 0, &available))) break; }
                        else available = GetMessageW(&message, nullptr, 0, 0);
                        if (available <= 0) { result = available == 0 ? 0 : 1; break; }
                        BOOL eaten = FALSE;
                        if (keys && message.message == WM_KEYDOWN && SUCCEEDED(keys->TestKeyDown(message.wParam, message.lParam, &eaten)) && eaten &&
                            SUCCEEDED(keys->KeyDown(message.wParam, message.lParam, &eaten)) && eaten) continue;
                        if (keys && message.message == WM_KEYUP && SUCCEEDED(keys->TestKeyUp(message.wParam, message.lParam, &eaten)) && eaten &&
                            SUCCEEDED(keys->KeyUp(message.wParam, message.lParam, &eaten)) && eaten) continue;
                        TranslateMessage(&message); DispatchMessageW(&message);
                    }
                }
            }
        }
        if (activated) probe.manager->Deactivate();
    }
    if (rich_module) FreeLibrary(rich_module);
    CoUninitialize(); return result;
}
