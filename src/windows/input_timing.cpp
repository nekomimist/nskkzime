#include "input_timing.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>

namespace nskkzime::windows {
namespace {
struct State {
    std::uint64_t epoch = 0, next = 0, pending = 0, discarded = 0;
    InputTimingToken active;
    TimingSamples key, paint;
    std::array<TimingSamples, input_timing_phase_count> phases;
};
// An app may have many threads. Allocate the arrays only on explicit opt-in,
// never in a key callback or for every thread which loads this DLL.
thread_local std::unique_ptr<State> state;
thread_local std::uint64_t epoch = 0;
std::int64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool valid(const InputTimingToken& token) noexcept {
    return state && token.id && token.epoch == state->epoch;
}
double elapsed(const InputTimingToken& token) noexcept { return static_cast<double>(now() - token.start) / 1000000.0; }
}
void TimingSamples::add(double milliseconds) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds < 0) return;
    values_[count_ % capacity] = milliseconds;
    ++count_;
}
TimingSummary TimingSamples::summary() const {
    TimingSummary result; result.total = count_;
    result.samples = static_cast<std::size_t>(std::min<std::uint64_t>(count_, capacity));
    if (!result.samples) return result;
    auto sorted = values_;
    std::sort(sorted.begin(), sorted.begin() + result.samples);
    result.p50_ms = sorted[(result.samples * 50 + 99) / 100 - 1];
    result.p95_ms = sorted[(result.samples * 95 + 99) / 100 - 1];
    result.max_ms = sorted[result.samples - 1];
    return result;
}
bool enable_input_timing(bool enabled) noexcept {
    state.reset();
    if (!enabled) return true;
    try { state = std::make_unique<State>(); state->epoch = ++epoch; return true; }
    catch (...) { return false; }
}
InputTimingSnapshot input_timing_snapshot() {
    if (!state) return {};
    InputTimingSnapshot result{true, state->key.summary(), state->paint.summary(), state->pending, state->discarded};
    for (std::size_t i = 0; i < result.phases.size(); ++i) result.phases[i] = state->phases[i].summary();
    return result;
}
KeyTiming::KeyTiming() noexcept {
    if (!state) return;
    previous_ = state->active;
    token_ = {state->epoch, ++state->next, now()};
    state->active = token_;
}
KeyTiming::~KeyTiming() {
    if (!valid(token_)) return;
    state->key.add(elapsed(token_));
    state->active = previous_;
}
InputPhaseTiming::InputPhaseTiming(InputTimingPhase phase) noexcept : phase_(phase) {
    if (!state || !valid(state->active) || static_cast<std::size_t>(phase) >= input_timing_phase_count) return;
    key_ = state->active; start_ = now();
}
InputPhaseTiming::~InputPhaseTiming() { finish(); }
void InputPhaseTiming::finish() noexcept {
    if (valid(key_) && state->active == key_)
        state->phases[static_cast<std::size_t>(phase_)].add(static_cast<double>(now() - start_) / 1000000.0);
    key_ = {};
}
void discard_candidate_timing(InputTimingToken& token) noexcept {
    if (valid(token)) { --state->pending; ++state->discarded; }
    token = {};
}
void replace_candidate_timing(InputTimingToken& token) noexcept {
    if (valid(token) && token == state->active) return;
    discard_candidate_timing(token);
    if (state && valid(state->active)) { token = state->active; ++state->pending; }
}
void finish_candidate_timing(InputTimingToken& token) noexcept {
    if (valid(token)) { state->paint.add(elapsed(token)); --state->pending; }
    token = {};
}
std::wstring input_timing_status() {
    if (!state) return L"遅延計測: オフ（本文・キーは収集しません）";
    const auto data = input_timing_snapshot();
    std::wostringstream out; out.imbue(std::locale::classic()); out << std::fixed << std::setprecision(3);
    const auto line = [&](const wchar_t* label, const TimingSummary& value) {
        out << label << L": n=" << value.samples << L"/累計" << value.total;
        if (value.samples) out << L"  p50=" << value.p50_ms << L"  p95=" << value.p95_ms << L"  max=" << value.max_ms << L" ms";
        else out << L"  未計測";
    };
    line(L"TSF OnKeyDown", data.key); out << L"\r\n";
    line(L"同キー開始→候補WM_PAINT終了", data.candidate_paint);
    out << L"\r\n描画待ち=" << data.pending_paints << L"  描画前の破棄=" << data.discarded_paints
        << L"  各直近2048件・OSキー配送/画面提示は対象外";
    constexpr std::array<const wchar_t*, input_timing_phase_count> labels{
        L"受付判定", L"編集要求（コールバックを含む）", L"編集コールバック", L"既存compositionの照合",
        L"入力コア", L"TSF本文編集", L"SetText/InsertText", L"表示属性", L"選択位置", L"候補更新", L"モード同期"};
    out << L"\r\n処理内訳: 呼出し単位・内側を含むため合算不可";
    for (std::size_t i = 0; i < data.phases.size(); ++i) { out << L"\r\n"; line(labels[i], data.phases[i]); }
    return out.str();
}
} // namespace nskkzime::windows
