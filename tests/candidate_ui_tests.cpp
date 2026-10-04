#include "candidate_ui.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace nskkzime::windows {
HINSTANCE module_instance = nullptr;
std::atomic<long> module_objects{0};
std::atomic<long> server_locks{0};
}
using namespace nskkzime;
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void type(Engine& engine, std::string_view text) { for (auto ch : text) engine.handle({KeyCode::Character, ch}); }
std::wstring text(ITfCandidateListUIElement* list, UINT index) {
    BSTR value = nullptr;
    check(list->GetString(index, &value) == S_OK, "candidate text unavailable");
    const std::wstring result(value, SysStringLen(value)); SysFreeString(value); return result;
}
}
int main() {
    try {
        {
            auto dictionary = std::make_shared<MemoryDictionary>();
            std::vector<Candidate> candidates;
            for (int i = 0; i < 16; ++i) candidates.push_back({std::to_string(i), U"候補" + ascii(std::to_string(i)), U"注釈"});
            dictionary->add({U"か", 'k'}, candidates);
            Engine engine(dictionary); type(engine, "KaKu");
            ComPtr<CandidateList> list; list.Attach(new CandidateList);
            list->update(engine, nullptr);
            UINT count = 0, selection = 99, page = 99;
            check(list->GetCount(&count) == S_OK && count == 16, "candidate count incorrect");
            check(text(list.Get(), 0) == L"候補0く", "okuri or annotation leaked into commit text");
            check(engine.candidate_text(99).empty(), "invalid core candidate index accepted");
            check(list->GetSelection(&selection) == S_OK && selection == 0, "first selection incorrect");
            check(list->GetPageIndex(nullptr, 0, &count) == S_FALSE && count == 3, "page count query failed");
            UINT pages[3]{};
            check(list->GetPageIndex(pages, 1, &count) == S_FALSE && count == 3 && pages[0] == 0, "small page buffer overflow or wrong status");
            check(list->GetPageIndex(pages, 3, &count) == S_OK && pages[1] == 7 && pages[2] == 14, "default page boundaries incorrect");
            check(list->shortcut_index(0) == 0 && list->shortcut_index(6) == 6 &&
                list->shortcut_index(7) == std::numeric_limits<std::size_t>::max(), "first page shortcuts incorrect");
            for (int i = 0; i < 8; ++i) engine.handle({KeyCode::Space});
            list->update(engine, nullptr);
            check(list->GetCurrentPage(&page) == S_OK && page == 1, "selection did not advance to next page");
            DWORD flags = 0; list->GetUpdatedFlags(&flags);
            check((flags & TF_CLUIE_SELECTION) && !(flags & TF_CLUIE_STRING), "selection update replaced strings");
            engine.handle({KeyCode::PageDown}); list->update(engine, nullptr);
            list->GetSelection(&selection); list->GetCurrentPage(&page);
            check(selection == 15 && page == 2, "page down did not reach last partial page");
            check(list->shortcut_index(0) == 14 && list->shortcut_index(1) == 15 &&
                list->shortcut_index(2) == std::numeric_limits<std::size_t>::max(), "last page shortcut overflow");
            engine.handle({KeyCode::PageUp}); list->update(engine, nullptr);
            list->GetSelection(&selection); list->GetCurrentPage(&page);
            check(selection == 8 && page == 1, "page up did not update UI selection");
            UINT custom[]{0, 4, 9};
            check(list->SetPageIndex(custom, 3) == S_OK, "host page boundaries rejected");
            list->GetCurrentPage(&page); check(page == 1, "host current page incorrect");
            check(list->shortcut_index(0) == 4 && list->shortcut_index(4) == 8 &&
                list->shortcut_index(5) == std::numeric_limits<std::size_t>::max(), "host page shortcuts incorrect");
            UINT wide[]{0}; check(list->SetPageIndex(wide, 1) == S_OK, "wide page rejected");
            check(list->shortcut_index(0) == 2 && list->shortcut_index(6) == 8, "wide page shortcuts differ from visible rows");
            check(list->SetPageIndex(custom, 3) == S_OK, "host pages not restored");
            UINT bad[]{0, 4, 4};
            check(list->SetPageIndex(bad, 3) == E_INVALIDARG, "duplicate page boundary accepted");
            bad[2] = 16; check(list->SetPageIndex(bad, 3) == E_INVALIDARG, "out-of-range page boundary accepted");
            check(list->GetString(16, nullptr) == E_POINTER, "null BSTR output accepted");
            BSTR invalid = nullptr; check(list->GetString(16, &invalid) == E_INVALIDARG && !invalid, "invalid candidate index accepted");
            check(list->GetPageIndex(nullptr, 1, &count) == E_POINTER, "null page buffer accepted");
            list->Show(FALSE); BOOL shown = TRUE; list->IsShown(&shown); check(!shown, "host-suppressed UI was shown");
            list->close(); list->Show(TRUE); list->IsShown(&shown); check(!shown, "retained element reopened after end");
            check(text(list.Get(), 8) == L"候補8く", "retained element lost its immutable snapshot");
            check(list->shortcut_index(0) == std::numeric_limits<std::size_t>::max(), "closed UI accepted shortcut");
        }
        {
            Engine engine(demo_dictionary()); type(engine, "Michi"); engine.handle({KeyCode::Space});
            ComPtr<CandidateList> popup; popup.Attach(new CandidateList(true)); popup->update(engine, nullptr);
            ComPtr<ITfUIElement> element; check(SUCCEEDED(popup.As(&element)), "registration UI element unavailable");
            ComPtr<ITfCandidateListUIElement> candidates;
            check(element.As(&candidates) == E_NOINTERFACE, "registration advertised editable text as a candidate list");
            GUID guid{}; element->GetGUID(&guid); check(guid == registration_ui_guid, "registration identity missing");
            type(engine, "Nihon"); engine.handle({KeyCode::Space}); popup->update(engine, nullptr);
            BSTR description = nullptr; element->GetDescription(&description);
            const std::wstring contents(description, SysStringLen(description)); SysFreeString(description);
            check(contents.find(L"みち") != contents.npos && contents.find(L"▼日本") != contents.npos, "registration description lost reading or draft");
            popup->close(); element->Show(TRUE); BOOL shown = TRUE; element->IsShown(&shown);
            check(!shown, "retained registration element reopened");
        }
        check(module_objects == 0, "candidate COM object leaked");
        std::cout << "PASS candidate text/okuri, 7-row pages, host pagination, bounds, update flags, retained UI lifetime\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
