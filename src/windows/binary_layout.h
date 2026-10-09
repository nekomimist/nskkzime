#pragma once
#include <filesystem>

namespace nskkzime::windows {
// The x86 companion lives in bin/x86; helpers and GUI tools belong to the
// x64 product in bin. Resolve relative to our module, never the host or PATH.
inline std::filesystem::path shared_binary_directory(const std::filesystem::path& module) {
    const auto directory = module.parent_path();
#ifdef NSKKZIME_X86_COMPANION
    return directory.parent_path();
#else
    return directory;
#endif
}
}
