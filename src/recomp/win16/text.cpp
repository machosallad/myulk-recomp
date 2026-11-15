#include "recomp/win16/text.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "recomp/win16/gdi.hpp"

namespace win16::fontdata {
struct Font { const char* name; const unsigned char* data; std::size_t size; };
extern const Font kFonts[];
extern const int kFontCount;
}

namespace win16::ttf {

struct Face {
    stbtt_fontinfo info{};
    bool ok = false;
};

struct Sized {
    const Face* face = nullptr;
    float scale = 1, asc = 0, desc = 0, em = 0;
    bool fakeBold = false;
    bool wingdings = false;   // drawn symbols (no Wingdings font is embedded)
    std::array<std::unique_ptr<Glyph>, 256> glyphs;
};

namespace {

// Windows-1252 0x80-0x9F to Unicode (other bytes are Latin-1)
const uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

const Face* face_named(const char* name) {
    static std::map<std::string, Face> faces;
    auto it = faces.find(name);
    if (it != faces.end()) return &it->second;
    Face& f = faces[name];
    for (int i = 0; i < fontdata::kFontCount; ++i) {
        if (std::string(fontdata::kFonts[i].name) != name) continue;
        const unsigned char* d = fontdata::kFonts[i].data;
        f.ok = stbtt_InitFont(&f.info, d, stbtt_GetFontOffsetForIndex(d, 0)) != 0;
    }
    return &f;
}

// The Wingdings characters the game uses, drawn as shapes: 0xA8 empty box,
// 0xFE box with a tick, 0xFD box with a cross, 0xFC tick, 0xFB cross.
void symbol(Glyph& g, float em, unsigned char ch) {
    int box = std::max(6, int(std::lround(em * 0.62f)));
    int stroke = std::max(1, int(std::lround(em / 14.0f)));
    g.w = g.h = box;
    g.xoff = int(std::lround(em * 0.1f));
    g.yoff = -box;
    g.advance = std::round(em * 0.85f);
    g.alpha.assign(size_t(box) * size_t(box), 0);
    auto dot = [&](float x, float y) {
        for (int j = 0; j < stroke; ++j)
            for (int i = 0; i < stroke; ++i) {
                int px = int(x) + i, py = int(y) + j;
                if (px >= 0 && py >= 0 && px < box && py < box) g.alpha[size_t(py) * size_t(box) + size_t(px)] = 255;
            }
    };
    auto line = [&](float x0, float y0, float x1, float y1) {
        int n = int(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0))) + 1;
        for (int k = 0; k <= n; ++k) dot(x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n);
    };
    float e = float(box - stroke);
    bool frame = ch == 0xA8 || ch == 0xFE || ch == 0xFD || (ch != 0xFC && ch != 0xFB);
    if (frame) { line(0, 0, e, 0); line(0, e, e, e); line(0, 0, 0, e); line(e, 0, e, e); }
    if (ch == 0xFE || ch == 0xFC) { line(e * 0.2f, e * 0.55f, e * 0.42f, e * 0.8f); line(e * 0.42f, e * 0.8f, e * 0.85f, e * 0.15f); }
    if (ch == 0xFD || ch == 0xFB) { line(e * 0.2f, e * 0.2f, e * 0.8f, e * 0.8f); line(e * 0.8f, e * 0.2f, e * 0.2f, e * 0.8f); }
}

}  // namespace

const Sized* sized_for(const GdiObject* font, int cellHeight) {
    std::string face;
    if (font) for (char ch : font->face) face += char(std::toupper(static_cast<unsigned char>(ch)));
    bool bold = font && font->weight >= 600;
    if (face == "WINGDINGS") {
        float em = font && font->height < 0 ? float(-font->height) : float(cellHeight) * 0.82f;
        static std::map<int, std::unique_ptr<Sized>> symbolCache;
        auto& slot = symbolCache[int(std::lround(em * 4))];
        if (!slot) {
            slot = std::make_unique<Sized>();
            slot->wingdings = true;
            slot->em = em;
            slot->asc = em * 0.9f;
            slot->desc = em * 0.2f;
        }
        return slot.get();
    }
    // the game's faces -> Liberation Sans / Serif / Mono
    const char* family = "sans";
    float em = 0;
    if (face.empty() || face == "SYSTEM" || face == "FIXEDSYS") {
        bold = true;             // the Windows System font is a bold face
        em = 12.5f;              // cell 16
    } else if (face == "MS SERIF" || face == "TMS RMN" || face == "TIMES NEW ROMAN" || face == "TIMES") {
        family = "serif";
    } else if (face == "COURIER" || face == "COURIER NEW") {
        family = "mono";
    }
    if (em == 0) {
        int h = font ? font->height : 0;
        em = h < 0 ? float(-h) : h > 0 ? float(h) * 0.82f : float(cellHeight) * 0.82f;
    }
    em = std::max(em, 6.0f);
    std::string faceName = std::string(family) + (bold && std::string(family) != "mono" ? "-bold" : "");
    static std::map<std::pair<std::string, int>, std::unique_ptr<Sized>> cache;
    auto key = std::make_pair(faceName + (bold ? "+b" : ""), int(std::lround(em * 4)));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    auto s = std::make_unique<Sized>();
    s->face = face_named(faceName.c_str());
    s->fakeBold = bold && std::string(family) == "mono";
    if (s->face->ok) {
        s->scale = stbtt_ScaleForMappingEmToPixels(&s->face->info, em);
        int a, d, g;
        stbtt_GetFontVMetrics(&s->face->info, &a, &d, &g);
        s->asc = float(a) * s->scale;
        s->desc = float(-d) * s->scale;
    }
    Sized* out = s.get();
    cache[key] = std::move(s);
    return out;
}

const Glyph& glyph(const Sized* s, unsigned char ch) {
    auto& slot = const_cast<Sized*>(s)->glyphs[ch];
    if (slot) return *slot;
    slot = std::make_unique<Glyph>();
    if (s->wingdings) { symbol(*slot, s->em, ch); return *slot; }
    if (!s->face || !s->face->ok) return *slot;
    int cp = ch >= 0x80 && ch < 0xA0 ? kCp1252High[ch - 0x80] : ch;
    const stbtt_fontinfo* info = &s->face->info;
    if (!stbtt_FindGlyphIndex(info, cp)) cp = '?';
    int adv, lsb;
    stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
    slot->advance = std::round(float(adv) * s->scale);   // whole pixels, like Windows
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(info, cp, s->scale, s->scale, &x0, &y0, &x1, &y1);
    slot->w = x1 - x0;
    slot->h = y1 - y0;
    slot->xoff = x0;
    slot->yoff = y0;
    if (slot->w > 0 && slot->h > 0) {
        slot->alpha.assign(size_t(slot->w) * size_t(slot->h), 0);
        stbtt_MakeCodepointBitmap(info, slot->alpha.data(), slot->w, slot->h, slot->w, s->scale, s->scale, cp);
    }
    return *slot;
}

float ascent(const Sized* s) { return s->asc; }
float descent(const Sized* s) { return s->desc; }
bool fake_bold(const Sized* s) { return s->fakeBold; }

}  // namespace win16::ttf
