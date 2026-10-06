#include "dictionary_cache.h"
#include "learning_ipc.h"
#include "local_storage.h"
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F run) {
    bool failed = false;
    try {
        run();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed, "operation unexpectedly succeeded");
}
struct Files {
    std::filesystem::path root = std::filesystem::current_path() / ("dictionary-cache-test-" + learning_client_id());
    std::vector<std::filesystem::path> files, dirs;
    Files() { std::filesystem::create_directory(root); }
    std::filesystem::path path(const wchar_t *name) {
        auto value = root / name;
        files.push_back(value);
        return value;
    }
    std::filesystem::path directory(const wchar_t *name) {
        auto value = root / name;
        std::filesystem::create_directory(value);
        dirs.push_back(value);
        return value;
    }
    ~Files() {
        std::error_code error;
        for (const auto &file : files)
            std::filesystem::remove(file, error);
        for (const auto &dir : dirs) {
            for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
                const auto name = it->path().filename().native();
                if (name == L"build.lock" || name == L"keep.txt" || (name.size() == 73 && name.ends_with(L"-v1.bin")))
                    std::filesystem::remove(it->path(), error);
            }
            std::filesystem::remove(dir, error);
        }
        std::filesystem::remove(root, error);
    }
};
struct Child {
    NativeHandle handle;
    Child(const std::filesystem::path &executable, const std::filesystem::path &source,
          const std::filesystem::path &cache, const std::filesystem::path &output, bool hold = false, bool lock = false)
        : handle([&] {
              auto command = L"\"" + executable.native() + (lock ? L"\" --lock \"" : L"\" --child \"") +
                             source.native() + L"\" \"" + cache.native() + L"\" \"" + output.native() + L"\"" +
                             (hold ? L" hold" : L"");
              STARTUPINFOW start{};
              start.cb = sizeof(start);
              PROCESS_INFORMATION process{};
              check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &start, &process) != 0,
                    "child launch failed");
              CloseHandle(process.hThread);
              return process.hProcess;
          }()) {}
    void wait() {
        check(WaitForSingleObject(handle.get(), 15000) == WAIT_OBJECT_0, "child timed out");
        DWORD code = 1;
        GetExitCodeProcess(handle.get(), &code);
        check(!code, "child failed");
    }
    void stop() {
        check(TerminateProcess(handle.get(), 99) != 0 && WaitForSingleObject(handle.get(), 2000) == WAIT_OBJECT_0,
              "cannot stop test child");
    }
    ~Child() {
        if (WaitForSingleObject(handle.get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(handle.get(), 99);
            WaitForSingleObject(handle.get(), 2000);
        }
    }
};
int number(const std::filesystem::path &file) {
    std::ifstream stream(file);
    int value = -1;
    stream >> value;
    check(static_cast<bool>(stream), "missing child result");
    return value;
}
void check_word(const CachedDictionary &dictionary, std::u32string_view value = U"漢字") {
    const auto list = dictionary.dictionary->lookup({U"かんじ"});
    check(list.size() == 1 && list[0].text == value && list[0].annotation == U"文字", "cached candidate changed");
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc >= 5 && std::wstring_view(argv[1]) == L"--lock")
        try {
            const auto lock_file = std::filesystem::path(argv[3]) / L"build.lock";
            NativeHandle lock(CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
            OVERLAPPED position{};
            check(lock.valid() && LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &position),
                  "child lock failed");
            write_file_atomic(argv[4], "0");
            Sleep(INFINITE);
            return 1;
        } catch (...) {
            return 1;
        }
    if (argc >= 5 && std::wstring_view(argv[1]) == L"--child")
        try {
            const auto value = load_cached_dictionary(argv[2], argv[3]);
            check_word(value);
            {
                std::ofstream out(argv[4]);
                out << static_cast<int>(value.use);
            }
            if (argc > 5)
                Sleep(1000);
            return 0;
        } catch (...) {
            return 1;
        }
    Files files;
    const auto source = files.path(L"辞書 with space.skk");
    write_file_atomic(source, utf8(U"かんじ /漢字;文字/\nかk /書/[く/描/]/\n"));
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    int failed = 0;
    const std::vector<std::pair<const char *, std::function<void()>>> cases = {
        {"build-share-across-processes-reopen-from-disk",
         [&] {
             const auto root = files.directory(L"shared");
             std::filesystem::path artifact;
             {
                 const auto first = load_cached_dictionary(source, root);
                 check_word(first);
                 check(first.use == DictionaryCacheUse::Created, "cache was not built");
                 artifact = first.file;
                 const auto second = load_cached_dictionary(source, root);
                 check_word(second);
                 check(second.use == DictionaryCacheUse::Shared, "section not reused");
                 const auto out = files.path(L"shared-result.txt");
                 Child child(self, source, root, out);
                 child.wait();
                 check(number(out) == static_cast<int>(DictionaryCacheUse::Shared), "child did not share section");
             }
             const auto disk = load_cached_dictionary(source, root);
             check_word(disk);
             check(disk.use == DictionaryCacheUse::Disk && disk.file == artifact, "disk cache not reused");
         }},
        {"concurrent-processes-build-once",
         [&] {
             const auto root = files.directory(L"concurrent"), one = files.path(L"one.txt"),
                        two = files.path(L"two.txt");
             Child first(self, source, root, one, true), second(self, source, root, two, true);
             first.wait();
             second.wait();
             const auto a = number(one), b = number(two);
             const int made = static_cast<int>(DictionaryCacheUse::Created);
             check((a == made) != (b == made), "concurrent processes both rebuilt or failed");
             check(a != static_cast<int>(DictionaryCacheUse::Fallback) &&
                       b != static_cast<int>(DictionaryCacheUse::Fallback),
                   "concurrent cache fell back");
         }},
        {"content-change-preserves-live-old-snapshot",
         [&] {
             const auto root = files.directory(L"changed");
             const auto file = files.path(L"changing.skk");
             write_file_atomic(file, utf8(U"かんじ /漢字;文字/\n"));
             const auto time = std::filesystem::last_write_time(file);
             const auto before = load_cached_dictionary(file, root);
             write_file_atomic(file, utf8(U"かんじ /感じ;文字/\n"));
             std::filesystem::last_write_time(file, time);
             const auto after = load_cached_dictionary(file, root);
             check_word(before);
             check_word(after, U"感じ");
             check(after.file != before.file && after.use == DictionaryCacheUse::Created,
                   "same-size/mtime edit used stale data");
         }},
        {"corrupt-file-rebuild-and-source-required",
         [&] {
             const auto root = files.directory(L"corrupt");
             const auto file = files.path(L"corrupt-source.skk");
             write_file_atomic(file, utf8(U"かんじ /漢字;文字/\n"));
             std::filesystem::path artifact;
             {
                 const auto value = load_cached_dictionary(file, root);
                 artifact = value.file;
             }
             write_file_atomic(artifact, "damaged");
             {
                 const auto rebuilt = load_cached_dictionary(file, root);
                 check_word(rebuilt);
                 check(rebuilt.use == DictionaryCacheUse::Created, "corrupt cache not rebuilt");
             }
             std::filesystem::remove(file);
             rejects([&] { load_cached_dictionary(file, root); });
         }},
        {"unwritable-cache-falls-back",
         [&] {
             const auto root = files.path(L"not-a-directory");
             write_file_atomic(root, "occupied");
             const auto value = load_cached_dictionary(source, root);
             check_word(value);
             check(value.use == DictionaryCacheUse::Fallback, "invalid cache directory not bypassed");
         }},
        {"stalled-builder-fallback-and-crashed-builder-recovery",
         [&] {
             const auto root = files.directory(L"locked"), ready = files.path(L"lock-ready.txt");
             Child child(self, source, root, ready, false, true);
             const auto deadline = GetTickCount64() + 3000;
             std::optional<int> ready_value;
             // Existence alone does not guarantee the file can yet be opened
             // and parsed as the child's complete readiness message.
             // Wait for the complete message, but fail promptly if the child exits.
             while (!ready_value && GetTickCount64() < deadline) {
                 try { ready_value = number(ready); } catch (const std::exception&) {}
                 if (ready_value) break;
                 check(WaitForSingleObject(child.handle.get(), 0) == WAIT_TIMEOUT, "lock child exited before readiness");
                 Sleep(10);
             }
             check(ready_value && *ready_value == 0, "builder did not report its acquired lock");
             const auto before = GetTickCount64();
             const auto fallback = load_cached_dictionary(source, root);
             check_word(fallback);
             check(fallback.use == DictionaryCacheUse::Fallback && GetTickCount64() - before < 10000,
                   "stalled builder blocked normal parsing");
             child.stop();
             const auto recovered = load_cached_dictionary(source, root);
             check_word(recovered);
             check(recovered.use == DictionaryCacheUse::Created, "dead builder left cache unusable");
         }},
        {"encoding-scopes-report-roundtrip",
         [&] {
             const auto root = files.directory(L"encoding");
             const auto euc = files.path(L"euc.skk");
             write_file_atomic(euc, ";; coding: euc-jp\nmark /\x8f\xb0\xa1;\x8f\xa2\xaf/\nbroken\n");
             for (int i = 0; i < 2; ++i) {
                 const auto value = load_cached_dictionary(euc, root);
                 const auto list = value.dictionary->lookup({U"mark"});
                 check(list.size() == 1 && list[0].text == U"丂" && list[0].annotation == U"˘",
                       "SS3 cache changed Unicode");
                 check(value.report.encoding == DictionaryEncoding::EucJp && value.report.malformed_lines == 1 &&
                           !value.report.diagnostics.empty(),
                       "cached report changed");
             }
             const auto value = load_cached_dictionary(source, root);
             const auto list = value.dictionary->lookup({U"か", 'k'}, U"く");
             check(list.size() == 2 && list[0].text == U"描" && list[1].text == U"書", "okuri scope order changed");
             rejects([&] { load_cached_dictionary(euc, root, DictionaryEncoding::Utf8); });
         }},
        {"bounded-pruning-preserves-unrelated-file", [&] {
             const auto root = files.directory(L"prune");
             write_file_atomic(root / L"keep.txt", "unrelated");
             for (int i = 0; i < 65; ++i) {
                 const auto value = std::to_wstring(i);
                 write_file_atomic(root / (std::wstring(64 - value.size(), L'0') + value + L"-0-v1.bin"), "old");
             }
             {
                 const auto current = load_cached_dictionary(source, root);
                 check_word(current);
             }
             std::size_t count = 0;
             for (const auto &entry : std::filesystem::directory_iterator(root))
                 if (entry.path().extension() == L".bin")
                     ++count;
             check(count <= 64 && std::filesystem::exists(root / L"keep.txt"), "cache pruning exceeded scope or limit");
         }}};
    for (const auto &[name, run] : cases)
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    return failed ? 1 : 0;
}
