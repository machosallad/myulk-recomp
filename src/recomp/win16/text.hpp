// TrueType text for the GDI layer: the game's fonts mapped to the embedded
// Liberation fonts and rendered with stb_truetype (anti-aliased glyphs).
#pragma once

#include <cstdint>
#include <vector>

namespace win16 {
struct GdiObject;

namespace ttf {

struct Glyph {
    int w = 0, h = 0, xoff = 0, yoff = 0;   // bitmap size and offset from the pen / baseline
    float advance = 0;
    std::vector<uint8_t> alpha;              // w * h coverage
};

struct Sized;   // one font face at one pixel size

// Face and size for a GDI font (cellHeight: the Windows cell height used for layout).
const Sized* sized_for(const GdiObject* font, int cellHeight);
const Glyph& glyph(const Sized* s, unsigned char cp1252);
float ascent(const Sized* s);    // pixels above the baseline
float descent(const Sized* s);   // pixels below the baseline (positive)
bool fake_bold(const Sized* s);  // no bold face available: draw twice

}  // namespace ttf
}  // namespace win16
