#include "dictionary_manager.h"
#include "dictionary_settings.h"
#include "dictionary_gui_ids.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <thread>

using namespace nskkzime;
using namespace nskkzime::windows;
using Microsoft::WRL::ComPtr;
namespace {
enum class Action { Reload, PrepareImport, Import, Forget, Export };
bool writes(Action action) { return action == Action::Import || action == Action::Forget || action == Action::Export; }
struct Work {
    Action action = Action::Reload;
    std::atomic<bool> done{false};
    bool rows_ready = false, reload_attempted = false;
    std::vector<DictionaryRow> rows;
    std::optional<DictionaryImportPreview> preview;
    std::wstring message;
};
struct Manager {
    HWND window = nullptr;
    std::filesystem::path file;
    bool valid = false;
    std::vector<DictionaryRow> rows;
    std::vector<std::size_t> visible;
    std::shared_ptr<Work> work;
};
void set(HWND window, int id, std::wstring_view value) { SetDlgItemTextW(window, id, std::wstring(value).c_str()); }
std::wstring get(HWND window, int id) {
    const auto control = GetDlgItem(window, id);
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    value.resize(GetWindowTextW(control, value.data(), static_cast<int>(value.size())));
    return value;
}
void status(Manager& manager, std::wstring text) {
    if (text.size() > 60000)
        text.resize(60000);
    set(manager.window, IDC_STATUS, text);
}
const DictionaryRow* selected(const Manager& manager) {
    const auto index = ListView_GetNextItem(GetDlgItem(manager.window, IDC_WORDS), -1, LVNI_SELECTED);
    if (index < 0 || static_cast<std::size_t>(index) >= manager.visible.size())
        return nullptr;
    return &manager.rows[manager.visible[static_cast<std::size_t>(index)]];
}
std::wstring describe(const DictionaryRow& row) {
    return L"読み: " + row.columns[0] + L"  /  送りキー: " + (row.columns[1].empty() ? L"なし" : row.columns[1]) +
           L"  /  送り: " + (row.columns[2].empty() ? L"共通" : row.columns[2]) + L"\r\n表記: " + row.columns[3] +
           L"\r\n注釈: " + row.columns[4];
}
void controls(Manager& manager) {
    const bool idle = !manager.work;
    for (const int id : {IDC_REFRESH, IDC_SEARCH, IDC_WORDS})
        EnableWindow(GetDlgItem(manager.window, id), idle);
    for (const int id : {IDC_IMPORT, IDC_EXPORT})
        EnableWindow(GetDlgItem(manager.window, id), idle && manager.valid);
    EnableWindow(GetDlgItem(manager.window, IDC_FORGET), idle && manager.valid && selected(manager));
    EnableWindow(GetDlgItem(manager.window, IDCANCEL), idle || !writes(manager.work->action));
}
void selection(Manager& manager) {
    const auto row = selected(manager);
    set(manager.window, IDC_DETAIL, row ? describe(*row) : L"");
    controls(manager);
}
void filter(Manager& manager) {
    auto visible = filter_dictionary_rows(manager.rows, get(manager.window, IDC_SEARCH));
    const auto list = GetDlgItem(manager.window, IDC_WORDS);
    ListView_SetItemCountEx(list, 0, 0);
    manager.visible = std::move(visible);
    ListView_SetItemCountEx(list, static_cast<int>(manager.visible.size()), 0);
    set(manager.window, IDC_COUNT,
        std::to_wstring(manager.visible.size()) + L" / " + std::to_wstring(manager.rows.size()) + L"件");
    selection(manager);
}
void replace_rows(Manager& manager, std::vector<DictionaryRow> rows) {
    ListView_SetItemCountEx(GetDlgItem(manager.window, IDC_WORDS), 0, 0);
    manager.visible.clear();
    manager.rows = std::move(rows);
    filter(manager);
}
std::optional<std::filesystem::path> choose_file(HWND window, bool save) {
    ComPtr<IFileDialog> dialog;
    HRESULT created;
    if (save)
        created = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    else
        created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(created))
        throw std::runtime_error("cannot open file picker");
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
    if (save)
        options &= ~FOS_OVERWRITEPROMPT; // All existing destinations are rejected, even after a race.
    else
        options |= FOS_FILEMUSTEXIST;
    dialog->SetOptions(options);
    const COMDLG_FILTERSPEC filters[] = {{L"SKK辞書", L"*.skk;SKK-JISYO*"}, {L"すべてのファイル", L"*.*"}};
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    if (save) {
        dialog->SetTitle(L"全件を新しいUTF-8 SKKファイルへ書き出す");
        dialog->SetDefaultExtension(L"skk");
        dialog->SetFileName(L"user-dictionary.skk");
    } else
        dialog->SetTitle(L"取り込むSKK辞書（UTF-8 / EUC-JP）を選ぶ");
    const auto result = dialog->Show(window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    if (FAILED(result))
        throw std::runtime_error("file picker failed");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        throw std::runtime_error("cannot read selected path");
    std::filesystem::path selected_path;
    try {
        selected_path = path;
    } catch (...) {
        CoTaskMemFree(path);
        throw;
    }
    CoTaskMemFree(path);
    if (save && std::filesystem::exists(selected_path))
        throw std::invalid_argument("既存のファイルへは書き出せません。新しい名前を指定してください。");
    return selected_path;
}
void start(Manager& manager, Action action, std::filesystem::path path = {}, LearningEvent entry = {},
           std::optional<DictionaryImportPreview> preview = {}) {
    if (manager.work)
        return;
    auto work = std::make_shared<Work>();
    work->action = action;
    const auto file = manager.file;
    // Establish completion delivery before allowing a worker to mutate anything.
    if (!SetTimer(manager.window, 1, 75, nullptr))
        throw std::runtime_error("cannot start operation timer");
    manager.work = work;
    try {
        std::thread([work, file, path = std::move(path), entry = std::move(entry),
                     preview = std::move(preview)] {
            try {
                switch (work->action) {
                case Action::PrepareImport:
                    work->preview = prepare_dictionary_import(file, path);
                    break;
                case Action::Import:
                    apply_dictionary_import(*preview);
                    work->message = L"取り込みを保存しました。既存の順位と注釈は保持しました。";
                    break;
                case Action::Forget:
                    forget_dictionary_entry(file, entry);
                    work->message = L"選択した範囲の語を学習辞書から削除しました。読み取り専用辞書の候補は残ります。";
                    break;
                case Action::Export:
                    export_user_dictionary(file, path);
                    work->message = L"全件を新しいUTF-8 SKKファイルへ書き出しました。\r\n" + path.native();
                    break;
                case Action::Reload:
                    work->message = L"一覧を読み込みました。絞り込みは各列の部分一致です（大文字・小文字を区別）。";
                    break;
                }
                if (work->action == Action::Reload || work->action == Action::Import ||
                    work->action == Action::Forget) {
                    work->reload_attempted = true;
                    try {
                        work->rows = read_dictionary_rows(file);
                        work->rows_ready = true;
                    } catch (const std::exception& error) {
                        if (work->action == Action::Reload)
                            work->message.clear();
                        work->message +=
                            L"\r\n一覧を読み込めません。再読込で確認してください。\r\n" + wide_message(error.what());
                    }
                }
            } catch (const std::exception& error) {
                work->message = L"操作を完了できませんでした。\r\n" + wide_message(error.what());
                if (work->action == Action::Import || work->action == Action::Forget)
                    work->message += L"\r\n通信失敗でも保存済みの場合があります。再操作の前に再読込してください。";
            } catch (...) {
                work->message = L"操作を完了できませんでした。再読込で状態を確認してください。";
            }
            work->done.store(true, std::memory_order_release);
        }).detach();
    } catch (...) {
        KillTimer(manager.window, 1);
        manager.work.reset();
        throw;
    }
    controls(manager);
    status(manager, action == Action::PrepareImport ? L"取り込み内容を検査しています。まだ保存していません。"
                    : action == Action::Reload      ? L"一覧を読み込んでいます…"
                                                    : L"保存処理中です。完了をお待ちください。");
}
void finish(Manager& manager) {
    if (!manager.work || !manager.work->done.load(std::memory_order_acquire))
        return;
    KillTimer(manager.window, 1);
    const auto work = std::move(manager.work);
    if (work->reload_attempted) {
        manager.valid = work->rows_ready;
        replace_rows(manager, std::move(work->rows));
    }
    controls(manager);
    if (work->preview) {
        auto& preview = *work->preview;
        if (preview.request.entries.empty()) {
            status(manager, L"取り込む候補は0件です。学習辞書は変更していません。");
            return;
        }
        const auto message =
            L"この検査済みの内容を学習辞書へ追加しますか？\r\n\r\n" + preview.source.native() + L"\r\n文字コード: " +
            wide_message(encoding_name(preview.encoding)) + L"\r\n候補: " + std::to_wstring(preview.candidates) +
            L"件 / 新規: " + std::to_wstring(preview.added) +
            L"件（検査時点）\r\n\r\n既存の順位と注釈は保持します。他アプリの学習が進むと追加件数は変わります。";
        if (MessageBoxW(manager.window, message.c_str(), L"SKK辞書の取り込み",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            start(manager, Action::Import, {}, {}, std::move(preview));
        else
            status(manager, L"取り込みを取り消しました。学習辞書は変更していません。");
    } else
        status(manager, work->message);
}
INT_PTR CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* manager = reinterpret_cast<Manager*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            manager = reinterpret_cast<Manager*>(lparam);
            manager->window = window;
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            set(window, IDC_STORE, L"学習辞書: " + manager->file.native());
            const auto list = GetDlgItem(window, IDC_WORDS);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            const std::array names{L"読み", L"送りキー", L"送り", L"表記", L"注釈"};
            // Scale the columns to the actual dialog width, including at startup DPI.
            RECT rect{};
            GetClientRect(list, &rect);
            const std::array percentages{24, 10, 14, 25, 23};
            for (std::size_t i = 0; i < names.size(); ++i) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(names[i]);
                column.cx = rect.right * percentages[i] / 100;
                ListView_InsertColumn(list, static_cast<int>(i), &column);
            }
            SendDlgItemMessageW(window, IDC_SEARCH, EM_SETLIMITTEXT, 1024, 0);
            start(*manager, Action::Reload);
            return TRUE;
        }
        if (!manager)
            return FALSE;
        if (message == WM_TIMER) {
            if (wparam == 1)
                finish(*manager);
            else if (wparam == 2) {
                KillTimer(window, 2);
                filter(*manager);
            }
            return TRUE;
        }
        if (message == WM_NOTIFY) {
            const auto header = reinterpret_cast<NMHDR*>(lparam);
            if (header->idFrom != IDC_WORDS)
                return FALSE;
            if (header->code == LVN_GETDISPINFOW) {
                const auto info = reinterpret_cast<NMLVDISPINFOW*>(lparam);
                if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 &&
                    static_cast<std::size_t>(info->item.iItem) < manager->visible.size() && info->item.iSubItem >= 0 &&
                    info->item.iSubItem < 5) {
                    const auto& row = manager->rows[manager->visible[static_cast<std::size_t>(info->item.iItem)]];
                    lstrcpynW(info->item.pszText, row.columns[static_cast<std::size_t>(info->item.iSubItem)].c_str(),
                              info->item.cchTextMax);
                }
                return TRUE;
            }
            if (header->code == LVN_ITEMCHANGED || header->code == LVN_ODSTATECHANGED)
                selection(*manager);
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            if (manager->work && writes(manager->work->action)) {
                status(*manager, L"保存処理中です。完了後に閉じてください。");
                return TRUE;
            }
            KillTimer(window, 1);
            KillTimer(window, 2);
            EndDialog(window, 0);
            return TRUE;
        }
        if (message == WM_COMMAND) {
            const auto id = LOWORD(wparam);
            if (id == IDC_SEARCH && HIWORD(wparam) == EN_CHANGE) {
                if (!SetTimer(window, 2, 150, nullptr))
                    filter(*manager);
            } else if (!manager->work) {
                if (id == IDC_REFRESH)
                    start(*manager, Action::Reload);
                else if (manager->valid && id == IDC_FORGET) {
                    if (const auto row = selected(*manager)) {
                        const auto entry = row->entry;
                        const auto prompt =
                            L"この語を、この送り範囲の学習辞書から削除しますか？\r\n\r\n" + describe(*row) +
                            L"\r\n\r\n読み取り専用辞書の候補は残ります。もう一度確定すると学習されます。";
                        if (MessageBoxW(window, prompt.c_str(), L"学習語の削除",
                                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                            start(*manager, Action::Forget, {}, entry);
                    }
                } else if (manager->valid && (id == IDC_IMPORT || id == IDC_EXPORT)) {
                    if (const auto path = choose_file(window, id == IDC_EXPORT))
                        start(*manager, id == IDC_EXPORT ? Action::Export : Action::PrepareImport, *path);
                }
            }
            return TRUE;
        }
    } catch (const std::exception& error) {
        if (manager) {
            status(*manager, L"操作を完了できませんでした。\r\n" + wide_message(error.what()));
            controls(*manager);
        }
    } catch (...) {
        if (manager)
            status(*manager, L"操作を完了できませんでした。");
    }
    return FALSE;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return 2;
    Manager manager;
    try {
        if (count == 1)
            manager.file = learning_file_path();
        else if (count == 3 && std::wstring_view(arguments[1]) == L"--file")
            manager.file = arguments[2];
        else
            throw std::invalid_argument("nskkzime_dictionary_gui.exe [--file 学習辞書の絶対パス]");
        validate_learning_file(manager.file);
    } catch (const std::exception& error) {
        LocalFree(arguments);
        MessageBoxW(nullptr, wide_message(error.what()).c_str(), L"nskkzime 学習辞書", MB_OK | MB_ICONERROR);
        return 2;
    }
    LocalFree(arguments);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 2;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_DICTIONARY_MANAGER), nullptr, dialog_proc,
                                        reinterpret_cast<LPARAM>(&manager));
    CoUninitialize();
    return result == -1 ? 2 : static_cast<int>(result);
}
