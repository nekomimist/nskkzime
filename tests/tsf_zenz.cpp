#include "dictionary_wait.h"
#include "module.h"
#include "tsf_test_host.h"
#include "inference_client.h"
#include "dictionary_settings.h"
#include <fstream>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace nskkzime::windows;
namespace {
void check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
void messages() { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
struct Session {
    ComPtr<ITfTextInputProcessorEx> service; ComPtr<ITfKeyEventSink> keys;
    ComPtr<test_host::Manager> host; ComPtr<test_host::Context> context;
    Session(IClassFactory* factory,const wchar_t* prefix) {
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        context->doc->text=prefix; context->doc->caret=static_cast<LONG>(context->doc->text.size());
        check(SUCCEEDED(factory->CreateInstance(nullptr,IID_PPV_ARGS(&service))),"create"); service.As(&keys);
        check(SUCCEEDED(service->ActivateEx(host.Get(),1,0)),"activate"); wait_for_dictionary();
    }
    ~Session() { service->Deactivate(); context->pump(); }
    void key(WPARAM code,bool shift=false) {
        BYTE keyboard[256]{}; if(shift)keyboard[VK_SHIFT]=keyboard[VK_LSHIFT]=0x80; SetKeyboardState(keyboard);
        const auto param=static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code),MAPVK_VK_TO_VSC))<<16;
        BOOL eaten=FALSE; keys->OnTestKeyDown(context.Get(),code,param,&eaten); check(eaten,"preflight");
        keys->OnKeyDown(context.Get(),code,param,&eaten); check(eaten,"key"); keys->OnKeyUp(context.Get(),code,param,&eaten);
    }
    void type(std::string_view input) { for(auto ch:input) { const auto vk=VkKeyScanExW(ch,GetKeyboardLayout(0)); key(LOBYTE(vk),(HIBYTE(vk)&1)!=0); } }
    void start(std::string_view input="Kaitou") { type(input); const auto now=GetTickCount64(); key(VK_SPACE); check(GetTickCount64()-now<200,"key blocked on model"); }
    void queued() { const auto end=GetTickCount64()+5000; while(context->queued.empty()&&GetTickCount64()<end) { messages(); Sleep(5); } check(!context->queued.empty(),"no model completion edit"); }
    void complete() { queued(); context->pump(); }
    void wait() { const auto end=GetTickCount64()+350; do { messages(); context->pump(); Sleep(5); } while(GetTickCount64()<end); }
};
NativeHandle server_process(std::wstring_view token) {
    const auto name=inference_pipe_name(token);
    NativeHandle pipe(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_ANONYMOUS,nullptr));
    check(pipe.valid(),"cannot inspect isolated server"); ULONG pid=0;
    check(GetNamedPipeServerProcessId(pipe.get(),&pid)!=0,"server pid");
    NativeHandle process(OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,pid)); check(process.valid(),"server process"); return process;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=4)return 2;
    if(!std::filesystem::exists(argv[3]))return 77;
    const auto raw=learning_client_id(); const std::wstring token(raw.begin(),raw.end());
    const auto root=std::filesystem::current_path()/("tsf-zenz-"+raw); std::filesystem::create_directory(root);
    const auto file=root/L"設定.ini";
    const auto dictionary=root/L"辞書.skk";
    DictionarySettings settings; settings.learning=false; settings.zenz={true,argv[3],argv[2],4,2000};
    save_dictionary_settings(file,settings);
    SetEnvironmentVariableW(L"NSKKZIME_CONFIG",file.c_str()); SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY",nullptr);
    SetEnvironmentVariableW(L"NSKKZIME_LEARNING",L"0"); SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY",nullptr);
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TOKEN",nullptr); SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TIMEOUT_MS",nullptr);
    BYTE keyboard[256]{}; GetKeyboardState(keyboard); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const auto library=LoadLibraryW(argv[1]); int result=1;
    try {
        // Deterministic sharing keys change with the effective model/limits.
        const auto config=current_inference_config(); check(config&&config->model==settings.zenz.model,"saved zenz config ignored");
        check(current_inference_config()->token==config->token,"server key changed across activations");
        settings.zenz.threads=2; save_dictionary_settings(file,settings);
        check(current_inference_config()->token!=config->token,"incompatible settings reused server");
        settings.zenz.threads=4; save_dictionary_settings(file,settings);
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY",L"0"); check(!current_inference_config(),"disable override ignored");
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY",nullptr);
        // This unique test-only token owns every server launched below.
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TOKEN",token.c_str());
        check(library!=nullptr,"DLL load");
        using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
#pragma warning(push)
#pragma warning(disable:4191)
        const auto create=reinterpret_cast<Factory>(GetProcAddress(library,"DllGetClassObject"));
#pragma warning(pop)
        ComPtr<IClassFactory> factory; check(create&&SUCCEEDED(create(service_clsid,IID_PPV_ARGS(&factory))),"factory");
        { Session s(factory.Get(),L"試験問題の"), concurrent(factory.Get(),L"冷凍食品を電子レンジで");
          s.start(); concurrent.start(); s.complete(); concurrent.complete();
          check(concurrent.context->doc->text==L"冷凍食品を電子レンジで解凍","concurrent cold startup failed");
          check(s.context->doc->text==L"試験問題の解答","DLL did not launch/apply zenz"); s.key(VK_RETURN);
          s.type("desu"); check(s.context->doc->text==L"試験問題の解答です","input after generated commit"); }
        { Session a(factory.Get(),L"試験問題の"), b(factory.Get(),L"冷凍食品を電子レンジで");
          a.start(); b.start(); a.complete(); b.complete();
          check(a.context->doc->text==L"試験問題の解答"&&b.context->doc->text==L"冷凍食品を電子レンジで解凍","shared model mixed contexts"); }
        { Session s(factory.Get(),L"試験問題の"); s.start(); s.queued(); s.key(VK_ESCAPE); s.context->pump(); s.wait();
          check(s.context->doc->text==L"試験問題の","cancelled real result applied"); }
        { Session s(factory.Get(),L"試験問題の"); s.start(); s.queued(); s.context->host_replace(0,1,L"別"); s.context->pump();
          check(s.context->doc->text==L"別験問題のかいとう","stale real context applied"); }
        { Session s(factory.Get(),L"試験問題の"); s.context->input_scope=IS_PRIVATE; s.start(); s.wait();
          check(s.context->doc->text==L"試験問題のかいとう","private field inferred"); }
        { auto process=server_process(token); check(TerminateProcess(process.get(),99)!=0,"stop isolated helper");
          check(WaitForSingleObject(process.get(),3000)==WAIT_OBJECT_0,"helper did not stop"); }
        { Session s(factory.Get(),L"試験問題の"); s.start(); s.complete(); check(s.context->doc->text==L"試験問題の解答","server did not restart"); }
        {std::ofstream output(dictionary,std::ios::binary);output<<"かいとう /回答/解答/解凍/\nきょうはいいてんきですね /旧候補/\n";}
        settings.dictionaries={dictionary};settings.zenz.policy.rerank=true;settings.zenz.policy.score_timeout_ms=1000;save_dictionary_settings(file,settings);
        {Session s(factory.Get(),L"試験問題の");s.start();check(s.context->doc->text==L"試験問題の回答","dictionary was not immediately available");
         s.complete();check(s.context->doc->text==L"試験問題の解答","real model score did not promote best candidate");}
        {Session s(factory.Get(),L"");s.start("Kyouhaiitenkidesune");check(s.context->doc->text==L"旧候補","long dictionary candidate missing");
         s.complete();check(s.context->doc->text==L"今日はいい天気ですね","real long generation was not preferred");}
        auto process=server_process(token);
        // Broken optional model affects generation only; kana entry stays usable.
        settings.dictionaries.clear();settings.zenz.policy.rerank=false;
        settings.zenz.model=root/L"missing.gguf"; settings.zenz.timeout_ms=150; save_dictionary_settings(file,settings);
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TOKEN",nullptr);
        { Session s(factory.Get(),L""); s.start(); s.complete();
          // This fixture disables learning/registration, so fallback retains the reading.
          check(s.context->doc->text==L"かいとう"&&!s.host->ui,"missing model did not retain reading");
          s.key(VK_RETURN); s.type("desu"); check(s.context->doc->text==L"かいとうです","missing model broke subsequent kana input"); }
        check(WaitForSingleObject(process.get(),13000)==WAIT_OBJECT_0,"auto-launched server did not idle out");
        std::cout<<"PASS saved zenz configuration through DLL, nonblocking launch, shared contexts, commit, stale edits, privacy, restart and fallback\n"; result=0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';}
    if(library)FreeLibrary(library); CoUninitialize(); SetKeyboardState(keyboard);
    std::error_code ignored; std::filesystem::remove(file,ignored); std::filesystem::remove(dictionary,ignored); std::filesystem::remove(root,ignored); return result;
}
