// Keyboard and mouse shortcuts for moving around the adventure scenes (QoL):
// arrow keys / WASD turn and walk, Backspace, right-click and the mouse wheel
// go back. Each one clicks where the game itself would expect the click, so
// the game's own scene code runs.
#pragma once

#include <SDL3/SDL.h>

namespace win16::nav {

bool key(SDL_Keycode key);            // true when the key moved the player
bool right_click(int x, int y);       // true when it went back (swallow the click)
bool wheel(float dy);                 // up: forward, down: back
void mouse_at(int x, int y);          // the real cursor, restored after a click

}  // namespace win16::nav
