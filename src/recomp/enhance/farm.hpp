// Farm helper (QoL, setting "farm_helper"): click the day clock (or press N)
// for the next day, short notes when the balance changes or a season begins.
// P (pause) belongs to it too (display.cpp).
#pragma once

namespace win16::farm {

void pump();                    // every host_pump: watch balance and date
bool click(int x, int y);       // true when the click was on the day clock (swallow it)
bool next_day();                // N key; false when the farm is not in front

}  // namespace win16::farm
