#include "dictionary_wait.h"
#include "runtime_dictionary.h"
#include "tsf_test_host.h"
#include "inference_client.h"
#include "candidate_ui.h"
#include <iostream>
#include <stdexcept>
namespace nskkzime::windows { HINSTANCE module_instance=nullptr; std::atomic<long> module_objects{0},server_locks{0}; }
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void messages() { MSG msg{}; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
struct Session {
    ComPtr<test_host::Manager> host; ComPtr<test_host::Context> context; ComPtr<ITfTextInputProcessorEx> service; ComPtr<ITfKeyEventSink> keys;
    Session() {
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context); context->doc->text=L"前後"; context->doc->caret=1;
        check(SUCCEEDED(create_text_service(IID_PPV_ARGS(&service))),"create"); service.As(&keys); check(SUCCEEDED(service->ActivateEx(host.Get(),1,0)),"activate"); wait_for_dictionary(nskkzime::windows::runtime_dictionary_ready);
    }
    ~Session() { service->Deactivate(); context->pump(); }
    void key(WPARAM code,bool shift=false) {
        BYTE state[256]{}; if (shift) state[VK_SHIFT]=state[VK_LSHIFT]=0x80; SetKeyboardState(state);
        const auto param=static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code),MAPVK_VK_TO_VSC))<<16;
        BOOL eaten=FALSE; keys->OnTestKeyDown(context.Get(),code,param,&eaten); check(eaten,"preflight");
        keys->OnKeyDown(context.Get(),code,param,&eaten); check(eaten,"key"); keys->OnKeyUp(context.Get(),code,param,&eaten);
    }
    void type(std::string_view text) { for (const auto ch:text) { const auto vk=VkKeyScanExW(ch,GetKeyboardLayout(0)); key(LOBYTE(vk),(HIBYTE(vk)&1)!=0); } }
    void start() { type("Mikan"); const auto now=GetTickCount64(); key(VK_SPACE); check(GetTickCount64()-now<200,"key waited for inference"); }
    void wait(DWORD duration=600,bool edit=true) { const auto end=GetTickCount64()+duration; do { messages(); if(edit)context->pump(); Sleep(5); } while(GetTickCount64()<end); }
    void queued() { const auto end=GetTickCount64()+2500; while(context->queued.empty()&&GetTickCount64()<end) { messages(); Sleep(5); } check(!context->queued.empty(),"no completion edit"); }
    bool registration() { GUID id{}; return host->ui&&SUCCEEDED(host->ui->GetGUID(&id))&&id==registration_ui_guid; }
};
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    const auto raw_id=learning_client_id(); const std::wstring id(raw_id.begin(),raw_id.end());
    const auto root=std::filesystem::current_path()/("tsf-inference-"+raw_id); std::filesystem::create_directory(root); const auto file=root/L"user.nskkdb";
    SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY",argv[2]); SetEnvironmentVariableW(L"NSKKZIME_LEARNING",L"1"); SetEnvironmentVariableW(L"NSKKZIME_USER_DICTIONARY",file.c_str());
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_DUMMY",L"1"); SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TOKEN",id.c_str());
    SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TIMEOUT_MS",L"1500");
    BYTE previous[256]{}; GetKeyboardState(previous); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int result=1;
    try {
        const auto process=launch_dummy_server(argv[1],id,250,2000); Sleep(100);
        { Session s; s.start(); check(!s.registration()&&s.context->doc->text==L"前みかん後","request changed reading or entered registration early");
          s.queued(); s.context->pump(); check(s.context->doc->text==L"前試験〈みかん〉後","response not applied");
          s.key(VK_RETURN); check(!s.host->ui,"commit retained UI"); }
        { Session s; s.start(); s.key(VK_ESCAPE); s.wait(); check(s.context->doc->text==L"前後"&&!s.host->ui,"cancelled response returned"); }
        { Session s; s.start(); s.type("a"); s.wait(); check(s.context->doc->text==L"前みかんあ後","continued input overwritten"); }
        { Session s; s.start(); s.context->host_replace(0,1,L"別"); s.context->host_replace(0,1,L"前"); s.wait(); check(s.context->doc->text==L"前みかん後","changed-and-restored context accepted old response"); }
        { Session s; s.start(); auto old=s.context; s.context.Attach(new test_host::Context); s.type("Ka"); s.wait(); old->pump(); check(s.context->doc->text==L"か"&&old->doc->text==L"前みかん後","response mixed fields"); }
        { Session s; s.start(); s.queued(); s.key(VK_ESCAPE); s.context->pump(); check(s.context->doc->text==L"前後","queued edit ignored cancellation"); }
        { Session s; s.start(); s.queued(); s.context->readonly=true; s.context->pump(); check(s.context->doc->text==L"前みかん後","response edited readonly field"); }
        { Session s; s.start(); s.queued(); s.context->doc->reject_write=true; s.context->pump();
          check(s.context->doc->text==L"前みかん後"&&!s.host->ui,"rejected write adopted candidates");
          check(inference_status().find(L"反映できません")!=std::wstring::npos,"rejected write reported success");
          s.context->doc->reject_write=false; s.type("a"); check(s.context->doc->text==L"前みかんあ後","failed response damaged input state"); }
        { Session s; s.start(); s.context->input_scope=IS_PRIVATE; s.context->notify(); s.wait(); check(s.context->doc->text==L"前みかん後","protected context accepted response"); }
        { Session s; s.context->input_scope=IS_PRIVATE; s.start(); s.wait(); check(s.context->doc->text==L"前みかん後"&&!s.registration(),"private field submitted"); }
        { Session s; s.context->unknown_scope=true; s.start(); s.queued(); s.context->pump();
          check(s.context->doc->text==L"前試験〈みかん〉後","unknown field blocked reading-only conversion"); }
        { Session s; s.context->unknown_scope=true; s.start(); s.queued();
          s.context->unknown_scope=false; s.context->input_scope=IS_PRIVATE; s.context->notify(); s.context->pump();
          check(s.context->doc->text==L"前みかん後","newly protected field accepted reading-only response"); }
        { Session s; s.start(); s.queued(); Sleep(1600); s.context->pump(); check(s.context->doc->text==L"前みかん後"&&s.registration(),"late edit did not fall back to registration"); }
        { Session s; s.start(); s.key(VK_SPACE); s.wait(); check(s.registration()&&s.context->doc->text==L"前みかん後","second Space did not interrupt wait"); }
        { Session s; s.type("Kanji"); s.key(VK_SPACE); s.key(VK_SPACE); s.key(VK_SPACE); s.queued(); s.context->pump();
          check(s.context->doc->text==L"前試験〈かんじ〉後","exhaustion generation failed"); auto list=static_cast<CandidateList*>(s.host->ui.Get()); UINT count=0; list->GetCount(&count); check(count==4,"dictionary candidates lost");
          s.key(VK_SPACE); s.key(VK_SPACE); check(s.registration(),"generated exhaustion requested again");
          s.wait(); check(s.registration(),"late response replaced exhausted registration"); }
        { Session s; s.start(); s.queued(); s.service->Deactivate(); s.context->pump(); check(s.context->doc->text==L"前みかん後","deactivated response returned"); }
        SetEnvironmentVariableW(L"NSKKZIME_INFERENCE_TIMEOUT_MS",L"25");
        { Session s; s.start(); s.wait(); check(s.registration()&&s.context->doc->text==L"前みかん後","timeout fallback failed"); }
        check(module_objects==0,"inference retained COM objects");
        check(!std::filesystem::exists(file)||nskkzime::windows::read_user_dictionary(file).revision()==0,"dummy text was learned");
        check(WaitForSingleObject(process.get(),6000)==WAIT_OBJECT_0,"helper did not exit");
        std::cout<<"PASS asynchronous TSF generation, stale edits, context changes, protected fields, fallback and no learning\n"; result=0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; }
    SetKeyboardState(previous); CoUninitialize(); Sleep(12000);
    for(const auto& path:{file,std::filesystem::path(file.native()+L".lock")}) { std::error_code ignored; std::filesystem::remove(path,ignored); }
    std::error_code ignored; std::filesystem::remove(root,ignored); return result;
}
