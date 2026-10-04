#include "nskkzime/engine.h"
#include "nskkzime/dictionary.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::string json_string(std::u32string_view text) {
    std::string result = "\"";
    for (char ch : nskkzime::utf8(text)) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += ch; }
        else if (static_cast<unsigned char>(ch) < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\u00"; result += hex[(ch >> 4) & 15]; result += hex[ch & 15];
        } else result += ch;
    }
    return result + '"';
}
nskkzime::Key token(std::string_view name) {
    using enum nskkzime::KeyCode;
    if (name == "Space") return {Space};
    if (name == "Enter") return {Enter};
    if (name == "BS") return {Backspace};
    if (name == "C-g" || name == "Esc") return {Cancel};
    if (name == "C-j") return {Hiragana};
    throw std::invalid_argument("unknown key token: " + std::string(name));
}
}
int run(const std::vector<std::string>& arguments) {
    try {
        std::vector<std::filesystem::path> paths;
        nskkzime::DictionaryEncoding encoding = nskkzime::DictionaryEncoding::Auto;
        std::string sequence;
        bool has_sequence = false;
        bool positional = false;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& argument = arguments[i];
            if (!positional && argument == "--") { positional = true; continue; }
            if (!positional && (argument == "--dict" || argument == "--encoding")) {
                if (++i == arguments.size()) throw std::invalid_argument("missing value for " + argument);
                if (argument == "--dict") {
                    paths.emplace_back(std::u8string(arguments[i].begin(), arguments[i].end()));
                }
                else if (arguments[i] == "auto") encoding = nskkzime::DictionaryEncoding::Auto;
                else if (arguments[i] == "utf-8") encoding = nskkzime::DictionaryEncoding::Utf8;
                else if (arguments[i] == "euc-jp") encoding = nskkzime::DictionaryEncoding::EucJp;
                else throw std::invalid_argument("encoding must be auto, utf-8, or euc-jp");
            } else {
                if (!positional && argument.starts_with("--")) throw std::invalid_argument("unknown option: " + argument);
                if (has_sequence) throw std::invalid_argument("expected one key sequence");
                sequence = argument; has_sequence = true;
            }
        }
        if (!has_sequence) throw std::invalid_argument("usage: skk_trace [--dict PATH]... [--encoding auto|utf-8|euc-jp] [--] \"Kanji<Space><Enter>\"");
        std::shared_ptr<const nskkzime::Dictionary> dictionary;
        if (paths.empty()) dictionary = nskkzime::demo_dictionary();
        else {
            std::vector<std::shared_ptr<const nskkzime::Dictionary>> dictionaries;
            for (const auto& path : paths) {
                const auto encoded_path = path.u8string();
                std::cerr << "Loading ";
                std::cerr.write(reinterpret_cast<const char*>(encoded_path.data()), static_cast<std::streamsize>(encoded_path.size()));
                std::cerr << '\n';
                auto loaded = nskkzime::SkkDictionary::load(path, encoding);
                const auto& report = loaded->report();
                std::cerr << nskkzime::encoding_name(report.encoding) << ": " << report.entries << " entries, "
                          << report.candidates << " candidates, " << report.malformed_lines << " malformed lines, "
                          << report.unsupported_candidates << " unsupported candidates\n";
                for (const auto& diagnostic : report.diagnostics) std::cerr << diagnostic << '\n';
                dictionaries.push_back(std::move(loaded));
            }
            dictionary = std::make_shared<nskkzime::DictionarySet>(std::move(dictionaries));
        }
        nskkzime::Engine engine(std::move(dictionary));
        std::u32string document;
        const std::string_view keys(sequence);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            nskkzime::Key key;
            if (keys[i] == '<') {
                const auto end = keys.find('>', i);
                if (end == std::string_view::npos) throw std::invalid_argument("unclosed key token");
                key = token(keys.substr(i + 1, end - i - 1)); i = end;
            } else if (keys[i] == ' ') key.code = nskkzime::KeyCode::Space;
            else {
                if (static_cast<unsigned char>(keys[i]) < 0x20 || static_cast<unsigned char>(keys[i]) > 0x7e)
                    throw std::invalid_argument("key sequence must be printable ASCII");
                key.character = keys[i];
            }
            const auto result = engine.handle(key);
            document += result.committed;
            if (!result.consumed) {
                if (key.code == nskkzime::KeyCode::Character) document += static_cast<unsigned char>(key.character);
                else if (key.code == nskkzime::KeyCode::Space) document += U' ';
                else if (key.code == nskkzime::KeyCode::Backspace && !document.empty()) document.pop_back();
            }
            std::cout << "{\"revision\":" << engine.revision()
                      << ",\"consumed\":" << (result.consumed ? "true" : "false")
                      << ",\"committed\":" << json_string(result.committed)
                      << ",\"preedit\":" << json_string(engine.preedit())
                      << ",\"document\":" << json_string(document)
                      << ",\"dictionary_miss\":" << (result.dictionary_miss ? "true" : "false")
                      << ",\"candidates_exhausted\":" << (result.candidates_exhausted ? "true" : "false")
                      << ",\"candidate_index\":" << engine.candidate_index() << ",\"candidates\":[";
            bool first = true;
            for (const auto& item : engine.candidates()) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"text\":" << json_string(item.text) << ",\"annotation\":" << json_string(item.annotation) << '}';
            }
            std::cout << "]}\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!length) return 2;
        std::string value(static_cast<std::size_t>(length), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), length, nullptr, nullptr)) return 2;
        value.pop_back(); arguments.push_back(std::move(value));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
