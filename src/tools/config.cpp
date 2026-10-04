#include "dictionary_settings.h"
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
        "  --set-dictionaries PATH... Validate and save absolute paths, highest priority first\n"
        "  --disable-dictionaries    Save an empty list (kana input only)\n"
        "  --set-learning on|off     Enable or disable persistent candidate learning\n"
        "  --help                    Show this help\n"
        "Default: %LOCALAPPDATA%\\nskkzime\\settings.ini (UTF-8)\n"
        "NSKKZIME_CONFIG overrides that file; NSKKZIME_DICTIONARY overrides its dictionary list.\n"
        "Restart the target applications after saving. Output is UTF-8.\n";
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help")) { help(); return 0; }
        int index = 1;
        std::optional<std::filesystem::path> explicit_file;
        if (std::wstring_view(argv[index]) == L"--config") {
            if (argc < 4) { help(); return 2; }
            explicit_file = argv[++index]; ++index;
            if (!explicit_file->is_absolute()) throw std::invalid_argument("--config requires an absolute path");
        }
        const std::wstring_view action(argv[index++]);
        if (action != L"--show" && action != L"--check" && action != L"--set-dictionaries" && action != L"--disable-dictionaries" && action != L"--set-learning") {
            help(); return 2;
        }
        const bool set = action == L"--set-dictionaries", disable = action == L"--disable-dictionaries";
        const bool learning = action == L"--set-learning";
        if ((!set && !learning && index != argc) || (set && index == argc) || (learning && index + 1 != argc)) { help(); return 2; }
        if (set && argc - index > 32) throw std::invalid_argument("at most 32 dictionaries are supported");
        if (action == L"--check" && !explicit_file) {
            const auto loaded = load_dictionaries(current_dictionary_configuration());
            show(loaded.status); return loaded.failed ? 2 : 0;
        }
        const auto file = explicit_file ? *explicit_file : dictionary_settings_path();
        const auto dictionary_override = environment_value(L"NSKKZIME_DICTIONARY");
        if (learning) {
            const auto value = std::wstring_view(argv[index]);
            if (value != L"on" && value != L"off") throw std::invalid_argument("learning must equal on or off");
            auto settings = std::filesystem::exists(file) ? read_dictionary_settings(file) : DictionarySettings{};
            settings.learning = value == L"on"; save_dictionary_settings(file, settings);
            show(L"学習設定を保存しました。利用するアプリを起動し直すと反映されます。"); return 0;
        }
        if (set || disable) {
            DictionaryConfiguration proposed; proposed.file = file;
            if (std::filesystem::exists(file)) proposed.settings.learning = read_dictionary_settings(file).learning;
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
            const auto configuration = resolve_dictionary_configuration(file, {}, missing_is_demo);
            if (configuration.demo) show(L"保存された設定はありません。環境変数も未設定なら内蔵テスト用辞書を使います。");
            else {
                show(configuration.settings.learning ? L"学習: on" : L"学習: off");
                std::size_t number = 0;
                for (const auto& path : configuration.settings.dictionaries) show(std::to_wstring(++number) + L". " + path.native());
                if (!number) show(L"辞書は無効です（かな入力のみ）。");
            }
            return 0;
        }
        const auto loaded = load_dictionaries(resolve_dictionary_configuration(file, dictionary_override, missing_is_demo));
        show(loaded.status);
        return loaded.failed ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "nskkzime_config: " << error.what() << '\n'; return 2;
    }
}
