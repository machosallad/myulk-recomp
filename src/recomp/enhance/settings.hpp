// Player settings that persist between sessions (key=value text file in the
// user's app-data folder; in the browser under /saves, kept in IndexedDB).
#pragma once

#include <string>

namespace settings {

int get_int(const std::string& key, int def);
void set_int(const std::string& key, int value);   // saves the file

// Picture scaling: how the 640x480 picture is enlarged to the window.
enum Scaling { kSharpPixels = 0, kSharpSmooth = 1, kSmooth = 2, kInteger = 3, kScalingCount = 4 };
inline int scaling() { return get_int("scaling", kSharpSmooth); }
// Text: anti-aliased (0) or crisp like the original raster fonts (1).
inline bool crisp_text() { return get_int("crisp_text", 0) != 0; }
// De-dither: smooth away the art's dither speckle (enhance/dedither.cpp).
inline bool dedither() { return get_int("dedither", 0) != 0; }

}  // namespace settings
