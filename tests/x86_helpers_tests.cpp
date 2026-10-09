#include "app_container.h"
#include "tool_launcher.h"
#include "runtime_configuration.h"
#include <iostream>
#include <stdexcept>

using namespace nskkzime::windows;
int main() {
    static_assert(sizeof(void*) == 4);
    try {
        wchar_t self[32768]{};
        if (!GetModuleFileNameW(nullptr, self, ARRAYSIZE(self))) return 1;
        const auto host = std::filesystem::path(self).parent_path().parent_path();
        const auto commands = tool_commands(host / L"fixture-settings.ini", host / L"fixture-user.nskkdb");
        if (commands[0].executable != host / L"nskkzime_settings.exe" ||
            commands[1].executable != host / L"nskkzime_dictionary_gui.exe" ||
            container_broker_path() != host / L"nskkzime_broker.exe")
            throw std::runtime_error("companion helper directory mismatch");
        for (const auto& command : commands) if (!std::filesystem::is_regular_file(command.executable))
            throw std::runtime_error("missing x64 GUI");
        std::cout << "PASS x86 GUI and broker paths resolve to shared x64 directory\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
