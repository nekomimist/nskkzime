#include "dictionary_settings.h"
#include "romaji_settings.h"
#include "cpu_requirements.h"
#include <algorithm>
#include <iostream>

using namespace nskkzime::windows;
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
        "  --check-cpu               Check zenz CPU/OS requirements without loading a model\n"
        "  --set-dictionaries PATH... Validate and save absolute paths, highest priority first\n"
        "  --disable-dictionaries    Save an empty list (kana input only)\n"
        "  --set-learning on|off     Enable or disable persistent candidate learning\n"
        "  --set-romaji PATH         Validate and save a custom UTF-8 romaji rules file\n"
        "  --reset-romaji            Use built-in romaji rules\n"
        "  --set-zenz MODEL SERVER    Enable zenz using absolute model and server file paths\n"
        "  --disable-inference       Disable zenz and clear its saved paths\n"
        "  --set-zenz-min-length N   Prefer generation for N or more kana (0 disables)\n"
        "  --set-zenz-rerank on|off  Promote the best dictionary candidate when confident\n"
        "  --set-zenz-learning on|off Learn short fallback generations after successful commit\n"
        "  --set-zenz-unknown-context on|off  Allow local context use when input scope is unknown\n"
        "  --enable-input-log DIR    Save readings, surrounding text and candidates for diagnosis\n"
        "  --disable-input-log       Stop logging after restarting the target applications\n"
        "  --help                    Show this help\n"
        "Default: %LOCALAPPDATA%\\nskkzime\\settings.ini (UTF-8)\n"
        "NSKKZIME_CONFIG overrides that file; NSKKZIME_DICTIONARY selects isolated dictionary settings.\n"
        "NSKKZIME_ROMAJI overrides romaji rules (absolute path or builtin).\n"
        "Restart the target applications after saving. Output is UTF-8.\n";
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help")) { help(); return 0; }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--check-cpu") {
            const auto features = current_cpu_features();
            std::cout << zenz_cpu_report(features) << '\n';
            return zenz_cpu_supported(features) ? 0 : 3;
        }
        int index = 1;
        std::optional<std::filesystem::path> explicit_file;
        if (std::wstring_view(argv[index]) == L"--config") {
            if (argc < 4) { help(); return 2; }
            explicit_file = argv[++index]; ++index;
            if (!explicit_file->is_absolute()) throw std::invalid_argument("--config requires an absolute path");
        }
        const std::wstring_view action(argv[index++]);
        const bool policy = action == L"--set-zenz-min-length" || action == L"--set-zenz-rerank" || action == L"--set-zenz-learning" || action == L"--set-zenz-unknown-context";
        const bool enable_log = action == L"--enable-input-log", disable_log = action == L"--disable-input-log";
        if (!policy && !enable_log && !disable_log && action != L"--show" && action != L"--check" && action != L"--set-dictionaries" && action != L"--disable-dictionaries" && action != L"--set-learning" && action != L"--set-romaji" && action != L"--reset-romaji" && action != L"--set-zenz" && action != L"--disable-inference") {
            help(); return 2;
        }
        const bool set = action == L"--set-dictionaries", disable = action == L"--disable-dictionaries";
        const bool learning = action == L"--set-learning";
        const bool romaji = action == L"--set-romaji", reset_romaji = action == L"--reset-romaji";
        const bool zenz = action == L"--set-zenz", disable_inference = action == L"--disable-inference";
        if ((!set && !learning && !romaji && !zenz && !policy && !enable_log && index != argc) || (set && index == argc) || ((learning || romaji || policy || enable_log) && index + 1 != argc) || (zenz && index + 2 != argc)) { help(); return 2; }
        if (set && argc - index > 32) throw std::invalid_argument("at most 32 dictionaries are supported");
        if (action == L"--check" && !explicit_file) {
            const auto configuration = current_dictionary_configuration();
            const auto loaded = load_dictionaries(configuration);
            const auto rules = load_romaji_rules(configuration,environment_value(L"NSKKZIME_ROMAJI"));
            show(zenz_settings_status(configuration.settings.zenz)); validate_zenz_settings(configuration.settings.zenz,true);
            show(loaded.status); show(rules.status); return loaded.failed || rules.failed ? 2 : 0;
        }
        const auto file = explicit_file ? *explicit_file : dictionary_settings_path();
        const auto dictionary_override = environment_value(L"NSKKZIME_DICTIONARY");
        if (zenz || disable_inference || policy || enable_log || disable_log) {
            auto settings = std::filesystem::exists(file) ? read_dictionary_settings(file) : DictionarySettings{};
            if (zenz) { settings.zenz.enabled = true; settings.zenz.model = argv[index]; settings.zenz.server = argv[index+1]; }
            else if (disable_inference) settings.zenz = {};
            else if (enable_log) { settings.zenz.log_input = true; settings.zenz.log_directory = argv[index]; }
            else if (disable_log) settings.zenz.log_input = false;
            else {
                const std::wstring value(argv[index]);
                if (action == L"--set-zenz-min-length") {
                    if (value.empty() || !std::all_of(value.begin(),value.end(),[](auto ch) { return ch >= L'0' && ch <= L'9'; })) throw std::invalid_argument("minimum length must be 0..1024");
                    const auto parsed = std::stoul(value); if (parsed > 1024) throw std::invalid_argument("minimum length must be 0..1024"); settings.zenz.policy.long_min_length = parsed;
                } else {
                    if (value != L"on" && value != L"off") throw std::invalid_argument("zenz toggle must equal on or off");
                    if (action == L"--set-zenz-unknown-context") settings.zenz.allow_unknown_context = value == L"on";
                    else (action == L"--set-zenz-rerank" ? settings.zenz.policy.rerank : settings.zenz.policy.learn_fallback) = value == L"on";
                }
            }
            validate_zenz_settings(settings.zenz,zenz || enable_log); save_dictionary_settings(file,settings);
            show(zenz_settings_status(settings.zenz));
            show(L"保存しました。利用するアプリを起動し直すと反映されます。モデルの実行確認にはサーバーの --convert を使ってください。"); return 0;
        }
        if (romaji || reset_romaji) {
            DictionaryConfiguration proposed; proposed.file = file;
            if (std::filesystem::exists(file)) proposed.settings = read_dictionary_settings(file);
            proposed.settings.romaji_rules = romaji ? std::filesystem::path(argv[index]) : std::filesystem::path{};
            if (romaji && proposed.settings.romaji_rules.empty()) throw std::invalid_argument("--set-romaji requires an absolute path");
            const auto loaded = load_romaji_rules(proposed,{}); show(loaded.status);
            if (loaded.failed) { std::cerr << "Not saved: invalid romaji rules.\n"; return 2; }
            save_dictionary_settings(file,proposed.settings);
            show(L"ローマ字設定を保存しました。利用するアプリを起動し直すと反映されます。"); return 0;
        }
        if (learning) {
            const auto value = std::wstring_view(argv[index]);
            if (value != L"on" && value != L"off") throw std::invalid_argument("learning must equal on or off");
            auto settings = std::filesystem::exists(file) ? read_dictionary_settings(file) : DictionarySettings{};
            settings.learning = value == L"on"; save_dictionary_settings(file, settings);
            show(L"学習設定を保存しました。利用するアプリを起動し直すと反映されます。"); return 0;
        }
        if (set || disable) {
            DictionaryConfiguration proposed; proposed.file = file;
            if (std::filesystem::exists(file)) proposed.settings = read_dictionary_settings(file);
            proposed.settings.dictionaries.clear();
            for (; index < argc; ++index) {
                std::filesystem::path path(argv[index]);
                if (!path.is_absolute()) throw std::invalid_argument("dictionary paths must be absolute");
                proposed.settings.dictionaries.push_back(std::move(path));
            }
            const auto loaded = load_dictionaries(proposed);
            show(loaded.status);
            if (loaded.failed) { std::cerr << "Not saved: one or more dictionaries could not be loaded.\n"; return 2; }
            save_dictionary_settings(file, proposed.settings);
            show(L"保存しました: " + file.native());
            show(L"利用するアプリを終了して起動し直すと反映されます。");
            if (dictionary_override) show(L"注意: NSKKZIME_DICTIONARY が設定されているアプリでは、環境変数が優先されます。");
            return 0;
        }
        const bool missing_is_demo = !explicit_file && !environment_value(L"NSKKZIME_CONFIG");
        if (action == L"--show") {
            show(L"設定ファイル: " + file.native());
            if (dictionary_override) show(L"優先する環境変数 NSKKZIME_DICTIONARY: " + *dictionary_override);
            if (const auto rules_override = environment_value(L"NSKKZIME_ROMAJI")) show(L"優先する環境変数 NSKKZIME_ROMAJI: " + *rules_override);
            const auto configuration = resolve_dictionary_configuration(file, {}, missing_is_demo);
            if (configuration.demo) show(L"保存された設定はありません。環境変数も未設定なら内蔵テスト用辞書を使います。");
            else {
                show(configuration.settings.learning ? L"学習: on" : L"学習: off");
                show(configuration.settings.romaji_rules.empty() ? L"ローマ字: 内蔵ルール" : L"ローマ字: " + configuration.settings.romaji_rules.native());
                show(zenz_settings_status(configuration.settings.zenz));
                std::size_t number = 0;
                for (const auto& path : configuration.settings.dictionaries) show(std::to_wstring(++number) + L". " + path.native());
                if (!number) show(configuration.settings.zenz.enabled ? L"辞書は未設定です。zenzによる変換を利用します。" : L"辞書は無効です（かな入力のみ）。");
            }
            return 0;
        }
        const auto configuration = resolve_dictionary_configuration(file,dictionary_override,missing_is_demo);
        const auto loaded = load_dictionaries(configuration);
        const auto rules = load_romaji_rules(configuration,environment_value(L"NSKKZIME_ROMAJI"));
        show(loaded.status); show(rules.status);
        show(zenz_settings_status(configuration.settings.zenz)); validate_zenz_settings(configuration.settings.zenz,true);
        return loaded.failed || rules.failed ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "nskkzime_config: " << error.what() << '\n'; return 2;
    }
}
