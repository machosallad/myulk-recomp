// Hang reports: when the game stops pumping window messages for a few
// seconds, a background thread writes hang.txt in the app-data folder with
// what the game was doing (recently entered game routines, and on Windows
// where the program itself was running). Desktop only.
#pragma once

namespace win16::watchdog {

void start();   // from the game's thread, once
void alive();   // on every message pump

}  // namespace win16::watchdog
