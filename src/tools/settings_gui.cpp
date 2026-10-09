#include "settings_editor.h"
#include "settings_gui_ids.h"
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace nskkzime::windows;
namespace {
struct Work {
    DictionarySettings draft;
    SettingsCheck check;
    std::atomic<bool> done{false};
    bool save = false;
};
struct Editor {
    std::filesystem::path file;
    SettingsDocument document;
    std::vector<std::filesystem::path> dictionaries;
    std::array<HWND, 7> pages{};
    HWND window = nullptr;
    bool valid = false, loading = false, dirty = false, confirming_close = false;
    std::shared_ptr<Work> work;
};
void set(HWND parent, int id, std::wstring_view value) { SetDlgItemTextW(parent, id, std::wstring(value).c_str()); }
std::wstring get(HWND parent, int id) {
    const auto item = GetDlgItem(parent, id);
    const int size = GetWindowTextLengthW(item);
    std::wstring value(static_cast<std::size_t>(size) + 1, L'\0');
    value.resize(GetWindowTextW(item, value.data(), size + 1));
    return value;
}
bool checked(HWND page, int id) { return IsDlgButtonChecked(page, id) == BST_CHECKED; }
void check(HWND page, int id, bool value) { CheckDlgButton(page, id, value ? BST_CHECKED : BST_UNCHECKED); }
void enabled(HWND page, int id, bool value) { EnableWindow(GetDlgItem(page, id), value); }
void report(Editor& editor, std::wstring value) {
    if (value.size() > 60000)
        value.resize(60000);
    set(editor.window, IDC_REPORT, value);
}
void update_enabled(Editor& editor) {
    SetWindowTextW(editor.window, editor.dirty ? L"nskkzime 設定（未保存）" : L"nskkzime 設定");
    const bool ready = editor.valid && !editor.work;
    for (const auto page : editor.pages)
        EnableWindow(page, ready);
    enabled(editor.window, IDOK, ready);
    enabled(editor.window, IDC_CHECK, ready);
    enabled(editor.window, IDC_RELOAD, !editor.work);
    const auto romaji = editor.pages[1], inference = editor.pages[2];
    enabled(romaji, IDC_ROMAJI_PATH, checked(romaji, IDC_CUSTOM_ROMAJI));
    enabled(romaji, IDC_ROMAJI_BROWSE, checked(romaji, IDC_CUSTOM_ROMAJI));
    for (const auto id : {IDC_MODEL, IDC_MODEL_BROWSE, IDC_SERVER, IDC_SERVER_BROWSE, IDC_RERANK, IDC_LEARN_GENERATED})
        enabled(inference, id, checked(inference, IDC_ZENZ));
    enabled(editor.pages[5], IDC_ALLOW_UNKNOWN_CONTEXT, checked(inference, IDC_ZENZ));
    for (const auto id : {IDC_LOG_DIRECTORY, IDC_LOG_BROWSE})
        enabled(editor.pages[4], id, checked(editor.pages[4], IDC_INPUT_LOG));
    const auto selection = SendDlgItemMessageW(editor.pages[0], IDC_DICTIONARIES, LB_GETCURSEL, 0, 0);
    enabled(editor.pages[0], IDC_REMOVE, selection != LB_ERR);
    enabled(editor.pages[0], IDC_UP, selection > 0);
    enabled(editor.pages[0], IDC_DOWN,
            selection != LB_ERR && static_cast<std::size_t>(selection + 1) < editor.dictionaries.size());
    enabled(editor.pages[0], IDC_ADD, editor.dictionaries.size() < 32);
}
void list(Editor& editor, int selected = -1) {
    const auto control = GetDlgItem(editor.pages[0], IDC_DICTIONARIES);
    SendMessageW(control, LB_RESETCONTENT, 0, 0);
    int extent = 0;
    const auto dc = GetDC(control);
    const auto previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
    for (const auto& path : editor.dictionaries) {
        SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
        SIZE size{};
        GetTextExtentPoint32W(dc, path.c_str(), static_cast<int>(path.native().size()), &size);
        extent = (std::max)(extent, static_cast<int>(size.cx) + 16);
    }
    SelectObject(dc, previous);
    ReleaseDC(control, dc);
    SendMessageW(control, LB_SETHORIZONTALEXTENT, extent, 0);
    SendMessageW(control, LB_SETCURSEL, selected, 0);
    update_enabled(editor);
}
void reload(Editor& editor) {
    editor.valid = false;
    editor.loading = true;
    try {
        auto loaded = open_settings_document(editor.file);
        editor.document = std::move(loaded);
        const auto& settings = editor.document.settings;
        editor.dictionaries = settings.dictionaries;
        list(editor);
        check(editor.pages[0], IDC_LEARNING, settings.learning);
        check(editor.pages[1], IDC_CUSTOM_ROMAJI, !settings.romaji_rules.empty());
        set(editor.pages[1], IDC_ROMAJI_PATH, settings.romaji_rules.native());
        set(editor.pages[6], IDC_CANDIDATE_SHOW_AFTER, std::to_wstring(settings.candidates.show_after));
        SendDlgItemMessageW(editor.pages[6], IDC_CANDIDATE_KEYS, CB_SETCURSEL,
            settings.candidates.keys == CandidateKeys::Asdfjkl ? 1 : 0, 0);
        const auto& zenz = settings.zenz;
        check(editor.pages[2], IDC_ZENZ, zenz.enabled);
        set(editor.pages[2], IDC_MODEL, zenz.model.native());
        set(editor.pages[2], IDC_SERVER, zenz.server.native());
        check(editor.pages[2], IDC_RERANK, zenz.policy.rerank);
        check(editor.pages[2], IDC_LEARN_GENERATED, zenz.policy.learn_fallback);
        check(editor.pages[5], IDC_ALLOW_UNKNOWN_CONTEXT, zenz.allow_unknown_context);
        const auto advanced = editor.pages[3];
        set(advanced, IDC_THREADS, std::to_wstring(zenz.threads));
        set(advanced, IDC_TIMEOUT, std::to_wstring(zenz.timeout_ms));
        set(advanced, IDC_IDLE_SECONDS, std::to_wstring(zenz.idle_seconds));
        set(advanced, IDC_MIN_LENGTH, std::to_wstring(zenz.policy.long_min_length));
        set(advanced, IDC_RERANK_LIMIT, std::to_wstring(zenz.policy.rerank_limit));
        set(advanced, IDC_SCORE_TIMEOUT, std::to_wstring(zenz.policy.score_timeout_ms));
        set(advanced, IDC_RERANK_WAIT, std::to_wstring(zenz.policy.rerank_wait_ms));
        set(advanced, IDC_PROMOTE_THRESHOLD, std::to_wstring(zenz.policy.promote_threshold));
        set(advanced, IDC_MAX_GAP, std::to_wstring(zenz.policy.max_score_gap));
        check(editor.pages[4], IDC_INPUT_LOG, zenz.log_input);
        set(editor.pages[4], IDC_LOG_DIRECTORY, zenz.log_directory.native());
        editor.valid = true;
        editor.dirty = false;
        std::wstring message = editor.document.original ? L"設定を読み込みました。"
                                                        : L"まだ設定は保存されていません。辞書を追加して保存してくださ"
                                                          L"い。空のまま保存すると、かな入力のみになります。";
        message += L"\r\n保存後は、IMEを使うアプリを終了して起動し直してください。";
        for (const auto name :
             {L"NSKKZIME_DICTIONARY", L"NSKKZIME_ROMAJI", L"NSKKZIME_LEARNING", L"NSKKZIME_INFERENCE_DUMMY"})
            if (environment_value(name))
                message +=
                    L"\r\n環境変数 " + std::wstring(name) + L" があるアプリでは保存設定より優先される場合があります。";
        report(editor, message);
    } catch (const std::exception& error) {
        report(editor,
               L"設定を読み込めません。元ファイルは変更しません。ファイルを修正して「再読込」を押してください。\r\n" +
                   wide_message(error.what()));
    }
    editor.loading = false;
    update_enabled(editor);
}
template <class T> T number(HWND page, int id, std::wstring_view label, T minimum, T maximum) {
    const auto text = utf8_message(get(page, id));
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !(result >= minimum && result <= maximum))
        throw std::invalid_argument(utf8_message(std::wstring(label) + L"を表示された範囲の数値で入力してください。"));
    return result;
}
DictionarySettings draft(Editor& editor) {
    auto result = editor.document.settings;
    result.dictionaries = editor.dictionaries;
    result.learning = checked(editor.pages[0], IDC_LEARNING);
    result.candidates.show_after = number<unsigned>(editor.pages[6], IDC_CANDIDATE_SHOW_AFTER, L"候補一覧のSpace回数", 1, 99);
    const auto keys = SendDlgItemMessageW(editor.pages[6], IDC_CANDIDATE_KEYS, CB_GETCURSEL, 0, 0);
    if (keys != 0 && keys != 1) throw std::invalid_argument("候補一覧の選択キーを選んでください。");
    result.candidates.keys = keys == 1 ? CandidateKeys::Asdfjkl : CandidateKeys::Numbers;
    result.romaji_rules = checked(editor.pages[1], IDC_CUSTOM_ROMAJI) ? get(editor.pages[1], IDC_ROMAJI_PATH) : L"";
    if (checked(editor.pages[1], IDC_CUSTOM_ROMAJI) && result.romaji_rules.empty())
        throw std::invalid_argument("ローマ字ルールのファイルを指定してください。");
    auto& zenz = result.zenz;
    zenz.enabled = checked(editor.pages[2], IDC_ZENZ);
    zenz.model = get(editor.pages[2], IDC_MODEL);
    zenz.server = get(editor.pages[2], IDC_SERVER);
    zenz.policy.rerank = checked(editor.pages[2], IDC_RERANK);
    zenz.policy.learn_fallback = checked(editor.pages[2], IDC_LEARN_GENERATED);
    zenz.allow_unknown_context = checked(editor.pages[5], IDC_ALLOW_UNKNOWN_CONTEXT);
    const auto page = editor.pages[3];
    zenz.threads = number<unsigned>(page, IDC_THREADS, L"スレッド数", 1, 64);
    zenz.timeout_ms = number<unsigned>(page, IDC_TIMEOUT, L"生成の期限", 1, 10000);
    zenz.idle_seconds = number<unsigned>(page, IDC_IDLE_SECONDS, L"サーバーの待機秒数", 1, 86400);
    zenz.policy.long_min_length = number<unsigned>(page, IDC_MIN_LENGTH, L"文字数", 0, 1024);
    zenz.policy.rerank_limit = number<unsigned>(page, IDC_RERANK_LIMIT, L"候補数", 2, 64);
    zenz.policy.score_timeout_ms = number<unsigned>(page, IDC_SCORE_TIMEOUT, L"採点の期限", 1, 10000);
    zenz.policy.rerank_wait_ms = number<unsigned>(page, IDC_RERANK_WAIT, L"採点の待ち時間", 0, 1000);
    zenz.policy.promote_threshold = number<float>(page, IDC_PROMOTE_THRESHOLD, L"必要なスコア差", 0, 100);
    zenz.policy.max_score_gap = number<float>(page, IDC_MAX_GAP, L"スコア差の上限", 0, 100);
    zenz.log_input = checked(editor.pages[4], IDC_INPUT_LOG);
    zenz.log_directory = get(editor.pages[4], IDC_LOG_DIRECTORY);
    return result;
}
void validate(Editor& editor, bool save) {
    if (!editor.valid || editor.work)
        return;
    auto work = std::make_shared<Work>();
    work->draft = draft(editor);
    work->save = save;
    std::thread([work] {
        try {
            work->check = check_settings_draft(work->draft);
        } catch (...) {
            work->check.valid = false;
        }
        work->done.store(true, std::memory_order_release);
    }).detach();
    if (!SetTimer(editor.window, 1, 75, nullptr))
        throw std::runtime_error("cannot start validation timer");
    editor.work = std::move(work);
    update_enabled(editor);
    report(editor,
           save ? L"保存する前に辞書と設定を検査しています…" : L"辞書と設定を検査しています…（保存はしません）");
}
void finish(Editor& editor) {
    // MessageBox runs a nested message loop. Do not save behind the discard prompt.
    if (editor.confirming_close || !editor.work || !editor.work->done.load(std::memory_order_acquire))
        return;
    KillTimer(editor.window, 1);
    auto work = std::move(editor.work);
    update_enabled(editor);
    if (work->check.report.empty())
        work->check.report = L"検査を完了できませんでした。設定は保存していません。";
    if (work->save && work->check.valid) {
        try {
            save_settings_document(editor.document, work->draft);
            editor.dirty = false;
            work->check.report =
                L"保存しました。IMEを使うアプリを終了して起動し直すと反映されます。\r\n\r\n" + work->check.report;
        } catch (const std::exception& error) {
            work->check.report = L"保存できませんでした。編集内容は残っています。\r\n" + wide_message(error.what()) +
                                 L"\r\n\r\n" + work->check.report;
        }
    }
    update_enabled(editor);
    report(editor, work->check.report);
}
std::vector<std::filesystem::path> choose(HWND window, bool multiple, const COMDLG_FILTERSPEC* filters, UINT count, bool folder = false) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("cannot open file picker");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT |
                       (multiple ? FOS_ALLOWMULTISELECT : 0) | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) dialog->SetFileTypes(count, filters);
    const auto shown = dialog->Show(window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    if (FAILED(shown))
        throw std::runtime_error("file picker failed");
    ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(&items)))
        throw std::runtime_error("cannot read selected files");
    DWORD size = 0;
    items->GetCount(&size);
    std::vector<std::filesystem::path> paths;
    if (size > 32)
        throw std::invalid_argument("一度に指定できる辞書は32個までです。");
    for (DWORD i = 0; i < size; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            throw std::runtime_error("cannot read selected path");
        try {
            paths.emplace_back(path);
        } catch (...) {
            CoTaskMemFree(path);
            throw;
        }
        CoTaskMemFree(path);
    }
    return paths;
}
void browse(Editor& editor, int id) {
    const COMDLG_FILTERSPEC all[] = {{L"すべてのファイル", L"*.*"}};
    const COMDLG_FILTERSPEC model[] = {{L"GGUFモデル", L"*.gguf"}, {L"すべてのファイル", L"*.*"}};
    const COMDLG_FILTERSPEC server[] = {{L"実行ファイル", L"*.exe"}};
    const auto paths = choose(editor.window, id == IDC_ADD,
                              id == IDC_MODEL_BROWSE    ? model
                              : id == IDC_SERVER_BROWSE ? server
                                                        : all,
                              id == IDC_MODEL_BROWSE ? 2 : 1, id == IDC_LOG_BROWSE);
    if (paths.empty())
        return;
    if (id == IDC_ADD) {
        auto next = editor.dictionaries;
        for (const auto& path : paths)
            if (std::find(next.begin(), next.end(), path) == next.end())
                next.push_back(path);
        if (next.size() > 32)
            throw std::invalid_argument("辞書は32個までです。選択を減らしてください。");
        editor.dictionaries = std::move(next);
        list(editor, static_cast<int>(editor.dictionaries.size()) - 1);
    } else if (id == IDC_ROMAJI_BROWSE)
        set(editor.pages[1], IDC_ROMAJI_PATH, paths[0].native());
    else if (id == IDC_LOG_BROWSE)
        set(editor.pages[4], IDC_LOG_DIRECTORY, paths[0].native());
    else
        set(editor.pages[2], id == IDC_MODEL_BROWSE ? IDC_MODEL : IDC_SERVER, paths[0].native());
    editor.dirty = true;
    update_enabled(editor);
}
void reorder(Editor& editor, int id) {
    const auto selected = SendDlgItemMessageW(editor.pages[0], IDC_DICTIONARIES, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || static_cast<std::size_t>(selected) >= editor.dictionaries.size())
        return;
    auto next = static_cast<int>(selected);
    if (id == IDC_REMOVE) {
        editor.dictionaries.erase(editor.dictionaries.begin() + next);
        next = (std::min)(next, static_cast<int>(editor.dictionaries.size()) - 1);
    } else {
        const auto target = next + (id == IDC_UP ? -1 : 1);
        if (target < 0 || static_cast<std::size_t>(target) >= editor.dictionaries.size())
            return;
        std::swap(editor.dictionaries[next], editor.dictionaries[target]);
        next = target;
    }
    editor.dirty = true;
    list(editor, next);
}
void show_page(Editor& editor) {
    const auto tab = GetDlgItem(editor.window, IDC_TABS);
    RECT content{};
    GetWindowRect(tab, &content);
    MapWindowPoints(nullptr, editor.window, reinterpret_cast<POINT*>(&content), 2);
    TabCtrl_AdjustRect(tab, FALSE, &content);
    const auto selected = TabCtrl_GetCurSel(tab);
    for (std::size_t i = 0; i < editor.pages.size(); ++i) {
        SetWindowPos(editor.pages[i], nullptr, content.left + 4, content.top + 4, content.right - content.left - 8,
                     content.bottom - content.top - 8, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(editor.pages[i], static_cast<int>(i) == selected ? SW_SHOW : SW_HIDE);
    }
}
INT_PTR CALLBACK page_proc(HWND page, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND)
        return SendMessageW(GetParent(page), message, wparam, lparam);
    return FALSE;
}
INT_PTR CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            editor = reinterpret_cast<Editor*>(lparam);
            editor->window = window;
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            set(window, IDC_FILE, L"保存先: " + editor->file.native());
            SendDlgItemMessageW(window, IDC_REPORT, EM_SETLIMITTEXT, 65535, 0);
            const std::array names{L"辞書", L"ローマ字", L"推論", L"詳細", L"診断", L"文脈", L"候補一覧"};
            for (std::size_t i = 0; i < editor->pages.size(); ++i) {
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<wchar_t*>(names[i]);
                TabCtrl_InsertItem(GetDlgItem(window, IDC_TABS), static_cast<int>(i), &item);
                editor->pages[i] = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DICTIONARIES + i),
                                                      window, page_proc, 0);
                if (!editor->pages[i])
                    throw std::runtime_error("cannot create settings page");
            }
            for (const auto label : {L"1–7", L"ASDFJKL"})
                SendDlgItemMessageW(editor->pages[6], IDC_CANDIDATE_KEYS, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            show_page(*editor);
            reload(*editor);
            return TRUE;
        }
        if (!editor)
            return FALSE;
        if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->idFrom == IDC_TABS &&
            reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
            show_page(*editor);
            return TRUE;
        }
        if (message == WM_TIMER) {
            finish(*editor);
            return TRUE;
        }
        if (message == WM_DPICHANGED) {
            PostMessageW(window, WM_APP, 0, 0);
            return FALSE;
        }
        if (message == WM_APP) {
            show_page(*editor);
            list(*editor,
                 static_cast<int>(SendDlgItemMessageW(editor->pages[0], IDC_DICTIONARIES, LB_GETCURSEL, 0, 0)));
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            if (editor->dirty || editor->work) {
                editor->confirming_close = true;
                const auto answer = MessageBoxW(window, L"保存していない変更と検査を破棄して閉じますか？",
                                                L"nskkzime 設定", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
                editor->confirming_close = false;
                if (answer != IDYES)
                    return TRUE;
            }
            KillTimer(window, 1);
            EndDialog(window, 0);
            return TRUE;
        }
        if (message == WM_COMMAND) {
            const auto id = LOWORD(wparam), notification = HIWORD(wparam);
            if (id == IDC_RELOAD) {
                if (!editor->dirty ||
                    MessageBoxW(window, L"編集中の変更を破棄して読み込み直しますか？", L"nskkzime 設定",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                    reload(*editor);
            } else if (id == IDOK || id == IDC_CHECK)
                validate(*editor, id == IDOK);
            else if (!editor->work && editor->valid && !editor->loading) {
                if (id == IDC_ADD || id == IDC_ROMAJI_BROWSE || id == IDC_MODEL_BROWSE || id == IDC_SERVER_BROWSE || id == IDC_LOG_BROWSE)
                    browse(*editor, id);
                else if (id == IDC_REMOVE || id == IDC_UP || id == IDC_DOWN)
                    reorder(*editor, id);
                else if (id == IDC_DICTIONARIES && notification == LBN_SELCHANGE)
                    update_enabled(*editor);
                else if (id >= IDC_LEARNING && id <= IDC_CANDIDATE_KEYS &&
                         (notification == BN_CLICKED || notification == EN_CHANGE || notification == CBN_SELCHANGE)) {
                    editor->dirty = true;
                    update_enabled(*editor);
                }
            }
            return TRUE;
        }
    } catch (const std::exception& error) {
        if (editor) {
            report(*editor, L"操作を完了できませんでした。\r\n" + wide_message(error.what()));
            update_enabled(*editor);
        }
    } catch (...) {
        if (editor)
            report(*editor, L"操作を完了できませんでした。");
    }
    return FALSE;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return 2;
    Editor editor;
    try {
        if (count == 1)
            editor.file = dictionary_settings_path();
        else if (count == 3 && std::wstring_view(arguments[1]) == L"--config" &&
                 std::filesystem::path(arguments[2]).is_absolute())
            editor.file = arguments[2];
        else
            throw std::invalid_argument("nskkzime_settings.exe [--config 絶対パス]");
    } catch (const std::exception& error) {
        LocalFree(arguments);
        MessageBoxW(nullptr, wide_message(error.what()).c_str(), L"nskkzime 設定", MB_OK | MB_ICONERROR);
        return 2;
    }
    LocalFree(arguments);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
        return 2;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, dialog_proc,
                                        reinterpret_cast<LPARAM>(&editor));
    CoUninitialize();
    return result == -1 ? 2 : static_cast<int>(result);
}
