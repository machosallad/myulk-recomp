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

// Gameplay helpers (QoL), each on or off; all default on. The play style is
// derived from them: Original = all off and the farm at 1x, Modern = all on.
inline constexpr const char* kHelpers[] = {"click_skips", "autosave", "quick_save", "save_screen",
                                           "start_card",  "nav_keys", "hints",      "farm_helper"};
inline bool helper(const char* key) { return get_int(key, 1) != 0; }
enum Style { kOriginal = 0, kModern = 1, kCustom = 2 };
int style();
void set_style(int style);   // kOriginal or kModern: sets every helper (and the farm speed for Original)
bool style_chosen();         // the first-start question has been answered

}  // namespace settings
