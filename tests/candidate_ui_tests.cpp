#include "candidate_ui.h"
#include "tsf_test_host.h"
#include <algorithm>
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
std::vector<DWORD> ime_events;
HWND ime_window = nullptr;
void CALLBACK ime_event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (object == OBJID_CLIENT && child == CHILDID_SELF) ime_events.push_back(event);
    if (event == EVENT_OBJECT_IME_SHOW) ime_window = window;
}
void drain_events() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
void presenter_fonts() {
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager);
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space});
    CandidatePresenter presenter;
    presenter.update(host.Get(), context.Get(), engine);
    auto* list = static_cast<CandidateList*>(host->ui.Get());
    check(list && list->font_face() == default_ui_font, "default popup font changed");
    // Retain the previous element so an allocation cannot reuse its address.
    const auto previous = host->ui;
    presenter.update(host.Get(), context.Get(), engine, {}, true, CandidateKeys::Numbers, {}, L"Meiryo");
    list = static_cast<CandidateList*>(host->ui.Get());
    check(list && host->ui != previous && list->font_face() == L"Meiryo", "font change retained the old popup");
    presenter.update(host.Get(), context.Get(), engine, {}, true, CandidateKeys::Numbers, {}, L"Meiryo");
    check(host->ui.Get() == list, "unchanged font recreated the popup");
    presenter.update(host.Get(), context.Get(), engine, {}, true, CandidateKeys::Numbers, L"登録できません", L"Meiryo");
    list = static_cast<CandidateList*>(host->ui.Get());
    check(list && list->kind() == PopupKind::Notice && list->font_face() == L"Meiryo", "notification lost configured font");
    presenter.close();
}
void popup_events() {
    struct Hook {
        HWINEVENTHOOK value = SetWinEventHook(EVENT_OBJECT_IME_SHOW, EVENT_OBJECT_IME_CHANGE, nullptr,
            ime_event, GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
        ~Hook() { if (value) UnhookWinEvent(value); }
    } hook;
    check(hook.value != nullptr, "IME event hook unavailable");
    Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space});
    ComPtr<CandidateList> popup; popup.Attach(new CandidateList);
    popup->update(engine, nullptr);
    RECT anchor{100, 100, 110, 120}; popup->locate(nullptr, &anchor);
    popup->Show(TRUE); drain_events();
    check(!ime_events.empty() && ime_events.front() == EVENT_OBJECT_IME_SHOW, "visible popup did not notify IME show");
    check(ime_window && (GetClassLongPtrW(ime_window, GCL_STYLE) & CS_IME), "popup not identified as IME UI to immersive hosts");
    ime_events.clear();
    ValidateRect(ime_window, nullptr);
    popup->locate(nullptr, &anchor);
    check(!GetUpdateRect(ime_window, nullptr, FALSE), "unchanged candidate anchor dirtied the popup");
    drain_events();
    check(ime_events.empty(), "unchanged candidate anchor notified another move");
    OffsetRect(&anchor, 20, 20); popup->locate(nullptr, &anchor); drain_events();
    check(std::find(ime_events.begin(), ime_events.end(), DWORD{EVENT_OBJECT_IME_CHANGE}) != ime_events.end(), "popup relocation did not notify IME change");
    ime_events.clear(); popup->Show(FALSE); drain_events();
    check(ime_events == std::vector<DWORD>{EVENT_OBJECT_IME_HIDE}, "popup hiding did not notify exactly once");
    ime_events.clear(); popup->Show(FALSE); drain_events();
    check(ime_events.empty(), "already-hidden popup notified again");
    popup->Show(TRUE); drain_events(); ime_events.clear();
    popup->close(); drain_events();
    check(ime_events == std::vector<DWORD>{EVENT_OBJECT_IME_HIDE}, "closing visible popup lost IME hide");
    ime_events.clear(); popup->close(); popup->Show(TRUE); drain_events();
    check(ime_events.empty(), "closed popup emitted visible IME events");
}
void presenter_positions() {
    ComPtr<test_host::Manager> host; host.Attach(new test_host::Manager); host->show_ui = TRUE;
    ComPtr<test_host::Context> context; context.Attach(new test_host::Context);
    ComPtr<test_host::ContextView> view; view.Attach(new test_host::ContextView); context->active_view = view.Get();
    ComPtr<test_host::Composition> composition; composition.Attach(new test_host::Composition);
    composition->range.Attach(new test_host::Range(context->doc, 0, 0));
    Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space});
    CandidatePresenter presenter;
    presenter.update(host.Get(), context.Get(), engine, {}, false);
    presenter.locate(context.Get(), composition.Get(), 1, 7, true); // No visible candidate list yet.
    engine.handle({KeyCode::Space}); engine.handle({KeyCode::Space});
    check(engine.registering(), "test did not exhaust candidates");
    view->text_status = TF_E_NOLAYOUT;
    presenter.update(host.Get(), context.Get(), engine);
    presenter.locate(context.Get(), composition.Get(), 1, 7);
    BOOL shown = FALSE; check(host->ui && SUCCEEDED(host->ui->IsShown(&shown)) && shown && presenter.presentation_available(),
        "registration did not use the captured position while layout was pending");
    const auto popup_window = [&] {
        wchar_t name[96]{}; swprintf_s(name, L"nskkzime.Candidates.%p", static_cast<void*>(host->ui.Get()));
        return FindWindowW(name, nullptr);
    }();
    RECT before{}; check(popup_window && GetWindowRect(popup_window, &before), "registration window missing");
    presenter.invalidate_position(); view->text_status = S_OK; OffsetRect(&view->rect, 100, 100);
    presenter.locate(context.Get(), composition.Get(), 1, 7);
    RECT after{}; check(GetWindowRect(popup_window, &after) && !EqualRect(&before, &after), "ready layout did not move the registration window");

    // A different view/session or explicitly clipped range cannot reuse it.
    ComPtr<test_host::ContextView> replacement; replacement.Attach(new test_host::ContextView);
    replacement->text_status = TF_E_NOLAYOUT; context->active_view = replacement.Get();
    presenter.locate(context.Get(), composition.Get(), 1, 7);
    check(!presenter.presentation_available(), "replacement view reused a stale position");
    context->active_view = view.Get(); presenter.locate(context.Get(), composition.Get(), 1, 7);
    view->text_status = TF_E_NOLAYOUT; presenter.locate(context.Get(), composition.Get(), 1, 8);
    check(!presenter.presentation_available(), "new composition session reused a stale position");
    view->text_status = S_OK; presenter.locate(context.Get(), composition.Get(), 1, 8);
    view->clipped = TRUE; presenter.locate(context.Get(), composition.Get(), 1, 8);
    check(!presenter.presentation_available(), "clipped editor remained visible");
    view->clipped = FALSE; presenter.locate(context.Get(), composition.Get(), 1, 8);
    view->text_status = E_FAIL; presenter.locate(context.Get(), composition.Get(), 1, 8);
    check(!presenter.presentation_available(), "hard layout failure reused a cached position");
    view->text_status = S_OK; presenter.locate(context.Get(), composition.Get(), 1, 8);
    presenter.hide(); view->text_status = TF_E_NOLAYOUT; presenter.locate(context.Get(), composition.Get(), 1, 8);
    check(!presenter.presentation_available(), "hidden view restored a stale position");
    const Key number{KeyCode::Character, '1'};
    check(presenter.selection_key(number, engine).code == KeyCode::Character,
        "hidden popup captured a candidate shortcut");

    // A reentrant close during a query must invalidate its eventual result.
    view->text_status = S_OK; bool closed = false;
    view->text_query = [&](ITfRange*) { if (!closed) { closed = true; presenter.close(); } return S_OK; };
    presenter.locate(context.Get(), composition.Get(), 1, 8, true);
    check(!host->ui, "reentrant close restored a popup");
    view->text_query = {}; view->text_status = TF_E_NOLAYOUT;
    presenter.update(host.Get(), context.Get(), engine); presenter.locate(context.Get(), composition.Get(), 1, 8);
    check(!presenter.presentation_available(), "reentrant query restored a cached position");
    presenter.close();

    view->text_status = S_OK; presenter.locate(context.Get(), composition.Get(), 1, 9, true);
    presenter.update(host.Get(), context.Get(), engine, {}, true, CandidateKeys::Numbers, L"保護された入力欄のため、単語登録を行いません。");
    check(host->ui != nullptr, "notice UI missing");
    GUID guid{}; host->ui->GetGUID(&guid); check(guid == notice_ui_guid, "notice identity incorrect");
    ComPtr<ITfCandidateListUIElement> candidates;
    check(host->ui.As(&candidates) == E_NOINTERFACE, "notice exposed candidate selection");
    BSTR reason = nullptr; host->ui->GetDescription(&reason);
    const bool explained = reason && std::wstring_view(reason).find(L"保護された入力欄") != std::wstring_view::npos;
    SysFreeString(reason); check(explained, "notice reason unavailable to host");
    presenter.locate(context.Get(), composition.Get(), 1, 9);
    check(presenter.presentation_available(), "notice popup not visible");
    presenter.dismiss_notice(); check(!host->ui, "notice not dismissed");
    presenter.update(host.Get(), context.Get(), engine);
    presenter.locate(context.Get(), composition.Get(), 1, 9);
    host->ui_end_query = [&] { presenter.close(); };
    presenter.update(host.Get(), context.Get(), engine, {}, true, CandidateKeys::Numbers, L"保護された入力欄のため、単語登録を行いません。");
    check(!host->ui, "UI kind switch restored a popup after reentrant close");
    host->ui_end_query = {};
}
void ui_theme_and_icon_tests() {
    const auto light = make_ui_theme(false, false, RGB(0, 103, 192));
    const auto dark = make_ui_theme(true, false, RGB(96, 205, 255));
    const auto hc = make_ui_theme(false, true, RGB(0, 103, 192));
    check(!light.dark && dark.dark && hc.high_contrast, "theme flags incorrect");
    check(light.surface != dark.surface && light.text != dark.text, "light and dark surfaces did not differ");
    check(hc.surface == GetSysColor(COLOR_WINDOW) && hc.selection_bg == GetSysColor(COLOR_HIGHLIGHT),
        "high contrast did not fall back to system colors");

    const auto p_hira = mode_palette(light, Mode::Hiragana);
    const auto p_kata = mode_palette(light, Mode::Katakana);
    const auto p_wide = mode_palette(light, Mode::FullWidthLatin);
    const auto p_latin = mode_palette(light, Mode::Latin);
    check(p_hira.background != p_kata.background && p_hira.background != p_wide.background &&
          p_hira.background != p_latin.background && p_kata.background != p_wide.background,
          "mode badges did not distinguish input modes");

    const HICON icon = create_mode_tray_icon(L"あ", default_ui_font, 24, 24, RGB(255, 255, 255));
    check(icon != nullptr, "anti-aliased tray icon creation failed");
    ICONINFO info{};
    check(GetIconInfo(icon, &info) != FALSE && info.hbmColor != nullptr && info.hbmMask != nullptr,
        "tray icon missing 32bpp color bitmap");
    BITMAP bm{};
    check(GetObjectW(info.hbmColor, sizeof(bm), &bm) == sizeof(bm) && bm.bmWidth == 24 && bm.bmHeight == 24 && bm.bmBitsPixel == 32,
        "tray icon color bitmap dimensions or depth incorrect");
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(bm.bmWidth) * static_cast<std::size_t>(bm.bmHeight));
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const HDC screen = GetDC(nullptr);
    const int copied = GetDIBits(screen, info.hbmColor, 0, static_cast<UINT>(bm.bmHeight), pixels.data(), &bi, DIB_RGB_COLORS);
    if (screen) ReleaseDC(nullptr, screen);
    check(copied == bm.bmHeight, "failed to inspect tray icon pixels");
    const bool has_crisp_stroke = std::any_of(pixels.begin(), pixels.end(), [](std::uint32_t p) { return (p >> 24) >= 220; });
    const bool has_transparent = std::any_of(pixels.begin(), pixels.end(), [](std::uint32_t p) { return (p >> 24) == 0; });
    check(has_crisp_stroke && has_transparent, "tray icon lacked crisp opaque strokes or transparent background");
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    DestroyIcon(icon);
}
void paint_and_wheel_tests() {
    auto dictionary = std::make_shared<MemoryDictionary>();
    dictionary->add({U"かんじ"}, {{"1", U"漢字", U"文字"}, {"2", U"感じ", U"感覚"}});
    Engine engine(dictionary);
    type(engine, "Kanji");
    engine.handle({KeyCode::Space});
    engine.append_generated({{"ai-1", U"幹事", U"AI候補", CandidateSource::GeneratedFallback, LearningPolicy::DoNotLearn}});

    auto actions = std::make_shared<CandidateActions>();
    int stepped = 0; std::uint64_t step_rev = 0;
    actions->step = [&](int delta, std::uint64_t rev) { stepped += delta; step_rev = rev; };

    ComPtr<CandidateList> popup;
    popup.Attach(new CandidateList(PopupKind::Candidates, actions, CandidateKeys::Numbers, default_ui_font, true));
    popup->update(engine, nullptr);
    RECT anchor{120, 120, 140, 140};
    popup->locate(nullptr, &anchor);
    popup->Show(TRUE);
    drain_events();
    popup->request_step(1);
    check(stepped == 1 && step_rev == engine.revision(), "wheel step callback lost delta or revision");
    popup->close();
    popup->request_step(1);
    check(stepped == 1, "closed popup dispatched wheel step");

    // Exercise painting for registration (with mode badge & nested candidates), forget prompt, and notice.
    Engine reg_engine(dictionary);
    type(reg_engine, "Michi"); reg_engine.handle({KeyCode::Space});
    type(reg_engine, "Kanji"); reg_engine.handle({KeyCode::Space});
    ComPtr<CandidateList> editor;
    editor.Attach(new CandidateList(PopupKind::Editor, {}, CandidateKeys::Asdfjkl, default_ui_font, true));
    editor->update(reg_engine, nullptr);
    editor->locate(nullptr, &anchor);
    editor->Show(TRUE);
    drain_events();
    editor->close();

    Engine forget_engine(dictionary);
    type(forget_engine, "Kanji"); forget_engine.handle({KeyCode::Space}); type(forget_engine, "X");
    ComPtr<CandidateList> forget_ui;
    forget_ui.Attach(new CandidateList(PopupKind::Editor));
    forget_ui->update(forget_engine, nullptr);
    forget_ui->locate(nullptr, &anchor);
    forget_ui->Show(TRUE);
    drain_events();
    forget_ui->close();

    ComPtr<CandidateList> notice_ui;
    notice_ui.Attach(new CandidateList(PopupKind::Notice));
    notice_ui->notice(L"保護された入力欄のため、単語登録を行いません。", nullptr);
    notice_ui->locate(nullptr, &anchor);
    notice_ui->Show(TRUE);
    drain_events();
    notice_ui->close();
}
}
int main() {
    try {
        ui_theme_and_icon_tests();
        paint_and_wheel_tests();
        popup_events();
        presenter_fonts();
        presenter_positions();
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
            ComPtr<CandidateList> list; list.Attach(new CandidateList(PopupKind::Candidates, actions)); list->update(engine, nullptr);
            list->request_selection(2); check(calls == 0, "out-of-range click dispatched");
            list->request_selection(1); check(calls == 1 && selected == 1 && revision == engine.revision(), "click lost snapshot revision");
            actions.reset(); list->request_selection(0); check(calls == 1, "expired service received click");
            actions = std::make_shared<CandidateActions>(); actions->select = [&](std::size_t, std::uint64_t) { ++calls; };
            list.Attach(new CandidateList(PopupKind::Candidates, actions)); list->update(engine, nullptr); list->close(); list->request_selection(0);
            check(calls == 1, "closed snapshot dispatched click");
        }
        {
            Engine engine(demo_dictionary()); type(engine, "Michi"); engine.handle({KeyCode::Space});
            ComPtr<CandidateList> popup; popup.Attach(new CandidateList(PopupKind::Editor)); popup->update(engine, nullptr);
            ComPtr<ITfUIElement> element; check(SUCCEEDED(popup.As(&element)), "registration UI element unavailable");
            ComPtr<ITfCandidateListUIElement> candidates;
            check(element.As(&candidates) == E_NOINTERFACE, "registration advertised editable text as a candidate list");
            GUID guid{}; element->GetGUID(&guid); check(guid == registration_ui_guid, "registration identity missing");
            type(engine, "Nihon"); engine.handle({KeyCode::Space}); popup->update(engine, nullptr);
            BSTR description = nullptr; element->GetDescription(&description);
            const std::wstring contents(description, SysStringLen(description)); SysFreeString(description);
            check(contents.find(L"みち") != contents.npos && contents.find(L"▼日本") != contents.npos, "registration description lost reading or draft");
            check(contents.find(L"/Enter: 欄へ追加") != contents.npos && contents.find(L"Enter: 登録") == contents.npos,
                "registration candidate hint offered to register on Enter");
            engine.handle({KeyCode::Enter}); popup->update(engine, nullptr);
            element->GetDescription(&description);
            const std::wstring settled(description, SysStringLen(description)); SysFreeString(description);
            check(settled.find(L"日本") != settled.npos && settled.find(L"▼") == settled.npos && settled.find(L"Enter: 登録") != settled.npos,
                "settled registration draft lost its registration hint");
            popup->close(); element->Show(TRUE); BOOL shown = TRUE; element->IsShown(&shown);
            check(!shown, "retained registration element reopened");
        }
        {
            Engine engine(demo_dictionary()); type(engine, "Kanji"); engine.handle({KeyCode::Space}); type(engine, "X");
            unsigned selections = 0;
            auto actions = std::make_shared<CandidateActions>(); actions->select = [&](std::size_t, std::uint64_t) { ++selections; };
            ComPtr<CandidateList> popup; popup.Attach(new CandidateList(PopupKind::Editor, actions)); popup->update(engine, nullptr);
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
