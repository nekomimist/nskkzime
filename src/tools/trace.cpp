#include "nskkzime/engine.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::string quoted(std::u32string_view text) {
    std::string result = "\"";
    for (char ch : nskkzime::utf8(text)) {
        if (ch == '"' || ch == '\\') result += '\\';
        result += ch;
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
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: skk_trace \"Kanji<Space><Space><Enter>\"\n"
                  << "demo dictionary only; tokens: Space Enter BS C-g Esc C-j\n";
        return 2;
    }
    try {
        nskkzime::Engine engine(nskkzime::demo_dictionary());
        std::u32string document;
        const std::string_view keys(argv[1]);
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
                      << ",\"committed\":" << quoted(result.committed)
                      << ",\"preedit\":" << quoted(engine.preedit())
                      << ",\"document\":" << quoted(document)
                      << ",\"dictionary_miss\":" << (result.dictionary_miss ? "true" : "false")
                      << ",\"candidates_exhausted\":" << (result.candidates_exhausted ? "true" : "false") << "}\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
