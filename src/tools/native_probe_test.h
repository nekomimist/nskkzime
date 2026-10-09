#pragma once

// Drive the probe's real HWND text stores through its TSF keystroke manager.
// RichEdit uses direct TSF delivery; EDIT uses balanced native key batches
// only while the probe owns foreground and control focus.
inline void native_probe_test(Probe& probe, HWND window, const std::filesystem::path& output) {
    std::ofstream log(output);
    if (!log) throw std::runtime_error("cannot write native probe result");
    BYTE saved[256]{}; GetKeyboardState(saved);
    struct Restore { BYTE* state; ~Restore() { SetKeyboardState(state); } } restore{saved};
    ComPtr<ITfMessagePump> pump; probe.manager.As(&pump);
    const auto drain = [&] {
        MSG message{}; BOOL available = FALSE;
        while (pump && SUCCEEDED(pump->PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE, &available)) && available) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    };
    const auto wait = [&](auto predicate) {
        const auto deadline = GetTickCount64() + 5000;
        while (!predicate()) {
            if (GetTickCount64() >= deadline) throw std::runtime_error("native input timed out");
            drain(); Sleep(5);
        }
    };
    const auto text = [](HWND edit) {
        wchar_t value[1024]{}; GetWindowTextW(edit, value, ARRAYSIZE(value)); return std::wstring(value);
    };
    try {
        SetWindowTextW(window, L"nskkzime 入力検証 — 自動試験を始めるにはここをクリック");
        SetForegroundWindow(window);
        const auto activation_deadline = GetTickCount64() + 300000;
        while (GetForegroundWindow() != window) {
            if (GetTickCount64() >= activation_deadline) throw std::runtime_error("click the probe to start native input test");
            drain(); Sleep(20);
        }
        SetWindowTextW(window, L"nskkzime 入力検証 — 自動試験中");
        SendMessageW(window, WM_COMMAND, activate_id, 0);
        if (!probe.switched) throw std::runtime_error("registered TSF profile activation failed");
        ComPtr<ITfKeystrokeMgr> keys; probe.manager.As(&keys);
        if (!keys) throw std::runtime_error("no TSF keystroke manager");
        using Ready = HRESULT (WINAPI*)(BOOL*);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto ready = reinterpret_cast<Ready>(GetProcAddress(GetModuleHandleW(L"nskkzime_tsf.dll"), "NskkzimeIsDictionaryReady"));
#pragma warning(pop)
        wait([&] { BOOL value = FALSE; return ready && SUCCEEDED(ready(&value)) && value; });
        HWND native_edit = nullptr;
        const auto key = [&](WPARAM code, bool shift = false, bool control = false) {
            if (native_edit == probe.plain) {
                if (GetForegroundWindow() != window || GetFocus() != native_edit) throw std::runtime_error("native input focus changed");
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
                const auto deadline = GetTickCount64() + 80;
                do { drain(); Sleep(5); } while (GetTickCount64() < deadline);
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
            native_edit = edit;
            SetForegroundWindow(window); SetActiveWindow(window); SetFocus(edit);
            const auto focus_deadline = GetTickCount64() + 500;
            do { drain(); Sleep(5); } while (GetTickCount64() < focus_deadline);
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
        log << "process_bits=" << sizeof(void*) * 8 << "\ndll=" << std::filesystem::path(library).generic_string() << '\n';
    } catch (const std::exception& error) {
        log << "FAIL " << error.what() << '\n'; throw;
    }
}
