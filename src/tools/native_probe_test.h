#pragma once

// Drive the probe's real HWND text stores through its TSF keystroke manager.
// RichEdit uses direct TSF delivery; EDIT uses balanced native key batches
// only while the probe owns foreground and control focus.
inline void native_probe_test(Probe& probe, HWND window, const std::filesystem::path& output) {
    std::ofstream log(output);
    if (!log) throw std::runtime_error("cannot write native probe result");
    log << std::unitbuf;
    const char* stage = "foreground activation";
    const auto started = GetTickCount64();
    const auto deadline = started + 300000;
    BYTE saved[256]{}; GetKeyboardState(saved);
    struct Restore { BYTE* state; ~Restore() { SetKeyboardState(state); } } restore{saved};
    ComPtr<ITfMessagePump> pump; probe.manager.As(&pump);
    const auto drain = [&] {
        MSG message{}; BOOL available = FALSE;
        while (true) {
            if (!IsWindow(window)) throw std::runtime_error("probe window closed");
            if (!pump || FAILED(pump->PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE, &available)))
                throw std::runtime_error("TSF message pump failed");
            if (!available) break;
            if (message.message == WM_QUIT) throw std::runtime_error("probe window closed");
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    };
    const auto wait = [&](auto predicate) {
        const auto limit = (std::min)(deadline, GetTickCount64() + 5000);
        while (!predicate()) {
            if (GetTickCount64() >= limit) throw std::runtime_error(std::string(stage) + " timed out");
            drain(); Sleep(5);
        }
    };
    const auto focused = [&](HWND edit) {
        return GetForegroundWindow() == window && GetActiveWindow() == window && GetFocus() == edit;
    };
    const auto focus = [&](HWND edit) {
        stage = "foreground activation";
        SetWindowTextW(window, L"nskkzime 入力検証 — 自動試験を始めるにはここをクリック");
        while (true) {
            drain();
            if (GetTickCount64() >= deadline) throw std::runtime_error("click the probe to start native input test");
            // Change control focus only after this window owns foreground.
            if (GetForegroundWindow() == window) {
                SetActiveWindow(window); SetFocus(edit); drain();
                if (focused(edit)) break;
            }
            Sleep(20);
        }
        SetWindowTextW(window, L"nskkzime 入力検証 — 自動試験中");
    };
    const auto context_ready = [&] {
        ComPtr<ITfDocumentMgr> document; ComPtr<ITfContext> context;
        return focused(probe.rich) && probe.manager->GetFocus(&document) == S_OK && document &&
            document->GetTop(&context) == S_OK && context;
    };
    const auto text = [](HWND edit) {
        wchar_t value[1024]{}; GetWindowTextW(edit, value, ARRAYSIZE(value)); return std::wstring(value);
    };
    try {
        SetForegroundWindow(window);
        focus(probe.rich);
        SendMessageW(window, WM_COMMAND, activate_id, 0);
        if (!probe.switched) throw std::runtime_error("registered TSF profile activation failed");
        log << "activation_hr=" << std::hex << static_cast<unsigned long>(probe.activation_result) << std::dec << '\n';
        focus(probe.rich);
        stage = "RichEdit TSF context";
        wait([&] {
            TF_INPUTPROCESSORPROFILE active{};
            return probe.profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active) == S_OK &&
                active.clsid == nskkzime::windows::service_clsid && active.guidProfile == nskkzime::windows::profile_guid && context_ready();
        });
        ComPtr<ITfKeystrokeMgr> keys; probe.manager.As(&keys);
        if (!keys) throw std::runtime_error("no TSF keystroke manager");
        using Ready = HRESULT (WINAPI*)(BOOL*);
        stage = "dictionary readiness";
        log << "stage=" << stage << '\n';
        // Activation can load the registered DLL while the message pump runs.
        // Resolve again until it exists; a null export is not a lasting state.
        wait([&] {
#pragma warning(push)
#pragma warning(disable: 4191)
            const auto module = GetModuleHandleW(L"nskkzime_tsf.dll");
            const auto ready = module ? reinterpret_cast<Ready>(GetProcAddress(module, "NskkzimeIsDictionaryReady")) : nullptr;
#pragma warning(pop)
            BOOL value = FALSE; return ready && SUCCEEDED(ready(&value)) && value;
        });
        HWND native_edit = nullptr;
        const auto key = [&](WPARAM code, bool shift = false, bool control = false) {
            if (!focused(native_edit)) throw std::runtime_error("native input focus changed");
            if (GetTickCount64() >= deadline) throw std::runtime_error("native input deadline exceeded");
            if (native_edit == probe.plain) {
                for (const auto modifier : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
                    if (GetAsyncKeyState(modifier) & 0x8000) throw std::runtime_error("release physical modifiers before native input test");
                INPUT input[6]{}; UINT count = 0;
                const auto add = [&](WORD vk, DWORD flags) {
                    input[count].type = INPUT_KEYBOARD; input[count].ki.wVk = vk; input[count++].ki.dwFlags = flags;
                };
                if (shift) add(VK_SHIFT, 0);
                if (control) add(VK_CONTROL, 0);
                add(static_cast<WORD>(code), 0); add(static_cast<WORD>(code), KEYEVENTF_KEYUP);
                if (control) add(VK_CONTROL, KEYEVENTF_KEYUP);
                if (shift) add(VK_SHIFT, KEYEVENTF_KEYUP);
                if (SendInput(count, input, sizeof(INPUT)) != count) throw std::runtime_error("native input injection failed");
                const auto delivery_limit = GetTickCount64() + 80;
                do { drain(); Sleep(5); } while (GetTickCount64() < delivery_limit);
                return;
            }
            BYTE state[256]{};
            if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
            if (control) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
            SetKeyboardState(state);
            const auto parameter = (static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_VSC)) << 16) | 1;
            BOOL eaten = FALSE;
            const auto test_hr = keys->TestKeyDown(code, parameter, &eaten);
            if (test_hr != S_OK || !eaten) throw std::runtime_error("native key preflight declined");
            const auto key_hr = keys->KeyDown(code, parameter, &eaten);
            if (key_hr != S_OK || !eaten) throw std::runtime_error("native key declined");
            const auto up = parameter | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
            keys->TestKeyUp(code, up, &eaten); keys->KeyUp(code, up, &eaten);
            drain();
        };
        const auto type = [&](std::string_view value) {
            for (const auto ch : value) {
                const auto vk = VkKeyScanExW(ch, GetKeyboardLayout(0));
                if (vk == -1 || (HIBYTE(vk) & 6)) throw std::runtime_error("test keyboard mapping unavailable");
                key(LOBYTE(vk), (HIBYTE(vk) & 1) != 0);
            }
        };
        for (const auto edit : {probe.rich, probe.plain}) {
            focus(edit);
            if (edit == probe.rich) { stage = "RichEdit TSF context"; wait(context_ready); }
            stage = edit == probe.rich ? "RichEdit input" : "EDIT input";
            native_edit = edit;
            log << "control=" << (edit == probe.plain ? "EDIT" : "RichEdit") << " active=" << (GetActiveWindow() == window) << " foreground=" << (GetForegroundWindow() == window) << " focus=" << (GetFocus() == edit) << '\n';
            ComPtr<ITfDocumentMgr> document;
            const auto focus_hr = probe.manager->GetFocus(&document);
            log << "document_hr=" << std::hex << static_cast<unsigned long>(focus_hr) << " document=" << bool(document) << std::dec << '\n';
            key('J', false, true);
            type("Kanji"); if (edit == probe.rich) wait([&] { return text(edit) == L"かんじ"; });
            key(VK_SPACE); if (edit == probe.rich) wait([&] { return text(edit) == L"漢字"; });
            key(VK_RETURN); type("desu"); wait([&] { return text(edit) == L"漢字です"; });
            type("Kanji"); key(VK_SPACE); key(VK_ESCAPE); key(VK_ESCAPE);
            wait([&] { return text(edit) == L"漢字です"; });
            type("KaKu"); key(VK_SPACE); key(VK_RETURN);
            wait([&] { return text(edit) == L"漢字です描く"; });
            log << "PASS " << (edit == probe.plain ? "EDIT native conversion/commit/cancel/okuri" : "RichEdit composition/conversion/commit/cancel/okuri") << '\n';
        }
        wchar_t library[32768]{};
        GetModuleFileNameW(GetModuleHandleW(L"nskkzime_tsf.dll"), library, ARRAYSIZE(library));
#if defined(_M_ARM64EC)
        constexpr auto host = "arm64ec";
#elif defined(_M_ARM64)
        constexpr auto host = "arm64";
#elif defined(_M_IX86)
        constexpr auto host = "x86";
#else
        constexpr auto host = "x64";
#endif
        log << "host=" << host << '\n';
#if defined(NSKKZIME_LOCAL_PROBE)
        log << "activation=local-manifest\n";
#else
        log << "activation=registered-profile\n";
#endif
        log << "process_bits=" << sizeof(void*) * 8 << "\ndll=" << std::filesystem::path(library).generic_string() << '\n';
        const auto restored = probe.restore();
        TF_INPUTPROCESSORPROFILE active{};
        const auto queried = probe.profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
        if (restored != S_OK || queried != S_OK || active.dwProfileType != probe.previous.dwProfileType ||
            active.langid != probe.previous.langid || active.clsid != probe.previous.clsid ||
            active.guidProfile != probe.previous.guidProfile || active.hkl != probe.previous.hkl)
            throw std::runtime_error("previous input profile was not restored");
        log << "PASS previous process input profile restored\n";
    } catch (const std::exception& error) {
        log << "FAIL " << error.what() << '\n';
        DWORD foreground_pid = 0;
        const auto foreground = GetForegroundWindow(); GetWindowThreadProcessId(foreground, &foreground_pid);
        log << "stage=" << stage << " elapsed_ms=" << GetTickCount64() - started
            << " foreground=" << foreground << " foreground_pid=" << foreground_pid
            << " active=" << GetActiveWindow() << " focus=" << GetFocus() << '\n';
        ComPtr<ITfDocumentMgr> document; ComPtr<ITfContext> context;
        const auto document_hr = probe.manager->GetFocus(&document);
        const auto context_hr = document ? document->GetTop(&context) : E_POINTER;
        log << "document_hr=" << std::hex << static_cast<unsigned long>(document_hr)
            << " context_hr=" << static_cast<unsigned long>(context_hr) << std::dec
            << " document=" << bool(document) << " context=" << bool(context) << '\n';
        const auto module = GetModuleHandleW(L"nskkzime_tsf.dll");
        wchar_t path[32768]{};
        if (module) GetModuleFileNameW(module, path, ARRAYSIZE(path));
        log << "module_loaded=" << bool(module) << " dll=" << std::filesystem::path(path).generic_string() << '\n';
        TF_INPUTPROCESSORPROFILE active{};
        const auto hr = probe.profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
        log << "active_profile_hr=" << std::hex << static_cast<unsigned long>(hr) << std::dec
            << " ours=" << (hr == S_OK && active.clsid == nskkzime::windows::service_clsid && active.guidProfile == nskkzime::windows::profile_guid)
            << " language=" << active.langid << '\n';
        if (module) {
            using Ready = HRESULT (WINAPI*)(BOOL*);
            using Status = HRESULT (WINAPI*)(BSTR*);
#pragma warning(push)
#pragma warning(disable: 4191)
            const auto ready = reinterpret_cast<Ready>(GetProcAddress(module, "NskkzimeIsDictionaryReady"));
            const auto status = reinterpret_cast<Status>(GetProcAddress(module, "NskkzimeGetDictionaryStatus"));
#pragma warning(pop)
            BOOL ready_value = FALSE; const auto ready_hr = ready ? ready(&ready_value) : E_NOINTERFACE;
            log << "ready_export=" << bool(ready) << " ready_hr=" << std::hex << static_cast<unsigned long>(ready_hr)
                << std::dec << " ready=" << ready_value << '\n';
            BSTR value = nullptr;
            if (status && SUCCEEDED(status(&value)) && value) {
                const auto length = static_cast<int>(SysStringLen(value));
                std::string utf8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr)), '\0');
                if (!utf8.empty()) WideCharToMultiByte(CP_UTF8, 0, value, length, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
                log << "dictionary_status=" << utf8 << '\n';
            }
            SysFreeString(value);
        }
        log << "failure_restore_hr=" << std::hex << static_cast<unsigned long>(probe.restore()) << std::dec << '\n';
        throw;
    }
}
