// F12 screenshots (QoL).
#pragma once

#include <string>

#include "recomp/win16/gdi.hpp"

namespace win16 {

// Saves `s` as a PNG; returns where it went ("" on failure).
std::string save_screenshot(const Surface& s);

}  // namespace win16
