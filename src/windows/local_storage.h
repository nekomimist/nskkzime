#pragma once
#include <filesystem>
#include <string_view>
namespace nskkzime::windows {
// Prepare, flush, then replace a file in the same directory. Failed replacement
// preserves the previous destination. Caller serializes concurrent writers.
void write_file_atomic(const std::filesystem::path&, std::string_view);
}
