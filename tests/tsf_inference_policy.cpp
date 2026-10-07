#include "dictionary_wait.h"
#include "runtime_dictionary.h"
#include "tsf_test_host.h"
#include "inference_client.h"
#include "dictionary_settings.h"
#include "candidate_ui.h"
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
namespace nskkzime::windows { HINSTANCE module_instance=nullptr; std::atomic<long> module_objects{0},server_locks{0}; }
using namespace nskkzime;
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void messages() { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);} }
struct Session {
    ComPtr<test_host::Manager> host; ComPtr<test_host::Context> context, origin; ComPtr<ITfTextInputProcessorEx> service; ComPtr<ITfKeyEventSink> keys;
    explicit Session(const wchar_t* prefix=L"前") {
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context); context->doc->text=std::wstring(prefix)+L"後"; context->doc->caret=static_cast<LONG>(wcslen(prefix));
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))),"create"); service.As(&keys); check(SUCCEEDED(service->ActivateEx(host.Get(),1,0)),"activate"); wait_for_dictionary(nskkzime::windows::runtime_dictionary_ready);
    }
    ~Session() {service->Deactivate();context->pump();}
    void transitory() { origin = test_host::transitory_context(context.Get()); }
    void key(WPARAM code,bool shift=false) {
        BYTE state[256]{}; if(shift)state[VK_SHIFT]=state[VK_LSHIFT]=0x80; SetKeyboardState(state);
        const auto param=static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code),MAPVK_VK_TO_VSC))<<16;
        auto* input = origin ? origin.Get() : context.Get();
        BOOL eaten=FALSE; keys->OnTestKeyDown(input,code,param,&eaten); check(eaten,"preflight");
        keys->OnKeyDown(input,code,param,&eaten); check(eaten,"key"); keys->OnKeyUp(input,code,param,&eaten);
    }
    void type(std::string_view text) {for(const auto ch:text) {const auto vk=VkKeyScanExW(ch,GetKeyboardLayout(0));key(LOBYTE(vk),(HIBYTE(vk)&1)!=0);}}
    void start(std::string_view text="Kaitou") {type(text);const auto time=GetTickCount64();key(VK_SPACE);check(GetTickCount64()-time<150,"key blocked on scoring");}
    void queued() {const auto end=GetTickCount64()+2500;while(context->queued.empty()&&GetTickCount64()<end) {messages();Sleep(5);}check(!context->queued.empty(),"missing completion edit");}
    void complete() {queued();context->pump();}
    void wait(DWORD ms=350) {const auto end=GetTickCount64()+ms;do {messages();context->pump();Sleep(5);}while(GetTickCount64()<end);}
    ComPtr<CandidateList> list() {check(host->ui!=nullptr,"no candidate list");return ComPtr<CandidateList>(static_cast<CandidateList*>(host->ui.Get()));}
};
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    const auto id=learning_client_id();const std::wstring token(id.begin(),id.end());
    const auto root=std::filesystem::current_path()/("tsf-policy-"+id);std::filesystem::create_directory(root);
    const auto config=root/L"settings.ini", dictionary=root/L"dictionary.skk", user=root/L"user.nskkdb";
    const auto log_directory = root / L"input-logs";
    {std::ofstream file(dictionary,std::ios::binary);file<<"かいとう /回答;元一/解答;元二/解凍;元三/\nありがとうございます /元一;辞書/元二/\nかk /書;文章/描;絵/\n";}
    DictionarySettings settings{{dictionary},false};settings.zenz={true,root/L"fixture.gguf",argv[1],4,1500}; settings.zenz.policy.rerank=true;
    save_dictionary_settings(config,settings);
    SetEnvironmentVariableW(L"NSKKZIME_CONFIG",config.c_str());SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY",nullptr);
    SetEnvironmentVariableW(L"NSKKZIME_LEARNING",L"0");SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY",user.c_str());
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY",nullptr);SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TOKEN",token.c_str());
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TIMEOUT_MS",nullptr);
    std::mutex mutex;std::vector<InferenceRequest> requests;std::atomic<DWORD> delay=180;std::atomic<bool> empty=false;std::atomic<int> count=0;
    auto server=std::async(std::launch::async,[&]{return serve_inference(token,[&](const InferenceRequest& request,const std::function<bool()>& cancelled) {
        {const std::lock_guard lock(mutex);requests.push_back(request);}++count;
        const auto end=GetTickCount64()+delay;while(GetTickCount64()<end) {if(cancelled())throw std::runtime_error("cancelled");Sleep(5);}
        InferenceResponse response;response.id=request.id;response.operation=request.operation;
        if(request.operation==InferenceOperation::Score) {response.scores.assign(request.candidates.size(),-5.0f);if(response.scores.size()>1)response.scores[1]=0;}
        else if(!empty) {response.candidates={request.reading==U"みかん" ? U"蜜柑" : U"有難うございます",U"共通候補"};response.scores={0,-2};}
        return response;
    },"fixture",5000,3000);});
    BYTE previous[256]{};GetKeyboardState(previous);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int result=1;
    try {
        NativeHandle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr)); const auto ready=GetTickCount64()+3000;
        for(;;) try {inference_exchange(token,{1,U"あ"},ready,stop.get());break;} catch(const InferenceFailure& failure) {if(failure.status!=InferenceStatus::Unavailable||GetTickCount64()>=ready)throw;Sleep(10);}
        {Session s;s.start();check(s.context->doc->text==L"前回答後","dictionary was not shown immediately");s.complete();check(s.context->doc->text==L"前解答後","score promotion not applied");
         BSTR text=nullptr;s.list()->GetString(1,&text);check(text&&std::wstring_view(text)==L"回答","remaining dictionary order changed");SysFreeString(text);}
        {Session s;s.transitory();s.start();s.complete();
         check(s.context->doc->text==L"前解答後"&&s.origin->doc->text.empty(),"transitory score did not update the parent");
         const std::lock_guard lock(mutex);const auto& request=requests.back();
         check(request.left==U"前"&&request.right==U"後","transitory score lost full surrounding text");}
        {Session s;s.transitory();s.start();s.queued();s.context->move_selection(0,0);s.context->pump();s.wait(50);
         check(s.context->doc->text==L"前回答後"&&!s.host->ui,"parent selection change applied a stale score");}
        {Session s(L"絵を");s.type("KaKu");check(s.context->doc->text==L"絵を書く後","initial okuri");s.complete();check(s.context->doc->text==L"絵を描く後","okuri score not applied");
         const std::lock_guard lock(mutex);const auto& request=requests.back();check(request.reading==U"かく"&&request.candidates==std::vector<std::u32string>{U"書く",U"描く"}&&request.left==U"絵を"&&request.right==U"後","okuri or surrounding context mixed");}
        {Session s;s.start("Arigatougozaimasu");check(s.context->doc->text==L"前元一後","long reading blocked dictionary");s.complete();
         UINT size=0;s.list()->GetCount(&size);check(s.context->doc->text==L"前有難うございます後"&&size==4,"long candidates were not prepended");}
        for(const auto key:std::initializer_list<WPARAM>{VK_SPACE,VK_NEXT,VK_PRIOR,'2','7',VK_ESCAPE}) {
            Session s;s.start();s.queued();s.key(static_cast<WPARAM>(key));const auto text=s.context->doc->text;s.context->pump();s.wait(50);check(s.context->doc->text==text,"late scoring reordered operated candidates");
        }
        {Session s;s.start();s.queued();auto list=s.list();list->begin_interaction();s.context->pump();check(s.context->doc->text==L"前回答後","pointer down failed to freeze order");
         list->request_selection(2);s.context->pump();check(s.context->doc->text==L"前解凍後","pointer freeze prevented click commit");}
        {Session s;s.start("Arigatougozaimasu");s.queued();s.key(VK_SPACE);s.context->pump();check(s.context->doc->text==L"前元二後","long generation ignored candidate navigation");
         const auto before=count.load();s.key(VK_SPACE);s.wait();check(count==before,"long generation retried at exhaustion");}
        {Session s;s.start();s.queued();s.context->host_replace(0,1,L"別");s.context->pump();check(s.context->doc->text==L"別回答後","score used stale context");}
        {Session s;s.start();s.queued();s.context->doc->reject_write=true;s.context->pump();check(s.context->doc->text==L"前回答後","failed edit adopted score");
         s.context->doc->reject_write=false;s.key(VK_SPACE);check(s.context->doc->text==L"前解答後","failed score damaged original order");}
        for(const bool long_reading:{false,true}) {
            delay=450;Session s;s.start(long_reading ? "Arigatougozaimasu" : "Kaitou");
            if(long_reading) {s.queued();Sleep(1550);s.context->pump();} else s.complete();
            check(s.context->doc->text==(long_reading ? L"前元一後" : L"前回答後"),"late result replaced dictionary fallback");delay=180;
        }
        empty=true;{Session s;s.start("Arigatougozaimasu");s.complete();check(s.context->doc->text==L"前元一後","empty long result lost dictionary");}empty=false;
        {Session s;const auto before=count.load();s.context->input_scope=IS_PRIVATE;s.start();s.wait();check(count==before&&s.context->doc->text==L"前回答後","private field scored");}
        // Logging is opt-in per client, even when the inference server is shared.
        std::filesystem::create_directory(log_directory);
        settings.zenz.log_input = true; settings.zenz.log_directory = log_directory; save_dictionary_settings(config, settings);
        auto log_files = [&] {
            std::vector<std::filesystem::path> paths;
            for (const auto& item : std::filesystem::directory_iterator(log_directory))
                if (item.path().extension() == L".jsonl") paths.push_back(item.path());
            return paths;
        };
        {Session s(L"ログ前😀"); s.start(); s.complete();
         const auto until = GetTickCount64() + 2000;
         while (inference_status().find(L"保存中") == std::wstring::npos && GetTickCount64() < until) s.wait(10);
         check(inference_status().find(L"保存中") != std::wstring::npos && log_files().size() == 1, "validated response was not logged");
         std::ifstream input(log_files()[0], std::ios::binary); const std::string bytes((std::istreambuf_iterator<char>(input)), {});
         check(bytes.find("ログ前😀") != std::string::npos && bytes.find("かいとう") != std::string::npos && bytes.find("回答") != std::string::npos,
               "diagnostic log lost reading/context/score candidates");}
        for (const auto scope : {IS_PASSWORD, IS_PRIVATE, IS_NUMERIC_PIN}) {
            Session s(L"保存禁止"); s.context->input_scope = scope; s.start(); s.wait();
        }
        for (const bool unknown : {false,true}) {
            Session s(L"親文書保護");s.transitory();s.context->unknown_scope=unknown;
            if (!unknown) s.context->input_scope=IS_PRIVATE;
            const auto before=count.load();s.start();
            if (unknown) {
                s.complete();
                check(count==before+1&&s.context->doc->text==L"親文書保護解答後","unknown parent blocked local scoring");
                const std::lock_guard lock(mutex);const auto& request=requests.back();
                check(request.left.empty()&&request.right.empty(),"unknown parent exposed surrounding text");
            } else { s.wait();check(count==before,"private parent was sent to inference"); }
        }
        {Session s(L"親文書途中保護");s.transitory();s.start();s.queued();
         s.context->input_scope=IS_PRIVATE;s.context->notify();s.context->pump();s.wait(50);
         check(s.context->doc->text==L"親文書途中保護回答後","private parent accepted a late response");}
        for (const auto status : {S_OK, S_FALSE, E_FAIL}) {
            Session s(L"属性不明");s.context->unknown_scope=status==S_OK;s.context->scope_value_status=status;
            s.start("Mikan");s.complete();
            check(s.context->doc->text==L"属性不明蜜柑後","missing or failed input scope blocked fallback generation");
            const std::lock_guard lock(mutex);const auto& request=requests.back();
            check(request.reading==U"みかん"&&request.left.empty()&&request.right.empty(),"unknown field exposed context instead of reading only");
        }
        {Session s(L"途中保護"); s.start(); s.context->input_scope = IS_PRIVATE; s.context->notify(); s.wait();}
        {Session s(L"取消応答"); s.start(); s.queued(); s.key(VK_ESCAPE); s.context->pump(); s.wait(50);}
        {Session s(L"編集拒否"); s.start(); s.queued(); s.context->doc->reject_write = true; s.context->pump(); s.wait(50);}
        {Session s(L"適用直後"); s.start(); s.queued();
         s.context->scope_query = [&](ITfRange*, InputScope& scope) {
             if (s.context->doc->text.find(L"解答") != std::wstring::npos) scope = IS_PRIVATE;
             return S_OK;
         };
         s.context->pump(); s.wait(50);}
        check(log_files().size() == 1, "protected, cancelled or rejected input was logged");
        settings.zenz.log_input = false; save_dictionary_settings(config, settings);
        {Session s(L"無効化後"); s.start(); s.complete(); s.wait(50);}
        check(log_files().size() == 1, "new activation ignored input log opt-out");
        // Successful commit learns a short fallback only when explicitly enabled.
        settings.learning=true;settings.zenz.policy.learn_fallback=true;save_dictionary_settings(config,settings);SetEnvironmentVariableW(L"NSKKZIME_LEARNING",L"1");
        {Session s;s.context->unknown_scope=true;s.start("Mikan");s.complete();s.key(VK_RETURN);s.wait(250);
         check(!std::filesystem::exists(user)||read_user_dictionary(user).lookup({U"みかん"}).empty(),"unknown field persisted a generated candidate");}
        {Session s;s.start("Mikan");s.complete();check(s.context->doc->text==L"前蜜柑後","short fallback missing");s.key(VK_RETURN);
         const auto until=GetTickCount64()+4000;bool learned=false;do {s.wait(30);try {learned=!read_user_dictionary(user).lookup({U"みかん"}).empty();} catch(...) {} }while(!learned&&GetTickCount64()<until);check(learned,"short fallback did not persist");}
        {Session s;s.start("Arigatougozaimasu");s.complete();s.key(VK_RETURN);s.wait(250);check(read_user_dictionary(user).lookup({U"ありがとうございます"}).empty(),"long generation polluted dictionary");}
        check(module_objects==0,"policy retained COM objects");std::cout<<"PASS asynchronous promote/long priority, okuri context, key/pointer freeze, deadlines, failed edits, privacy, opt-in logging and learning\n";result=0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';}
    SetKeyboardState(previous);CoUninitialize();try {server.get();}catch(...) {result=1;}Sleep(11000);
    for(const auto& path:{config,dictionary,user,std::filesystem::path(user.native()+L".lock")}) {std::error_code ignored;std::filesystem::remove(path,ignored);}
    if (std::filesystem::exists(log_directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(log_directory)) { std::error_code ignored; std::filesystem::remove(entry.path(), ignored); }
        std::error_code ignored; std::filesystem::remove(log_directory, ignored);
    }
    std::error_code ignored;std::filesystem::remove(root,ignored);return result;
}
