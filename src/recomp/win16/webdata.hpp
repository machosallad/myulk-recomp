// Game data on demand for the web build: the CD's folder tree exists in the
// in-memory file system from the start (empty placeholder files built from a
// manifest), and each file is fetched from the server the first time the game
// opens it. Native builds read the extracted CD directly; these are no-ops.
#pragma once

#include <cstdint>
#include <string>

namespace win16::webdata {

// Fetch game/manifest.txt and create the placeholder tree under `root`.
bool init(const std::string& root);
// Make sure a host path under the root holds the real file (fetches it once).
void ensure(const std::string& hostPath);
// Size from the manifest for a file not fetched yet, or -1.
long pending_size(const std::string& hostPath);

}  // namespace win16::webdata
