// Game calendar, ported from the original's seg 1 helpers.
//
// The farm simulation counts days in an int (status record +9). One timer
// tick is one day. Months have 30 days, years 360. Arithmetic mirrors the
// original Pascal code exactly, including truncating division, so edge
// cases (day mod 360 == 0) behave the same.
#pragma once

#include <cstdint>

namespace myulk {

enum class Season : std::uint8_t { Winter = 0, Spring = 1, Summer = 2, Autumn = 3 };

struct Calendar {
    static constexpr int kDaysPerMonth = 30;
    static constexpr int kDaysPerYear = 360;
    static constexpr int kNewGameDay = 61;     // 1 March, year 1 (1:0ABF)
    static constexpr int kChapter2Day = 421;   // 1 March, year 2 (11:3C1A)
    static constexpr int kChapter3Day = 781;   // 1 March, year 3 (5:3F21)
    static constexpr int kLastDay = 0x473;     // 1139: day counter stops (1:1BD1)

    // 1-based year (1:11D3).
    static int year(int day);
    // 0-based month, 0 = January (1:0EE8).
    static int month0(int day);
    // 1..30 (1:10E9).
    static int dayOfMonth(int day);
    // Season of the month (1:1A3E).
    static Season season(int day);
    // True on day 1, or when the season differs from yesterday's (1:1B2E).
    static bool seasonChanged(int day);
    // seasonChanged(day + 1) (1:1AF2).
    static bool seasonChangesTomorrow(int day);
};

}  // namespace myulk
