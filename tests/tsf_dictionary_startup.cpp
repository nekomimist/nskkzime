#include "module.h"
#include "tsf_test_host.h"
#include "dictionary_wait.h"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace nskkzime::windows;
namespace {
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
bool key(ITfKeyEventSink* sink,ITfContext* context,WPARAM code,bool shift=false) {
    BYTE keyboard[256]{};if(shift)keyboard[VK_SHIFT]=keyboard[VK_LSHIFT]=0x80;SetKeyboardState(keyboard);
    const auto param=static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code),MAPVK_VK_TO_VSC))<<16;
    BOOL eaten=FALSE;sink->OnTestKeyDown(context,code,param,&eaten);
    if(eaten)sink->OnKeyDown(context,code,param,&eaten);
    BOOL up=FALSE;sink->OnKeyUp(context,code,param,&up);return eaten!=FALSE;
}
void type(ITfKeyEventSink* sink,ITfContext* context,std::string_view input) {
    for(auto ch:input){const auto vk=VkKeyScanExW(ch,GetKeyboardLayout(0));check(key(sink,context,LOBYTE(vk),(HIBYTE(vk)&1)!=0),"key not consumed");}
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 && !(argc==3 && std::wstring_view(argv[2])==L"--cache"))return 2;
    const bool cached=argc==3;
    const auto file=std::filesystem::current_path()/(L"startup-dictionary-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".skk");
    {std::ofstream out(file,std::ios::binary);out<<";; coding: utf-8\nかんじ /漢字/\n";
     for(int i=0;i<200000;++i)out<<"entry"<<i<<" /試験;入力確認/\n";
     check(static_cast<bool>(out),"cannot write startup fixture");}
    SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY",file.c_str());SetEnvironmentVariableW(L"NSKKZIME_LEARNING",L"0");
    const auto cache=std::filesystem::path(file.native()+L".cache");
    if(cached)SetEnvironmentVariableW(L"NSKKZIME_DICTIONARY_CACHE",cache.c_str());
    BYTE previous[256]{};GetKeyboardState(previous);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    HMODULE library=LoadLibraryW(argv[1]);int result=1;
    try {
        check(library!=nullptr,"DLL load failed");
        using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);using Ready=HRESULT(WINAPI*)(BOOL*);using Unload=HRESULT(WINAPI*)();using Status=HRESULT(WINAPI*)(BSTR*);
#pragma warning(push)
#pragma warning(disable:4191)
        const auto create=reinterpret_cast<Factory>(GetProcAddress(library,"DllGetClassObject"));
        const auto ready=reinterpret_cast<Ready>(GetProcAddress(library,"NskkzimeIsDictionaryReady"));
        const auto unload=reinterpret_cast<Unload>(GetProcAddress(library,"DllCanUnloadNow"));
        const auto status=reinterpret_cast<Status>(GetProcAddress(library,"NskkzimeGetDictionaryStatus"));
#pragma warning(pop)
        check(create&&ready&&unload,"missing exports");check(ready(nullptr)==E_POINTER,"readiness null output accepted");
        const auto is_ready=[&]{BOOL value=FALSE;check(SUCCEEDED(ready(&value)),"readiness failed");return value!=FALSE;};
        const auto status_contains=[&](std::wstring_view expected){
            BSTR value=nullptr;check(status && SUCCEEDED(status(&value)),"status failed");
            const bool found=value && std::wstring_view(value).find(expected)!=std::wstring_view::npos;SysFreeString(value);return found;
        };
        {
            ComPtr<IClassFactory> factory;check(SUCCEEDED(create(service_clsid,IID_PPV_ARGS(&factory))),"factory");
            ComPtr<ITfTextInputProcessorEx> service;check(SUCCEEDED(factory->CreateInstance(nullptr,IID_PPV_ARGS(&service))),"service");
            ComPtr<ITfKeyEventSink> keys;service.As(&keys);ComPtr<test_host::Manager> host;host.Attach(new test_host::Manager);
            const auto start=std::chrono::steady_clock::now();check(SUCCEEDED(service->ActivateEx(host.Get(),1,0)),"activate");
            const auto activation_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            check(!is_ready(),"large dictionary was loaded synchronously during activation");
            ComPtr<test_host::Context> context;context.Attach(new test_host::Context);
            type(keys.Get(),context.Get(),"kanaKanji");key(keys.Get(),context.Get(),VK_SPACE);
            check(context->doc->text==L"かなかんじ" || context->doc->text==L"かな漢字","startup dropped input");
            const auto before=context->doc->text;
            if(!is_ready())check(before==L"かなかんじ" && !host->ui,"loading dictionary opened registration or candidates");
            wait_for_dictionary(is_ready);
            if(cached)check(status_contains(L"キャッシュ作成"),"cold cache was not created by the DLL");
            check(context->doc->text==before,"load completion changed composition without a key");
            if(before==L"かなかんじ")key(keys.Get(),context.Get(),VK_SPACE);
            check(context->doc->text==L"かな漢字","retry did not use loaded dictionary");
            key(keys.Get(),context.Get(),VK_RETURN);
            std::cout<<"PASS 200001 entries: ActivateEx="<<activation_ms<<"ms, loaded after "
                <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"ms; kana/reading/retry\n";
            check(SUCCEEDED(service->Deactivate()),"deactivate");context->pump();
        }
        if(cached) {
            ComPtr<IClassFactory> factory;check(SUCCEEDED(create(service_clsid,IID_PPV_ARGS(&factory))),"cached factory");
            ComPtr<ITfTextInputProcessorEx> service;check(SUCCEEDED(factory->CreateInstance(nullptr,IID_PPV_ARGS(&service))),"cached service");
            ComPtr<ITfKeyEventSink> keys;service.As(&keys);ComPtr<test_host::Manager> host;host.Attach(new test_host::Manager);
            const auto start=std::chrono::steady_clock::now();check(SUCCEEDED(service->ActivateEx(host.Get(),1,0)),"cached activate");
            const auto activation_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            wait_for_dictionary(is_ready);
            check(status_contains(L"永続キャッシュ") || status_contains(L"共有キャッシュ"),"DLL did not reuse the compiled dictionary");
            ComPtr<test_host::Context> context;context.Attach(new test_host::Context);
            type(keys.Get(),context.Get(),"Kanji");key(keys.Get(),context.Get(),VK_SPACE);key(keys.Get(),context.Get(),VK_RETURN);
            check(context->doc->text==L"漢字","cached conversion changed candidate text");
            std::cout<<"PASS cache reuse: ActivateEx="<<activation_ms<<"ms, ready/converted after "
                <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"ms\n";
            check(SUCCEEDED(service->Deactivate()),"cached deactivate");context->pump();
        }
        // A fresh load after releasing the last snapshot must also survive
        // immediate COM teardown and the caller releasing its DLL reference.
        {
            ComPtr<IClassFactory> factory;check(SUCCEEDED(create(service_clsid,IID_PPV_ARGS(&factory))),"second factory");
            ComPtr<ITfTextInputProcessorEx> service;check(SUCCEEDED(factory->CreateInstance(nullptr,IID_PPV_ARGS(&service))),"second service");
            ComPtr<test_host::Manager> host;host.Attach(new test_host::Manager);
            check(SUCCEEDED(service->ActivateEx(host.Get(),1,0)),"second activate");
            if(!cached)check(!is_ready(),"second load did not run asynchronously");
            check(SUCCEEDED(service->Deactivate()),"early deactivate");
        }
        check(unload()==S_OK,"COM teardown leaked objects");
        FreeLibrary(library);library=nullptr;
        wait_for_dictionary([] {return GetModuleHandleW(L"nskkzime_tsf.dll")==nullptr;});
        std::cout<<"PASS background loader retained DLL until safe unload\n";result=0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';}
    // A failed assertion can leave test-host subscriptions alive until exit.
    if(!result && library)FreeLibrary(library);
    SetKeyboardState(previous);CoUninitialize();std::error_code ignored;std::filesystem::remove(file,ignored);
    if(cached) {
        for(std::filesystem::directory_iterator it(cache,ignored),end;!ignored&&it!=end;it.increment(ignored)) {
            const auto name=it->path().filename().native();
            if(name==L"build.lock" || (name.size()==73 && name.ends_with(L"-v1.bin")))std::filesystem::remove(it->path(),ignored);
        }
        std::filesystem::remove(cache,ignored);
    }
    return result;
}
