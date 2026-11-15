// Quick-save, quick-load and autosave (QoL). Requests run at the next safe
// point (the game polling its message queue with the adventure or the farm in
// front), through the game's own menu handlers, so saves stay compatible.
#pragma once

namespace win16::quick {

void request_save();   // F5: SNABB.MLK
void request_load();   // F9: SNABB.MLK
void pump();           // every host_pump: start card ("Fortsätt") and its fast-forward

}  // namespace win16::quick
