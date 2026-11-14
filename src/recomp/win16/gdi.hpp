// GDI: device contexts, bitmaps, brushes, pens, fonts, palettes; software
// rendering into 32-bit surfaces (the screen frame buffer or bitmaps).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace win16 {

using Color = uint32_t;   // 0x00RRGGBB

struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
    int w() const { return right - left; }
    int h() const { return bottom - top; }
    bool contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
    Rect intersect(const Rect& o) const {
        Rect r{std::max(left, o.left), std::max(top, o.top), std::min(right, o.right), std::min(bottom, o.bottom)};
        if (r.empty()) r = Rect{};
        return r;
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return Rect{std::min(left, o.left), std::min(top, o.top), std::max(right, o.right), std::max(bottom, o.bottom)};
    }
    Rect offset(int dx, int dy) const { return Rect{left + dx, top + dy, right + dx, bottom + dy}; }
    bool operator==(const Rect& o) const {
        return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

// Pixel storage for bitmaps and the screen. bpp 1 marks a monochrome bitmap
// (pixels still stored as 0x000000 / 0xFFFFFF).
struct Surface {
    int w = 0, h = 0;
    int bpp = 8;   // what the program is told
    std::vector<Color> px;
    Surface() = default;
    Surface(int w_, int h_, int bpp_) : w(w_), h(h_), bpp(bpp_), px(size_t(std::max(w_, 0)) * std::max(h_, 0), 0) {}
    Color get(int x, int y) const { return px[size_t(y) * w + x]; }
    void set(int x, int y, Color c) { px[size_t(y) * w + x] = c; }
};

enum class ObjType : uint8_t { Pen, Brush, Font, Bitmap, Palette, Region };

struct GdiObject {
    ObjType type;
    bool stock = false;
    // pen / brush
    int style = 0;          // PS_* / BS_*
    int width = 1;
    Color color = 0;
    int hatch = 0;
    std::shared_ptr<Surface> pattern;   // BS_PATTERN
    // font
    int height = 13, weight = 400;
    bool italic = false, underline = false;
    std::string face;
    // bitmap
    std::shared_ptr<Surface> bitmap;
    // palette
    std::vector<Color> entries;
};

struct DC {
    uint16_t handle = 0;
    uint16_t hwnd = 0;            // window DC
    bool memory = false;
    bool info = false;            // CreateIC / CreateDC("DISPLAY")
    uint16_t pen = 0, brush = 0, font = 0, bitmap = 0, palette = 0;
    int originX = 0, originY = 0; // device origin of the window client area on the screen
    Rect clip;                    // device coordinates
    Color textColor = 0x000000, bkColor = 0xFFFFFF;
    int bkMode = 2;               // OPAQUE
    int rop2 = 13;                // R2_COPYPEN
    int mapMode = 1;              // MM_TEXT
    int stretchMode = 1;
    int curX = 0, curY = 0;
    int vpOrgX = 0, vpOrgY = 0, winOrgX = 0, winOrgY = 0;
    int vpExtX = 1, vpExtY = 1, winExtX = 1, winExtY = 1;
    struct Saved;
    std::vector<std::shared_ptr<DC>> saved;
};

// object handles
uint16_t gdi_add(std::unique_ptr<GdiObject> obj);
GdiObject* gdi_get(uint16_t h);
uint16_t gdi_stock(int index);
uint16_t gdi_create_bitmap(std::shared_ptr<Surface> s);

// device contexts
DC* dc_get(uint16_t h);
uint16_t dc_create_window(uint16_t hwnd, int originX, int originY, const Rect& clip);
uint16_t dc_create_memory();
void dc_delete(uint16_t h);
Surface* dc_surface(DC& dc);     // target pixels (screen or selected bitmap)

// drawing primitives in logical coordinates of a DC
void draw_fill_rect(DC& dc, const Rect& r, uint16_t brush);
void draw_frame_rect(DC& dc, const Rect& r, uint16_t brush);
void draw_text(DC& dc, int x, int y, const std::string& text, const Rect* clipOpaque, int options);
void draw_line(DC& dc, int x0, int y0, int x1, int y1);
void blit(DC& dst, int dx, int dy, int dw, int dh, DC* src, int sx, int sy, int sw, int sh, uint32_t rop);
int text_width(uint16_t font, const std::string& text);
int text_height(uint16_t font);

// the screen
Surface& screen();
// the palette last realised (for 8-bit conversions)
std::vector<Color>& system_palette();
Color nearest_index_color(uint8_t index);
uint8_t nearest_index(Color c);

void gdi_init();

}  // namespace win16
