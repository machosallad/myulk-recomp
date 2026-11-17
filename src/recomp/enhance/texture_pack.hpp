// HD texture packs: larger versions of the game's pictures, made by the player
// from their own game files (tools/texturepack.py, docs/texture-pack.md).
//
// The game keeps drawing with the original bitmaps. When a bitmap is created
// whose pixels match a picture in the pack, the HD picture is attached to it
// as an HD layer (gdi.hpp: HdLayer); blits carry it along into the screen's
// layer, which is what the window shows.
#pragma once

#include <string>

#include "recomp/win16/gdi.hpp"

namespace win16::texture_pack {

// Looks for a pack (MYULK_TEXTURE_PACK, "texturepack" next to the program,
// in the user's app-data folder, or next to the game folder). `gameDir` is
// the MYULK folder. Whether it is shown is settings::texture_pack().
void init(const std::string& gameDir);
int scale();   // 0 when no pack is in use
// Called for every bitmap the game creates from pixel data.
void attach(Surface& s);

}  // namespace win16::texture_pack
