// Minimal assertion helpers for tests registered with CTest.
#pragma once

#include <iostream>

namespace test_util {

inline int& failures() {
    static int count = 0;
    return count;
}

// CTest treats this exit code as "skipped" (see SKIP_RETURN_CODE).
constexpr int kSkip = 77;

inline int finish() {
    if (failures() > 0) {
        std::cerr << failures() << " check(s) failed\n";
        return 1;
    }
    return 0;
}

}  // namespace test_util

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ")" \
                      << " failed\n";                                        \
            ++test_util::failures();                                         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const auto& a_ = (actual);                                            \
        const auto& e_ = (expected);                                          \
        if (!(a_ == e_)) {                                                    \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #actual \
                      << ", " #expected ") failed\n  actual:   " << a_        \
                      << "\n  expected: " << e_ << "\n";                      \
            ++test_util::failures();                                          \
        }                                                                     \
    } while (0)
