#include "inference_ipc.h"
#include <iostream>
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--version") { std::cout << nskkzime::inference_hello() << '\n'; return 0; }
        if (argc != 8 || std::wstring_view(argv[1]) != L"--dummy" || std::wstring_view(argv[2]) != L"--pipe-token" ||
            std::wstring_view(argv[4]) != L"--delay-ms" || std::wstring_view(argv[6]) != L"--idle-ms") {
            std::cerr << "Usage: nkkz-ime-server --dummy --pipe-token TOKEN --delay-ms N --idle-ms N\n"; return 2;
        }
        auto number = [](const wchar_t* value) { std::size_t end = 0; const auto n = std::stoul(value,&end); if (end != wcslen(value) || n > 60000) throw std::invalid_argument("invalid limit"); return n; };
        const auto delay = number(argv[5]), idle = number(argv[7]); if (delay > 10000 || idle < 100) return 2;
        return nskkzime::windows::serve_dummy_inference(argv[3],delay,idle);
    } catch (...) { std::cerr << "inference server failed\n"; return 2; }
}
