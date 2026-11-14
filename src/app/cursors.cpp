#include "app/cursors.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <SDL3/SDL.h>

namespace myulk {
namespace {

// Order of Screen.Cursors[1..10] in TAdvForm.FormCreate.
constexpr const char* kNames[] = {nullptr, "BACKWARD", "FLAT", "FORWARD", "GRAB", "LEFT",
                                  "RIGHT", "UP", "DOWN", "YXA", "SPADE"};

std::uint16_t u16(const std::vector<std::uint8_t>& d, size_t o) { return d[o] | (d[o + 1] << 8); }
std::uint32_t u32(const std::vector<std::uint8_t>& d, size_t o) { return u16(d, o) | (u16(d, o + 2) << 16); }

}  // namespace

Cursors::Cursors(SDL_Renderer* renderer, const std::string& dir) : renderer_(renderer) {
    for (int i = 1; i <= 10; ++i) cursors_[i] = load(dir + "/" + kNames[i] + ".cur");
}

Cursors::~Cursors() {
    for (auto& c : cursors_)
        if (c.tex) SDL_DestroyTexture(c.tex);
}

// .cur: ICONDIR, ICONDIRENTRY (hotspot in planes/bitcount fields), then a
// DIB: BITMAPINFOHEADER (height doubled), palette, XOR bitmap, AND mask.
// Windows 3.1 cursors are 1 bpp; AND=1 & XOR=1 (screen invert) is drawn black.
Cursors::Cur Cursors::load(const std::string& path) {
    Cur cur;
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    if (d.size() < 22 + 40) return cur;
    cur.hx = u16(d, 6 + 4);
    cur.hy = u16(d, 6 + 6);
    size_t off = u32(d, 6 + 12);
    if (off + 40 > d.size()) return cur;
    int w = int(u32(d, off + 4));
    int h = int(u32(d, off + 8)) / 2;
    int bpp = u16(d, off + 14);
    if (bpp != 1 || w <= 0 || h <= 0) return cur;
    size_t pal = off + u32(d, off);
    size_t stride = ((w + 31) / 32) * 4;
    size_t xorOff = pal + 2 * 4;
    size_t andOff = xorOff + stride * h;
    if (andOff + stride * h > d.size()) return cur;

    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!s) return cur;
    for (int y = 0; y < h; ++y) {
        int row = h - 1 - y;   // DIBs are bottom-up
        auto* px = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(s->pixels) + y * s->pitch);
        for (int x = 0; x < w; ++x) {
            bool a = d[andOff + row * stride + x / 8] & (0x80 >> (x % 8));
            bool xr = d[xorOff + row * stride + x / 8] & (0x80 >> (x % 8));
            std::uint8_t rgba[4] = {0, 0, 0, 0};
            if (!a) {                     // opaque pixel
                std::uint8_t v = xr ? 255 : 0;
                rgba[0] = rgba[1] = rgba[2] = v;
                rgba[3] = 255;
            } else if (xr) {              // inverted pixel
                rgba[3] = 255;
            }
            std::memcpy(&px[x], rgba, 4);
        }
    }
    cur.tex = SDL_CreateTextureFromSurface(renderer_, s);
    SDL_DestroySurface(s);
    if (cur.tex) SDL_SetTextureScaleMode(cur.tex, SDL_SCALEMODE_NEAREST);
    cur.w = w;
    cur.h = h;
    return cur;
}

void Cursors::draw(int index, float x, float y) {
    if (index < 1 || index > 10 || !cursors_[index].tex) index = 2;
    const Cur& c = cursors_[index];
    if (!c.tex) return;
    SDL_FRect dst{x - c.hx, y - c.hy, float(c.w), float(c.h)};
    SDL_RenderTexture(renderer_, c.tex, nullptr, &dst);
}

}  // namespace myulk
