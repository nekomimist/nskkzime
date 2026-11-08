#include "dictionary_wait.h"
#include "module.h"
#include "tsf_test_host.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using namespace nskkzime::windows;
namespace {
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
struct Session {
    ComPtr<ITfTextInputProcessorEx> service; ComPtr<ITfKeyEventSink> keys;
    ComPtr<test_host::Manager> host; ComPtr<test_host::Context> context;
    explicit Session(IClassFactory* factory) {
        check(SUCCEEDED(factory->CreateInstance(nullptr,IID_PPV_ARGS(&service))),"create"); service.As(&keys);
        host.Attach(new test_host::Manager); context.Attach(new test_host::Context);
        check(SUCCEEDED(service->ActivateEx(host.Get(),1,0)),"activate"); wait_for_dictionary();
    }
    ~Session() { service->Deactivate(); context->pump(); }
    bool key(WPARAM code,bool shift=false,bool control=false) {
        BYTE keyboard[256]{}; if(shift)keyboard[VK_SHIFT]=keyboard[VK_LSHIFT]=0x80; SetKeyboardState(keyboard);
        if(control) { keyboard[VK_CONTROL]=keyboard[VK_LCONTROL]=0x80; SetKeyboardState(keyboard); }
        const auto param=static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(code),MAPVK_VK_TO_VSC))<<16;
        BOOL eaten=FALSE; keys->OnTestKeyDown(context.Get(),code,param,&eaten);
        if(eaten) keys->OnKeyDown(context.Get(),code,param,&eaten);
        BOOL up=FALSE; keys->OnKeyUp(context.Get(),code,param,&up); return eaten!=FALSE;
    }
    void type(std::string_view input) { for(auto ch:input) { const auto vk=VkKeyScanExW(ch,GetKeyboardLayout(0)); check(key(LOBYTE(vk),(HIBYTE(vk)&1)!=0),"key not handled"); } }
};
std::string path_text(const std::filesystem::path& path) { const auto text=path.u8string(); return {text.begin(),text.end()}; }
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=4)return 2;
    const auto root=std::filesystem::current_path()/(L"tsf-romaji-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root); const auto config=root/L"settings.ini", rules=root/L"rules.tsv";
    std::filesystem::copy_file(argv[3],rules);
    {std::ofstream out(config,std::ios::binary); out<<"version=1\nlearning=off\ndictionary="<<path_text(argv[2])<<"\nromaji="<<path_text(rules)<<'\n';}
    SetEnvironmentVariableW(L"NSKKZIME_CONFIG",config.c_str());
    BYTE keyboard[256]{}; GetKeyboardState(keyboard); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const auto library=LoadLibraryW(argv[1]); int result=1;
    try {
        check(library!=nullptr,"DLL load");
        using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
        using Status=HRESULT(WINAPI*)(BSTR*);
#pragma warning(push)
#pragma warning(disable:4191)
        const auto create=reinterpret_cast<Factory>(GetProcAddress(library,"DllGetClassObject"));
        const auto status=reinterpret_cast<Status>(GetProcAddress(library,"NskkzimeGetDictionaryStatus"));
#pragma warning(pop)
        ComPtr<IClassFactory> factory; check(create&&SUCCEEDED(create(service_clsid,IID_PPV_ARGS(&factory))),"factory");
        {
            Session s(factory.Get()); s.type("wiqwiq"); check(s.context->doc->text==L"ゐヰ","custom modes");
            s.type("w"); s.context->doc->reject_write=true; check(!s.key('I'),"failed custom write eaten"); s.context->doc->reject_write=false;
            s.type("i"); check(s.context->doc->text==L"ゐヰゐ","failed edit changed pending rules");
            s.type("z"); check(s.key(VK_SPACE)&&s.context->doc->text==L"ゐヰゐ　","z Space did not reach rule");
            s.type("KAnji"); s.key(VK_SPACE); s.key(VK_RETURN); check(s.context->doc->text==L"ゐヰゐ　漢字","shift or dictionary conversion regressed");
            std::filesystem::remove(rules); // Active engines must never reread a rule file during input.
            s.type("wi"); check(s.context->doc->text.ends_with(L"漢字ゐ"),"active rules reread removed file");
            Session fallback(factory.Get()); fallback.type("wi"); check(fallback.context->doc->text==L"うぃ","missing rules broke default input");
            BSTR text=nullptr; check(status&&SUCCEEDED(status(&text)),"status unavailable");
            const bool shown=text&&std::wstring_view(text).find(L"読み込み失敗。内蔵ルール")!=std::wstring_view::npos; SysFreeString(text); check(shown,"rule failure hidden");
            s.type("wi"); check(s.context->doc->text.ends_with(L"ゐゐ"),"another service changed active rules");
        }
        for (const auto keyboard_name : {"jp106", "us101"}) {
            { std::ofstream out(rules,std::ios::binary); out << "version=1\nwi\tゐ\n"; }
            { std::ofstream out(config,std::ios::binary); out << "version=1\nlearning=off\ninput-method=azik\nazik-keyboard=" << keyboard_name << "\ndictionary=" << path_text(argv[2]) << "\nromaji=" << path_text(rules) << '\n'; }
            Session azik(factory.Get());
            check(!azik.key('D',false,true) && !azik.key(VK_DELETE), "idle deletion shortcuts consumed");
            azik.type("q"); check(azik.context->doc->text == L"ん", "AZIK q became a mode command");
            azik.type("Kzji"); azik.key(VK_SPACE);
            check(azik.context->doc->text == L"ん漢字", "AZIK dictionary conversion failed");
            check(azik.key('D',false,true), "Ctrl+D did not open deletion");
            check(azik.key('D',false,true) && azik.context->doc->text == L"ん漢字", "repeat changed deletion target/text");
            azik.key(VK_ESCAPE);
            check(azik.key(VK_DELETE) && azik.context->doc->text == L"ん漢字", "Delete did not open deletion");
            azik.key(VK_ESCAPE); azik.type("Xa"); azik.key(VK_RETURN);
            check(azik.context->doc->text == L"ん漢字しゃ", "AZIK X still deleted a candidate");
            azik.type(std::string_view(keyboard_name) == "jp106" ? "@vkvl@" : "[vkvl[");
            check(azik.context->doc->text.ends_with(L"ヴィンヴォン"), "AZIK layout/kana commands failed");
            std::filesystem::remove(rules);
            Session fallback(factory.Get()); fallback.type("q"); check(fallback.context->doc->text == L"ん", "missing overlay reverted AZIK commands");
            for (const auto shortcut : {0, 1, 2}) {
                Session modifiers(factory.Get()); modifiers.type("Kzji"); modifiers.key(VK_SPACE);
                const bool eaten = shortcut == 0 ? modifiers.key('D',true,true) :
                    shortcut == 1 ? modifiers.key(VK_DELETE,true) : modifiers.key(VK_DELETE,false,true);
                check(!eaten && modifiers.context->doc->text == L"漢字", "modified deletion became a dictionary command");
            }
        }
        std::cout<<"PASS saved SKK/AZIK profiles, JIS/US, deletion keys, transactional edits, snapshots and fallback\n"; result=0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';}
    if(library)FreeLibrary(library); CoUninitialize(); SetKeyboardState(keyboard);
    std::error_code ignored; std::filesystem::remove_all(root,ignored);
    return result;
}
