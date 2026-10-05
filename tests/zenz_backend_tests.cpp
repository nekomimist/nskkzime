#include "zenz_backend.h"
#include "inference_ipc.h"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
struct Child {
    NativeHandle process;
    ~Child() { if (process.valid() && WaitForSingleObject(process.get(),3000)==WAIT_TIMEOUT) { TerminateProcess(process.get(),99); WaitForSingleObject(process.get(),3000); } }
};
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=3) return 2;
    if (!std::filesystem::exists(argv[2])) return 77;
    try {
        ZenzBackend model(argv[2]);
        InferenceRequest question{1,U"かいとう",U"試験問題の",{},1};
        check(model.run(question).candidates.front()==U"解答","context conversion");
        bool cancelled=false; std::atomic<int> polls=0;
        try { model.run(question,[&]{return ++polls>=4;}); } catch (const std::runtime_error&) {cancelled=true;}
        check(cancelled && polls>=4,"decode cancellation did not propagate");
        check(model.run(question).candidates.front()==U"解答","aborted context could not recover");
        // Hold one request after it acquires the context, then cancel a waiter.
        std::promise<void> reached; auto started=reached.get_future(); std::atomic<bool> released=false, notified=false;
        auto first=std::async(std::launch::async,[&]{return model.run(question,[&]{
            if (!notified.exchange(true)) reached.set_value();
            while (!released) std::this_thread::yield(); return false;
        });});
        started.get(); cancelled=false; const auto begin=GetTickCount64();
        try { model.run(question,[]{return true;}); } catch (const std::runtime_error&) {cancelled=true;}
        const auto elapsed=GetTickCount64()-begin; released=true; const auto result=first.get();
        check(cancelled && elapsed<500 && result.candidates.front()==U"解答","context contention ignored cancellation");
        const auto id=learning_client_id(); const std::wstring token(id.begin(),id.end());
        Child server{launch_zenz_server(argv[1],argv[2],token,4,2000,1000)};
        NativeHandle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr)); check(stop.valid(),"event");
        const auto ready=GetTickCount64()+15000;
        for (;;) {
            try { check(inference_exchange(token,question,ready,stop.get()).candidates.front()==U"解答","pipe conversion"); break; }
            catch (const InferenceFailure& failure) {
                if (failure.status!=InferenceStatus::Unavailable || GetTickCount64()>=ready) throw;
                check(WaitForSingleObject(server.process.get(),0)==WAIT_TIMEOUT,"model server exited before readiness"); Sleep(20);
            }
        }
        std::vector<std::future<InferenceResponse>> peers;
        { // Duplicate startup must exit before trying to load even a missing model.
            Child duplicate{launch_zenz_server(argv[1],std::filesystem::path(argv[2]).parent_path()/L"missing-duplicate.gguf",token)};
            check(WaitForSingleObject(duplicate.process.get(),3000)==WAIT_OBJECT_0,"duplicate startup did not exit");
            DWORD exit=1; GetExitCodeProcess(duplicate.process.get(),&exit); check(exit==0,"duplicate startup loaded another model");
        }
        for(unsigned i=0;i<4;++i) peers.push_back(std::async(std::launch::async,[&,i]{
            auto input=question; input.id=10+i; input.operation=InferenceOperation::Score;
            input.candidates={U"回答",U"解答",U"解凍"};
            if(i%2) input.left=U"冷凍食品を電子レンジで";
            return inference_exchange(token,input,GetTickCount64()+5000,stop.get());
        }));
        for(unsigned i=0;i<peers.size();++i) {
            const auto scored=peers[i].get(); const auto best=i%2 ? 2u : 1u;
            check(scored.id==10+i && scored.scores.size()==3 && scored.scores[best]>scored.scores[0] && scored.scores[best]>scored.scores[3-best],"parallel scores or contexts mixed");
        }
        // Disconnect an expensive request, then ensure the same context serves another request.
        auto lengthy=question; lengthy.operation=InferenceOperation::Score; lengthy.candidates.assign(32,std::u32string(100,U'あ'));
        try { inference_exchange(token,lengthy,GetTickCount64()+20,stop.get()); } catch(const InferenceFailure&) {}
        check(inference_exchange(token,question,GetTickCount64()+5000,stop.get()).candidates.front()==U"解答","pipe cancellation prevented recovery");
        check(WaitForSingleObject(server.process.get(),5000)==WAIT_OBJECT_0,"model helper failed to idle out");
        DWORD exit=1; GetExitCodeProcess(server.process.get(),&exit); check(exit==0,"model helper failed");
        std::cout<<"PASS real zenz conversion, scoring, cancellation, serialized context, parallel pipes and idle shutdown\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n'; return 1;}
}
