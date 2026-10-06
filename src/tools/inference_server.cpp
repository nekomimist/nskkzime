#include "inference_ipc.h"
#include "dictionary_settings.h"
#include "cpu_requirements.h"
#include "nskkzime/romaji.h"
#include <algorithm>
#include <iostream>
#include <set>
#ifdef NSKKZIME_WITH_ZENZ
#include "zenz_backend.h"
#endif
using namespace nskkzime;
using namespace nskkzime::windows;
namespace {
#ifdef NSKKZIME_WITH_ZENZ
constexpr auto backend = "zenz";
#else
constexpr auto backend = "dummy";
#endif
void help() {
    std::cout << "nkkz-ime-server --dummy --pipe-token TOKEN [--delay-ms N] [--idle-ms N]\n"
        "nkkz-ime-server --model PATH [--stdio | --pipe-token TOKEN] [--threads N] [--timeout-ms N]\n"
        "nkkz-ime-server --model PATH --convert KANA [--left TEXT] [--right TEXT] [-n N] [--score TEXT]...\n"
        "--version prints the protocol and compiled backend. Output is UTF-8 JSON Lines.\n"
        "Product builds include zenz; --dummy is for tests. Default threads: 4; request timeout: 1000 ms.\n";
}
unsigned number(std::wstring_view text, unsigned maximum) {
    if (text.empty() || !std::all_of(text.begin(),text.end(),[](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
        throw std::invalid_argument("invalid numeric option");
    const auto value = std::stoul(std::wstring(text));
    if (value > maximum) throw std::invalid_argument("numeric option exceeds limit");
    return static_cast<unsigned>(value);
}
#ifdef NSKKZIME_WITH_ZENZ
int stdio_server(ZenzBackend& model, unsigned timeout) {
    std::cout << inference_hello(backend) << std::endl;
    for (;;) {
        std::string line; bool overflow = false; char ch = 0;
        while (std::cin.get(ch) && ch != '\n') {
            if (line.size() < inference_max_line+1) line += ch; else overflow = true;
        }
        if (line.empty() && !overflow && !std::cin) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() && !overflow) continue;
        std::uint64_t id = 0;
        try {
            if (overflow) throw std::invalid_argument("oversized request");
            const auto request = decode_request(line); id = request.id;
            const auto deadline = GetTickCount64()+timeout;
            std::cout << encode_response(model.run(request,[deadline] { return GetTickCount64() >= deadline; })) << std::endl;
        } catch (...) {
            std::cout << "{\"id\":" << (id ? std::to_string(id) : "null") << ",\"error\":\"invalid request, timeout or inference failure\"}" << std::endl;
        }
        if (!std::cin) break;
    }
    return 0;
}
#endif
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::wstring_view(argv[1]) == L"--help")) { help(); return 0; }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--version") { std::cout << inference_hello(backend) << '\n'; return 0; }
        bool dummy = false, standard = false, convert = false;
        std::filesystem::path model; std::wstring token; unsigned delay = 150, idle = 10000, timeout = 1000, threads = 4;
        InferenceRequest request; request.id = 1; request.count = 1;
        std::set<std::wstring> seen;
        for (int i = 1; i < argc; ++i) {
            const std::wstring option(argv[i]);
            if (option != L"--score" && !seen.insert(option).second) throw std::invalid_argument("duplicate option");
            if (option == L"--dummy") { dummy = true; continue; }
            if (option == L"--stdio") { standard = true; continue; }
            if (++i == argc) throw std::invalid_argument("missing option value");
            const std::wstring_view value(argv[i]);
            if (option == L"--model") model = value;
            else if (option == L"--pipe-token") { token = value; (void)inference_pipe_name(token); }
            else if (option == L"--delay-ms") delay = number(value,10000);
            else if (option == L"--idle-ms") idle = number(value,60000);
            else if (option == L"--timeout-ms") timeout = number(value,10000);
            else if (option == L"--threads") threads = number(value,64);
            else if (option == L"-n") request.count = number(value,8);
            else if (option == L"--convert") { request.reading = from_utf8(utf8_message(value)); convert = true; }
            else if (option == L"--left") request.left = from_utf8(utf8_message(value));
            else if (option == L"--right") request.right = from_utf8(utf8_message(value));
            else if (option == L"--score") {
                request.operation = InferenceOperation::Score; request.candidates.push_back(from_utf8(utf8_message(value)));
                if (request.candidates.size() > 64) throw std::invalid_argument("at most 64 scores per request");
            } else throw std::invalid_argument("unknown option");
        }
        if (idle < 100 || !timeout || !threads || !request.count || (standard && (!token.empty() || convert)) || (!token.empty() && convert))
            throw std::invalid_argument("invalid or conflicting options");
        if (dummy) {
            if (!model.empty() || standard || convert || token.empty() || request.operation == InferenceOperation::Score || seen.contains(L"--threads") || seen.contains(L"--timeout-ms") || seen.contains(L"--left") || seen.contains(L"--right") || seen.contains(L"-n"))
                throw std::invalid_argument("dummy mode requires only pipe options");
            return serve_dummy_inference(token,delay,idle);
        }
        if (model.empty() || seen.contains(L"--delay-ms")) throw std::invalid_argument("model path required");
        if (!convert && (seen.contains(L"--left") || seen.contains(L"--right") || request.operation == InferenceOperation::Score || seen.contains(L"-n")))
            throw std::invalid_argument("conversion options require --convert");
#ifdef NSKKZIME_WITH_ZENZ
        if (convert) (void)encode_request(request);
        require_zenz_cpu(current_cpu_features());
        const auto instance = token.empty() ? NativeHandle{} : claim_inference_server(token);
        if (!token.empty() && !instance.valid()) return 0; // Concurrent startup already owns this model endpoint.
        ZenzBackend inference(model,static_cast<int>(threads));
        if (!token.empty()) return serve_inference(token,[&](const auto& input,const auto& cancel) { return inference.run(input,cancel); },backend,idle,timeout);
        if (!convert) return stdio_server(inference,timeout);
        const auto deadline = GetTickCount64()+timeout;
        std::cout << encode_response(inference.run(request,[deadline] { return GetTickCount64() >= deadline; })) << '\n'; return 0;
#else
        throw std::runtime_error("this build has no zenz backend; use scripts/build.ps1 -Zenz");
#endif
    } catch (const std::exception& error) { std::cerr << "inference server: " << error.what() << '\n'; return 2; }
}
