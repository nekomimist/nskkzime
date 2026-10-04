#include "nskkzime/dictionary.h"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace nskkzime;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void texts(const std::vector<Candidate>& candidates, std::initializer_list<std::u32string_view> expected) {
    require(candidates.size() == expected.size(), "candidate count differs");
    std::size_t index = 0;
    for (const auto value : expected) {
        if (candidates[index].text != value) throw std::runtime_error("expected " + utf8(value) + ", got " + utf8(candidates[index].text));
        require(!candidates[index].id.empty(), "missing candidate id");
        require(candidates[index].source == CandidateSource::Dictionary, "wrong candidate source");
        ++index;
    }
}
template<typename Action> void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid input was accepted");
}
struct Files {
    std::filesystem::path root = std::filesystem::current_path() /
        ("dictionary-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<std::filesystem::path> paths;
    Files() { std::filesystem::create_directory(root); }
    ~Files() {
        std::error_code ignored;
        for (const auto& path : paths) std::filesystem::remove(path, ignored);
        std::filesystem::remove(root, ignored); // Only remove the now-empty test directory.
    }
    std::filesystem::path write(const std::filesystem::path& name, std::string_view bytes) {
        const auto path = root / name;
        paths.push_back(path);
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("cannot write test dictionary");
        return path;
    }
};
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc != 2) return 2;
    Files files;
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"parser/plain-and-annotations", [] {
            auto d = SkkDictionary::parse(U"かんじ /漢字;文字/感じ;感覚/\n");
            auto list = d->lookup({U"かんじ"}); texts(list, {U"漢字", U"感じ"});
            require(list[0].annotation == U"文字", "annotation lost");
            require(d->report().entries == 1 && d->report().candidates == 2, "wrong import counts");
        }},
        {"parser/sections-distinguish-ascii-suffix", [] {
            auto d = SkkDictionary::parse(U";; okuri-ari entries.\nかk /書/\n;; okuri-nasi entries.\nかk /表記/\n");
            texts(d->lookup({U"か", 'k'}), {U"書"}); texts(d->lookup({U"かk"}), {U"表記"});
        }},
        {"parser/headerless-okuri-and-abbrev", [] {
            auto d = SkkDictionary::parse(U"かk /書/\ntest /試験/\n");
            texts(d->lookup({U"か", 'k'}), {U"書"}); texts(d->lookup({U"test"}), {U"試験"});
        }},
        {"parser/crlf-bom-no-final-newline", [] {
            auto d = SkkDictionary::parse(U"\ufeff;; comment\r\n\r\nかな\t/仮名/  \r\nにほん /日本/");
            texts(d->lookup({U"かな"}), {U"仮名"}); texts(d->lookup({U"にほん"}), {U"日本"});
        }},
        {"parser/okuri-precedence", [] {
            auto d = SkkDictionary::parse(U"おおk /大/多/[く/多/]/[き/大/]/\n");
            texts(d->lookup({U"おお", 'k'}, U"く"), {U"多", U"大"});
            texts(d->lookup({U"おお", 'k'}, U"き"), {U"大", U"多"});
            texts(d->lookup({U"おお", 'k'}, U"け"), {U"大", U"多"});
        }},
        {"parser/block-only-and-unmatched", [] {
            auto d = SkkDictionary::parse(U"かk /[く/書/]/[け/描/]/\n");
            texts(d->lookup({U"か", 'k'}, U"く"), {U"書"}); texts(d->lookup({U"か", 'k'}, U"こ"), {});
        }},
        {"parser/duplicate-entries-first-annotation-wins", [] {
            auto d = SkkDictionary::parse(U"かな /仮名;先/仮名;後/\nかな /仮名/かな/\n");
            auto list = d->lookup({U"かな"}); texts(list, {U"仮名", U"かな"});
            require(list[0].annotation == U"先", "duplicate replaced the first annotation");
        }},
        {"parser/literal-concat", [] {
            auto d = SkkDictionary::parse(U"io /(concat \"I\\057O\");(concat \"in\" \"put\")/(concat \"a\\073b\")/\n");
            auto list = d->lookup({U"io"}); texts(list, {U"I/O", U"a;b"});
            require(list[0].annotation == U"input", "literal annotation not decoded");
        }},
        {"parser/quoted-semicolon-and-backslash", [] {
            auto d = SkkDictionary::parse(U"test /(concat \"a;b\\\\c\\\"\")/\n");
            texts(d->lookup({U"test"}), {U"a;b\\c\""});
        }},
        {"parser/no-evaluation", [] {
            auto d = SkkDictionary::parse(U"test /(progn (message \"bad\"))/(concat (symbol-name 'test))/(concat \"a\\n\")/正常/\n");
            texts(d->lookup({U"test"}), {U"正常"}); require(d->report().unsupported_candidates == 3, "unsupported forms not counted");
        }},
        {"parser/literal-brackets-in-nasi", [] {
            auto d = SkkDictionary::parse(U"かっこ /[/]/\n"); texts(d->lookup({U"かっこ"}), {U"[", U"]"});
        }},
        {"parser/parenthesized-japanese-abbreviations", [] {
            auto d = SkkDictionary::parse(U"かぶ /(株)/（株）/\nわらい /(笑)/\n");
            texts(d->lookup({U"かぶ"}), {U"(株)", U"（株）"}); texts(d->lookup({U"わらい"}), {U"(笑)"});
            require(d->report().unsupported_candidates == 0, "ordinary abbreviations were skipped");
        }},
        {"parser/broken-block-is-atomic", [] {
            auto d = SkkDictionary::parse(U"かk /書/[く/描/\nかな /仮名/\n");
            texts(d->lookup({U"か", 'k'}, U"く"), {}); texts(d->lookup({U"かな"}), {U"仮名"});
            require(d->report().malformed_lines == 1, "broken block not counted");
        }},
        {"parser/malformed-lines-do-not-poison-others", [] {
            auto d = SkkDictionary::parse(U"broken\n /空/\nかな 仮名/\nかな /仮名\nかな /仮名/\n");
            texts(d->lookup({U"かな"}), {U"仮名"}); require(d->report().malformed_lines == 4, "invalid line counts differ");
        }},
        {"parser/oversized-line-and-bounded-diagnostics", [] {
            std::u32string input(65537, U'a'); input += U" /値/\n";
            for (int i = 0; i < 40; ++i) input += U"broken\n";
            auto d = SkkDictionary::parse(input);
            require(d->report().malformed_lines == 41 && d->report().diagnostics.size() == 32, "diagnostics not bounded");
        }},
        {"parser/stable-id-independent-of-order", [] {
            auto a = SkkDictionary::parse(U"かな /仮名/かな/\n"); auto b = SkkDictionary::parse(U"かな /かな/仮名;注釈/\n");
            require(a->lookup({U"かな"})[0].id == b->lookup({U"かな"})[1].id, "id depends on rank or annotation");
        }},
        {"parser/non-bmp", [] { auto d = SkkDictionary::parse(U"よし /𠮷/吉/\n"); texts(d->lookup({U"よし"}), {U"𠮷", U"吉"}); }},
        {"merge/priority-and-deduplication", [] {
            DictionarySet d({SkkDictionary::parse(U"かな /かな;個人/仮名/\n"), SkkDictionary::parse(U"かな /仮名;共有/かな/カナ/\n")});
            auto list = d.lookup({U"かな"}); texts(list, {U"かな", U"仮名", U"カナ"}); require(list[0].annotation == U"個人", "priority lost");
        }},
        {"merge/okuri-does-not-cross-dictionary-priority", [] {
            DictionarySet d({SkkDictionary::parse(U"かk /書/\n"), SkkDictionary::parse(U"かk /書/描/[く/描/]/\n")});
            texts(d.lookup({U"か", 'k'}, U"く"), {U"書", U"描"});
        }},
        {"engine/actual-okuri-reaches-dictionary", [] {
            Engine e(SkkDictionary::parse(U"おおk /大/多/[く/多/]/[き/大/]/\n"));
            for (char ch : std::string("OoKu")) e.handle({KeyCode::Character, ch});
            require(e.preedit() == U"▼多く", "engine did not pass typed okuri");
        }},
        {"file/utf8-fixture", [&] {
            auto d = SkkDictionary::load(std::filesystem::path(argv[1]));
            texts(d->lookup({U"じしょ"}), {U"辞書"}); require(d->report().malformed_lines == 0, "valid fixture not fully parsed");
        }},
        {"file/unicode-path-and-bom", [&] {
            auto path = files.write(std::filesystem::path(U"辞書 with space.skk"), "\xef\xbb\xbf" + utf8(U"かな /仮名/\r\n"));
            auto d = SkkDictionary::load(path); texts(d->lookup({U"かな"}), {U"仮名"});
        }},
        {"file/missing-rejected", [&] { rejects([&] { SkkDictionary::load(files.root / "missing"); }); }},
        {"file/invalid-utf8-with-header-rejected", [&] {
            auto path = files.write("invalid.skk", ";; -*- coding: utf-8 -*-\n\xc0\xaf /x/\n");
            rejects([&] { SkkDictionary::load(path); });
        }},
        {"file/unsupported-header-rejected", [&] {
            auto path = files.write("unknown.skk", ";; -*- coding: euc-jis-2004 -*-\n");
            rejects([&] { SkkDictionary::load(path); });
        }},
        {"file/explicit-encoding-overrides-header", [&] {
            auto path = files.write("override.skk", ";; -*- coding: euc-jp -*-\n" + utf8(U"かな /仮名/\n"));
            texts(SkkDictionary::load(path, DictionaryEncoding::Utf8)->lookup({U"かな"}), {U"仮名"});
        }},
        {"unicode/strict-decoding", [] {
            for (const std::string bytes : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe3\x81", "\x80"})
                rejects([&] { from_utf8(bytes); });
            require(from_utf8(utf8(U"あ𠮷")) == U"あ𠮷", "UTF-8 roundtrip failed");
        }},
#ifdef _WIN32
        {"file/euc-jp-with-header", [&] {
            auto path = files.write("euc.skk", ";; -*- coding: euc-jp -*-\n\xa4\xab\xa4\xf3\xa4\xb8 /\xb4\xc1\xbb\xfa/\n");
            auto d = SkkDictionary::load(path); texts(d->lookup({U"かんじ"}), {U"漢字"});
            require(d->report().encoding == DictionaryEncoding::EucJp, "wrong detected encoding");
        }},
        {"file/headerless-euc-jp-and-halfwidth", [&] {
            auto path = files.write("halfwidth.skk", "kana /\x8e\xb6/\n");
            texts(SkkDictionary::load(path)->lookup({U"kana"}), {U"ｶ"});
        }},
        {"file/euc-jp-ss3-rejected-with-utf8-alternative", [&] {
            auto path = files.write("supplementary.skk", ";; coding:\teuc-jp\nmark /\x8f\xa2\xaf/\n");
            rejects([&] { SkkDictionary::load(path); });
            const auto alternative = files.write("supplementary.utf8.skk", ";; coding:\tutf-8\n" + utf8(U"mark /\u02d8/\n"));
            texts(SkkDictionary::load(alternative)->lookup({U"mark"}), {U"\u02d8"});
        }},
        {"file/invalid-euc-jp-rejected", [&] {
            for (const std::string bytes : {"\x8e\xff", "\x8f\xa1", "\xa4", "\xa4\x20", "\xff\xa1"}) {
                auto path = files.write("invalid-euc.skk", bytes);
                rejects([&] { SkkDictionary::load(path, DictionaryEncoding::EucJp); });
            }
        }},
#endif
    };
    int failed = 0;
    for (const auto& [name, run] : cases) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << cases.size() - failed << '/' << cases.size() << " passed\n";
    return failed ? 1 : 0;
}
