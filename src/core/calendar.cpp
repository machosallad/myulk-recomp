#include "core/calendar.hpp"

namespace myulk {

// C++ integer division truncates toward zero, like Pascal's div.

int Calendar::year(int day) { return day / kDaysPerYear + 1; }

int Calendar::month0(int day) {
    // Original: uVar1 = (day mod 360 - 1) div 30 + 1, then counts up from 1
    // to uVar1, returning the number of steps, i.e. uVar1 - 1.
    return (day % kDaysPerYear - 1) / kDaysPerMonth;
}

int Calendar::dayOfMonth(int day) {
    int d = day % kDaysPerMonth;
    return d == 0 ? kDaysPerMonth : d;
}

Season Calendar::season(int day) {
    switch (month0(day)) {
        case 0: case 1: case 11: return Season::Winter;
        case 2: case 3: case 4:  return Season::Spring;
        case 5: case 6: case 7:  return Season::Summer;
        default:                 return Season::Autumn;   // 8, 9, 10
    }
}

bool Calendar::seasonChanged(int day) {
    if (day == 1) return true;
    return season(day - 1) != season(day);
}

bool Calendar::seasonChangesTomorrow(int day) { return seasonChanged(day + 1); }

}  // namespace myulk
