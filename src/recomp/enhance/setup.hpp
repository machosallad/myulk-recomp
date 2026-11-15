// First start without game files: "choose your Myulk disc". The player picks
// a CD image (.iso, .bin, .cue), a CD drive or a folder with the CD's files
// (or drops one on the window); the game's folders are checked against the
// MYULK.EXE this program was made from and copied once:
//   desktop: into the user's folder (imported_dir()),
//   browser: into the browser's storage (Cache Storage "myulk-own"), where the
//            on-demand loader (win16/webdata.cpp) finds them.
// Nothing is uploaded anywhere.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace win16::setup {

// Desktop: where imported game files live (…/myulk-recomp/Myulk/game).
std::string imported_dir();

// Runs the screen; true when the game files are in place, false if the
// player quit. Needs the display (win16::init_host()).
bool run();

// Does this MYULK.EXE match the one the program was made from?
bool exe_matches(const std::vector<uint8_t>& exe);

}  // namespace win16::setup
