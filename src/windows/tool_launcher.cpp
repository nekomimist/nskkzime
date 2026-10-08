#include "tool_launcher.h"
#include "dictionary_settings.h"
#include "learning_ipc.h"
#include "container_runtime.h"
#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace nskkzime::windows {
namespace {
int anchor;
HMODULE pin_module() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&anchor), &module))
        throw std::runtime_error("cannot pin tool launcher");
    return module;
}
std::filesystem::path module_directory() {
    const auto module = pin_module();
    wchar_t path[32768]{};
    const auto count = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    FreeLibrary(module);
    if (!count || count == ARRAYSIZE(path)) throw std::runtime_error("cannot locate IME tools");
    return std::filesystem::path(path).parent_path();
}
std::size_t index(ImeTool tool) { return static_cast<std::size_t>(tool); }
void validate(const std::filesystem::path& path) {
    const auto& value = path.native();
    if (value.empty() || !path.is_absolute() || value.size() > 32000 || value.back() == L' ' ||
        std::any_of(value.begin(), value.end(), [](wchar_t ch) { return ch < L' ' || ch == 0x7f || ch == L'"'; }))
        throw std::invalid_argument("tool path must be absolute and contain no quotes or control characters");
}
std::wstring quote(const std::filesystem::path& path) {
    validate(path);
    auto value = path.native();
    // Windows argument decoding doubles a trailing backslash before a quote.
    const auto last = value.find_last_not_of(L'\\');
    if (last != value.npos) value.append(value.size() - last - 1, L'\\');
    return L"\"" + value + L"\"";
}
void report_failure(ImeTool tool, DWORD error) {
    const auto title = tool == ImeTool::Settings ? L"nskkzime 設定" : L"nskkzime 学習辞書";
    const auto message = L"画面を起動できませんでした（Windowsエラー " + std::to_wstring(error) +
        L"）。\nIMEと同じフォルダーに対応するGUIが配置されているか確認してください。";
    MessageBoxW(nullptr, message.c_str(), title, MB_OK | MB_ICONERROR);
}
}
std::array<ToolCommand, 2> current_tool_commands() noexcept {
    std::array<ToolCommand, 2> commands;
    commands[1].tool = ImeTool::Dictionary;
    try {
        const auto directory = module_directory();
        commands[0].executable = directory / L"nskkzime_settings.exe";
        commands[1].executable = directory / L"nskkzime_dictionary_gui.exe";
        try { commands[0].file = dictionary_settings_path(); } catch (...) {}
        try { commands[1].file = learning_file_path(); } catch (...) {}
    } catch (...) {}
    return commands;
}
std::wstring tool_command_line(const ToolCommand& command) {
    if (index(command.tool) >= 2) throw std::invalid_argument("invalid IME tool");
    auto result = quote(command.executable) + (command.tool == ImeTool::Settings ? L" --config " : L" --file ") + quote(command.file);
    if (result.size() >= 32767) throw std::length_error("tool command line too long");
    return result;
}
DWORD launch_tool_process(const ToolCommand& command) noexcept {
    try {
        auto line = tool_command_line(command);
        if (uses_data_broker()) {
            NativeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            return container_operation("GUI1", command.tool == ImeTool::Settings ? "settings" : "dictionary",
                GetTickCount64() + 5000, cancel.get()) == "OKAY" ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
        }
        STARTUPINFOW start{}; start.cb = sizeof(start); start.dwFlags = STARTF_USESHOWWINDOW; start.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION child{};
        // Explicit executable and working directory; no shell, PATH search, or
        // inherited handles from the application's document process.
        if (!CreateProcessW(command.executable.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, command.executable.parent_path().c_str(), &start, &child)) return GetLastError();
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { return ERROR_INVALID_PARAMETER; }
}
struct ToolLauncher::State {
    std::array<ToolCommand, 2> commands;
    std::array<bool, 2> enabled{};
    std::array<std::atomic<bool>, 2> busy{};
    std::atomic<bool> closed{false};
    Runner runner;
    Reporter reporter;
    struct Work { std::shared_ptr<State> state; ImeTool tool; HMODULE module; };
    static void CALLBACK callback(PTP_CALLBACK_INSTANCE instance, void* raw) {
        std::unique_ptr<Work> work(static_cast<Work*>(raw));
        FreeLibraryWhenCallbackReturns(instance, work->module);
        CallbackMayRunLong(instance);
        const auto& state = work->state;
        const auto slot = index(work->tool);
        if (!state->closed.load()) {
            DWORD error = ERROR_GEN_FAILURE;
            try { error = state->runner(state->commands[slot]); } catch (...) {}
            if (error && !state->closed.load()) try { state->reporter(work->tool, error); } catch (...) {}
        }
        state->busy[slot].store(false);
    }
};
ToolLauncher::ToolLauncher(std::array<ToolCommand, 2> commands, Runner runner, Reporter reporter) : state_(std::make_shared<State>()) {
    state_->commands = std::move(commands);
    state_->runner = std::move(runner);
    state_->reporter = reporter ? std::move(reporter) : report_failure;
    for (std::size_t i = 0; i < 2; ++i) {
        try { (void)tool_command_line(state_->commands[i]); state_->enabled[i] = index(state_->commands[i].tool) == i; } catch (...) {}
    }
}
ToolLauncher::~ToolLauncher() { close(); }
void ToolLauncher::close() noexcept { state_->closed.store(true); }
bool ToolLauncher::available(ImeTool tool) const noexcept {
    return index(tool) < 2 && !state_->closed.load() && state_->enabled[index(tool)];
}
HRESULT ToolLauncher::open(ImeTool tool) const noexcept {
    if (!available(tool)) return S_FALSE;
    const auto slot = index(tool);
    if (state_->busy[slot].exchange(true)) return S_FALSE;
    HMODULE module = nullptr;
    try {
        auto work = std::make_unique<State::Work>(); work->state = state_; work->tool = tool;
        module = work->module = pin_module();
        if (TrySubmitThreadpoolCallback(State::callback, work.get(), nullptr)) { work.release(); return S_OK; }
    } catch (...) {}
    if (module) FreeLibrary(module);
    state_->busy[slot].store(false);
    return E_FAIL;
}
} // namespace nskkzime::windows
