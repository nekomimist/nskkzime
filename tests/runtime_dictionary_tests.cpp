#include "runtime_dictionary.h"
#include "learning_ipc.h"
#include "nskkzime/inference_policy.h"
#include "dictionary_wait.h"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void type(Engine& engine, std::string_view text) { for (char ch : text) engine.handle({KeyCode::Character,ch}); }
struct Gate {
    NativeHandle entered{CreateEventW(nullptr,TRUE,FALSE,nullptr)}, release{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    void wait() { SetEvent(entered.get()); check(WaitForSingleObject(release.get(),5000)==WAIT_OBJECT_0,"load gate timed out"); }
    void started() { check(WaitForSingleObject(entered.get(),3000)==WAIT_OBJECT_0,"worker did not start"); }
    void open() { SetEvent(release.get()); }
};
struct OpenOnExit { std::shared_ptr<Gate> gate; ~OpenOnExit() { gate->open(); } };
RuntimeDictionarySnapshot loaded() { return {demo_dictionary(),L"辞書: 読み込み済み",true}; }
}
int main() {
    int failed = 0;
    const std::vector<std::pair<const char*,std::function<void()>>> cases = {
        {"failed-bootstrap-retries-without-losing-reading",[] {
            auto attempts = std::make_shared<std::atomic<int>>(0);
            auto gate = std::make_shared<Gate>(); OpenOnExit release{gate};
            auto dictionary = std::make_shared<RuntimeDictionary>([attempts,gate] {
                if (++*attempts == 1) throw std::runtime_error("broker not started yet");
                gate->wait(); return loaded();
            }, true);
            Engine engine(dictionary); type(engine,"Kanji");
            gate->started();
            check(*attempts == 2 && !dictionary->ready(),"failed bootstrap appeared ready or did not retry");
            check(engine.handle({KeyCode::Space}).dictionary_pending && engine.preedit()==U"▽かんじ","retry lost pending reading");
            gate->open(); wait_for_dictionary([&] { return dictionary->ready(); });
            engine.handle({KeyCode::Space}); check(engine.preedit()==U"▼漢字","recovered broker did not convert existing reading");
        }},
        {"pending-keeps-reading-without-registration-or-inference",[] {
            auto gate = std::make_shared<Gate>(); OpenOnExit release{gate};
            const auto caller = GetCurrentThreadId(); auto worker=std::make_shared<std::atomic<DWORD>>(0);
            const auto start = GetTickCount64();
            auto dictionary = std::make_shared<RuntimeDictionary>([worker,gate] { *worker=GetCurrentThreadId(); gate->wait(); return loaded(); });
            check(GetTickCount64()-start < 500,"construction waited for loader"); gate->started();
            check(*worker!=caller && !dictionary->ready() && !dictionary->registration_allowed(),"loader ran on caller or appeared ready");
            check(dictionary->status().find(L"読み込み中")!=std::wstring::npos,"loading status missing");
            Engine engine(dictionary); type(engine,"Kanji"); const auto before=engine;
            const auto tick=GetTickCount64(); const auto result=engine.handle({KeyCode::Space});
            check(GetTickCount64()-tick<100,"conversion blocked on loader");
            check(result.consumed && result.dictionary_pending && !result.dictionary_miss && !result.conversion_started && !result.learning,
                "pending dictionary reported a miss or learning");
            check(engine.preedit()==U"▽かんじ" && !engine.registering(),"pending conversion changed reading");
            check(!plan_inference(before,engine,result,{}),"pending dictionary started inference");
            Engine kana(dictionary); type(kana,"kana"); check(kana.preedit().empty(),"ordinary kana stopped committing");
            Engine okuri(dictionary); type(okuri,"KaKu"); check(okuri.preedit()==U"▽か*く" && !okuri.registering(),"pending okuri lost");
            Engine abbrev(dictionary); type(abbrev,"/unknown"); check(abbrev.handle({KeyCode::Space}).dictionary_pending,"abbrev not pending");
            Engine cancel=engine; cancel.handle({KeyCode::Cancel});
            Engine commit=engine; check(commit.handle({KeyCode::Enter}).committed==U"かんじ","pending reading could not commit");
            gate->open(); wait_for_dictionary([&] {return dictionary->ready();});
            check(engine.preedit()==U"▽かんじ" && cancel.preedit().empty() && commit.preedit().empty(),"completion changed existing input");
            check(dictionary->registration_allowed(),"completed dictionary did not enable registration");
            engine.handle({KeyCode::Space}); check(engine.preedit()==U"▼漢字","retry did not convert");
            okuri.handle({KeyCode::Space}); check(okuri.preedit()==U"▼書く","retry lost okuri");
            abbrev.handle({KeyCode::Space}); check(abbrev.registering(),"true miss after readiness did not register");
        }},
        {"failure-finishes-loading-and-keeps-kana",[] {
            auto dictionary=std::make_shared<RuntimeDictionary>([]() -> RuntimeDictionarySnapshot {throw std::runtime_error("fixture failure");});
            wait_for_dictionary([&]{return dictionary->ready();});
            check(!dictionary->registration_allowed() && dictionary->status().find(L"読み込めません")!=std::wstring::npos,"load failure not visible");
            Engine engine(dictionary);type(engine,"Kanji");const auto result=engine.handle({KeyCode::Space});
            check(result.dictionary_miss && !result.dictionary_pending && !engine.registering(),"failure remained pending or enabled registration");
            check(engine.handle({KeyCode::Enter}).committed==U"かんじ","failure lost reading");
        }},
        {"destruction-does-not-wait-and-other-loads-progress",[] {
            auto gate=std::make_shared<Gate>(); OpenOnExit release{gate};
            auto sentinel=std::make_shared<int>(1); std::weak_ptr<int> lifetime=sentinel;
            auto first=std::make_shared<RuntimeDictionary>([gate,sentinel]{gate->wait();return loaded();});sentinel.reset();gate->started();
            auto second=std::make_shared<RuntimeDictionary>([]{return loaded();});
            wait_for_dictionary([&]{return second->ready();});
            const auto start=GetTickCount64();first.reset();check(GetTickCount64()-start<100,"destruction joined loader");
            gate->open();wait_for_dictionary([&]{return lifetime.expired();});
            check(second->lookup({U"かんじ"}).size()==2,"abandoned loader damaged independent snapshot");
        }},
        {"readers-see-only-complete-snapshots",[] {
            auto gate=std::make_shared<Gate>(); OpenOnExit release{gate};
            auto dictionary=std::make_shared<RuntimeDictionary>([gate]{gate->wait();return loaded();});gate->started();
            std::vector<std::future<void>> readers;
            for(int i=0;i<4;++i)readers.push_back(std::async(std::launch::async,[dictionary]{
                for(int j=0;j<5000;++j){const auto list=dictionary->lookup({U"かんじ"});check(list.empty() ||
                    (list.size()==2 && list[0].text==U"漢字" && list[1].text==U"感じ"),"partial snapshot observed");}
            }));
            gate->open();for(auto& reader:readers)reader.get();wait_for_dictionary([&]{return dictionary->ready();});
            check(dictionary->lookup({U"かんじ"}).size()==2,"published snapshot missing");
        }}
    };
    for(const auto& [name,run]:cases)try{run();std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}
    return failed ? 1 : 0;
}
