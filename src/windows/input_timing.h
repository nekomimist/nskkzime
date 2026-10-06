#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace nskkzime::windows {
// Diagnostics contain durations only, never keys, readings or document text.
struct TimingSummary {
    std::uint64_t total = 0;
    std::size_t samples = 0;
    double p50_ms = 0, p95_ms = 0, max_ms = 0;
};
class TimingSamples {
public:
    static constexpr std::size_t capacity = 2048;
    void add(double milliseconds) noexcept;
    TimingSummary summary() const;
private:
    std::array<double, capacity> values_{};
    std::uint64_t count_ = 0;
};
struct InputTimingSnapshot {
    bool enabled = false;
    TimingSummary key, candidate_paint;
    std::uint64_t pending_paints = 0, discarded_paints = 0;
};
struct InputTimingToken {
    std::uint64_t epoch = 0, id = 0;
    std::int64_t start = 0;
    bool operator==(const InputTimingToken&) const = default;
};
bool enable_input_timing(bool) noexcept; // Resets samples/tokens; false only if allocation fails.
InputTimingSnapshot input_timing_snapshot();
std::wstring input_timing_status();
class KeyTiming {
public:
    KeyTiming() noexcept;
    ~KeyTiming();
    KeyTiming(const KeyTiming&) = delete;
    KeyTiming& operator=(const KeyTiming&) = delete;
private:
    InputTimingToken token_, previous_;
};
// A popup owns its pending token until paint, replacement, hiding or destruction.
void replace_candidate_timing(InputTimingToken&) noexcept;
void discard_candidate_timing(InputTimingToken&) noexcept;
void finish_candidate_timing(InputTimingToken&) noexcept;
} // namespace nskkzime::windows
