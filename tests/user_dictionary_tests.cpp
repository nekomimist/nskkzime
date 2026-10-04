#include "nskkzime/dictionary.h"
#include "learning_ipc.h"
#include "local_storage.h"
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch (...) { failed = true; } check(failed, "invalid data accepted"); }
LearningRequest update(std::uint64_t seq, LookupKey key, std::u32string value, std::u32string okuri = {}, std::u32string annotation = {}) {
    return {std::string(32, 'a'), seq, {std::move(key), std::move(okuri), {"", std::move(value), std::move(annotation)}}};
}
UserDictionary fixture() {
    UserDictionary data;
    data.apply(update(1, {U"かんじ"}, U"漢字"));
    data.apply(update(2, {U"かんじ"}, U"感じ", {}, U"先頭"));
    data.apply(update(3, {U"か", 'k'}, U"描", U"く"));
    data.apply(update(4, {U"か", 'k'}, U"書", U"け"));
    data.apply(update(5, {U"AI"}, U"(a/b);\\\"𠮷", {}, U"注釈;/[\\\"]"));
    return data;
}
void roundtrip(const UserDictionary& data) {
    const auto imported = SkkDictionary::parse(from_utf8(data.export_skk()));
    check(imported->report().malformed_lines == 0 && imported->report().unsupported_candidates == 0, "export did not parse cleanly");
    for (const auto& entry : data.entries()) {
        const auto expected = data.lookup(entry.key, entry.okuri), actual = imported->lookup(entry.key, entry.okuri);
        check(expected.size() == actual.size(), "export changed candidate count");
        for (std::size_t i = 0; i < expected.size(); ++i)
            check(actual[i].text == expected[i].text && actual[i].annotation == expected[i].annotation, "export changed rank or spelling");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--seed") {
        try { write_file_atomic(argv[2], fixture().serialize(), false); return 0; } catch (...) { return 2; }
    }
    if (argc != 1) return 2;
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"forget/exact-key-okuri-spelling-and-retry", [] {
            auto data = fixture(); auto request = update(6, {U"か", 'k'}, U"描", U"く"); request.action = DictionaryAction::Forget;
            const auto decoded = decode_learning(encode_learning(request));
            check(decoded.action == DictionaryAction::Forget && data.apply(decoded), "forget codec failed");
            check(data.lookup({U"か", 'k'}, U"く").empty() && data.lookup({U"か", 'k'}, U"け").size() == 1, "forget crossed okuri boundary");
            data = UserDictionary::parse(data.serialize()); check(!data.apply(request) && data.revision() == 6, "forget retry changed revision");
            data.apply(update(7, {U"か", 'k'}, U"描", U"く"));
            check(!data.apply(request) && data.lookup({U"か", 'k'}, U"く").size() == 1, "old forget retry erased a later registration");
        }},
        {"forget/preserves-rank-other-keys-and-base-dictionary", [] {
            auto data = fixture(); auto request = update(6, {U"かんじ"}, U"感じ"); request.action = DictionaryAction::Forget;
            data.apply(request);
            check(data.lookup({U"かんじ"}).size() == 1 && data.lookup({U"かんじ"})[0].text == U"漢字", "forget lost another candidate");
            DictionarySet merged({std::make_shared<UserDictionary>(data), demo_dictionary()});
            check(merged.lookup({U"かんじ"}).size() == 2, "forget blacklisted a shared dictionary word");
            check(data.lookup({U"AI"}).size() == 1, "forget affected another key");
        }},
        {"forget/missing-entry-is-acknowledged-once", [] {
            UserDictionary data; auto request = update(1, {U"ない"}, U"無い"); request.action = DictionaryAction::Forget;
            check(data.apply(request) && data.entries().empty(), "missing entry created a candidate");
            data = UserDictionary::parse(data.serialize()); check(!data.apply(request) && data.revision() == 1, "missing entry not acknowledged durably");
            request.action = static_cast<DictionaryAction>(99); rejects([&] { encode_learning(request); });
        }},
        {"export/order-annotation-escaping-and-exact-okuri", [] {
            auto data = fixture(); roundtrip(data);
            const auto loaded = SkkDictionary::parse(from_utf8(data.export_skk()));
            check(loaded->lookup({U"か", 'k'}, U"こ").empty(), "export widened okuri scope");
            const auto bytes = data.export_skk();
            check(bytes.find("AI /") < bytes.find(utf8(U"かんじ /")), "okuri-nasi entries not sorted");
            check(UserDictionary::parse(data.serialize()).export_skk() == bytes, "export not deterministic");
        }},
        {"export/brackets-parentheses-spaces-and-empty-dictionary", [] {
            UserDictionary data; std::uint64_t sequence = 0;
            for (const auto value : {U"]", U"[く", U"(shell-command)", U"(株)", U"white space", U" leading ", U";", U"/", U"\\", U"\""})
                data.apply(update(++sequence, {U"え", 'k'}, value, U"く"));
            roundtrip(data); roundtrip(UserDictionary{});
        }},
        {"export/rejects-unrepresentable-keys-without-dropping-entries", [] {
            for (const auto reading : {U"white space", U";comment", U"\ufffd"}) {
                UserDictionary data; data.apply(update(1, {reading}, U"候補")); rejects([&] { data.export_skk(); });
                check(data.entries().size() == 1, "failed export changed source");
            }
            UserDictionary data; data.apply(update(1, {U"か", 'k'}, U"書", U"く/")); rejects([&] { data.export_skk(); });
            UserDictionary large;
            for (std::uint64_t i = 1; i <= 32; ++i) large.apply(update(i, {U"ながい"}, std::u32string(1023, U'/') + static_cast<char32_t>(U'あ' + i)));
            rejects([&] { large.export_skk(); }); // Escaping can exceed SKK's line limit.
        }}
    };
    unsigned passed = 0;
    for (const auto& [name, test] : cases) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << '/' << cases.size() << " passed\n";
    return passed == cases.size() ? 0 : 1;
}
