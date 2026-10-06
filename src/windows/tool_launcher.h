#pragma once
#include <windows.h>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>

namespace nskkzime::windows {
enum class ImeTool { Settings, Dictionary };
struct ToolCommand {
    std::filesystem::path executable, file;
    ImeTool tool = ImeTool::Settings;
};
// Capture paths at service activation, without opening either data file.
std::array<ToolCommand, 2> current_tool_commands() noexcept;
std::wstring tool_command_line(const ToolCommand&);
DWORD launch_tool_process(const ToolCommand&) noexcept;

class ToolLauncher {
public:
    using Runner = std::function<DWORD(const ToolCommand&)>;
    using Reporter = std::function<void(ImeTool, DWORD)>;
    explicit ToolLauncher(std::array<ToolCommand, 2>, Runner = launch_tool_process, Reporter = {});
    ToolLauncher(const ToolLauncher&) = delete;
    ToolLauncher& operator=(const ToolLauncher&) = delete;
    ~ToolLauncher();
    bool available(ImeTool) const noexcept;
    HRESULT open(ImeTool) const noexcept; // Queue only; never wait for process startup in TSF.
    void close() noexcept; // No join. A queued callback holds its own module reference.
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace nskkzime::windows
