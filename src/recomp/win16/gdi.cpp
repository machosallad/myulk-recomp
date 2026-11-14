// GDI implementation: objects, DCs, software drawing, and the GDI exports.
#include "recomp/win16/gdi.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/win16.hpp"

using recomp::Args;
using recomp::Cpu;

namespace win16 {
namespace {

std::map<uint16_t, std::unique_ptr<GdiObject>> g_objs;
std::map<uint16_t, std::shared_ptr<DC>> g_dcs;
uint16_t g_nextObj = 0x4000;
uint16_t g_nextDC = 0x6000;
uint16_t g_stock[17] = {};
Surface g_screen(640, 480, 8);
std::vector<Color> g_syspal;

// 8x8 glyphs for 0x20..0x7F, rendered once with SDL's debug font
uint8_t g_glyphs[96][8];

Color colorref(uint32_t cr) {
    // COLORREF 0x00BBGGRR; PALETTEINDEX (0x01xxxxxx) and PALETTERGB (0x02...)
    if ((cr >> 24) == 1) return nearest_index_color(uint8_t(cr));
    return ((cr & 0xFF) << 16) | (cr & 0xFF00) | ((cr >> 16) & 0xFF);
}

uint32_t to_colorref(Color c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

void make_glyphs() {
    SDL_Surface* s = SDL_CreateSurface(96 * 8, 8, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* r = s ? SDL_CreateSoftwareRenderer(s) : nullptr;
    std::memset(g_glyphs, 0, sizeof g_glyphs);
    if (!r) return;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    for (int ch = 0x20; ch < 0x80; ++ch) {
        char str[2] = {char(ch), 0};
        SDL_RenderDebugText(r, float((ch - 0x20) * 8), 0, str);
    }
    SDL_RenderPresent(r);
    for (int g = 0; g < 96; ++g)
        for (int y = 0; y < 8; ++y) {
            uint8_t bits = 0;
            for (int x = 0; x < 8; ++x) {
                auto* p = static_cast<uint8_t*>(s->pixels) + y * s->pitch + (g * 8 + x) * 4;
                if (p[0] > 128) bits |= uint8_t(0x80 >> x);
            }
            g_glyphs[g][y] = bits;
        }
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(s);
}

int map_x(const DC& dc, int x) {
    return (x - dc.winOrgX) * dc.vpExtX / (dc.winExtX ? dc.winExtX : 1) + dc.vpOrgX + dc.originX;
}
int map_y(const DC& dc, int y) {
    return (y - dc.winOrgY) * dc.vpExtY / (dc.winExtY ? dc.winExtY : 1) + dc.vpOrgY + dc.originY;
}
Rect map_rect(const DC& dc, const Rect& r) {
    Rect d{map_x(dc, r.left), map_y(dc, r.top), map_x(dc, r.right), map_y(dc, r.bottom)};
    if (d.left > d.right) std::swap(d.left, d.right);
    if (d.top > d.bottom) std::swap(d.top, d.bottom);
    return d;
}

Rect dc_bounds(DC& dc) {
    Surface* s = dc_surface(dc);
    Rect b{0, 0, s ? s->w : 0, s ? s->h : 0};
    return dc.memory ? b : b.intersect(dc.clip);
}

// ROP3 on packed RGB: bit i of the code is the result for P,S,D bits (P<<2|S<<1|D)
Color rop3(uint8_t code, Color p, Color s, Color d) {
    Color out = 0;
    for (int i = 0; i < 8; ++i) {
        if (!((code >> i) & 1)) continue;
        Color m = ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
        out |= m;
    }
    return out & 0xFFFFFF;
}

Color rop2(int mode, Color pen, Color d) {
    switch (mode) {
        case 1: return 0;                       // R2_BLACK
        case 6: return ~d & 0xFFFFFF;           // R2_NOT
        case 7: return (pen ^ d) & 0xFFFFFF;    // R2_XORPEN
        case 9: return ~(pen ^ d) & 0xFFFFFF;   // R2_NOTXORPEN
        case 11: return d;                      // R2_NOP
        case 16: return 0xFFFFFF;               // R2_WHITE
        default: return pen;                    // R2_COPYPEN
    }
}

Color brush_color(GdiObject* b, int x, int y) {
    if (!b) return 0xFFFFFF;
    if (b->style == 3 && b->pattern && b->pattern->w > 0)   // BS_PATTERN
        return b->pattern->get(x % b->pattern->w, y % b->pattern->h);
    return b->color;
}

bool brush_hollow(GdiObject* b) { return !b || b->style == 1; }   // BS_HOLLOW / BS_NULL

void plot(DC& dc, Surface* s, const Rect& clip, int x, int y, Color c) {
    if (!clip.contains(x, y)) return;
    s->set(x, y, rop2(dc.rop2, c, s->get(x, y)));
}

std::shared_ptr<DC> new_dc() {
    auto dc = std::make_shared<DC>();
    dc->handle = g_nextDC;
    g_nextDC = uint16_t(g_nextDC + 2);
    dc->pen = gdi_stock(7);      // BLACK_PEN
    dc->brush = gdi_stock(0);    // WHITE_BRUSH
    dc->font = gdi_stock(13);    // SYSTEM_FONT
    dc->palette = gdi_stock(15);
    g_dcs[dc->handle] = dc;
    return dc;
}

}  // namespace

// ---- objects ----------------------------------------------------------------

uint16_t gdi_add(std::unique_ptr<GdiObject> obj) {
    uint16_t h = g_nextObj;
    g_nextObj = uint16_t(g_nextObj + 2);
    if (g_nextObj >= 0x5FFE) g_nextObj = 0x4000;
    while (g_objs.count(h)) h = uint16_t(h + 2);
    g_objs[h] = std::move(obj);
    return h;
}

GdiObject* gdi_get(uint16_t h) {
    auto it = g_objs.find(h);
    return it == g_objs.end() ? nullptr : it->second.get();
}

uint16_t gdi_stock(int i) { return (i >= 0 && i < 17) ? g_stock[i] : 0; }

uint16_t gdi_create_bitmap(std::shared_ptr<Surface> s) {
    auto o = std::make_unique<GdiObject>();
    o->type = ObjType::Bitmap;
    o->bitmap = std::move(s);
    return gdi_add(std::move(o));
}

Surface& screen() { return g_screen; }
std::vector<Color>& system_palette() { return g_syspal; }

Color nearest_index_color(uint8_t i) { return i < g_syspal.size() ? g_syspal[i] : 0; }

uint8_t nearest_index(Color c) {
    int best = 0, bestd = 1 << 30;
    for (size_t i = 0; i < g_syspal.size(); ++i) {
        Color p = g_syspal[i];
        int dr = int((p >> 16) & 255) - int((c >> 16) & 255);
        int dg = int((p >> 8) & 255) - int((c >> 8) & 255);
        int db = int(p & 255) - int(c & 255);
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = int(i); if (!d) break; }
    }
    return uint8_t(best);
}

void gdi_init() {
    auto add_stock = [](int i, ObjType t, int style, Color c) {
        auto o = std::make_unique<GdiObject>();
        o->type = t;
        o->stock = true;
        o->style = style;
        o->color = c;
        if (t == ObjType::Font) { o->height = 16; o->face = "System"; o->weight = 700; }
        g_stock[i] = gdi_add(std::move(o));
    };
    add_stock(0, ObjType::Brush, 0, 0xFFFFFF);
    add_stock(1, ObjType::Brush, 0, 0xC0C0C0);
    add_stock(2, ObjType::Brush, 0, 0x808080);
    add_stock(3, ObjType::Brush, 0, 0x404040);
    add_stock(4, ObjType::Brush, 0, 0x000000);
    add_stock(5, ObjType::Brush, 1, 0);          // NULL_BRUSH
    add_stock(6, ObjType::Pen, 0, 0xFFFFFF);
    add_stock(7, ObjType::Pen, 0, 0x000000);
    add_stock(8, ObjType::Pen, 5, 0);            // NULL_PEN (PS_NULL)
    for (int i : {10, 11, 12, 13, 14, 16}) add_stock(i, ObjType::Font, 0, 0);
    {
        auto o = std::make_unique<GdiObject>();
        o->type = ObjType::Palette;
        o->stock = true;
        // the 20 static colours of the Windows default palette
        const Color def[20] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080,
                               0xC0C0C0, 0xC0DCC0, 0xA6CAF0, 0xFFFBF0, 0xA0A0A4, 0x808080, 0xFF0000,
                               0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
        o->entries.assign(def, def + 20);
        g_stock[15] = gdi_add(std::move(o));
        g_syspal.assign(256, 0);
        for (int i = 0; i < 10; ++i) g_syspal[i] = def[i];
        for (int i = 0; i < 10; ++i) g_syspal[246 + i] = def[10 + i];
    }
    make_glyphs();
}

// ---- DCs ---------------------------------------------------------------------

DC* dc_get(uint16_t h) {
    auto it = g_dcs.find(h);
    return it == g_dcs.end() ? nullptr : it->second.get();
}

uint16_t dc_create_window(uint16_t hwnd, int originX, int originY, const Rect& clip) {
    auto dc = new_dc();
    dc->hwnd = hwnd;
    dc->originX = originX;
    dc->originY = originY;
    dc->clip = clip.intersect(Rect{0, 0, g_screen.w, g_screen.h});
    return dc->handle;
}

uint16_t dc_create_memory() {
    auto dc = new_dc();
    dc->memory = true;
    // a memory DC starts with a 1x1 monochrome bitmap
    static uint16_t mono = 0;
    if (!mono) mono = gdi_create_bitmap(std::make_shared<Surface>(1, 1, 1));
    dc->bitmap = mono;
    return dc->handle;
}

void dc_delete(uint16_t h) { g_dcs.erase(h); }

Surface* dc_surface(DC& dc) {
    if (dc.memory) {
        GdiObject* b = gdi_get(dc.bitmap);
        return b && b->bitmap ? b->bitmap.get() : nullptr;
    }
    if (dc.info) return nullptr;
    return &g_screen;
}

// ---- drawing -----------------------------------------------------------------

void draw_fill_rect(DC& dc, const Rect& r, uint16_t brush) {
    Surface* s = dc_surface(dc);
    GdiObject* b = gdi_get(brush);
    if (!s || brush_hollow(b)) return;
    Rect d = map_rect(dc, r).intersect(dc_bounds(dc));
    for (int y = d.top; y < d.bottom; ++y)
        for (int x = d.left; x < d.right; ++x) s->set(x, y, brush_color(b, x, y));
}

void draw_frame_rect(DC& dc, const Rect& r, uint16_t brush) {
    draw_fill_rect(dc, Rect{r.left, r.top, r.right, r.top + 1}, brush);
    draw_fill_rect(dc, Rect{r.left, r.bottom - 1, r.right, r.bottom}, brush);
    draw_fill_rect(dc, Rect{r.left, r.top, r.left + 1, r.bottom}, brush);
    draw_fill_rect(dc, Rect{r.right - 1, r.top, r.right, r.bottom}, brush);
}

void draw_line(DC& dc, int x0, int y0, int x1, int y1) {
    Surface* s = dc_surface(dc);
    GdiObject* pen = gdi_get(dc.pen);
    if (!s || !pen || pen->style == 5) return;
    Rect clip = dc_bounds(dc);
    int ax = map_x(dc, x0), ay = map_y(dc, y0), bx = map_x(dc, x1), by = map_y(dc, y1);
    int dx = std::abs(bx - ax), dy = -std::abs(by - ay);
    int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1, err = dx + dy;
    int w = std::max(1, pen->width);
    for (;;) {
        if (ax == bx && ay == by) break;   // Windows excludes the end point
        for (int oy = 0; oy < w; ++oy)
            for (int ox = 0; ox < w; ++ox) plot(dc, s, clip, ax + ox - w / 2, ay + oy - w / 2, pen->color);
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; ax += sx; }
        if (e2 <= dx) { err += dx; ay += sy; }
    }
}

// Cell height (tmHeight) as Windows 3.1 reports it. Delphi 1 rescales forms
// when this differs from the design-time TextHeight, so it must match:
// the raster fonts come in fixed sizes, TrueType adds internal leading.
static int font_cell_height(GdiObject* f) {
    if (!f) return 16;
    std::string face;
    for (char ch : f->face) face += char(std::toupper(static_cast<unsigned char>(ch)));
    int h = f->height;
    if (face.empty() || face == "SYSTEM") return 16;
    if (h > 0) return h;                       // positive: already a cell height
    int em = -h;
    auto pick = [em](std::initializer_list<std::pair<int, int>> sizes) {
        int cell = 0;
        for (auto [maxEm, c] : sizes) { cell = c; if (em <= maxEm) break; }
        return cell;
    };
    if (face == "MS SANS SERIF" || face == "MS SERIF" || face == "HELV" || face == "TMS RMN")
        return pick({{11, 13}, {13, 16}, {16, 20}, {19, 24}, {24, 29}, {1000, 37}});
    if (face == "COURIER") return pick({{13, 13}, {16, 16}, {1000, 20}});
    return em + (em * 15 + 50) / 100;          // TrueType: em + ~15% internal leading
}

static int font_scale(GdiObject* f) { return std::max(1, (font_cell_height(f) + 4) / 16); }

int text_height(uint16_t font) { return font_cell_height(gdi_get(font)); }

int text_width(uint16_t font, const std::string& text) {
    GdiObject* f = gdi_get(font);
    int avg = std::max(5, font_cell_height(f) * 7 / 16);   // System: 16 -> 7
    return int(text.size()) * avg + ((f && f->weight >= 700) ? 1 : 0);
}

void draw_text(DC& dc, int x, int y, const std::string& text, const Rect* opaque, int) {
    Surface* s = dc_surface(dc);
    if (!s) return;
    GdiObject* f = gdi_get(dc.font);
    int sc = font_scale(f);
    bool bold = f && f->weight >= 700;
    Rect clip = dc_bounds(dc);
    int h = text_height(dc.font);
    int dx0 = map_x(dc, x), dy0 = map_y(dc, y);
    if (dc.bkMode == 2) {   // OPAQUE
        Rect bg = opaque ? map_rect(dc, *opaque) : Rect{dx0, dy0, dx0 + text_width(dc.font, text), dy0 + h};
        bg = bg.intersect(clip);
        for (int yy = bg.top; yy < bg.bottom; ++yy)
            for (int xx = bg.left; xx < bg.right; ++xx) s->set(xx, yy, dc.bkColor);
    }
    int gy = dy0 + (h - 8 * sc) / 2;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        int accent = 0;   // 1 ring (Å å), 2 dots (Ä ä Ö ö), 3 acute (é)
        switch (ch) {
            case 0xC5: ch = 'A'; accent = 1; break;
            case 0xE5: ch = 'a'; accent = 1; break;
            case 0xC4: ch = 'A'; accent = 2; break;
            case 0xE4: ch = 'a'; accent = 2; break;
            case 0xD6: ch = 'O'; accent = 2; break;
            case 0xF6: ch = 'o'; accent = 2; break;
            case 0xE9: ch = 'e'; accent = 3; break;
            case 0xC9: ch = 'E'; accent = 3; break;
            default: break;
        }
        if (ch < 0x20 || ch >= 0x80) ch = '?';
        int adv = std::max(5, font_cell_height(f) * 7 / 16);
        int gx = dx0 + int(i) * adv + (adv - 8 * sc) / 2;
        const uint8_t* g = g_glyphs[ch - 0x20];
        for (int yy = 0; yy < 8; ++yy)
            for (int xx = 0; xx < 8; ++xx)
                if (g[yy] & (0x80 >> xx))
                    for (int sy = 0; sy < sc; ++sy)
                        for (int sx = 0; sx < sc + (bold ? 1 : 0); ++sx) {
                            int px = gx + xx * sc + sx, py = gy + yy * sc + sy;
                            if (clip.contains(px, py)) s->set(px, py, dc.textColor);
                        }
        if (accent) {
            int top = gy - (ch >= 'a' ? 0 : 2 * sc);
            auto dot = [&](int ox, int oy) {
                for (int sy = 0; sy < sc; ++sy)
                    for (int sx = 0; sx < sc; ++sx) {
                        int px = gx + ox * sc + sx, py = top + oy * sc + sy;
                        if (clip.contains(px, py)) s->set(px, py, dc.textColor);
                    }
            };
            if (accent == 1) { dot(2, 0); dot(3, -1); dot(4, 0); }
            if (accent == 2) { dot(2, 0); dot(5, 0); }
            if (accent == 3) { dot(4, 0); dot(5, -1); }
        }
    }
}

void blit(DC& dst, int dx, int dy, int dw, int dh, DC* src, int sx, int sy, int sw, int sh, uint32_t rop) {
    Surface* ds = dc_surface(dst);
    if (!ds || dw == 0 || dh == 0) return;
    Surface* ss = src ? dc_surface(*src) : nullptr;
    uint8_t code = uint8_t(rop >> 16);
    bool usesSrc = ((code >> 2) ^ code) & 0x33;
    if (usesSrc && !ss) return;
    GdiObject* brush = gdi_get(dst.brush);
    Rect clip = dc_bounds(dst);
    int ox = map_x(dst, dx), oy = map_y(dst, dy);
    int sox = src ? map_x(*src, sx) : 0, soy = src ? map_y(*src, sy) : 0;
    bool flipX = dw < 0, flipY = dh < 0;
    if (flipX) { dw = -dw; ox -= dw; }
    if (flipY) { dh = -dh; oy -= dh; }
    bool srcMono = ss && ss->bpp == 1, dstMono = ds->bpp == 1;
    for (int y = 0; y < dh; ++y) {
        int ty = oy + y;
        if (ty < clip.top || ty >= clip.bottom) continue;
        int syy = soy + (sh == dh ? y : int((long long)y * sh / dh));
        if (flipY) syy = soy + (sh - 1 - (sh == dh ? y : int((long long)y * sh / dh)));
        for (int x = 0; x < dw; ++x) {
            int tx = ox + x;
            if (tx < clip.left || tx >= clip.right) continue;
            Color s = 0;
            if (usesSrc) {
                int sxx = sox + (sw == dw ? x : int((long long)x * sw / dw));
                if (flipX) sxx = sox + (sw - 1 - (sw == dw ? x : int((long long)x * sw / dw)));
                if (sxx < 0 || syy < 0 || sxx >= ss->w || syy >= ss->h) continue;
                s = ss->get(sxx, syy);
                if (srcMono && !dstMono) s = s ? dst.bkColor : dst.textColor;
                else if (!srcMono && dstMono) s = (s == (src ? src->bkColor : 0xFFFFFF)) ? 0xFFFFFF : 0;
            }
            Color p = brush_color(brush, tx, ty);
            Color d = ds->get(tx, ty);
            Color r = (code == 0xCC) ? s : rop3(code, p, s, d);
            if (dstMono) r = r ? 0xFFFFFF : 0;
            ds->set(tx, ty, r);
        }
    }
}

}  // namespace win16

// ============================================================================
// GDI exports
// ============================================================================
using namespace win16;

static Rect read_rect(Cpu& c, uint32_t p) {
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    return Rect{int16_t(c.rd16(s, o)), int16_t(c.rd16(s, uint16_t(o + 2))),
                int16_t(c.rd16(s, uint16_t(o + 4))), int16_t(c.rd16(s, uint16_t(o + 6)))};
}
static void write_point(Cpu& c, uint32_t p, int x, int y) {
    if (!p) return;
    c.wr16(uint16_t(p >> 16), uint16_t(p), uint16_t(x));
    c.wr16(uint16_t(p >> 16), uint16_t(uint16_t(p) + 2), uint16_t(y));
}
static std::string read_text(Cpu& c, uint32_t p, int n) {
    std::string s;
    if (n < 0) return recomp::read_cstr(c, p);
    for (int i = 0; i < n; ++i) s += char(c.rd8(uint16_t(p >> 16), uint16_t(uint16_t(p) + i)));
    return s;
}

#define DCARG(var) DC* var = dc_get(hdc); if (!var) { a.ret(0); return; }

API(GDI, 1, SetBkColor, 6) {
    uint32_t cr = a.l(); uint16_t hdc = a.w(); DCARG(dc);
    uint32_t old = to_colorref(dc->bkColor); dc->bkColor = colorref(cr); a.ret32(old);
}
API(GDI, 9, SetTextColor, 6) {
    uint32_t cr = a.l(); uint16_t hdc = a.w(); DCARG(dc);
    uint32_t old = to_colorref(dc->textColor); dc->textColor = colorref(cr); a.ret32(old);
}
API(GDI, 2, SetBkMode, 4) { uint16_t m = a.w(); uint16_t hdc = a.w(); DCARG(dc); int o = dc->bkMode; dc->bkMode = m; a.ret(uint16_t(o)); }
API(GDI, 3, SetMapMode, 4) { uint16_t m = a.w(); uint16_t hdc = a.w(); DCARG(dc); int o = dc->mapMode; dc->mapMode = m; a.ret(uint16_t(o)); }
API(GDI, 4, SetROP2, 4) { uint16_t m = a.w(); uint16_t hdc = a.w(); DCARG(dc); int o = dc->rop2; dc->rop2 = m; a.ret(uint16_t(o)); }
API(GDI, 7, SetStretchBltMode, 4) { uint16_t m = a.w(); uint16_t hdc = a.w(); DCARG(dc); int o = dc->stretchMode; dc->stretchMode = m; a.ret(uint16_t(o)); }

API(GDI, 19, LineTo, 6) {
    int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    draw_line(*dc, dc->curX, dc->curY, x, y); dc->curX = x; dc->curY = y; a.ret(1);
}
API(GDI, 483, MoveToEx, 10) {
    uint32_t pt = a.p(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    write_point(c, pt, dc->curX, dc->curY); dc->curX = x; dc->curY = y; a.ret(1);
}
API(GDI, 470, GetCurrentPositionEx, 6) {
    uint32_t pt = a.p(); uint16_t hdc = a.w(); DCARG(dc); write_point(c, pt, dc->curX, dc->curY); a.ret(1);
}
API(GDI, 37, Polyline, 8) {
    int16_t n = a.i(); uint32_t pts = a.p(); uint16_t hdc = a.w(); DCARG(dc);
    for (int i = 0; i + 1 < n; ++i) {
        uint16_t s = uint16_t(pts >> 16), o = uint16_t(uint16_t(pts) + 4 * i);
        draw_line(*dc, int16_t(c.rd16(s, o)), int16_t(c.rd16(s, uint16_t(o + 2))),
                  int16_t(c.rd16(s, uint16_t(o + 4))), int16_t(c.rd16(s, uint16_t(o + 6))));
    }
    a.ret(1);
}
static void shape_rect(DC& dc, Rect r) {
    draw_fill_rect(dc, Rect{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1}, dc.brush);
    GdiObject* pen = gdi_get(dc.pen);
    if (pen && pen->style != 5) {
        draw_line(dc, r.left, r.top, r.right - 1, r.top);
        draw_line(dc, r.right - 1, r.top, r.right - 1, r.bottom - 1);
        draw_line(dc, r.right - 1, r.bottom - 1, r.left, r.bottom - 1);
        draw_line(dc, r.left, r.bottom - 1, r.left, r.top);
    } else {
        draw_fill_rect(dc, r, dc.brush);
    }
}
API(GDI, 27, Rectangle, 10) {
    int16_t b = a.i(); int16_t r = a.i(); int16_t t = a.i(); int16_t l = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    shape_rect(*dc, Rect{l, t, r, b}); a.ret(1);
}
API(GDI, 28, RoundRect, 14) {
    a.i(); a.i(); int16_t b = a.i(); int16_t r = a.i(); int16_t t = a.i(); int16_t l = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    shape_rect(*dc, Rect{l, t, r, b}); a.ret(1);
}
API(GDI, 24, Ellipse, 10) {
    int16_t b = a.i(); int16_t r = a.i(); int16_t t = a.i(); int16_t l = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0, rx = (r - l) / 2.0, ry = (b - t) / 2.0;
    GdiObject* br = gdi_get(dc->brush);
    for (int y = t; y < b; ++y) {
        double fy = (y + 0.5 - cy) / ry;
        if (std::fabs(fy) > 1) continue;
        double hw = rx * std::sqrt(1 - fy * fy);
        int x0 = int(std::lround(cx - hw)), x1 = int(std::lround(cx + hw));
        if (br && br->style != 1) draw_fill_rect(*dc, Rect{x0, y, x1, y + 1}, dc->brush);
        draw_line(*dc, x0, y, x0 + 1, y);
        draw_line(*dc, x1 - 1, y, x1, y);
    }
    a.ret(1);
}
API(GDI, 26, Pie, 18) { for (int i = 0; i < 8; ++i) a.i(); a.w(); a.ret(1); }
API(GDI, 29, PatBlt, 14) {
    uint32_t rop = a.l(); int16_t h = a.i(); int16_t w = a.i(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    blit(*dc, x, y, w, h, nullptr, 0, 0, w, h, rop); a.ret(1);
}
API(GDI, 31, SetPixel, 10) {
    uint32_t cr = a.l(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    Surface* s = dc_surface(*dc);
    int px = x + dc->originX, py = y + dc->originY;
    if (s && px >= 0 && py >= 0 && px < s->w && py < s->h) s->set(px, py, colorref(cr));
    a.ret32(cr);
}
API(GDI, 83, GetPixel, 6) {
    int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    Surface* s = dc_surface(*dc);
    int px = x + dc->originX, py = y + dc->originY;
    if (!s || px < 0 || py < 0 || px >= s->w || py >= s->h) { a.ret32(0xFFFFFFFF); return; }
    a.ret32(to_colorref(s->get(px, py)));
}
API(GDI, 33, TextOut, 12) {
    int16_t n = a.i(); uint32_t str = a.p(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    draw_text(*dc, x, y, read_text(c, str, n), nullptr, 0); a.ret(1);
}
API(GDI, 351, ExtTextOut, 22) {
    uint32_t dx = a.p(); int16_t n = a.i(); uint32_t str = a.p(); uint32_t prc = a.p(); uint16_t opts = a.w();
    int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    (void)dx;
    Rect r;
    if (prc) r = read_rect(c, prc);
    if (prc && (opts & 2)) {   // ETO_OPAQUE
        auto b = std::make_unique<GdiObject>(); b->type = ObjType::Brush; b->color = dc->bkColor;
        uint16_t hb = gdi_add(std::move(b)); draw_fill_rect(*dc, r, hb); g_objs.erase(hb);
    }
    int oldMode = dc->bkMode;
    if (prc && (opts & 2)) dc->bkMode = 1;
    draw_text(*dc, x, y, read_text(c, str, n), nullptr, opts);
    dc->bkMode = oldMode;
    a.ret(1);
}
API(GDI, 471, GetTextExtentPoint, 12) {
    uint32_t sz = a.p(); int16_t n = a.i(); uint32_t str = a.p(); uint16_t hdc = a.w(); DCARG(dc);
    write_point(c, sz, text_width(dc->font, read_text(c, str, n)), text_height(dc->font)); a.ret(1);
}
API(GDI, 93, GetTextMetrics, 6) {
    uint32_t tm = a.p(); uint16_t hdc = a.w(); DCARG(dc);
    uint16_t s = uint16_t(tm >> 16), o = uint16_t(tm);
    for (int i = 0; i < 31; ++i) c.wr8(s, uint16_t(o + i), 0);
    int h = text_height(dc->font);
    GdiObject* f = gdi_get(dc->font);
    int cw = text_width(dc->font, "x");
    c.wr16(s, o, uint16_t(h));                       // tmHeight
    c.wr16(s, uint16_t(o + 2), uint16_t(h * 3 / 4)); // tmAscent
    c.wr16(s, uint16_t(o + 4), uint16_t(h / 4));     // tmDescent
    c.wr16(s, uint16_t(o + 10), uint16_t(cw));       // tmAveCharWidth
    c.wr16(s, uint16_t(o + 12), uint16_t(cw));       // tmMaxCharWidth
    c.wr16(s, uint16_t(o + 14), uint16_t(f ? f->weight : 400));
    c.wr8(s, uint16_t(o + 20), 0x20);                // tmFirstChar
    c.wr8(s, uint16_t(o + 21), 0xFF);
    c.wr8(s, uint16_t(o + 22), '?');
    c.wr8(s, uint16_t(o + 23), ' ');
    c.wr8(s, uint16_t(o + 24), 0x06);                // pitch: variable, vector
    c.wr8(s, uint16_t(o + 25), 0);                   // ANSI charset
    c.wr16(s, uint16_t(o + 28), 96);
    c.wr16(s, uint16_t(o + 30), 96);
    a.ret(1);
}
API(GDI, 70, EnumFonts, 14) { a.p(); a.p(); a.p(); a.w(); a.ret(1); }

API(GDI, 34, BitBlt, 20) {
    uint32_t rop = a.l(); int16_t sy = a.i(); int16_t sx = a.i(); uint16_t hsrc = a.w();
    int16_t h = a.i(); int16_t w = a.i(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    blit(*dc, x, y, w, h, dc_get(hsrc), sx, sy, w, h, rop); a.ret(1);
}
API(GDI, 35, StretchBlt, 24) {
    uint32_t rop = a.l(); int16_t sh = a.i(); int16_t sw = a.i(); int16_t sy = a.i(); int16_t sx = a.i(); uint16_t hsrc = a.w();
    int16_t h = a.i(); int16_t w = a.i(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    blit(*dc, x, y, w, h, dc_get(hsrc), sx, sy, sw, sh, rop); a.ret(1);
}

API(GDI, 30, SaveDC, 2) {
    uint16_t hdc = a.w(); DCARG(dc);
    dc->saved.push_back(std::make_shared<DC>(*dc));
    a.ret(uint16_t(dc->saved.size()));
}
API(GDI, 39, RestoreDC, 4) {
    int16_t n = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    if (dc->saved.empty()) { a.ret(0); return; }
    size_t idx = n < 0 ? dc->saved.size() - size_t(-n) : size_t(n - 1);
    if (idx >= dc->saved.size()) { a.ret(0); return; }
    auto keep = dc->saved;
    DC restored = *keep[idx];
    keep.resize(idx);
    *dc = restored;
    dc->saved = keep;
    a.ret(1);
}
API(GDI, 21, ExcludeClipRect, 10) { a.i(); a.i(); a.i(); a.i(); a.w(); a.ret(2); }
API(GDI, 22, IntersectClipRect, 10) {
    int16_t b = a.i(); int16_t r = a.i(); int16_t t = a.i(); int16_t l = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    Rect d = map_rect(*dc, Rect{l, t, r, b});
    if (dc->memory) { a.ret(2); return; }   // memory DCs: ignore (no clip state)
    dc->clip = dc->clip.intersect(d);
    a.ret(dc->clip.empty() ? 1 : 2);
}
API(GDI, 77, GetClipBox, 6) {
    uint32_t pr = a.p(); uint16_t hdc = a.w(); DCARG(dc);
    Rect b = dc->memory ? Rect{0, 0, dc_surface(*dc) ? dc_surface(*dc)->w : 0, dc_surface(*dc) ? dc_surface(*dc)->h : 0}
                        : dc->clip.offset(-dc->originX, -dc->originY);
    uint16_t s = uint16_t(pr >> 16), o = uint16_t(pr);
    c.wr16(s, o, uint16_t(b.left)); c.wr16(s, uint16_t(o + 2), uint16_t(b.top));
    c.wr16(s, uint16_t(o + 4), uint16_t(b.right)); c.wr16(s, uint16_t(o + 6), uint16_t(b.bottom));
    a.ret(b.empty() ? 1 : 2);
}
API(GDI, 104, RectVisible, 6) {
    uint32_t pr = a.p(); uint16_t hdc = a.w(); DCARG(dc);
    Rect r = map_rect(*dc, read_rect(c, pr));
    a.ret(dc->memory || !r.intersect(dc->clip).empty() ? 1 : 0);
}

API(GDI, 45, SelectObject, 4) {
    uint16_t h = a.w(); uint16_t hdc = a.w(); DCARG(dc);
    GdiObject* o = gdi_get(h);
    if (!o) { a.ret(0); return; }
    uint16_t old = 0;
    switch (o->type) {
        case ObjType::Pen: old = dc->pen; dc->pen = h; break;
        case ObjType::Brush: old = dc->brush; dc->brush = h; break;
        case ObjType::Font: old = dc->font; dc->font = h; break;
        case ObjType::Bitmap: old = dc->bitmap; dc->bitmap = h; break;
        case ObjType::Palette: old = dc->palette; dc->palette = h; break;
        default: break;
    }
    a.ret(old);
}
API(GDI, 69, DeleteObject, 2) {
    uint16_t h = a.w();
    GdiObject* o = gdi_get(h);
    if (o && !o->stock) g_objs.erase(h);
    a.ret(o ? 1 : 0);
}
API(GDI, 87, GetStockObject, 2) { a.ret(gdi_stock(a.i())); }
API(GDI, 150, UnrealizeObject, 2) { a.w(); a.ret(1); }

API(GDI, 66, CreateSolidBrush, 4) {
    auto o = std::make_unique<GdiObject>(); o->type = ObjType::Brush; o->color = colorref(a.l());
    a.ret(gdi_add(std::move(o)));
}
API(GDI, 50, CreateBrushIndirect, 4) {
    uint32_t p = a.p(); uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    auto b = std::make_unique<GdiObject>(); b->type = ObjType::Brush;
    b->style = c.rd16(s, o); b->color = colorref(c.rd32(s, uint16_t(o + 2))); b->hatch = c.rd16(s, uint16_t(o + 6));
    if (b->style == 3) {   // BS_PATTERN: lbHatch is an HBITMAP
        if (GdiObject* bm = gdi_get(uint16_t(b->hatch))) b->pattern = bm->bitmap;
    }
    if (b->style == 2) b->style = 0;   // hatched -> solid colour
    a.ret(gdi_add(std::move(b)));
}
API(GDI, 62, CreatePenIndirect, 4) {
    uint32_t p = a.p(); uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    auto pen = std::make_unique<GdiObject>(); pen->type = ObjType::Pen;
    pen->style = c.rd16(s, o); pen->width = std::max<int>(1, int16_t(c.rd16(s, uint16_t(o + 2))));
    pen->color = colorref(c.rd32(s, uint16_t(o + 6)));
    a.ret(gdi_add(std::move(pen)));
}
API(GDI, 57, CreateFontIndirect, 4) {
    uint32_t p = a.p(); uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    auto f = std::make_unique<GdiObject>(); f->type = ObjType::Font;
    f->height = int16_t(c.rd16(s, o)); f->weight = c.rd16(s, uint16_t(o + 8));
    f->italic = c.rd8(s, uint16_t(o + 10)); f->underline = c.rd8(s, uint16_t(o + 11));
    f->face = recomp::read_cstr(c, (uint32_t(s) << 16) | uint16_t(o + 18), 32);
    if (f->height == 0) f->height = 13;
    a.ret(gdi_add(std::move(f)));
}

// ---- bitmaps ----
static std::shared_ptr<Surface> surface_from_bits(Cpu& c, int w, int h, int bpp, uint32_t bits, bool dib,
                                                  const std::vector<Color>& pal) {
    auto s = std::make_shared<Surface>(w, h, bpp == 1 ? 1 : 8);
    if (!bits || w <= 0 || h <= 0) return s;
    int stride = dib ? ((w * bpp + 31) / 32) * 4 : ((w * bpp + 15) / 16) * 2;
    std::vector<uint8_t> raw(size_t(stride) * h);
    huge_read(c, bits, raw.data(), uint32_t(raw.size()));
    for (int y = 0; y < h; ++y) {
        int row = dib ? h - 1 - y : y;
        const uint8_t* r = raw.data() + size_t(row) * stride;
        for (int x = 0; x < w; ++x) {
            Color col = 0;
            switch (bpp) {
                case 1: { int i = (r[x / 8] >> (7 - x % 8)) & 1;
                          col = bpp == 1 && !dib ? (i ? 0xFFFFFF : 0) : (size_t(i) < pal.size() ? pal[i] : (i ? 0xFFFFFF : 0)); break; }
                case 4: { int i = (x & 1) ? (r[x / 2] & 15) : (r[x / 2] >> 4); col = size_t(i) < pal.size() ? pal[i] : 0; break; }
                case 8: { int i = r[x]; col = size_t(i) < pal.size() ? pal[i] : nearest_index_color(uint8_t(i)); break; }
                case 24: col = (uint32_t(r[x * 3 + 2]) << 16) | (uint32_t(r[x * 3 + 1]) << 8) | r[x * 3]; break;
                default: break;
            }
            if (bpp == 1 && dib && s->bpp == 1) col = (col == 0xFFFFFF || (col & 0xFFFFFF) > 0x7F7F7F) ? 0xFFFFFF : 0;
            s->set(x, y, col);
        }
    }
    return s;
}

API(GDI, 48, CreateBitmap, 12) {
    uint32_t bits = a.p(); uint16_t bpp = a.w(); uint16_t planes = a.w(); int16_t h = a.i(); int16_t w = a.i();
    int depth = bpp * planes;
    a.ret(gdi_create_bitmap(surface_from_bits(c, w, h, depth, bits, false, system_palette())));
}
API(GDI, 51, CreateCompatibleBitmap, 6) {
    int16_t h = a.i(); int16_t w = a.i(); uint16_t hdc = a.w();
    DC* dc = dc_get(hdc);
    int bpp = 8;
    if (dc && dc->memory) { Surface* s = dc_surface(*dc); if (s) bpp = s->bpp; }
    a.ret(gdi_create_bitmap(std::make_shared<Surface>(std::max<int>(w, 0), std::max<int>(h, 0), bpp)));
}
static std::vector<Color> read_dib_palette(Cpu& c, uint32_t bmi, int hdrSize, int bpp, int clrUsed, int usage, DC* dc) {
    std::vector<Color> pal;
    int n = clrUsed ? clrUsed : (bpp <= 8 ? 1 << bpp : 0);
    uint16_t s = uint16_t(bmi >> 16);
    uint16_t o = uint16_t(uint16_t(bmi) + hdrSize);
    std::vector<Color> dcpal;
    if (usage == 1 && dc) if (GdiObject* p = gdi_get(dc->palette)) dcpal = p->entries;
    for (int i = 0; i < n; ++i) {
        if (usage == 1) {   // DIB_PAL_COLORS: WORD indices into the DC palette
            uint16_t idx = c.rd16(s, uint16_t(o + 2 * i));
            pal.push_back(idx < dcpal.size() ? dcpal[idx] : nearest_index_color(uint8_t(idx)));
        } else if (hdrSize == 12) {
            uint16_t q = uint16_t(o + 3 * i);
            pal.push_back((uint32_t(c.rd8(s, uint16_t(q + 2))) << 16) | (uint32_t(c.rd8(s, uint16_t(q + 1))) << 8) | c.rd8(s, q));
        } else {
            uint16_t q = uint16_t(o + 4 * i);
            pal.push_back((uint32_t(c.rd8(s, uint16_t(q + 2))) << 16) | (uint32_t(c.rd8(s, uint16_t(q + 1))) << 8) | c.rd8(s, q));
        }
    }
    return pal;
}
// Decode BI_RLE8 / BI_RLE4 data into palette indices (bottom-up rows).
// The data is read byte by byte from a huge pointer, so a zero biSizeImage
// (legal for RLE) works: decoding stops at the end-of-bitmap marker.
static std::vector<uint8_t> decode_rle(Cpu& c, uint32_t fp, uint32_t limit, int w, int h, bool rle4) {
    std::vector<uint8_t> out(size_t(w) * h, 0);
    int x = 0, y = 0;
    uint32_t i = 0;
    auto rd = [&](uint32_t k) -> uint8_t {
        uint32_t off = (fp & 0xFFFF) + k;
        uint16_t sel = uint16_t((fp >> 16) + (off >> 16) * 8);
        return c.rd8(sel, uint16_t(off));
    };
    auto put = [&](uint8_t v) { if (x < w && y < h) out[size_t(y) * w + x] = v; ++x; };
    while (i + 1 < limit && y < h) {
        uint8_t n = rd(i), v = rd(i + 1);
        i += 2;
        if (n) {   // encoded run
            for (int k = 0; k < n; ++k) put(rle4 ? ((k & 1) ? (v & 15) : (v >> 4)) : v);
        } else if (v == 0) { x = 0; ++y; }          // end of line
        else if (v == 1) { break; }                 // end of bitmap
        else if (v == 2) {                          // delta
            x += rd(i); y += rd(i + 1); i += 2;
        } else {                                    // absolute run of v pixels
            int bytes = rle4 ? (v + 1) / 2 : v;
            for (int k = 0; k < v; ++k) {
                uint8_t b = rd(i + uint32_t(rle4 ? k / 2 : k));
                put(rle4 ? ((k & 1) ? (b & 15) : (b >> 4)) : b);
            }
            i += uint32_t((bytes + 1) & ~1);        // word aligned
        }
    }
    return out;
}

API(GDI, 442, CreateDIBitmap, 20) {
    uint16_t usage = a.w(); uint32_t bmi = a.p(); uint32_t bits = a.p(); uint32_t init = a.l(); uint32_t hdr = a.p(); uint16_t hdc = a.w();
    uint16_t s = uint16_t(hdr >> 16), o = uint16_t(hdr);
    int hs = int(c.rd32(s, o));
    int w, h, bpp, clr = 0;
    if (hs == 12) { w = c.rd16(s, uint16_t(o + 4)); h = c.rd16(s, uint16_t(o + 6)); bpp = c.rd16(s, uint16_t(o + 10)); }
    else { w = int(c.rd32(s, uint16_t(o + 4))); h = int(c.rd32(s, uint16_t(o + 8))); bpp = c.rd16(s, uint16_t(o + 14)); clr = int(c.rd32(s, uint16_t(o + 32))); }
    if (!(init & 4) || !bmi) {   // CBM_INIT not set
        a.ret(gdi_create_bitmap(std::make_shared<Surface>(w, std::abs(h), bpp == 1 ? 1 : 8)));
        return;
    }
    auto pal = read_dib_palette(c, bmi, hs, bpp, clr, usage, dc_get(hdc));
    uint32_t compression = hs >= 40 ? c.rd32(s, uint16_t(o + 16)) : 0;
    if ((compression == 1 || compression == 2) && bits) {
        uint32_t sizeImage = c.rd32(s, uint16_t(o + 20));
        int ah = std::abs(h);
        auto idx = decode_rle(c, bits, sizeImage ? sizeImage : 0xFFFFFFFFu, w, ah, compression == 2);
        auto surf = std::make_shared<Surface>(w, ah, 8);
        for (int y = 0; y < ah; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t i = idx[size_t(ah - 1 - y) * w + x];
                surf->set(x, y, i < pal.size() ? pal[i] : 0);
            }
        if (const char* dir = std::getenv("MYULK_DUMP_DIB"); dir && w >= 400) {
            static int n = 0;
            std::string path = std::string(dir) + "/dib" + std::to_string(n++) + ".bmp";
            SDL_Surface* ss = SDL_CreateSurfaceFrom(w, ah, SDL_PIXELFORMAT_XRGB8888, surf->px.data(), w * 4);
            if (ss) { SDL_SaveBMP(ss, path.c_str()); SDL_DestroySurface(ss); }
            recomp::log("dumped RLE dib %dx%d -> %s", w, ah, path.c_str());
        }
        a.ret(gdi_create_bitmap(surf));
        return;
    }
    auto surf = surface_from_bits(c, w, std::abs(h), bpp, bits, true, pal);
    surf->bpp = bpp == 1 ? 1 : 8;
    a.ret(gdi_create_bitmap(surf));
}
API(GDI, 441, GetDIBits, 18) {
    uint16_t usage = a.w(); uint32_t bmi = a.p(); uint32_t bits = a.p(); uint16_t lines = a.w(); uint16_t start = a.w();
    uint16_t hbm = a.w(); uint16_t hdc = a.w();
    (void)usage;
    GdiObject* b = gdi_get(hbm);
    if (!b || !b->bitmap || !bmi) { a.ret(0); return; }
    Surface& sf = *b->bitmap;
    uint16_t s = uint16_t(bmi >> 16), o = uint16_t(bmi);
    int bpp = c.rd16(s, uint16_t(o + 14));
    if (bpp == 0 || !bits) {
        bpp = sf.bpp == 1 ? 1 : 8;
        c.wr32(s, o, 40);
        c.wr32(s, uint16_t(o + 4), uint32_t(sf.w));
        c.wr32(s, uint16_t(o + 8), uint32_t(sf.h));
        c.wr16(s, uint16_t(o + 12), 1);
        c.wr16(s, uint16_t(o + 14), uint16_t(bpp));
        c.wr32(s, uint16_t(o + 16), 0);
        c.wr32(s, uint16_t(o + 20), uint32_t(((sf.w * bpp + 31) / 32) * 4 * sf.h));
        c.wr32(s, uint16_t(o + 32), 0);
        if (!bits) { a.ret(uint16_t(sf.h)); return; }
    }
    // palette: the DC's palette (or the system palette) for 8 bpp
    std::vector<Color> pal;
    DC* dc = dc_get(hdc);
    if (bpp == 8) {
        if (dc) if (GdiObject* p = gdi_get(dc->palette)) if (p->entries.size() > 20) pal = p->entries;
        if (pal.empty()) pal = system_palette();
        pal.resize(256, 0);
    } else if (bpp == 1) {
        pal = {0x000000, 0xFFFFFF};
    }
    for (size_t i = 0; i < pal.size(); ++i) {
        uint16_t q = uint16_t(o + 40 + 4 * i);
        c.wr8(s, q, uint8_t(pal[i])); c.wr8(s, uint16_t(q + 1), uint8_t(pal[i] >> 8));
        c.wr8(s, uint16_t(q + 2), uint8_t(pal[i] >> 16)); c.wr8(s, uint16_t(q + 3), 0);
    }
    int stride = ((sf.w * bpp + 31) / 32) * 4;
    std::vector<uint8_t> raw(size_t(stride) * lines, 0);
    std::map<Color, uint8_t> cache;
    for (int l = 0; l < lines; ++l) {
        int y = sf.h - 1 - (start + l);
        if (y < 0 || y >= sf.h) continue;
        uint8_t* r = raw.data() + size_t(l) * stride;
        for (int x = 0; x < sf.w; ++x) {
            Color col = sf.get(x, y);
            if (bpp == 24) { r[x * 3] = uint8_t(col); r[x * 3 + 1] = uint8_t(col >> 8); r[x * 3 + 2] = uint8_t(col >> 16); }
            else if (bpp == 1) { if (col) r[x / 8] |= uint8_t(0x80 >> (x % 8)); }
            else if (bpp == 8) {
                auto it = cache.find(col);
                uint8_t idx;
                if (it != cache.end()) idx = it->second;
                else {
                    int best = 0, bestd = 1 << 30;
                    for (size_t i = 0; i < pal.size(); ++i) {
                        int dr = int((pal[i] >> 16) & 255) - int((col >> 16) & 255);
                        int dg = int((pal[i] >> 8) & 255) - int((col >> 8) & 255);
                        int db = int(pal[i] & 255) - int(col & 255);
                        int d = dr * dr + dg * dg + db * db;
                        if (d < bestd) { bestd = d; best = int(i); if (!d) break; }
                    }
                    idx = uint8_t(best);
                    cache[col] = idx;
                }
                r[x] = idx;
            }
        }
    }
    huge_write(c, bits, raw.data(), uint32_t(raw.size()));
    a.ret(lines);
}
API(GDI, 74, GetBitmapBits, 10) {
    uint32_t bits = a.p(); uint32_t count = a.l(); uint16_t hbm = a.w();
    GdiObject* b = gdi_get(hbm);
    if (!b || !b->bitmap) { a.ret32(0); return; }
    Surface& sf = *b->bitmap;
    int bpp = sf.bpp == 1 ? 1 : 8;
    int stride = ((sf.w * bpp + 15) / 16) * 2;
    std::vector<uint8_t> raw(size_t(stride) * sf.h, 0);
    for (int y = 0; y < sf.h; ++y)
        for (int x = 0; x < sf.w; ++x) {
            Color col = sf.get(x, y);
            if (bpp == 1) { if (col) raw[size_t(y) * stride + x / 8] |= uint8_t(0x80 >> (x % 8)); }
            else raw[size_t(y) * stride + x] = nearest_index(col);
        }
    uint32_t n = std::min<uint32_t>(count, uint32_t(raw.size()));
    huge_write(c, bits, raw.data(), n);
    a.ret32(n);
}
API(GDI, 82, GetObject, 8) {
    uint32_t buf = a.p(); int16_t size = a.i(); uint16_t h = a.w();
    GdiObject* o = gdi_get(h);
    if (!o) { a.ret(0); return; }
    uint16_t s = uint16_t(buf >> 16), off = uint16_t(buf);
    std::vector<uint8_t> out;
    auto w16 = [&](int v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
    auto w32 = [&](uint32_t v) { w16(int(v & 0xFFFF)); w16(int(v >> 16)); };
    switch (o->type) {
        case ObjType::Bitmap: {
            Surface& sf = *o->bitmap;
            int bpp = sf.bpp == 1 ? 1 : 8;
            w16(0); w16(sf.w); w16(sf.h); w16(((sf.w * bpp + 15) / 16) * 2);
            out.push_back(1); out.push_back(uint8_t(bpp)); w32(0);
            break;
        }
        case ObjType::Pen: w16(o->style); w16(o->width); w16(0); w32(to_colorref(o->color)); break;
        case ObjType::Brush: w16(o->style); w32(to_colorref(o->color)); w16(o->hatch); break;
        case ObjType::Font: {
            w16(o->height); w16(0); w16(0); w16(0); w16(o->weight);
            out.push_back(o->italic); out.push_back(o->underline); out.push_back(0); out.push_back(0);
            out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
            for (int i = 0; i < 32; ++i) out.push_back(i < int(o->face.size()) ? uint8_t(o->face[i]) : 0);
            break;
        }
        case ObjType::Palette: w16(int(o->entries.size())); break;
        default: break;
    }
    if (!buf) { a.ret(uint16_t(out.size())); return; }
    int n = std::min<int>(size, int(out.size()));
    for (int i = 0; i < n; ++i) c.wr8(s, uint16_t(off + i), out[size_t(i)]);
    a.ret(uint16_t(n));
}

// ---- DCs ----
API(GDI, 52, CreateCompatibleDC, 2) { a.w(); a.ret(dc_create_memory()); }
API(GDI, 53, CreateDC, 16) {
    a.p(); a.p(); a.p(); a.p();
    uint16_t h = dc_create_memory();
    DC* dc = dc_get(h); dc->memory = false; dc->info = false;
    dc->clip = Rect{0, 0, 640, 480};
    a.ret(h);
}
API(GDI, 153, CreateIC, 16) {
    a.p(); a.p(); a.p(); a.p();
    uint16_t h = dc_create_memory();
    DC* dc = dc_get(h); dc->memory = false; dc->info = true;
    a.ret(h);
}
API(GDI, 68, DeleteDC, 2) { dc_delete(a.w()); a.ret(1); }
API(GDI, 80, GetDeviceCaps, 4) {
    int16_t idx = a.i(); a.w();
    int v = 0;
    switch (idx) {
        case 0: v = 0x300; break; case 2: v = 1; break; case 4: v = 208; break; case 6: v = 156; break;
        case 8: v = 640; break; case 10: v = 480; break; case 12: v = 8; break; case 14: v = 1; break;
        case 16: v = -1; break; case 18: v = 100; break; case 22: v = 0; break; case 24: v = 20; break;
        case 34: v = 0x0007; break; case 36: v = 1; break; case 38: v = 0x0E99; break;
        case 40: v = 36; break; case 42: v = 36; break; case 44: v = 51; break;
        case 88: v = 96; break; case 90: v = 96; break;
        case 104: v = 256; break; case 106: v = 20; break; case 108: v = 18; break;
        default: v = 0; break;
    }
    a.ret(uint16_t(v));
}
API(GDI, 128, MulDiv, 6) {
    int16_t den = a.i(); int16_t num = a.i(); int16_t v = a.i();
    if (!den) { a.ret(0xFFFF); return; }
    long long r = (long long)v * num;
    r = r >= 0 ? (r + std::abs(den) / 2) / den : (r - std::abs(den) / 2) / den;
    a.ret(uint16_t(int16_t(r)));
}
API(GDI, 473, GetViewportOrgEx, 6) {
    uint32_t pt = a.p(); uint16_t hdc = a.w(); DCARG(dc); write_point(c, pt, dc->vpOrgX, dc->vpOrgY); a.ret(1);
}
API(GDI, 480, SetViewportOrgEx, 10) {
    uint32_t pt = a.p(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    write_point(c, pt, dc->vpOrgX, dc->vpOrgY); dc->vpOrgX = x; dc->vpOrgY = y; a.ret(1);
}
API(GDI, 479, SetViewportExtEx, 10) {
    uint32_t pt = a.p(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    write_point(c, pt, dc->vpExtX, dc->vpExtY); if (dc->mapMode >= 7) { dc->vpExtX = x; dc->vpExtY = y; } a.ret(1);
}
API(GDI, 481, SetWindowExtEx, 10) {
    uint32_t pt = a.p(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w(); DCARG(dc);
    write_point(c, pt, dc->winExtX, dc->winExtY); if (dc->mapMode >= 7) { dc->winExtX = x; dc->winExtY = y; } a.ret(1);
}

// ---- palettes ----
API(GDI, 360, CreatePalette, 4) {
    uint32_t p = a.p(); uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    int n = c.rd16(s, uint16_t(o + 2));
    auto pal = std::make_unique<GdiObject>(); pal->type = ObjType::Palette;
    for (int i = 0; i < n; ++i) {
        uint16_t q = uint16_t(o + 4 + 4 * i);
        pal->entries.push_back((uint32_t(c.rd8(s, q)) << 16) | (uint32_t(c.rd8(s, uint16_t(q + 1))) << 8) | c.rd8(s, uint16_t(q + 2)));
    }
    a.ret(gdi_add(std::move(pal)));
}
API(GDI, 363, GetPaletteEntries, 10) {
    uint32_t buf = a.p(); uint16_t n = a.w(); uint16_t start = a.w(); uint16_t h = a.w();
    GdiObject* p = gdi_get(h);
    if (!p) { a.ret(0); return; }
    if (!buf) { a.ret(uint16_t(p->entries.size())); return; }
    uint16_t s = uint16_t(buf >> 16), o = uint16_t(buf);
    int k = 0;
    for (; k < n && start + k < int(p->entries.size()); ++k) {
        Color col = p->entries[size_t(start + k)];
        c.wr8(s, uint16_t(o + 4 * k), uint8_t(col >> 16)); c.wr8(s, uint16_t(o + 4 * k + 1), uint8_t(col >> 8));
        c.wr8(s, uint16_t(o + 4 * k + 2), uint8_t(col)); c.wr8(s, uint16_t(o + 4 * k + 3), 0);
    }
    a.ret(uint16_t(k));
}
API(GDI, 375, GetSystemPaletteEntries, 10) {
    uint32_t buf = a.p(); uint16_t n = a.w(); uint16_t start = a.w(); a.w();
    uint16_t s = uint16_t(buf >> 16), o = uint16_t(buf);
    for (int k = 0; k < n && start + k < 256; ++k) {
        Color col = system_palette()[size_t(start + k)];
        c.wr8(s, uint16_t(o + 4 * k), uint8_t(col >> 16)); c.wr8(s, uint16_t(o + 4 * k + 1), uint8_t(col >> 8));
        c.wr8(s, uint16_t(o + 4 * k + 2), uint8_t(col)); c.wr8(s, uint16_t(o + 4 * k + 3), 0);
    }
    a.ret(n);
}

// ---- metafiles and printing: not used for display ----
API(GDI, 123, PlayMetaFile, 4) { a.w(); a.w(); a.ret(1); }
API(GDI, 127, DeleteMetaFile, 2) { a.w(); a.ret(1); }
API(GDI, 151, CopyMetaFile, 6) { a.p(); a.w(); a.ret(0); }
API(GDI, 159, GetMetaFileBits, 2) { a.ret(a.w()); }
API(GDI, 196, SetMetaFileBitsBetter, 2) { a.ret(a.w()); }
API(GDI, 378, EndDoc, 2) { a.w(); a.ret(1); }
API(GDI, 380, EndPage, 2) { a.w(); a.ret(1); }
