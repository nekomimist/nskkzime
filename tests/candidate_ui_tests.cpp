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
            enable_input_timing(true);
            Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space});
            ComPtr<CandidateList> popup; popup.Attach(new CandidateList);
            { KeyTiming key; popup->update(engine, nullptr); }
            check(input_timing_snapshot().pending_paints == 1, "candidate update lost key timing");
            // Hosts which draw their own UI must not count our undisplayed popup.
            popup->Show(FALSE);
            check(input_timing_snapshot().pending_paints == 0 && input_timing_snapshot().candidate_paint.total == 0 &&
                  input_timing_snapshot().discarded_paints == 1, "host-suppressed UI counted as a paint");
            engine.handle({KeyCode::Space});
            { KeyTiming key; popup->update(engine, nullptr); }
            popup->locate(nullptr, nullptr);
            check(input_timing_snapshot().pending_paints == 0 && input_timing_snapshot().discarded_paints == 2, "missing anchor kept a pending paint");
            popup->close(); popup.Reset(); enable_input_timing(false);
        }
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
            // Hit testing uses the same client dimensions, padding, header and
            // visible row interval as painting, independently of a desktop.
            for (const int scale : {1, 2, 3}) {
                const int padding = 10 * scale, line = 32 * scale;
                const SIZE area{500 * scale, 300 * scale};
                for (const bool registration : {false, true}) {
                    const int top = padding + (registration ? 2 * line : 0);
                    check(!candidate_row_at({5, top - 1}, area, padding, line, registration, 14, 16), "header or padding selected row");
                    check(candidate_row_at({0, top}, area, padding, line, registration, 14, 16) == 14, "first row boundary missed");
                    check(candidate_row_at({area.cx - 1, top + 2 * line - 1}, area, padding, line, registration, 14, 16) == 15, "last row boundary missed");
                    check(!candidate_row_at({5, top + 2 * line}, area, padding, line, registration, 14, 16), "footer selected row");
                    for (const auto point : {POINT{-1, top}, POINT{area.cx, top}, POINT{5, -1}, POINT{5, area.cy}})
                        check(!candidate_row_at(point, area, padding, line, registration, 14, 16), "outside point selected row");
                    check(!candidate_row_at({5, top}, area, padding, line, registration, 0, 0), "empty registration selected row");
                }
            }
            CandidatePointer pointer;
            check(!pointer.up(1), "release without press selected row");
            pointer.down(1); check(pointer.up(1) == 1 && !pointer.up(1), "click lost or repeated");
            pointer.down(1); check(!pointer.up(2), "drag to another row selected it");
            pointer.down(1); check(!pointer.up({}), "release outside selected row");
            pointer.down(1); pointer.cancel(); check(!pointer.up(1), "canceled gesture selected row");
        }
        {
            Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space});
            auto actions = std::make_shared<CandidateActions>();
            std::size_t selected = 99; std::uint64_t revision = 0; unsigned calls = 0;
            actions->select = [&](std::size_t index, std::uint64_t value) { selected = index; revision = value; ++calls; };
            ComPtr<CandidateList> list; list.Attach(new CandidateList(false, actions)); list->update(engine, nullptr);
            list->request_selection(2); check(calls == 0, "out-of-range click dispatched");
            list->request_selection(1); check(calls == 1 && selected == 1 && revision == engine.revision(), "click lost snapshot revision");
            actions.reset(); list->request_selection(0); check(calls == 1, "expired service received click");
            actions = std::make_shared<CandidateActions>(); actions->select = [&](std::size_t, std::uint64_t) { ++calls; };
            list.Attach(new CandidateList(false, actions)); list->update(engine, nullptr); list->close(); list->request_selection(0);
            check(calls == 1, "closed snapshot dispatched click");
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
        {
            Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space}); type(engine, "X");
            unsigned selections = 0;
            auto actions = std::make_shared<CandidateActions>(); actions->select = [&](std::size_t, std::uint64_t) { ++selections; };
            ComPtr<CandidateList> popup; popup.Attach(new CandidateList(true, actions)); popup->update(engine, nullptr);
            UINT count = 99; popup->GetCount(&count); popup->request_selection(0);
            ComPtr<ITfCandidateListUIElement> list;
            check(count == 0 && !selections && popup.As(&list) == E_NOINTERFACE, "deletion prompt exposed actionable candidates");
            BSTR description = nullptr; popup->GetDescription(&description);
            const bool unavailable = description && std::wstring_view(description).find(L"削除できる学習語がありません") != std::wstring_view::npos;
            SysFreeString(description); check(unavailable, "read-only deletion explanation unavailable to host");
        }
        check(module_objects == 0, "candidate COM object leaked");
        std::cout << "PASS candidate text/okuri, 7-row pages, host pagination, bounds, update flags, retained UI lifetime\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
