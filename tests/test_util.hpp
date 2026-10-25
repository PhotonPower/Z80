#pragma once

// Tiny dependency-free test helpers (no external test framework required).

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "z80/format.hpp"
#include "z80/types.hpp"

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
        }                                                                            \
    } while (0)

#define CHECK_EQ(actual, expected)                                                   \
    do {                                                                             \
        ++g_checks;                                                                  \
        const auto a_ = (actual);                                                    \
        const auto e_ = (expected);                                                  \
        if (!(a_ == e_)) {                                                           \
            ++g_failures;                                                            \
            std::cerr << __FILE__ << ":" << __LINE__ << ": " #actual " == " #expected \
                      << "\n   actual:   " << a_ << "\n   expected: " << e_ << "\n"; \
        }                                                                            \
    } while (0)

inline std::string hexBytes(const std::vector<z80::Byte>& bytes) {
    std::string s;
    for (z80::Byte b : bytes) {
        if (!s.empty()) s += ' ';
        s += z80::hexDigits(b, 2);
    }
    return s.empty() ? "(none)" : s;
}

inline int finish(const char* name) {
    std::cout << name << ": " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
