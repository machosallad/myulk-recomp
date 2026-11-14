// Ported scene procedures. Each function is named after the original
// routine's address (seg_offset) so it can be compared with decomp/ output.
#pragma once

#include "adventure/engine.hpp"

namespace myulk::adv::scenes {

// Intro sequence (TAdvForm, segment 2). Runs the opening movies and the
// three postcard pictures, then enters the first scene.
void startIntro(Engine& e);

// Chapter 1 opening area, "nbae.dll" (segment 18).
void s18_2EC0(Engine& e);   // AE1  - start: in front of the farm
void s18_3001(Engine& e);   //        hotspot: first time through (plays ae1.avi)
void s18_306F(Engine& e);   // AE1B - turn left
void s18_3105(Engine& e);   // AE1C - turn right
void s18_3162(Engine& e);   // AE2  - close-up hotspot
void s18_31CA(Engine& e);   // AE3
void s18_31F8(Engine& e);   // AE4
void s18_3258(Engine& e);   // AE5
void s18_32AD(Engine& e);   // AE6
void s18_3319(Engine& e);   //        first time through AE6 (plays ae6.avi)
void s18_335F(Engine& e);   // AE7
void s18_33DC(Engine& e);   // AE9

// Not ported yet: logs and stays in the current scene.
void s06_009A(Engine& e);

}  // namespace myulk::adv::scenes
