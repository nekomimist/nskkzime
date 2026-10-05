#include "learning_ipc.h"
#include "learning_client.h"
#include "local_storage.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch (...) { failed = true; } check(failed, "invalid data accepted"); }
template<class F> void until(F f) {
    const auto deadline = GetTickCount64() + 8000;
    do { if (f()) return; Sleep(20); } while (GetTickCount64() < deadline);
    throw std::runtime_error("background operation did not complete");
}
LearningRequest update(std::string client = std::string(32, 'a'), std::uint64_t seq = 1, std::u32string text = U"感じ") {
    return {std::move(client), seq, {{U"かんじ"}, {}, {"candidate", std::move(text), U"注釈"}}};
}
struct Files {
    std::filesystem::path root = std::filesystem::current_path() / ("learning-test-" + learning_client_id());
    Files() { std::filesystem::create_directory(root); }
    ~Files() {
        // Only these known test artifacts; no recursive deletion.
        for (const auto name : {L"user.nskkdb", L"user.nskkdb.lock", L"broken.nskkdb", L"broken.nskkdb.lock", L"forget.nskkdb", L"forget.nskkdb.lock"}) {
            std::error_code ignored; std::filesystem::remove(root / name, ignored);
        }
        std::error_code ignored; std::filesystem::remove(root, ignored);
    }
};
struct Child {
    NativeHandle process;
    ~Child() { if (process.valid() && WaitForSingleObject(process.get(), 2000) == WAIT_TIMEOUT) { TerminateProcess(process.get(), 99); WaitForSingleObject(process.get(), 2000); } }
};
void wait_ready(const std::filesystem::path& file) {
    until([&] { try { return learning_exchange(file, snapshot_request(UINT64_MAX)).starts_with("DATA"); } catch (...) { return false; } });
}
UserDictionary snapshot(const std::filesystem::path& file) {
    const auto bytes = learning_exchange(file, snapshot_request(UINT64_MAX));
    check(bytes.starts_with("DATA"), "snapshot failed"); return UserDictionary::parse(std::string_view(bytes).substr(4));
}
NativeHandle client_process(const std::filesystem::path& exe, const std::filesystem::path& file, wchar_t id) {
    auto command = L"\"" + exe.native() + L"\" --client \"" + file.native() + L"\" " + id;
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0, "cannot start test client");
    CloseHandle(pi.hThread); return NativeHandle(pi.hProcess);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--client") {
        try {
            for (std::uint64_t i = 1; i <= 12; ++i) {
                const auto request = update(std::string(32, static_cast<char>(argv[3][0])), i, U"候補" + ascii(std::to_string(i)));
                check(learning_exchange(argv[2], encode_learning(request), 5000) == "OKAY", "child write failed");
            }
            return 0;
        } catch (...) { return 1; }
    }
    if (argc != 2) return 2;
    Files files; const auto file = files.root / L"user.nskkdb";
    const std::filesystem::path server(argv[1]);
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"codec/roundtrip-special-characters-and-corruption", [] {
            auto request = update(); request.event.candidate.text = U"(a/b);\\\"𠮷";
            auto decoded = decode_learning(encode_learning(request)); check(decoded.event.candidate.text == request.event.candidate.text, "special characters changed");
            UserDictionary data; data.apply(request); const auto bytes = data.serialize();
            check(UserDictionary::parse(bytes).lookup({U"かんじ"})[0].text == request.event.candidate.text, "snapshot text changed");
            for (std::size_t i = 0; i < bytes.size(); ++i) { auto broken = bytes; broken[i] ^= 1; rejects([&] { UserDictionary::parse(broken); }); }
            rejects([&] { UserDictionary::parse(bytes.substr(0, bytes.size() - 1)); });
            rejects([&] { decode_learning(encode_learning(request) + "garbage"); });
        }},
        {"store/priority-okuri-and-durable-deduplication", [] {
            UserDictionary data; const auto a = update(); check(data.apply(a), "first event rejected");
            auto b = update(std::string(32, 'b'), 1, U"漢字"); data.apply(b);
            data = UserDictionary::parse(data.serialize()); check(!data.apply(a) && data.revision() == 2, "old retry applied twice");
            check(data.lookup({U"かんじ"})[0].text == U"漢字", "retry reordered newer candidate");
            auto okuri = update(std::string(32, 'a'), 2, U"描"); okuri.event.key = {U"か", 'k'}; okuri.event.okuri = U"く"; data.apply(okuri);
            check(data.lookup({U"か", 'k'}, U"く")[0].text == U"描" && data.lookup({U"か", 'k'}, U"け").empty(), "okuri preference leaked");
        }},
        {"store/reject-invalid-and-forbidden-learning", [] {
            for (int mode = 0; mode < 5; ++mode) {
                auto request = update();
                if (mode == 0) request.event.candidate.learning = LearningPolicy::DoNotLearn;
                if (mode == 1) request.event.candidate.source = CandidateSource::GeneratedLong;
                if (mode == 2) request.event.candidate.text = U"bad\ntext";
                if (mode == 3) request.client = "invalid";
                if (mode == 4) request.event.key.reading.assign(1025, U'あ');
                rejects([&] { encode_learning(request); });
            }
        }},
        {"ipc/serialize-multiple-processes-retry-and-restart", [&] {
            Child helper{launch_dictionary_server(server, file, 1000)}; wait_ready(file);
            Child duplicate{launch_dictionary_server(server, file, 1000)};
            check(WaitForSingleObject(duplicate.process.get(), 3000) == WAIT_OBJECT_0, "second writer remained alive");
            DWORD exit = 0; GetExitCodeProcess(duplicate.process.get(), &exit); check(exit == 3, "second writer not rejected");
            const auto request = update(); check(learning_exchange(file, encode_learning(request)) == "OKAY", "first update failed");
            check(learning_exchange(file, "BAD!") == "FAIL", "malformed message accepted");
            wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
            Child first{client_process(exe, file, L'b')}, second{client_process(exe, file, L'c')};
            for (const auto handle : {first.process.get(), second.process.get()}) {
                check(WaitForSingleObject(handle, 10000) == WAIT_OBJECT_0, "concurrent client hung");
                GetExitCodeProcess(handle, &exit); check(exit == 0, "concurrent client failed");
            }
            check(snapshot(file).revision() == 25, "concurrent updates were lost");
            check(TerminateProcess(helper.process.get(), 99) != 0 && WaitForSingleObject(helper.process.get(), 3000) == WAIT_OBJECT_0,
                "cannot simulate an owned helper crash");
            Child restarted{launch_dictionary_server(server, file, 1000)}; wait_ready(file);
            check(learning_exchange(file, encode_learning(request)) == "OKAY" && snapshot(file).revision() == 25, "restart lost deduplication");
            check(WaitForSingleObject(restarted.process.get(), 3000) == WAIT_OBJECT_0, "idle server did not exit");
        }},
        {"ipc/save-failure-preserves-disk-and-memory", [&] {
            Child helper{launch_dictionary_server(server, file, 1000)}; wait_ready(file);
            const auto revision = snapshot(file).revision(); const auto request = update(std::string(32, 'd'), 1, U"失敗テスト");
            {
                NativeHandle lock(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)); check(lock.valid(), "cannot lock fixture");
                check(learning_exchange(file, encode_learning(request)) == "FAIL", "locked save accepted");
                check(snapshot(file).revision() == revision && read_user_dictionary(file).revision() == revision, "failed save changed state");
            }
            check(learning_exchange(file, encode_learning(request)) == "OKAY" && snapshot(file).revision() == revision + 1, "retry after save failure failed");
        }},
        {"client/async-update-cross-client-snapshot-and-selection-stability", [&] {
            auto first = std::make_shared<LearningDictionary>(demo_dictionary(), file, server);
            auto second = std::make_shared<LearningDictionary>(demo_dictionary(), file, server);
            Engine active(first); for (const auto ch : std::string("Kanji")) active.handle({KeyCode::Character, ch}); active.handle({KeyCode::Space});
            const auto displayed = active.candidates();
            const auto start = GetTickCount64(); first->learn(update(std::string(32, 'e'), 1, U"感じ").event);
            check(GetTickCount64() - start < 500, "queue waited for IPC or disk");
            until([&] { return second->lookup({U"かんじ"})[0].text == U"感じ"; });
            check(active.candidates()[0].text == displayed[0].text, "snapshot change reordered active selection");
            check(read_user_dictionary(file).lookup({U"かんじ"})[0].text == U"感じ", "async acknowledgement was not durable");
            auto removed = update(std::string(32, 'f'), 1, U"感じ"); removed.action = DictionaryAction::Forget;
            check(learning_exchange(file, encode_learning(removed)) == "OKAY", "cross-client deletion failed");
            until([&] { return second->lookup({U"かんじ"})[0].text != U"感じ"; });
            check(active.candidates()[0].text == displayed[0].text, "deletion changed active selection");
            // Register then delete without ever looking up the saved generation.
            auto registration = update(std::string(32, 'a'), 1, U"未知語").event; registration.key = {U"みち"};
            Engine immediate(first); first->learn(registration); immediate.remember_registration(registration);
            until([&] { return !first->registration_pending(registration); });
            removed.sequence = 2; removed.event = registration;
            check(learning_exchange(file, encode_learning(removed)) == "OKAY", "registration deletion failed");
            until([&] { return first->lookup({U"みち"}).empty(); });
            for (const auto ch : std::string("Michi")) immediate.handle({KeyCode::Character, ch});
            immediate.handle({KeyCode::Space});
            check(immediate.registering(), "temporary cache resurrected a deleted registration");
        }},
        {"ipc/forget-save-failure-retry-and-restart", [&] {
            const auto isolated = files.root / L"forget.nskkdb";
            Child helper{launch_dictionary_server(server, isolated, 1000)}; wait_ready(isolated);
            const auto initial = update(); auto removed = update(std::string(32, 'b')); removed.action = DictionaryAction::Forget;
            check(learning_exchange(isolated, encode_learning(initial)) == "OKAY", "cannot seed deletion fixture");
            {
                NativeHandle lock(CreateFileW(isolated.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
                check(lock.valid(), "cannot lock deletion fixture");
                check(learning_exchange(isolated, encode_learning(removed)) == "FAIL", "failed deletion save acknowledged");
                check(snapshot(isolated).lookup({U"かんじ"}).size() == 1 && read_user_dictionary(isolated).revision() == 1, "failed delete changed memory or disk");
            }
            check(learning_exchange(isolated, encode_learning(removed)) == "OKAY", "delete retry failed");
            check(snapshot(isolated).lookup({U"かんじ"}).empty() && read_user_dictionary(isolated).revision() == 2, "deletion not durable");
            check(WaitForSingleObject(helper.process.get(), 3000) == WAIT_OBJECT_0, "deletion helper did not stop");
            Child restarted{launch_dictionary_server(server, isolated, 1000)}; wait_ready(isolated);
            check(learning_exchange(isolated, encode_learning(initial)) == "OKAY" && snapshot(isolated).revision() == 2, "old learning retry resurrected deleted word");
            check(learning_exchange(isolated, encode_learning(update(std::string(32, 'a'), 2))) == "OKAY", "explicit relearning failed");
            check(learning_exchange(isolated, encode_learning(removed)) == "OKAY" && snapshot(isolated).revision() == 3 &&
                snapshot(isolated).lookup({U"かんじ"}).size() == 1, "old delete retry removed a newer registration");
        }},
        {"ipc/atomic-import-save-failure-concurrent-learning-retry-and-refresh", [&] {
            Child helper{launch_dictionary_server(server,file,1000)}; wait_ready(file);
            auto observer=std::make_shared<LearningDictionary>(demo_dictionary(),file,server);
            ImportRequest batch{std::string(32,'1'),1,{}};
            for(unsigned i=0;i<200;++i) batch.entries.push_back({{U"とりこみ"+ascii(std::to_string(i))},{},{"",U"語"+ascii(std::to_string(i)),std::u32string(100,U'注')}});
            batch.entries.push_back({{U"わ",'k'},{},{"",U"湧",U"共通"}});
            const auto bytes=encode_import(batch); check(bytes.size()>20000,"batch did not exercise expanded IPC limit");
            const auto before=snapshot(file).serialize();
            {
                NativeHandle lock(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)); check(lock.valid(),"cannot lock import fixture");
                check(learning_exchange(file,bytes,5000)=="FAIL","failed import save acknowledged");
                check(snapshot(file).serialize()==before && read_user_dictionary(file).serialize()==before,"failed import changed memory or disk");
            }
            wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,ARRAYSIZE(exe));
            Child concurrent{client_process(exe,file,L'2')};
            check(learning_exchange(file,bytes,5000)=="OKAY","import retry failed");
            check(WaitForSingleObject(concurrent.process.get(),5000)==WAIT_OBJECT_0,"concurrent import learner hung");
            DWORD exit=1; GetExitCodeProcess(concurrent.process.get(),&exit); check(exit==0,"concurrent import learner failed");
            auto saved=snapshot(file);
            check(saved.revision()==UserDictionary::parse(before).revision()+13 && saved.lookup({U"とりこみ199"}).size()==1,"import overwrote concurrent learning or lost entries");
            until([&]{return observer->lookup({U"わ",'k'},U"く").size()==1;});
            check(observer->lookup({U"わ",'k'},U"け")[0].text==U"湧","snapshot lost generic okuri");
            auto removal=update(std::string(32,'3')); removal.action=DictionaryAction::Forget; removal.event=batch.entries.front();
            check(learning_exchange(file,encode_learning(removal))=="OKAY","cannot remove imported entry");
            observer.reset();
            check(WaitForSingleObject(helper.process.get(),5000)==WAIT_OBJECT_0,"import helper did not stop");
            Child restarted{launch_dictionary_server(server,file,1000)}; wait_ready(file);
            check(learning_exchange(file,bytes,5000)=="OKAY" && snapshot(file).lookup(batch.entries.front().key).empty(),"retry after restart resurrected a deleted import");
        }},
        {"client/unavailable-server-keeps-base-conversion", [&] {
            LearningDictionary client(demo_dictionary(), files.root / L"absent" / L"dictionary.nskkdb", files.root / L"not-installed.exe");
            const auto start = GetTickCount64(); client.learn(update().event);
            check(client.lookup({U"かんじ"})[0].text == U"漢字" && GetTickCount64() - start < 500, "failed helper blocked conversion");
        }},
        {"store/corrupt-file-is-not-overwritten", [&] {
            const auto broken = files.root / L"broken.nskkdb"; write_file_atomic(broken, "broken user data");
            Child helper{launch_dictionary_server(server, broken, 1000)};
            check(WaitForSingleObject(helper.process.get(), 3000) == WAIT_OBJECT_0, "corrupt database accepted");
            DWORD exit = 0; GetExitCodeProcess(helper.process.get(), &exit); check(exit == 2, "corrupt store did not fail");
            rejects([&] { read_user_dictionary(broken); });
            check(std::filesystem::file_size(broken) == 16, "corrupt database overwritten");
        }}
    };
    unsigned passed = 0;
    for (const auto& [name, test] : cases) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << '/' << cases.size() << " passed\n";
    // Detached workers have a bounded drain period; helper exits after idle.
    Sleep(12000);
    return passed == cases.size() ? 0 : 1;
}
