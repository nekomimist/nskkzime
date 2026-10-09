#pragma once
#include "learning_ipc.h"
#include <filesystem>

// Ends the dictionary helper serving this user dictionary, so a test can delete
// its folder without waiting for the helper's 10-second idle shutdown.
inline void stop_dictionary_helper(const std::filesystem::path& file) noexcept {
    using namespace nskkzime::windows;
    try {
        NativeHandle pipe(CreateFileW(learning_pipe_name(file).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
        ULONG pid = 0;
        if (!pipe.valid() || !GetNamedPipeServerProcessId(pipe.get(), &pid)) return;
        NativeHandle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
        if (process.valid() && TerminateProcess(process.get(), 0)) WaitForSingleObject(process.get(), 5000);
    } catch (...) {}
}
