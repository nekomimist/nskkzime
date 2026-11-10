#include "dictionary_loader.h"
#include "windows_text.h"
#include "settings_file.h"
#include "romaji_settings.h"
#include "cpu_requirements.h"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>

using namespace nskkzime::windows;
using nskkzime::InputMethod;
using nskkzime::AzikKeyboard;
namespace {
void show(std::wstring_view value) {
    // stdout is in Windows text mode; do not translate CRLF into CRCRLF.
    for (const auto ch : utf8_message(value)) if (ch != '\r') std::cout.put(ch);
    std::cout.put('\n');
}
void help() {
    std::cout << "Usage: nskkzime_config [--config ABSOLUTE_PATH] ACTION\n"
        "  --show                    Show stored paths and environment overrides\n"
        "  --check                   Load effective dictionaries and report diagnostics\n"
        "  --check-cpu               Check bundled zenz CPU/OS requirements without loading a model\n"
        "  --set-dictionaries PATH... Validate and save absolute paths, highest priority first\n"
        "  --disable-dictionaries    Save an empty list (kana input only)\n"
        "  --set-learning on|off     Enable or disable persistent candidate learning\n"
        "  --set-startup-ime-off on|off  Start applications with the IME off\n"
        "  --set-input-method skk|azik  Select built-in SKK or DDSKK-style AZIK\n"
        "  --set-azik-keyboard jp106|us101  Select AZIK punctuation keys\n"
        "  --set-romaji PATH         Validate and save a custom UTF-8 romaji rules file\n"
        "  --reset-romaji            Use built-in romaji rules\n"
        "  --set-candidate-show-after N  Show the list on conversion step N (1..99)\n"
        "  --set-candidate-keys numbers|asdfjkl  Choose visible candidate selection keys\n"
        "  --set-off-key-exclusions [EXE...]  Pass OFF-mode Ctrl+J/Q through in these apps; omit names to clear\n"
        "  --set-zenz MODEL [SERVER]  Enable zenz with an absolute model path; the bundled server is used\n"
        "                            unless SERVER (a development override) is given\n"
        "  --disable-inference       Disable zenz and clear its saved paths\n"
        "  --set-zenz-min-length N   Prefer generation for N or more kana (0 disables)\n"
        "  --set-zenz-rerank on|off  Promote the best dictionary candidate when confident\n"
        "  --set-zenz-rerank-learn on|off Learn raw-score gaps from choices against reranking\n"
        "  --set-zenz-learning on|off Learn short fallback generations after successful commit\n"
        "  --set-zenz-unknown-context on|off  Allow local context use when input scope is unknown\n"
        "  --enable-input-log DIR    Save readings, surrounding text and candidates for diagnosis\n"
        "  --disable-input-log       Stop logging after restarting the target applications\n"
        "  --enable-tsf-log DIR      Save TSF lifecycle/API diagnostics without text or keys\n"
        "  --disable-tsf-log         Stop TSF logging after restarting the target applications\n"
        "  --help                    Show this help\n"
        "Default: %LOCALAPPDATA%\\nskkzime\\settings.ini (UTF-8)\n"
        "NSKKZIME_CONFIG selects another settings file (its folder also holds the user dictionary and cache).\n"
        "Restart the target applications after saving. Output is UTF-8.\n";
}

using Arguments = std::span<wchar_t* const>;
enum class Action {
    Show, Check, SetDictionaries, DisableDictionaries, SetLearning, SetStartupImeOff,
    SetInputMethod, SetAzikKeyboard, SetRomaji, ResetRomaji, SetCandidateShowAfter,
    SetCandidateKeys, SetOffKeyExclusions, SetZenz, DisableInference, SetZenzMinLength,
    SetZenzRerank, SetZenzRerankLearn, SetZenzLearning, SetZenzUnknownContext,
    EnableInputLog, DisableInputLog, EnableTsfLog, DisableTsfLog
};
AppSettings read_or_default(const std::filesystem::path& file) {
    return std::filesystem::exists(file) ? read_settings(file) : AppSettings{};
}
bool parse_toggle(std::wstring_view value, const char* error) {
    if (value != L"on" && value != L"off") throw std::invalid_argument(error);
    return value == L"on";
}
int set_off_key_exclusions(const std::filesystem::path& file, Action, Arguments args) {
    auto settings = read_or_default(file);
    settings.off_key_exclusions.assign(args.begin(), args.end());
    save_settings(file, settings);
    show(L"OFFキーの除外設定を保存しました。利用するアプリを起動し直すと反映されます。");
    return 0;
}
int set_tsf_log(const std::filesystem::path& file, Action action, Arguments args) {
    auto settings = read_or_default(file);
    const bool enable = action == Action::EnableTsfLog;
    settings.tsf_log.enabled = enable;
    if (enable) settings.tsf_log.directory = args[0];
    validate_tsf_log_settings(settings.tsf_log, enable);
    save_settings(file, settings);
    show(tsf_log_settings_status(settings.tsf_log));
    show(L"保存しました。利用するアプリを起動し直すと反映されます。");
    return 0;
}
int set_candidates(const std::filesystem::path& file, Action action, Arguments args) {
    auto settings = read_or_default(file);
    const auto value = utf8_message(args[0]);
    if (action == Action::SetCandidateKeys) {
        if (value != "numbers" && value != "asdfjkl") throw std::invalid_argument("candidate keys must equal numbers or asdfjkl");
        settings.candidates.keys = value == "asdfjkl" ? CandidateKeys::Asdfjkl : CandidateKeys::Numbers;
    } else {
        unsigned int number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) throw std::invalid_argument("candidate-show-after must be 1..99");
        settings.candidates.show_after = number;
    }
    save_settings(file, settings);
    show(candidate_settings_status(settings.candidates));
    show(L"保存しました。利用するアプリを起動し直すと反映されます。");
    return 0;
}
unsigned long parse_min_length(std::wstring_view value) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](auto ch) { return ch >= L'0' && ch <= L'9'; }))
        throw std::invalid_argument("minimum length must be 0..1024");
    const auto parsed = std::stoul(std::wstring(value));
    if (parsed > 1024) throw std::invalid_argument("minimum length must be 0..1024");
    return parsed;
}
void update_zenz(ZenzSettings& settings, Action action, Arguments args) {
    constexpr auto toggle_error = "zenz toggle must equal on or off";
    switch (action) {
    case Action::SetZenz:
        settings.enabled = true;
        settings.model = args[0];
        settings.server = args.size() == 2 ? std::filesystem::path(args[1]) : std::filesystem::path{};
        break;
    case Action::DisableInference: settings = {}; break;
    case Action::EnableInputLog:
        settings.log_input = true;
        settings.log_directory = args[0];
        break;
    case Action::DisableInputLog: settings.log_input = false; break;
    case Action::SetZenzMinLength: settings.policy.long_min_length = parse_min_length(args[0]); break;
    case Action::SetZenzUnknownContext: settings.allow_unknown_context = parse_toggle(args[0], toggle_error); break;
    case Action::SetZenzRerankLearn: settings.policy.rerank_learn = parse_toggle(args[0], toggle_error); break;
    case Action::SetZenzRerank: settings.policy.rerank = parse_toggle(args[0], toggle_error); break;
    case Action::SetZenzLearning: settings.policy.learn_fallback = parse_toggle(args[0], toggle_error); break;
    default: throw std::logic_error("unexpected inference command");
    }
}
int set_zenz(const std::filesystem::path& file, Action action, Arguments args) {
    auto settings = read_or_default(file);
    update_zenz(settings.zenz, action, args);
    validate_zenz_settings(settings.zenz, action == Action::SetZenz || action == Action::EnableInputLog);
    save_settings(file, settings);
    show(zenz_settings_status(settings.zenz));
    show(L"保存しました。利用するアプリを起動し直すと反映されます。モデルの実行確認にはサーバーの --convert を使ってください。");
    return 0;
}
void update_input(AppSettings& settings, Action action, Arguments args) {
    switch (action) {
    case Action::SetInputMethod: {
        const std::wstring_view value(args[0]);
        if (value != L"skk" && value != L"azik") throw std::invalid_argument("input method must equal skk or azik");
        settings.input_method = value == L"azik" ? InputMethod::Azik : InputMethod::Skk;
        break;
    }
    case Action::SetAzikKeyboard: {
        const std::wstring_view value(args[0]);
        if (value != L"jp106" && value != L"us101") throw std::invalid_argument("AZIK keyboard must equal jp106 or us101");
        settings.azik_keyboard = value == L"us101" ? AzikKeyboard::Us101 : AzikKeyboard::Jp106;
        break;
    }
    case Action::SetRomaji:
        settings.romaji_rules = args[0];
        if (settings.romaji_rules.empty()) throw std::invalid_argument("--set-romaji requires an absolute path");
        break;
    case Action::ResetRomaji: settings.romaji_rules.clear(); break;
    default: throw std::logic_error("unexpected input command");
    }
}
int set_input(const std::filesystem::path& file, Action action, Arguments args) {
    SettingsConfiguration proposed;
    proposed.file = file;
    proposed.settings = read_or_default(file);
    update_input(proposed.settings, action, args);
    const auto loaded = load_romaji_rules(proposed);
    show(loaded.status);
    if (loaded.failed) { std::cerr << "Not saved: invalid romaji rules.\n"; return 2; }
    save_settings(file, proposed.settings);
    show(L"ローマ字設定を保存しました。利用するアプリを起動し直すと反映されます。");
    return 0;
}
int set_toggle(const std::filesystem::path& file, Action action, Arguments args) {
    const auto enabled = parse_toggle(args[0], "toggle must equal on or off");
    auto settings = read_or_default(file);
    (action == Action::SetStartupImeOff ? settings.startup_ime_off : settings.learning) = enabled;
    save_settings(file, settings);
    show(L"設定を保存しました。利用するアプリを起動し直すと反映されます。");
    return 0;
}
int set_dictionaries(const std::filesystem::path& file, Action, Arguments args) {
    SettingsConfiguration proposed;
    proposed.file = file;
    proposed.settings = read_or_default(file);
    proposed.settings.dictionaries.clear();
    for (const auto arg : args) {
        std::filesystem::path path(arg);
        if (!path.is_absolute()) throw std::invalid_argument("dictionary paths must be absolute");
        proposed.settings.dictionaries.push_back(std::move(path));
    }
    const auto loaded = load_dictionaries(proposed);
    show(loaded.status);
    if (loaded.failed) { std::cerr << "Not saved: one or more dictionaries could not be loaded.\n"; return 2; }
    save_settings(file, proposed.settings);
    show(L"保存しました: " + file.native());
    show(L"利用するアプリを終了して起動し直すと反映されます。");
    return 0;
}
void show_settings(const std::filesystem::path& file, bool missing_is_demo) {
    show(L"設定ファイル: " + file.native());
    const auto configuration = resolve_settings_configuration(file, missing_is_demo);
    if (configuration.demo) {
        show(L"保存された設定はありません。内蔵テスト用辞書を使います。");
        return;
    }
    const auto& settings = configuration.settings;
    show(settings.learning ? L"学習: on" : L"学習: off");
    show(settings.startup_ime_off ? L"アプリ起動時にIMEをオフ: on" : L"アプリ起動時にIMEをオフ: off");
    show(input_profile_status(settings));
    show(candidate_settings_status(settings.candidates));
    show(ui_settings_status(settings.ui));
    show(settings.romaji_rules.empty() ? L"ローマ字: 内蔵ルール" : L"ローマ字: " + settings.romaji_rules.native());
    show(zenz_settings_status(settings.zenz));
    show(tsf_log_settings_status(settings.tsf_log));
    for (const auto& executable : settings.off_key_exclusions) show(L"OFFキーの除外: " + executable);
    std::size_t number = 0;
    for (const auto& path : settings.dictionaries) show(std::to_wstring(++number) + L". " + path.native());
    if (!number) show(settings.zenz.enabled ? L"辞書は未設定です。zenzによる変換を利用します。" : L"辞書は無効です（かな入力のみ）。");
}
void check_inference(const AppSettings& settings) {
    show(zenz_settings_status(settings.zenz));
    validate_zenz_settings(settings.zenz, true);
    show(tsf_log_settings_status(settings.tsf_log));
    validate_tsf_log_settings(settings.tsf_log, true);
}
int check_settings(const SettingsConfiguration& configuration, bool inference_first) {
    const auto loaded = load_dictionaries(configuration);
    const auto rules = load_romaji_rules(configuration);
    // Keep the existing diagnostic order for implicit and explicit configuration.
    if (inference_first) check_inference(configuration.settings);
    show(loaded.status);
    show(rules.status);
    if (!inference_first) check_inference(configuration.settings);
    return loaded.failed || rules.failed ? 2 : 0;
}
struct CommandSpec {
    std::wstring_view name;
    Action action;
    std::size_t min_args, max_args;
    int (*execute)(const std::filesystem::path&, Action, Arguments);
};
constexpr auto unlimited_args = std::numeric_limits<std::size_t>::max();
const CommandSpec commands[] = {
    {L"--show", Action::Show, 0, 0, nullptr},
    {L"--check", Action::Check, 0, 0, nullptr},
    {L"--set-dictionaries", Action::SetDictionaries, 1, 32, set_dictionaries},
    {L"--disable-dictionaries", Action::DisableDictionaries, 0, 0, set_dictionaries},
    {L"--set-learning", Action::SetLearning, 1, 1, set_toggle},
    {L"--set-startup-ime-off", Action::SetStartupImeOff, 1, 1, set_toggle},
    {L"--set-input-method", Action::SetInputMethod, 1, 1, set_input},
    {L"--set-azik-keyboard", Action::SetAzikKeyboard, 1, 1, set_input},
    {L"--set-romaji", Action::SetRomaji, 1, 1, set_input},
    {L"--reset-romaji", Action::ResetRomaji, 0, 0, set_input},
    {L"--set-candidate-show-after", Action::SetCandidateShowAfter, 1, 1, set_candidates},
    {L"--set-candidate-keys", Action::SetCandidateKeys, 1, 1, set_candidates},
    {L"--set-off-key-exclusions", Action::SetOffKeyExclusions, 0, unlimited_args, set_off_key_exclusions},
    {L"--set-zenz", Action::SetZenz, 1, 2, set_zenz},
    {L"--disable-inference", Action::DisableInference, 0, 0, set_zenz},
    {L"--set-zenz-min-length", Action::SetZenzMinLength, 1, 1, set_zenz},
    {L"--set-zenz-rerank", Action::SetZenzRerank, 1, 1, set_zenz},
    {L"--set-zenz-rerank-learn", Action::SetZenzRerankLearn, 1, 1, set_zenz},
    {L"--set-zenz-learning", Action::SetZenzLearning, 1, 1, set_zenz},
    {L"--set-zenz-unknown-context", Action::SetZenzUnknownContext, 1, 1, set_zenz},
    {L"--enable-input-log", Action::EnableInputLog, 1, 1, set_zenz},
    {L"--disable-input-log", Action::DisableInputLog, 0, 0, set_zenz},
    {L"--enable-tsf-log", Action::EnableTsfLog, 1, 1, set_tsf_log},
    {L"--disable-tsf-log", Action::DisableTsfLog, 0, 0, set_tsf_log}
};
struct CommandRequest {
    const CommandSpec* command;
    Arguments args;
    std::optional<std::filesystem::path> explicit_file;
};
std::optional<CommandRequest> parse_command(int argc, wchar_t** argv) {
    int index = 1;
    std::optional<std::filesystem::path> file;
    if (std::wstring_view(argv[index]) == L"--config") {
        if (argc < 4) return {};
        file = argv[++index];
        ++index;
        if (!file->is_absolute()) throw std::invalid_argument("--config requires an absolute path");
    }
    const std::wstring_view name(argv[index++]);
    const auto command = std::find_if(std::begin(commands), std::end(commands),
        [name](const auto& entry) { return entry.name == name; });
    if (command == std::end(commands)) return {};
    const Arguments args(argv + index, static_cast<std::size_t>(argc - index));
    if (command->action == Action::SetDictionaries && args.size() > command->max_args)
        throw std::invalid_argument("at most 32 dictionaries are supported");
    if (args.size() < command->min_args || args.size() > command->max_args) return {};
    return CommandRequest{command, args, std::move(file)};
}
int execute_command(const CommandRequest& request) {
    const auto& command = *request.command;
    if (command.action == Action::Check && !request.explicit_file)
        return check_settings(current_settings_configuration(), true);
    const auto file = request.explicit_file ? *request.explicit_file : settings_file_path();
    if (command.action == Action::Show) {
        show_settings(file, !request.explicit_file && !environment_value(L"NSKKZIME_CONFIG"));
        return 0;
    }
    if (command.action == Action::Check)
        return check_settings(resolve_settings_configuration(file, false), false);
    return command.execute(file, command.action, request.args);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help")) { help(); return 0; }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--check-cpu") {
            const auto features = current_cpu_features();
            std::cout << zenz_cpu_report(features) << '\n';
            return zenz_cpu_supported(features) ? 0 : 3;
        }
        const auto request = parse_command(argc, argv);
        if (!request) { help(); return 2; }
        return execute_command(*request);
    } catch (const std::exception& error) {
        std::cerr << "nskkzime_config: " << error.what() << '\n'; return 2;
    }
}
