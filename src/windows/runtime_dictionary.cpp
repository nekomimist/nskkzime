#include "runtime_dictionary.h"
#include "dictionary_settings.h"
#include "learning_client.h"
#include <windows.h>
#include <mutex>

namespace nskkzime::windows {
namespace {
std::mutex mutex;
std::wstring cached_key, status = L"辞書: まだ読み込んでいません。";
std::weak_ptr<const Dictionary> cached;
}
std::shared_ptr<const Dictionary> runtime_dictionary() {
    const std::lock_guard lock(mutex);
    try {
        const auto configuration = current_dictionary_configuration();
        auto key = std::wstring(configuration.demo ? L"demo\n" : configuration.environment ? L"env\n" : L"file\n") + configuration.file.native();
        for (const auto& path : configuration.settings.dictionaries) key += L"\n" + path.native();
        key += configuration.settings.learning ? L"\non" : L"\noff";
        const auto learning_override = environment_value(L"NSKKZIME_LEARNING");
        const bool learning_enabled = learning_override ? *learning_override == L"1" : configuration.settings.learning;
        key += learning_enabled ? L"\nlearn" : L"\nno-learn";
        if (const auto file = environment_value(L"NSKKZIME_USER_DICTIONARY")) key += L"\n" + *file;
        // Reuse an immutable snapshot while another TIP in this process uses it.
        if (key == cached_key) if (auto dictionary = cached.lock()) return dictionary;
        auto loaded = load_dictionaries(configuration);
        // A broken, disabled, or unconfigured dictionary must not start learning.
        if (!configuration.demo && !configuration.settings.dictionaries.empty() && learning_enabled && !loaded.failed) {
            try { loaded.dictionary = with_learning(loaded.dictionary); }
            catch (...) { loaded.status += L"\r\n学習: 初期化に失敗（通常の変換は利用できます）"; }
        } else loaded.status += L"\r\n学習: 無効、または辞書未設定・読み込み失敗のため停止";
        cached_key = std::move(key); cached = loaded.dictionary; status = loaded.status;
        if (loaded.failed) { OutputDebugStringW(status.c_str()); OutputDebugStringW(L"\n"); }
        return loaded.dictionary;
    } catch (const std::exception&) {
        // Do not interpret potentially non-UTF-8 exception text in the UI.
        status = L"辞書設定を読み込めません。nskkzime_config --check で設定を確認してください。かな入力は利用できます。";
        cached_key.clear(); cached.reset();
        OutputDebugStringW(status.c_str()); OutputDebugStringW(L"\n");
        return std::make_shared<MemoryDictionary>();
    }
}
std::wstring runtime_dictionary_status() {
    const std::lock_guard lock(mutex);
    if (const auto dictionary = cached.lock()) {
        if (const auto* learning = dynamic_cast<const LearningDictionary*>(dictionary.get())) return status + L"\r\n" + learning->status();
    }
    return status;
}
}
