#include "app_container.h"
#include "broker_snapshot.h"
#include "container_runtime.h"
#include "inference_client.h"
#include "nskkzime/compiled_dictionary.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>

using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
ULONGLONG file_time(FILETIME time) { return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
void report(nlohmann::json value) {
    FILETIME now{}; GetSystemTimePreciseAsFileTime(&now);
    ULONGLONG unbiased=0; check(QueryUnbiasedInterruptTime(&unbiased), "unbiased clock unavailable");
    value["utc_filetime"]=file_time(now); value["tick_ms"]=GetTickCount64(); value["unbiased_100ns"]=unbiased;
    value["pid"]=GetCurrentProcessId(); std::cout << value.dump() << std::endl;
}
std::wstring event_name(std::wstring_view token, std::wstring_view suffix) {
    check(token.size()==32 && token.find_first_not_of(L"0123456789abcdef")==std::wstring_view::npos, "invalid checkpoint token");
    return L"Global\\nskkzime-continuity-"+user_sid()+L"-"+std::wstring(token)+L"-"+std::wstring(suffix);
}
NativeHandle make_event(const std::wstring& name) {
    SecurityDescriptor security(owner_only_sddl(user_sid()));
    NativeHandle event(CreateEventW(security.attributes(),TRUE,FALSE,name.c_str()));
    check(event.valid() && GetLastError()!=ERROR_ALREADY_EXISTS, "checkpoint event already exists or failed");
    return event;
}
struct Server {
    NativeHandle process;
    DWORD pid=0;
    ULONGLONG created=0;
};
Server server_identity(const InferenceConfig& config, HANDLE cancel) {
    auto pipe=open_pipe(inference_pipe_name(config.token),GetTickCount64()+5000,cancel);
    DWORD pid=0; check(GetNamedPipeServerProcessId(pipe.get(),&pid), "cannot identify private server");
    Server result{NativeHandle(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)),pid};
    check(result.process.valid(), "cannot inspect private server");
    wchar_t path[32768]{}; DWORD length=ARRAYSIZE(path);
    check(QueryFullProcessImageNameW(result.process.get(),0,path,&length) &&
        CompareStringOrdinal(path,static_cast<int>(length),config.server.c_str(),-1,TRUE)==CSTR_EQUAL, "unexpected server executable");
    const auto ours=process_identity(), peer=process_identity(result.process.get());
    check(peer.user==ours.user && peer.session==ours.session && peer.container.empty(), "unexpected server identity");
    FILETIME created{},exited{},kernel{},user{};
    check(GetProcessTimes(result.process.get(),&created,&exited,&kernel,&user), "cannot inspect server start time");
    result.created=file_time(created); return result;
}
struct Probe {
    NativeHandle cancel{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    HANDLE stop;
    std::filesystem::path config;
    std::string generation;
    std::shared_ptr<const Dictionary> retained;
    std::uint64_t id=0;
    InferenceResponse exchange(InferenceRequest request) {
        request.id=++id;
        return decode_response(container_operation(generation,"INF1",encode_request(request),GetTickCount64()+5000,cancel.get()),request);
    }
    void conversion(bool frozen) {
        const auto begin=GetTickCount64();
        const auto response=exchange({0,U"かいとう",frozen?U"冷凍食品を電子レンジで":U"試験問題の",{},1});
        check(response.candidates.size()==1 && response.candidates.front()==(frozen?U"解凍":U"解答"), "context conversion differs");
        report({{"event","conversion"},{"context",frozen?"frozen":"exam"},{"elapsed_ms",GetTickCount64()-begin}});
    }
    void dictionary(const Dictionary& value) {
        const auto candidates=value.lookup({U"かんじ"});
        check(candidates.size()==1 && candidates.front().text==U"NSKKZIME-CONTINUITY", "private dictionary differs");
    }
    void phase(const char* name) {
        check(WaitForSingleObject(stop,0)==WAIT_TIMEOUT, "probe stopped");
        const auto begin=GetTickCount64();
        auto bootstrap=decode_broker_bootstrap(container_exchange("INI3",begin+5000,cancel.get()));
        check(bootstrap.complete && !bootstrap.demo && !bootstrap.learning && bootstrap.images.size()==1 &&
            std::filesystem::path(wide_message(bootstrap.file))==config, "private bootstrap differs");
        check(generation.empty() || generation==bootstrap.generation, "private settings changed across checkpoint");
        generation=bootstrap.generation;
        auto bytes=std::make_shared<const std::string>(std::move(bootstrap.images.front()));
        auto loaded=std::make_shared<CompiledDictionary>(*bytes,bytes);
        dictionary(*loaded); if(retained)dictionary(*retained);else retained=loaded;
        report({{"event","bootstrap"},{"phase",name},{"elapsed_ms",GetTickCount64()-begin},{"retained_dictionary_valid",true}});
        conversion(false); conversion(true);
        InferenceRequest score{0,U"かいとう",U"試験問題の",{},1,InferenceOperation::Score,{U"回答",U"解答",U"解凍"}};
        auto scored=exchange(score);
        check(scored.scores.size()==3 && scored.scores[1]>scored.scores[0] && scored.scores[1]>scored.scores[2], "context scoring differs");
        score.candidates.assign(32,std::u32string(100,U'あ'));
        NativeHandle finished(CreateEventW(nullptr,TRUE,FALSE,nullptr)); check(finished.valid(), "cancel timer event");
        bool cancelled=false;
        {
            std::jthread timer([&]{if(WaitForSingleObject(finished.get(),10)==WAIT_TIMEOUT)SetEvent(cancel.get());});
            try { (void)exchange(score); }
            catch(const ContainerFailure& failure) { cancelled=failure.reason==ContainerReason::Cancelled; }
            SetEvent(finished.get());
        }
        ResetEvent(cancel.get()); check(cancelled,"in-flight request was not cancelled");
        conversion(false); report({{"event","cancellation_recovered"},{"phase",name}});
        const auto inference=current_inference_config(); check(inference.has_value(), "private inference disabled");
        auto old=server_identity(*inference,cancel.get());
        report({{"event","idle_wait"},{"server_pid",old.pid},{"server_created",old.created}});
        HANDLE handles[]={old.process.get(),stop};
        check(WaitForMultipleObjects(2,handles,FALSE,20000)==WAIT_OBJECT_0,"private server did not idle out");
        DWORD exit=1;check(GetExitCodeProcess(old.process.get(),&exit) && exit==0,"idle server failed");
        conversion(true);
        auto restarted=server_identity(*inference,cancel.get());
        check(old.pid!=restarted.pid || old.created!=restarted.created,"server did not restart");
        report({{"event","phase_passed"},{"phase",name},{"server_pid",restarted.pid},{"server_created",restarted.created}});
    }
};
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==3 && (std::wstring_view(argv[1])==L"--resume" || std::wstring_view(argv[1])==L"--stop")) {
            const auto name=event_name(argv[2],std::wstring_view(argv[1])==L"--resume"?L"resume":L"stop");
            NativeHandle event(OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str()));
            check(event.valid() && SetEvent(event.get()),"cannot signal existing checkpoint"); return 0;
        }
        check(argc==5 && std::wstring_view(argv[1])==L"--config" && std::wstring_view(argv[3])==L"--checkpoint", "usage: --config INI --checkpoint TOKEN | --resume TOKEN | --stop TOKEN");
        const std::filesystem::path config(argv[2]);check(config.is_absolute(),"absolute private configuration required");
        check(SetEnvironmentVariableW(L"NSKKZIME_CONFIG",config.c_str()),"cannot select private configuration");
        const auto settings=read_dictionary_settings(config);
        check(!settings.learning && !settings.zenz.log_input && !settings.tsf_log.enabled && settings.zenz.enabled &&
            settings.zenz.idle_seconds<=10,"probe requires isolated inference with short idle and learning/logging disabled");
        auto resume=make_event(event_name(argv[4],L"resume")),stop=make_event(event_name(argv[4],L"stop"));
        Probe probe{NativeHandle(CreateEventW(nullptr,TRUE,FALSE,nullptr)),stop.get(),config};
        check(probe.cancel.valid(),"request cancellation event");
        report({{"event","started"}}); probe.phase("before");
        report({{"event","checkpoint_wait"},{"requests_paused",true}});
        HANDLE handles[]={resume.get(),stop.get()};
        check(WaitForMultipleObjects(2,handles,FALSE,20*60*1000)==WAIT_OBJECT_0,"checkpoint stopped or timed out");
        report({{"event","checkpoint_released"}}); probe.phase("after");
        report({{"event","passed"},{"actual_sleep_verified",false}}); return 0;
    }catch(const std::exception& error){report({{"event","failed"},{"error",error.what()}});return 1;}
}
