#include "inference_client.h"
#include "dictionary_settings.h"
#include "nskkzime/engine.h"
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F fn) { bool failed = false; try { fn(); } catch (...) { failed = true; } check(failed,"invalid input accepted"); }
std::wstring token() { const auto id = learning_client_id(); return {id.begin(),id.end()}; }
void status_trace_tests() {
    const auto previous = environment_value(L"NSKKZIME_STATUS_TRACE");
    const auto path = std::filesystem::current_path() / (L"status-trace-" + token() + L".log");
    struct Cleanup {
        std::optional<std::wstring> previous; std::filesystem::path path;
        ~Cleanup() {
            SetEnvironmentVariableW(L"NSKKZIME_STATUS_TRACE", previous ? previous->c_str() : nullptr);
            std::error_code ignored; std::filesystem::remove(path, ignored);
        }
    } cleanup{previous,path};
    SetEnvironmentVariableW(L"NSKKZIME_STATUS_TRACE", path.filename().c_str());
    set_inference_status(nullptr,L"推論: 相対パス無効");
    check(!std::filesystem::exists(path),"relative status trace created a file");
    SetEnvironmentVariableW(L"NSKKZIME_STATUS_TRACE",path.c_str());
    set_inference_status(nullptr,L"推論: 試験");
    { std::ifstream input(path,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(input)),{});
      check(bytes.find("推論: 試験")!=std::string::npos,"explicit status trace lost fixed message"); }
    const auto length=std::filesystem::file_size(path);
    SetEnvironmentVariableW(L"NSKKZIME_STATUS_TRACE",nullptr);
    set_inference_status(nullptr,L"推論: 無効");
    check(std::filesystem::file_size(path)==length,"disabled status trace appended a message");
    std::filesystem::resize_file(path,65536);
    SetEnvironmentVariableW(L"NSKKZIME_STATUS_TRACE",path.c_str());
    set_inference_status(nullptr,L"推論: 上限");
    check(std::filesystem::file_size(path)==65536,"status trace exceeded its size threshold");
    SetEnvironmentVariableW(L"NSKKZIME_STATUS_TRACE",std::filesystem::current_path().c_str());
    set_inference_status(nullptr,L"推論: 保存不可");
    check(inference_status().find(L"推論: 保存不可")!=std::wstring::npos,"trace failure prevented status update");
    clear_inference_status(nullptr);
}
void protocol_tests() {
    const InferenceRequest request{42,U"かな",U"前\n\"\\😀",U"後",5};
    const auto decoded = decode_request(encode_request(request));
    check(decoded.id == request.id && decoded.reading == request.reading && decoded.left == request.left && decoded.right == request.right,"request roundtrip");
    const InferenceResponse response{42,{U"仮名",U"かな😀"}};
    check(decode_response(encode_response(response),42,5).candidates == response.candidates,"response roundtrip");
    validate_inference_hello(inference_hello());
    auto scoring = request; scoring.operation = InferenceOperation::Score; scoring.candidates = {U"仮名",U""};
    check(decode_request(encode_request(scoring)).candidates == scoring.candidates,"score request order");
    const InferenceResponse scored{42,{}, {-1.25f,-9.5f}, InferenceOperation::Score};
    check(decode_response(encode_response(scored),scoring).scores == scored.scores,"score response order");
    rejected([&] { auto bad=scoring; bad.candidates.resize(65,U"語"); encode_request(bad); });
    rejected([&] { decode_response(R"({"id":42,"scores":[0]})",scoring); });
    rejected([&] { decode_response(R"({"id":42,"scores":[null,0]})",scoring); });
    rejected([&] { decode_response(R"({"id":42,"scores":[1e300,0]})",scoring); });
    rejected([&] { decode_response(R"({"id":42,"candidates":["語"],"scores":[]})",42,5); });
    rejected([] { validate_inference_hello(R"({"hello":"nkkz-ime-server","protocol":99})"); });
    rejected([&] { decode_response(encode_response(response),43,5); });
    rejected([&] { decode_response(encode_response(response),42,1); });
    for (auto bad : {R"({"id":1,"id":2})",R"({"id":-1})",R"({"id":1,"error":"failure"})",R"({"id":1,"candidates":["\ud800"]})",R"({"id":1,"candidates":[""]})",R"({"id":1,"candidates":["a\n"]})",R"({"id":1,"candidates":[4]})"})
        rejected([&] { decode_response(bad,1,5); });
    rejected([] { decode_request(std::string(inference_max_line+1,'x')); });
    rejected([] { decode_request(R"({"id":1,"op":"convert","kana":"a","left":"","right":"","n":4294967297})"); });
    rejected([] { decode_request(R"({"id":1,"op":"convert","kana":"a","left":"","right":"","n":1.0})"); });
    rejected([] { decode_request(R"({"id":1,"op":"convert","kana":"a","left":"","right":"","n":1,"extra":[[[[[[[[[0]]]]]]]]]})"); });
    auto long_context = request; long_context.left.assign(41,U'a'); rejected([&] { encode_request(long_context); });
    rejected([] { inference_pipe_name(L"remote\\pipe"); });
    rejected([] { inference_pipe_name(L"../other"); });
}
void core_tests() {
    const Candidate generated{"test",U"蜜柑",U"",CandidateSource::GeneratedFallback,LearningPolicy::DoNotLearn};
    auto type = [](Engine& e, std::string_view text) { for (const auto ch : text) e.handle({KeyCode::Character,ch}); };
    Engine engine(demo_dictionary()); type(engine,"Mikan");
    check(engine.handle({KeyCode::Space}).dictionary_miss && engine.registering(),"missing dictionary contract");
    check(engine.inference_reading() == U"みかん","registration lost query"); engine.defer_registration();
    check(!engine.registering() && engine.composition_text() == U"みかん","defer lost reading");
    check(engine.append_generated({generated}) && engine.composition_text() == U"蜜柑","generation not selectable");
    const auto confirmed = engine.handle({KeyCode::Enter}); check(confirmed.committed == U"蜜柑" && !confirmed.learning,"dummy candidate learned");
    type(engine,"Kanji"); engine.handle({KeyCode::Space}); engine.handle({KeyCode::Space});
    check(engine.handle({KeyCode::Space}).candidates_exhausted,"exhaustion contract"); engine.defer_registration();
    check(engine.candidates().size() == 2 && engine.candidate_index() == 1,"defer lost dictionary candidates");
    check(engine.append_generated({generated,generated}) && engine.candidates().size() == 3 && engine.candidate_index() == 2,"append/dedup/order");
    engine.handle({KeyCode::Cancel}); check(engine.inference_reading() == U"かんじ","cancel lost reading");
    check(engine.resume_registration() && engine.registering(),"fallback registration");
    check(!engine.append_generated({generated}),"generation interrupted registration");
    engine.reset(); type(engine,"/abc"); check(engine.inference_reading().empty(),"abbrev submitted for generation");
    engine.reset(); type(engine,"KaKu"); check(engine.inference_reading().empty(),"okuri submitted for generation");
}
void ipc_tests(const std::filesystem::path& server) {
    const char* phase = "initial connection";
    try {
    const auto name = token(); const auto process = launch_dummy_server(server,name,250,1000);
    NativeHandle cancel(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    InferenceResponse first;
    const auto ready = GetTickCount64()+5000;
    for (;;) {
        try { first = inference_exchange(name,{1,U"みかん",U"前",U"後",5},GetTickCount64()+1500,cancel.get()); break; }
        catch (const InferenceFailure& f) { if (f.status != InferenceStatus::Unavailable || GetTickCount64() >= ready) throw; Sleep(10); }
    }
    check(first.candidates.front() == U"試験〈みかん〉","real pipe response");
    phase = "concurrent connections";
    std::vector<std::future<InferenceResponse>> peers;
    for (std::uint64_t i=2;i<8;++i) peers.push_back(std::async(std::launch::async,[&,i] { return inference_exchange(name,{i,U"同時",U"",U"",1},GetTickCount64()+3000,cancel.get()); }));
    for (std::size_t i=0;i<peers.size();++i) check(peers[i].get().id == i+2,"concurrent response mixed");
    const auto start = GetTickCount64();
    phase = "cancellation";
    auto cancelled = std::async(std::launch::async,[&] { try { inference_exchange(name,{9,U"取消",U"",U"",1},GetTickCount64()+1500,cancel.get()); } catch (const InferenceFailure& f) { return f.status; } return InferenceStatus::Ok; });
    Sleep(30); SetEvent(cancel.get()); check(cancelled.get() == InferenceStatus::Cancelled && GetTickCount64()-start < 800,"I/O cancellation stalled");
    ResetEvent(cancel.get());
    try { inference_exchange(name,{10,U"期限",U"",U"",1},GetTickCount64()+30,cancel.get()); check(false,"timeout accepted"); }
    catch (const InferenceFailure& f) { check(f.status == InferenceStatus::Timeout,"wrong timeout status"); }
    {
        phase = "client replacement";
        std::vector<InferenceCompletion> delivered; InferenceClient client({name,{},250,1500},[&](auto value) { delivered.push_back(std::move(value)); });
        const auto now = GetTickCount64(); client.submit({11,U"古い",U"",U"",1}); client.submit({12,U"新しい",U"",U"",1});
        check(GetTickCount64()-now < 100,"UI submission waited for server");
        const auto end = GetTickCount64()+3000;
        while (delivered.empty() && GetTickCount64()<end) { client.poll(); Sleep(5); }
        check(delivered.size()==1 && delivered[0].id==12 && delivered[0].status==InferenceStatus::Ok,"latest request did not win");
        client.submit({13,U"破棄",U"",U"",1}); client.cancel(); Sleep(300); client.poll(); check(delivered.size()==1,"cancelled callback delivered");
    }
    check(WaitForSingleObject(process.get(),5000)==WAIT_OBJECT_0,"dummy server failed to idle out");
    phase = "restart";
    DWORD exit = 1; GetExitCodeProcess(process.get(),&exit); check(exit==0,"dummy server failed");
    const auto restarted = launch_dummy_server(server,name,0,600);
    const auto restart_ready = GetTickCount64()+3000;
    for (;;) {
        try { check(inference_exchange(name,{14,U"再開",U"",U"",1},restart_ready,cancel.get()).id==14,"restart failed"); break; }
        catch (const InferenceFailure& failure) {
            if (failure.status!=InferenceStatus::Unavailable || GetTickCount64()>=restart_ready) throw;
            check(WaitForSingleObject(restarted.get(),0)==WAIT_TIMEOUT,"restarted server exited before readiness"); Sleep(10);
        }
    }
    phase = "idle-check boundary connections";
    for (std::uint64_t i=0;i<12;++i) {
        Sleep(90+static_cast<DWORD>(i%3)*10);
        check(inference_exchange(name,{100+i,U"接続",U"",U"",1},GetTickCount64()+1500,cancel.get()).id==100+i,"accept lost at idle-check boundary");
    }
    check(WaitForSingleObject(restarted.get(),3000)==WAIT_OBJECT_0,"restarted helper did not exit");
    phase = "launch with legacy idle fallback";
    {
        // A server from an older install rejects a long --idle-ms and exits with 2.
        const auto fresh = token(); std::vector<DWORD> launched;
        const InferenceLauncher launch = [&](DWORD idle) {
            launched.push_back(idle);
            if (idle <= inference_legacy_idle_ms) return launch_dummy_server(server,fresh,0,idle);
            auto command = L"\"" + server.native() + L"\" --dummy --pipe-token " + fresh + L" --idle-ms 99999999999";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
            check(CreateProcessW(server.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info) != 0,"cannot start legacy fixture");
            CloseHandle(info.hThread); return NativeHandle(info.hProcess);
        };
        check(inference_exchange_launching(fresh,{15,U"互換",U"",U"",1},GetTickCount64()+5000,cancel.get(),launch,inference_max_idle_ms).id==15 &&
            launched.size()==2 && launched[1]==inference_legacy_idle_ms,"legacy server was not relaunched with its idle limit");
        NativeHandle pipe(CreateFileW(inference_pipe_name(fresh).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_ANONYMOUS,nullptr));
        ULONG pid = 0;
        if (pipe.valid() && GetNamedPipeServerProcessId(pipe.get(),&pid)) {
            NativeHandle helper(OpenProcess(PROCESS_TERMINATE,FALSE,pid)); if (helper.valid()) TerminateProcess(helper.get(),0);
        }
    }
    } catch (const InferenceFailure& failure) {
        throw std::runtime_error(std::string(phase) + ": IPC status " + std::to_string(static_cast<int>(failure.status)));
    }
}
}
int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    try { status_trace_tests(); protocol_tests(); core_tests(); ipc_tests(argv[1]); std::cout<<"PASS status trace, protocol, core fallback, real pipes, concurrency, cancellation, deadlines and restart\n"; return 0; }
    catch (const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
