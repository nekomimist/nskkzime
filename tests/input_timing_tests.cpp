#include "input_timing.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace nskkzime::windows;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        TimingSamples samples;
        check(samples.summary().samples == 0, "empty samples reported a measurement");
        for (int i = 1; i <= 100; ++i) samples.add(i);
        auto summary = samples.summary();
        check(summary.total == 100 && summary.p50_ms == 50 && summary.p95_ms == 95 && summary.max_ms == 100, "nearest-rank percentiles incorrect");
        samples.add(-1); samples.add(std::numeric_limits<double>::infinity()); samples.add(std::nan(""));
        check(samples.summary().total == 100, "invalid durations counted");
        for (std::size_t i = 0; i < TimingSamples::capacity; ++i) samples.add(2);
        summary = samples.summary();
        check(summary.samples == TimingSamples::capacity && summary.total == TimingSamples::capacity + 100 && summary.max_ms == 2, "ring retained old samples or lost total count");
        InputTimingToken popup;
        { KeyTiming key; replace_candidate_timing(popup); finish_candidate_timing(popup); }
        check(!input_timing_snapshot().enabled && !input_timing_snapshot().key.total, "disabled timing collected data");
        enable_input_timing(true);
        {
            KeyTiming outer;
            replace_candidate_timing(popup);
            const auto saved = popup;
            replace_candidate_timing(popup);
            check(input_timing_snapshot().pending_paints == 1 && !input_timing_snapshot().discarded_paints, "same update counted twice");
            { KeyTiming inner; InputTimingToken nested; replace_candidate_timing(nested); finish_candidate_timing(nested); }
            InputTimingToken resumed; replace_candidate_timing(resumed);
            check(resumed == saved, "nested callback lost outer key identity");
            discard_candidate_timing(resumed);
        }
        finish_candidate_timing(popup); finish_candidate_timing(popup);
        auto snapshot = input_timing_snapshot();
        check(snapshot.key.total == 2 && snapshot.candidate_paint.total == 2 && snapshot.pending_paints == 0 && snapshot.discarded_paints == 1, "asynchronous paint or duplicate completion counted incorrectly");
        {
            KeyTiming key; replace_candidate_timing(popup);
        }
        // A later asynchronous model response must not inherit the old key.
        replace_candidate_timing(popup);
        check(!popup.id && input_timing_snapshot().discarded_paints == 2, "async replacement retained old key");
        {
            KeyTiming key; replace_candidate_timing(popup); enable_input_timing(true);
        }
        finish_candidate_timing(popup);
        check(!input_timing_snapshot().key.total && !input_timing_snapshot().candidate_paint.total && !input_timing_snapshot().pending_paints, "reset admitted old callback or paint");
        bool isolated = false;
        std::thread other([&] { isolated = !input_timing_snapshot().enabled; enable_input_timing(true); KeyTiming key; });
        other.join();
        check(isolated && !input_timing_snapshot().key.total, "thread diagnostics leaked across threads");
        const auto phase_count = [](InputTimingPhase phase) {
            return input_timing_snapshot().phases[static_cast<std::size_t>(phase)].total;
        };
        { InputPhaseTiming asynchronous(InputTimingPhase::Core); }
        check(phase_count(InputTimingPhase::Core) == 0, "operation outside a key callback counted");
        {
            KeyTiming outer;
            InputPhaseTiming request(InputTimingPhase::EditRequest);
            {
                InputPhaseTiming core(InputTimingPhase::Core);
                core.finish(); core.finish();
            }
            { KeyTiming inner; InputPhaseTiming nested(InputTimingPhase::Core); }
        }
        check(phase_count(InputTimingPhase::Core) == 2 && phase_count(InputTimingPhase::EditRequest) == 1,
              "reentrant operation lost parent or counted explicit finish twice");
        check(input_timing_status().find(L"入力コア: n=2") != std::wstring::npos, "operation diagnostics absent");
        std::optional<InputPhaseTiming> late;
        { KeyTiming key; late.emplace(InputTimingPhase::Selection); }
        late.reset();
        check(phase_count(InputTimingPhase::Selection) == 0, "operation outliving its key counted");
        {
            KeyTiming key;
            InputPhaseTiming invalid(InputTimingPhase::Count);
            InputPhaseTiming old(InputTimingPhase::Core);
            enable_input_timing(false); enable_input_timing(true);
        }
        check(phase_count(InputTimingPhase::Core) == 0 && !input_timing_snapshot().key.total,
              "operation survived a reset");
        enable_input_timing(false);
        { KeyTiming key; InputPhaseTiming disabled(InputTimingPhase::Core); }
        check(!input_timing_snapshot().enabled && input_timing_status().find(L"オフ") != std::wstring::npos, "disable retained data");
        std::cout << "PASS bounded percentiles, opt-in key/operation timing, deferred paints, reentry/reset and thread isolation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
