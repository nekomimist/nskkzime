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
ImportRequest imported(std::u32string_view text, std::string client = std::string(32,'b')) {
    const auto source = SkkDictionary::parse(text);
    check(!source->report().malformed_lines && !source->report().unsupported_candidates,"bad import fixture");
    return {std::move(client),1,source->entries()};
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
        {"import/generic-and-exact-scopes-rank-annotations-and-export", [] {
            auto request = imported(U";; okuri-ari entries.\nかk /書;一般/描/[く/描;個別/掻/]/[け/書;可能/]/\n;; okuri-nasi entries.\nかな /仮名/かな/\n");
            UserDictionary data; check(data.apply(decode_import(encode_import(request))),"import not applied");
            const auto exact = data.lookup({U"か",'k'},U"く"), other = data.lookup({U"か",'k'},U"こ");
            check(exact.size()==3 && exact[0].text==U"描" && exact[0].annotation==U"個別" && exact[1].text==U"掻" && exact[2].text==U"書","exact scopes changed");
            check(other.size()==2 && other[0].text==U"書" && other[1].text==U"描","generic candidates lost");
            check(data.serialize().starts_with("NKUD2"),"generic scopes exposed to old readers");
            data = UserDictionary::parse(data.serialize()); roundtrip(data);
            const auto restore = imported(from_utf8(data.export_skk())); UserDictionary copy; copy.apply(restore); roundtrip(copy);
            check(copy.entries().size()==data.entries().size() && copy.lookup({U"か",'k'},U"く")[0].annotation==U"個別","export/import lost a scope");
        }},
        {"import/existing-priority-annotation-and-durable-retry", [] {
            auto data = fixture(); const auto source = imported(U"かんじ /漢字;新注釈/新語/感じ/\n");
            check(data.apply(source),"merge rejected"); const auto values=data.lookup({U"かんじ"});
            check(values.size()==3 && values[0].text==U"感じ" && values[0].annotation==U"先頭" && values[1].text==U"漢字" && values[1].annotation.empty() && values[2].text==U"新語","import replaced old order or annotation");
            data=UserDictionary::parse(data.serialize()); check(data.serialize().starts_with("NKUD1") && !data.apply(source),"exact-only format or retry changed");
            auto remove=update(6,{U"かんじ"},U"新語"); remove.action=DictionaryAction::Forget; data.apply(remove);
            check(!data.apply(source) && data.lookup({U"かんじ"}).size()==2,"old import retry resurrected deletion");
            auto fresh=source; fresh.client=std::string(32,'c'); check(data.apply(fresh) && data.lookup({U"かんじ"})[2].text==U"新語","explicit new import failed");
        }},
        {"import/atomic-capacity-and-invalid-entry-rejection", [] {
            UserDictionary data;
            for(std::uint64_t i=1;i<=32;++i) data.apply(update(i,{U"いっぱい"},ascii(std::to_string(i))));
            const auto before=data.serialize(); auto request=imported(U"あ /新規/\nいっぱい /追加/\n");
            rejects([&]{data.apply(request);}); check(data.serialize()==before,"capacity failure partly imported or evicted entries");
            request=imported(U"あ /新規/\n"); request.entries.push_back({{U"不正"},{},{"",U"bad\ntext"}});
            rejects([&]{data.apply(request);}); check(data.serialize()==before,"invalid entry partly imported");
            request=imported(U"いっぱい /1/\n"); check(data.apply(request) && data.lookup({U"いっぱい"}).front().text==U"32","full group duplicate reranked or rejected");
        }},
        {"import/codec-limits-and-corruption", [] {
            const auto request=imported(U";; okuri-ari entries.\nかk /書/\n"); const auto bytes=encode_import(request);
            for(std::size_t size=0;size<bytes.size();++size) rejects([&]{decode_import(bytes.substr(0,size));});
            rejects([&]{decode_import(bytes+"extra");}); rejects([&]{decode_learning(bytes);});
            auto bad=request; bad.sequence=0; rejects([&]{encode_import(bad);});
            bad=request; bad.entries.clear(); rejects([&]{encode_import(bad);});
            bad=request; bad.entries.resize(import_max_candidates+1,request.entries.front()); rejects([&]{encode_import(bad);});
            bad=request; bad.entries.front().candidate.learning=LearningPolicy::DoNotLearn; rejects([&]{encode_import(bad);});
            bad=request; bad.entries.front().candidate.source=CandidateSource::GeneratedLong; rejects([&]{encode_import(bad);});
            LearningRequest learning{request.client,1,request.entries.front()}; rejects([&]{encode_learning(learning);});
        }},
        {"import/generic-delete-is-distinct-from-learned-okuri", [] {
            UserDictionary data; data.apply(imported(U";; okuri-ari entries.\nかk /書/描/[く/書/]/\n"));
            auto removal=update(1,{U"か",'k'},U"書"); removal.action=DictionaryAction::Forget;
            data.apply(decode_learning(encode_learning(removal)));
            check(data.lookup({U"か",'k'},U"く")[0].text==U"書" && data.lookup({U"か",'k'},U"け")[0].text==U"描","generic delete crossed exact scope");
            removal.sequence=2; removal.event.candidate.text=U"描"; data.apply(removal);
            check(data.serialize().starts_with("NKUD1") && data.lookup({U"か",'k'},U"け").empty(),"generic deletion failed to restore exact-only format");
            roundtrip(data);
        }},
        {"import/export-special-characters-and-abbrev-roundtrip", [] {
            const auto original=fixture(); UserDictionary restored; restored.apply(imported(from_utf8(original.export_skk())));
            check(restored.entries().size()==original.entries().size(),"export/import dropped entries");
            for(const auto& entry:original.entries()) {
                const auto expected=original.lookup(entry.key,entry.okuri), actual=restored.lookup(entry.key,entry.okuri);
                check(expected.size()==actual.size(),"export/import changed count");
                for(std::size_t i=0;i<expected.size();++i) check(expected[i].text==actual[i].text && expected[i].annotation==actual[i].annotation,"export/import lost characters");
            }
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
