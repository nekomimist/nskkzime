#pragma once
#include "installation.h"

namespace nskkzime::setup {
fs::path program_root();
fs::path executable_path();
std::wstring user_sid();
bool elevated();
void assert_native_x64();
void assert_install_context();
void protected_path(const fs::path& path);
void protected_tree(const fs::path& root);
void ensure_appcontainer_access(const fs::path& root);
void initialize_root(const fs::path& root);
void assert_root(const fs::path& root);
void assert_no_registrar();
std::wstring quote(const std::wstring& argument);
DWORD run_process(const fs::path& executable, const std::vector<std::wstring>& arguments, bool wait = true);
fs::path registrar_path(bool x86);
DWORD elevate(const std::vector<std::wstring>& arguments);
Backend windows_backend();
void append_log(const fs::path& root, const std::string& message);
std::string read_setup_log(const fs::path& root);
std::string appended_log(const std::string& before, const std::string& after);
}
