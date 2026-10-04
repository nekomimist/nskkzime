#include "learning_ipc.h"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    try {
        if ((argc != 3 && argc != 5) || std::wstring_view(argv[1]) != L"--file" ||
            (argc == 5 && std::wstring_view(argv[3]) != L"--idle-ms")) {
            std::cerr << "Usage: nskkzime_dictionary_server --file ABSOLUTE_PATH [--idle-ms 10000]\n"; return 2;
        }
        const auto idle = argc == 5 ? std::stoul(argv[4]) : 10000;
        if (idle < 100 || idle > 60000) return 2;
        return nskkzime::windows::serve_dictionary(argv[2], idle);
    } catch (const std::exception& error) { std::cerr << "dictionary server: " << error.what() << '\n'; return 2; }
}
