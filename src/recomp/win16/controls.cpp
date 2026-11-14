// Native window procedures for the Windows 3.1 predefined control classes
// (BUTTON, LISTBOX, EDIT, COMBOBOX, STATIC). VCL subclasses these: it reads
// the class procedure with GetClassInfo and forwards unhandled messages to
// it with CallWindowProc, so each procedure is registered as a host import.
//
// Message numbers are the Win16 ones (WM_USER based, e.g. LB_ADDSTRING =
// 0x401); notifications go to the parent as WM_COMMAND with
// lParam = MAKELONG(hwnd, code). Colours come from WM_CTLCOLOR, owner-drawn
// controls use WM_DRAWITEM / WM_MEASUREITEM like Windows does.
#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/vfs.hpp"
#include "recomp/win16/win16.hpp"

using recomp::Args;
using recomp::Cpu;
using recomp::Runtime;

namespace win16 {
namespace {

constexpr uint32_t WS_VSCROLL = 0x00200000u, WS_BORDER = 0x00800000u;
constexpr uint16_t ODT_LISTBOX = 2, ODT_COMBOBOX = 3, ODT_BUTTON = 4;
constexpr uint16_t ODA_DRAWENTIRE = 1;
constexpr uint16_t ODS_SELECTED = 1, ODS_DISABLED = 4, ODS_FOCUS = 0x10;
constexpr uint16_t CTLCOLOR_EDIT = 1, CTLCOLOR_LISTBOX = 2, CTLCOLOR_BTN = 3, CTLCOLOR_STATIC = 6;
constexpr int kScrollW = 16;

struct Item {
    std::string text;
    uint32_t data = 0;
    bool selected = false;   // multi-select list boxes
};

struct Ctl {
    uint16_t font = 0;
    // button
    int check = 0;
    bool pressed = false, tracking = false;
    // list box / combo box
    std::vector<Item> items;
    int cur = -1, top = 0, itemH = 0, caretIdx = 0;
    bool dropped = false;
    // edit
    size_t selStart = 0, selEnd = 0;
    bool modified = false;
    int limit = 0;
    bool readOnly = false;
    char password = 0;
};

std::map<uint16_t, Ctl> g_ctls;
uint16_t g_mem = 0;   // emulated memory for DRAWITEMSTRUCT / MEASUREITEMSTRUCT
std::map<std::string, uint32_t> g_procs;

Ctl& ctl(uint16_t h) { return g_ctls[h]; }
uint32_t mklong(int lo, int hi) { return (uint32_t(uint16_t(hi)) << 16) | uint16_t(lo); }

void notify(uint16_t h, uint16_t code) {
    WndInfo wi;
    if (!wnd_info(h, wi) || !wi.parent) return;
    send_message(wi.parent, WM_COMMAND, wi.id, mklong(h, code));
}

int line_height(uint16_t h) {
    int th = text_height(ctl(h).font);
    return th > 0 ? th : 16;
}

// Background brush from the parent (WM_CTLCOLOR), also setting the DC colours.
uint16_t ctl_color(uint16_t h, uint16_t hdc, uint16_t type) {
    WndInfo wi;
    uint16_t br = 0;
    if (wnd_info(h, wi) && wi.parent) br = uint16_t(send_message(wi.parent, WM_CTLCOLOR, hdc, mklong(h, type)));
    if (!br || !gdi_get(br)) {
        auto b = std::make_unique<GdiObject>();
        b->type = ObjType::Brush;
        b->color = type == CTLCOLOR_BTN ? 0xC0C0C0 : 0xFFFFFF;
        br = gdi_add(std::move(b));
    }
    return br;
}

uint16_t solid(Color c) {
    static std::map<Color, uint16_t> cache;
    auto it = cache.find(c);
    if (it != cache.end()) return it->second;
    auto b = std::make_unique<GdiObject>();
    b->type = ObjType::Brush;
    b->color = c;
    b->stock = true;
    return cache[c] = gdi_add(std::move(b));
}

void fill(DC& dc, Rect r, Color c) { draw_fill_rect(dc, r, solid(c)); }
void frame(DC& dc, Rect r, Color c) { draw_frame_rect(dc, r, solid(c)); }

void bevel(DC& dc, Rect r, bool sunken) {
    Color tl = sunken ? 0x808080 : 0xFFFFFF, br = sunken ? 0xFFFFFF : 0x808080;
    fill(dc, Rect{r.left, r.top, r.right, r.top + 1}, tl);
    fill(dc, Rect{r.left, r.top, r.left + 1, r.bottom}, tl);
    fill(dc, Rect{r.left, r.bottom - 1, r.right, r.bottom}, br);
    fill(dc, Rect{r.right - 1, r.top, r.right, r.bottom}, br);
}

void text_at(DC& dc, int x, int y, const std::string& s, Color col) {
    Color old = dc.textColor;
    int oldMode = dc.bkMode;
    dc.textColor = col;
    dc.bkMode = 1;   // TRANSPARENT
    draw_text(dc, x, y, s, nullptr, 0);
    dc.textColor = old;
    dc.bkMode = oldMode;
}

// Paint helper: DC from WM_PAINT's wParam or a fresh one for the client area.
struct PaintDC {
    uint16_t hdc = 0;
    bool own = false;
    PaintDC(uint16_t h, uint16_t wp) {
        if (wp && dc_get(wp)) hdc = wp;
        else { hdc = wnd_paint_begin(h); own = true; }
        if (DC* dc = dc_get(hdc)) dc->font = ctl(h).font;
    }
    ~PaintDC() { if (own && hdc) wnd_paint_end(hdc); }
    DC* get() const { return dc_get(hdc); }
};

// ---- owner draw -----------------------------------------------------------

void draw_item(uint16_t h, uint16_t hdc, uint16_t type, int index, Rect r, uint16_t state, uint32_t data) {
    WndInfo wi;
    if (!wnd_info(h, wi) || !wi.parent) return;
    Cpu& c = Runtime::get().cpu();
    uint16_t o = 0x100;
    c.wr16(g_mem, o, type);
    c.wr16(g_mem, uint16_t(o + 2), wi.id);
    c.wr16(g_mem, uint16_t(o + 4), uint16_t(index));
    c.wr16(g_mem, uint16_t(o + 6), ODA_DRAWENTIRE);
    c.wr16(g_mem, uint16_t(o + 8), state);
    c.wr16(g_mem, uint16_t(o + 10), h);
    c.wr16(g_mem, uint16_t(o + 12), hdc);
    c.wr16(g_mem, uint16_t(o + 14), uint16_t(r.left));
    c.wr16(g_mem, uint16_t(o + 16), uint16_t(r.top));
    c.wr16(g_mem, uint16_t(o + 18), uint16_t(r.right));
    c.wr16(g_mem, uint16_t(o + 20), uint16_t(r.bottom));
    c.wr32(g_mem, uint16_t(o + 22), data);
    send_message(wi.parent, WM_DRAWITEM, wi.id, (uint32_t(g_mem) << 16) | o);
}

int measure_item(uint16_t h, uint16_t type, int def) {
    WndInfo wi;
    if (!wnd_info(h, wi) || !wi.parent) return def;
    Cpu& c = Runtime::get().cpu();
    uint16_t o = 0x200;
    c.wr16(g_mem, o, type);
    c.wr16(g_mem, uint16_t(o + 2), wi.id);
    c.wr16(g_mem, uint16_t(o + 4), 0);
    c.wr16(g_mem, uint16_t(o + 6), uint16_t(wi.w));
    c.wr16(g_mem, uint16_t(o + 8), uint16_t(def));
    c.wr32(g_mem, uint16_t(o + 10), 0);
    send_message(wi.parent, WM_MEASUREITEM, wi.id, (uint32_t(g_mem) << 16) | o);
    int v = c.rd16(g_mem, uint16_t(o + 8));
    return v > 0 && v < 200 ? v : def;
}

// ---- common messages ------------------------------------------------------

// Messages every control handles the same way; returns true if handled.
bool common(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t& r) {
    (void)lp;
    switch (msg) {
        case WM_SETFONT:
            ctl(h).font = wp;
            if (lp & 0xFFFF) wnd_invalidate(h);
            r = 0;
            return true;
        case WM_GETFONT: r = ctl(h).font; return true;
        case WM_ERASEBKGND: r = 1; return true;
        case WM_ENABLE: wnd_invalidate(h); r = 0; return true;
        case WM_NCDESTROY: g_ctls.erase(h); r = 0; return true;
        default: return false;
    }
}

// ---- BUTTON ---------------------------------------------------------------

constexpr uint16_t BM_GETCHECK = 0x400, BM_SETCHECK = 0x401, BM_GETSTATE = 0x402, BM_SETSTATE = 0x403,
                   BM_SETSTYLE = 0x404;

void paint_button(uint16_t h, uint16_t wp) {
    WndInfo wi;
    if (!wnd_info(h, wi)) return;
    Ctl& b = ctl(h);
    PaintDC pdc(h, wp);
    DC* dc = pdc.get();
    if (!dc) return;
    int kind = int(wi.style & 0x0F);
    Rect r{0, 0, wi.w, wi.h};
    std::string text = wnd_text(h);
    text.erase(std::remove(text.begin(), text.end(), '&'), text.end());
    if (kind == 0x0B) {   // BS_OWNERDRAW
        uint16_t st = uint16_t((b.pressed ? ODS_SELECTED : 0) | (wi.enabled ? 0 : ODS_DISABLED) |
                               (wnd_focus() == h ? ODS_FOCUS : 0));
        draw_item(h, pdc.hdc, ODT_BUTTON, 0, r, st, 0);
        return;
    }
    Color fg = wi.enabled ? dc->textColor : 0x808080;
    int th = line_height(h);
    if (kind == 0 || kind == 1) {   // push buttons
        uint16_t br = ctl_color(h, pdc.hdc, CTLCOLOR_BTN);
        (void)br;
        fill(*dc, r, 0xC0C0C0);
        frame(*dc, r, 0x000000);
        Rect in{1, 1, r.right - 1, r.bottom - 1};
        if (kind == 1) { frame(*dc, in, 0x000000); in = Rect{2, 2, r.right - 2, r.bottom - 2}; }
        bevel(*dc, in, b.pressed);
        int tw = text_width(b.font, text);
        int off = b.pressed ? 1 : 0;
        text_at(*dc, (r.w() - tw) / 2 + off, (r.h() - th) / 2 + off, text, wi.enabled ? 0x000000 : 0x808080);
        return;
    }
    uint16_t br = ctl_color(h, pdc.hdc, CTLCOLOR_BTN);
    draw_fill_rect(*dc, r, br);
    if (kind == 7) {   // group box
        frame(*dc, Rect{0, th / 2, r.right, r.bottom}, 0x808080);
        if (!text.empty()) {
            int tw = text_width(b.font, text);
            draw_fill_rect(*dc, Rect{6, 0, 10 + tw, th}, br);
            text_at(*dc, 8, 0, text, fg);
        }
        return;
    }
    bool radio = kind == 4 || kind == 9;
    int box = 12;
    int by = (r.h() - box) / 2;
    Rect bx{0, by, box, by + box};
    fill(*dc, bx, 0xFFFFFF);
    if (radio) {
        // a rough circle: frame minus corners
        frame(*dc, bx, 0x808080);
        fill(*dc, Rect{0, by, 2, by + 2}, dc->bkColor);
        if (b.check) fill(*dc, Rect{4, by + 4, 8, by + 8}, 0x000000);
    } else {
        frame(*dc, bx, 0x000000);
        if (b.check == 1) {
            for (int i = 0; i < 6; ++i) {
                fill(*dc, Rect{2 + i, by + 5 + (i < 2 ? i : 2 - (i - 2)), 3 + i, by + 7 + (i < 2 ? i : 2 - (i - 2))}, 0x000000);
            }
        } else if (b.check == 2) {
            fill(*dc, Rect{3, by + 3, box - 3, by + box - 3}, 0x808080);
        }
    }
    text_at(*dc, box + 5, (r.h() - th) / 2, text, fg);
    if (wnd_focus() == h && !text.empty()) {
        int tw = text_width(b.font, text);
        frame(*dc, Rect{box + 3, (r.h() - th) / 2 - 1, box + 7 + tw, (r.h() + th) / 2 + 1}, 0x808080);
    }
}

void button_clicked(uint16_t h) {
    WndInfo wi;
    if (!wnd_info(h, wi)) return;
    Ctl& b = ctl(h);
    int kind = int(wi.style & 0x0F);
    if (kind == 3) b.check = b.check ? 0 : 1;                      // BS_AUTOCHECKBOX
    else if (kind == 6) b.check = (b.check + 1) % 3;               // BS_AUTO3STATE
    else if (kind == 9) {                                          // BS_AUTORADIOBUTTON
        for (uint16_t s : wnd_children(wi.parent)) {
            WndInfo si;
            if (s != h && wnd_info(s, si) && (si.style & 0x0F) == 9 && g_ctls.count(s) && ctl(s).check) {
                ctl(s).check = 0;
                wnd_invalidate(s);
            }
        }
        b.check = 1;
    }
    wnd_invalidate(h);
    notify(h, 0);   // BN_CLICKED
}

uint32_t button_proc(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp) {
    uint32_t r = 0;
    if (common(h, msg, wp, lp, r)) return r;
    Ctl& b = ctl(h);
    switch (msg) {
        case WM_PAINT: paint_button(h, wp); return 0;
        case BM_GETCHECK: return uint32_t(b.check);
        case BM_SETCHECK:
            if (b.check != wp) { b.check = wp; wnd_invalidate(h); }
            return 0;
        case BM_GETSTATE: return uint32_t(b.check | (b.pressed ? 4 : 0) | (wnd_focus() == h ? 8 : 0));
        case BM_SETSTATE:
            if (b.pressed != (wp != 0)) { b.pressed = wp != 0; wnd_invalidate(h); }
            return 0;
        case BM_SETSTYLE: wnd_invalidate(h); return 0;
        case WM_GETDLGCODE: return 0x2000;   // DLGC_BUTTON
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            b.tracking = true;
            b.pressed = true;
            wnd_set_capture(h);
            wnd_set_focus(h);
            wnd_invalidate(h);
            return 0;
        case WM_MOUSEMOVE:
            if (b.tracking) {
                WndInfo wi;
                wnd_info(h, wi);
                int x = int16_t(lp & 0xFFFF), y = int16_t(lp >> 16);
                bool in = x >= 0 && y >= 0 && x < wi.w && y < wi.h;
                if (in != b.pressed) { b.pressed = in; wnd_invalidate(h); }
            }
            return 0;
        case WM_LBUTTONUP:
            if (b.tracking) {
                b.tracking = false;
                if (wnd_capture() == h) wnd_set_capture(0);
                bool click = b.pressed;
                b.pressed = false;
                wnd_invalidate(h);
                if (click) button_clicked(h);
            }
            return 0;
        case WM_KEYUP:
            if (wp == 0x20) button_clicked(h);   // space
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            wnd_invalidate(h);
            return 0;
        default: return wnd_default_proc(h, msg, wp, lp);
    }
}

// ---- LISTBOX --------------------------------------------------------------

constexpr uint16_t LB_ADDSTRING = 0x401, LB_INSERTSTRING = 0x402, LB_DELETESTRING = 0x403, LB_RESETCONTENT = 0x405,
                   LB_SETSEL = 0x406, LB_SETCURSEL = 0x407, LB_GETSEL = 0x408, LB_GETCURSEL = 0x409,
                   LB_GETTEXT = 0x40A, LB_GETTEXTLEN = 0x40B, LB_GETCOUNT = 0x40C, LB_SELECTSTRING = 0x40D,
                   LB_DIR = 0x40E, LB_GETTOPINDEX = 0x40F, LB_FINDSTRING = 0x410, LB_GETSELCOUNT = 0x411,
                   LB_GETSELITEMS = 0x412, LB_SETTABSTOPS = 0x413, LB_GETHORIZONTALEXTENT = 0x414,
                   LB_SETHORIZONTALEXTENT = 0x415, LB_SETCOLUMNWIDTH = 0x416, LB_SETTOPINDEX = 0x418,
                   LB_GETITEMRECT = 0x419, LB_GETITEMDATA = 0x41A, LB_SETITEMDATA = 0x41B, LB_SELITEMRANGE = 0x41C,
                   LB_SETCARETINDEX = 0x41F, LB_GETCARETINDEX = 0x420, LB_SETITEMHEIGHT = 0x421,
                   LB_GETITEMHEIGHT = 0x422, LB_FINDSTRINGEXACT = 0x423;
constexpr uint32_t LB_ERR = 0xFFFFFFFFu;
constexpr uint32_t LBS_NOTIFY = 1, LBS_SORT = 2, LBS_MULTIPLESEL = 8, LBS_OWNERDRAWFIXED = 0x10,
                   LBS_OWNERDRAWVARIABLE = 0x20, LBS_HASSTRINGS = 0x40, LBS_EXTENDEDSEL = 0x800;

std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

int item_height(uint16_t h) {
    Ctl& l = ctl(h);
    return l.itemH > 0 ? l.itemH : line_height(h) + 1;
}

int visible_rows(uint16_t h, int height) { return std::max(1, height / item_height(h)); }

bool has_strings(uint32_t style) { return !(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (style & LBS_HASSTRINGS); }

// String argument of an add/insert/find message (or item data for owner-draw lists without strings).
Item item_from(uint32_t style, uint32_t lp) {
    Item it;
    if (has_strings(style)) it.text = lp ? recomp::read_cstr(Runtime::get().cpu(), lp, 1024) : std::string();
    it.data = has_strings(style) ? 0 : lp;
    return it;
}

int find_item(const std::vector<Item>& items, int start, const std::string& s, bool exact) {
    std::string u = upper(s);
    int n = int(items.size());
    for (int k = 1; k <= n; ++k) {
        int i = (start + k) % n;
        if (start < 0) i = k - 1;
        std::string t = upper(items[size_t(i)].text);
        if (exact ? t == u : t.compare(0, u.size(), u) == 0) return i;
    }
    return -1;
}

void ensure_visible(uint16_t h, Ctl& l, int height) {
    int rows = visible_rows(h, height);
    if (l.cur >= 0) {
        if (l.cur < l.top) l.top = l.cur;
        if (l.cur >= l.top + rows) l.top = l.cur - rows + 1;
    }
    l.top = std::max(0, std::min(l.top, std::max(0, int(l.items.size()) - rows)));
}

// Draw list items into rows of `area` (also used for the combo drop-down).
void paint_items(uint16_t h, DC& dc, uint16_t hdc, Ctl& l, Rect area, uint32_t style, uint16_t odType, uint16_t brush,
                 bool focused) {
    int ih = item_height(h);
    draw_fill_rect(dc, area, brush);
    bool owner = (style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) != 0;
    bool multi = (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0;
    for (int i = l.top, y = area.top; i < int(l.items.size()) && y < area.bottom; ++i, y += ih) {
        Rect ir{area.left, y, area.right, y + ih};
        bool sel = multi ? l.items[size_t(i)].selected : i == l.cur;
        if (owner) {
            uint16_t st = uint16_t((sel ? ODS_SELECTED : 0) | (focused && i == l.cur ? ODS_FOCUS : 0));
            draw_item(h, hdc, odType, i, ir, st, l.items[size_t(i)].data);
            continue;
        }
        if (sel) fill(dc, ir, 0x000080);
        text_at(dc, ir.left + 2, y, l.items[size_t(i)].text, sel ? 0xFFFFFF : dc.textColor);
    }
}

void paint_scrollbar(DC& dc, Rect r, int top, int rows, int count) {
    fill(dc, r, 0xC0C0C0);
    frame(dc, r, 0x808080);
    int aw = r.w();
    Rect up{r.left, r.top, r.right, r.top + aw}, dn{r.left, r.bottom - aw, r.right, r.bottom};
    for (Rect b : {up, dn}) {
        fill(dc, b, 0xC0C0C0);
        bevel(dc, b, false);
    }
    // arrows
    int cx = r.left + aw / 2;
    for (int i = 0; i < 4; ++i) {
        fill(dc, Rect{cx - i, up.top + 5 + i, cx + i + 1, up.top + 6 + i}, 0x000000);
        fill(dc, Rect{cx - i, dn.bottom - 6 - i, cx + i + 1, dn.bottom - 5 - i}, 0x000000);
    }
    int track = r.h() - 2 * aw;
    if (count > rows && track > 8) {
        int th = std::max(8, track * rows / count);
        int ty = r.top + aw + (track - th) * top / std::max(1, count - rows);
        Rect thumb{r.left, ty, r.right, ty + th};
        fill(dc, thumb, 0xC0C0C0);
        bevel(dc, thumb, false);
    }
}

bool needs_scroll(uint16_t h, const WndInfo& wi) {
    return (wi.style & WS_VSCROLL) && int(ctl(h).items.size()) > visible_rows(h, wi.h - 2);
}

void paint_listbox(uint16_t h, uint16_t wp) {
    WndInfo wi;
    if (!wnd_info(h, wi)) return;
    Ctl& l = ctl(h);
    PaintDC pdc(h, wp);
    DC* dc = pdc.get();
    if (!dc) return;
    uint16_t br = ctl_color(h, pdc.hdc, CTLCOLOR_LISTBOX);
    Rect r{0, 0, wi.w, wi.h};
    bool border = (wi.style & WS_BORDER) != 0;
    Rect in = border ? Rect{1, 1, r.right - 1, r.bottom - 1} : r;
    bool sb = needs_scroll(h, wi);
    if (sb) in.right -= kScrollW;
    paint_items(h, *dc, pdc.hdc, l, in, wi.style, ODT_LISTBOX, br, wnd_focus() == h);
    if (sb) paint_scrollbar(*dc, Rect{in.right, in.top, in.right + kScrollW, in.bottom}, l.top, visible_rows(h, in.h()),
                            int(l.items.size()));
    if (border) frame(*dc, r, 0x000000);
}

void lb_select(uint16_t h, int idx, bool notifyParent) {
    WndInfo wi;
    if (!wnd_info(h, wi)) return;
    Ctl& l = ctl(h);
    if (idx < -1 || idx >= int(l.items.size())) idx = -1;
    bool changed = idx != l.cur;
    l.cur = idx;
    l.caretIdx = std::max(idx, 0);
    if (wi.style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) {
        if (wi.style & LBS_EXTENDEDSEL)
            for (auto& it : l.items) it.selected = false;
        if (idx >= 0) l.items[size_t(idx)].selected = (wi.style & LBS_EXTENDEDSEL) ? true : !l.items[size_t(idx)].selected;
        changed = true;
    }
    ensure_visible(h, l, wi.h - 2);
    wnd_invalidate(h);
    if (changed && notifyParent && (wi.style & LBS_NOTIFY)) notify(h, 1);   // LBN_SELCHANGE
}

// List messages shared by LISTBOX and COMBOBOX (`base` maps the combo numbers).
uint32_t list_message(uint16_t h, uint32_t style, uint16_t msg, uint16_t wp, uint32_t lp, bool& handled) {
    Cpu& c = Runtime::get().cpu();
    Ctl& l = ctl(h);
    handled = true;
    switch (msg) {
        case LB_ADDSTRING: {
            Item it = item_from(style, lp);
            int pos = int(l.items.size());
            if (style & LBS_SORT) {
                std::string u = upper(it.text);
                pos = 0;
                while (pos < int(l.items.size()) && upper(l.items[size_t(pos)].text) <= u) ++pos;
            }
            l.items.insert(l.items.begin() + pos, it);
            if (l.cur >= pos) ++l.cur;
            wnd_invalidate(h);
            return uint32_t(pos);
        }
        case LB_INSERTSTRING: {
            int pos = int16_t(wp);
            if (pos < 0 || pos > int(l.items.size())) pos = int(l.items.size());
            l.items.insert(l.items.begin() + pos, item_from(style, lp));
            if (l.cur >= pos) ++l.cur;
            wnd_invalidate(h);
            return uint32_t(pos);
        }
        case LB_DELETESTRING: {
            int pos = int16_t(wp);
            if (pos < 0 || pos >= int(l.items.size())) return LB_ERR;
            l.items.erase(l.items.begin() + pos);
            if (l.cur == pos) l.cur = -1; else if (l.cur > pos) --l.cur;
            l.top = std::min(l.top, std::max(0, int(l.items.size()) - 1));
            wnd_invalidate(h);
            return uint32_t(l.items.size());
        }
        case LB_RESETCONTENT:
            l.items.clear();
            l.cur = -1;
            l.top = 0;
            wnd_invalidate(h);
            return 0;
        case LB_GETCOUNT: return uint32_t(l.items.size());
        case LB_GETCURSEL: return l.cur < 0 ? LB_ERR : uint32_t(l.cur);
        case LB_SETCURSEL:
            lb_select(h, int16_t(wp), false);
            return int16_t(wp) < 0 ? LB_ERR : uint32_t(l.cur);
        case LB_GETTEXT: {
            int i = int16_t(wp);
            if (i < 0 || i >= int(l.items.size())) return LB_ERR;
            if (!has_strings(style)) {
                c.wr32(uint16_t(lp >> 16), uint16_t(lp), l.items[size_t(i)].data);
                return 4;
            }
            const std::string& t = l.items[size_t(i)].text;
            recomp::write_cstr(c, lp, t, t.size() + 1);
            return uint32_t(t.size());
        }
        case LB_GETTEXTLEN: {
            int i = int16_t(wp);
            if (i < 0 || i >= int(l.items.size())) return LB_ERR;
            return has_strings(style) ? uint32_t(l.items[size_t(i)].text.size()) : 4;
        }
        case LB_GETITEMDATA: {
            int i = int16_t(wp);
            if (i < 0 || i >= int(l.items.size())) return LB_ERR;
            return l.items[size_t(i)].data;
        }
        case LB_SETITEMDATA: {
            int i = int16_t(wp);
            if (i < 0 || i >= int(l.items.size())) return LB_ERR;
            l.items[size_t(i)].data = lp;
            return 0;
        }
        case LB_FINDSTRING:
        case LB_FINDSTRINGEXACT: {
            if (l.items.empty()) return LB_ERR;
            int r = find_item(l.items, int16_t(wp), lp ? recomp::read_cstr(c, lp, 1024) : "", msg == LB_FINDSTRINGEXACT);
            return r < 0 ? LB_ERR : uint32_t(r);
        }
        case LB_SELECTSTRING: {
            if (l.items.empty()) return LB_ERR;
            int r = find_item(l.items, int16_t(wp), lp ? recomp::read_cstr(c, lp, 1024) : "", false);
            if (r >= 0) lb_select(h, r, false);
            return r < 0 ? LB_ERR : uint32_t(r);
        }
        case LB_GETTOPINDEX: return uint32_t(l.top);
        case LB_SETTOPINDEX:
            l.top = std::max(0, std::min(int(int16_t(wp)), int(l.items.size()) - 1));
            wnd_invalidate(h);
            return 0;
        case LB_SETITEMHEIGHT:
            if (lp & 0xFFFF) l.itemH = int(lp & 0xFFFF);
            wnd_invalidate(h);
            return 0;
        case LB_GETITEMHEIGHT: return uint32_t(item_height(h));
        case LB_GETITEMRECT: {
            int i = int16_t(wp);
            WndInfo wi;
            wnd_info(h, wi);
            int ih = item_height(h);
            int y = (i - l.top) * ih + ((wi.style & WS_BORDER) ? 1 : 0);
            uint16_t s = uint16_t(lp >> 16), o = uint16_t(lp);
            c.wr16(s, o, 0);
            c.wr16(s, uint16_t(o + 2), uint16_t(y));
            c.wr16(s, uint16_t(o + 4), uint16_t(wi.w));
            c.wr16(s, uint16_t(o + 6), uint16_t(y + ih));
            return 0;
        }
        case LB_SETSEL: {   // multi-select: wParam = select, lParam = index (-1: all)
            int i = int16_t(lp & 0xFFFF);
            for (int k = 0; k < int(l.items.size()); ++k)
                if (i < 0 || k == i) l.items[size_t(k)].selected = wp != 0;
            wnd_invalidate(h);
            return 0;
        }
        case LB_GETSEL: {
            int i = int16_t(wp);
            if (i < 0 || i >= int(l.items.size())) return LB_ERR;
            return (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) ? (l.items[size_t(i)].selected ? 1 : 0) : (i == l.cur ? 1 : 0);
        }
        case LB_GETSELCOUNT: {
            int n = 0;
            for (auto& it : l.items) n += it.selected ? 1 : 0;
            return uint32_t(n);
        }
        case LB_GETSELITEMS: {
            int n = 0;
            for (int k = 0; k < int(l.items.size()) && n < wp; ++k)
                if (l.items[size_t(k)].selected) c.wr16(uint16_t(lp >> 16), uint16_t(uint16_t(lp) + 2 * n++), uint16_t(k));
            return uint32_t(n);
        }
        case LB_SELITEMRANGE: {
            int a = int(lp & 0xFFFF), b = int(lp >> 16);
            for (int k = a; k <= b && k < int(l.items.size()); ++k) if (k >= 0) l.items[size_t(k)].selected = wp != 0;
            wnd_invalidate(h);
            return 0;
        }
        case LB_SETCARETINDEX: l.caretIdx = int16_t(wp); return 0;
        case LB_GETCARETINDEX: return uint32_t(l.caretIdx);
        case LB_SETTABSTOPS: case LB_SETHORIZONTALEXTENT: case LB_SETCOLUMNWIDTH: return 1;
        case LB_GETHORIZONTALEXTENT: return 0;
        case LB_DIR: {
            // wParam: DDL_* attributes, lParam: file spec (relative to the current directory)
            std::string spec = lp ? recomp::read_cstr(c, lp, 260) : "*.*";
            int last = -1;
            bool exclusive = (wp & 0x8000) != 0;
            auto add = [&](const std::string& s) {
                Item it;
                it.text = s;
                int pos = int(l.items.size());
                if (style & LBS_SORT) {
                    pos = 0;
                    while (pos < int(l.items.size()) && upper(l.items[size_t(pos)].text) <= upper(s)) ++pos;
                }
                l.items.insert(l.items.begin() + pos, it);
                last = pos;
            };
            if (!exclusive || (wp & 0x0037) != 0x0010) {
                for (const auto& e : vfs::find(spec, 0))
                    if (!e.dir) add(e.name);
            }
            if (wp & 0x0010) {   // DDL_DIRECTORY
                std::string dirSpec = spec;
                size_t slash = dirSpec.find_last_of("\\/:");
                dirSpec = (slash == std::string::npos ? std::string() : dirSpec.substr(0, slash + 1)) + "*.*";
                for (const auto& e : vfs::find(dirSpec, 0x10))
                    if (e.dir && e.name != ".") add("[" + e.name + "]");
            }
            if (wp & 0x4000) add("[-c-]");   // DDL_DRIVES
            wnd_invalidate(h);
            return last < 0 ? LB_ERR : uint32_t(last);
        }
        default: handled = false; return 0;
    }
}

int row_at(uint16_t h, int y, bool border) {
    return ctl(h).top + (y - (border ? 1 : 0)) / item_height(h);
}

uint32_t listbox_proc(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp) {
    static const bool trace = std::getenv("MYULK_TRACE_CTL") != nullptr;
    if (trace && msg >= 0x400) recomp::log("listbox %04X msg %04X wp %04X lp %08X", h, msg, wp, lp);
    uint32_t r = 0;
    if (common(h, msg, wp, lp, r)) return r;
    WndInfo wi;
    wnd_info(h, wi);
    Ctl& l = ctl(h);
    if (msg == WM_CREATE || msg == WM_NCCREATE) {
        if (wi.style & LBS_OWNERDRAWFIXED) l.itemH = measure_item(h, ODT_LISTBOX, line_height(h) + 1);
        return wnd_default_proc(h, msg, wp, lp);
    }
    bool handled = false;
    r = list_message(h, wi.style, msg, wp, lp, handled);
    if (handled) return r;
    bool border = (wi.style & WS_BORDER) != 0;
    int inner = wi.h - (border ? 2 : 0);
    switch (msg) {
        case WM_PAINT: paint_listbox(h, wp); return 0;
        case WM_GETDLGCODE: return 0x0001 | 0x0004;   // DLGC_WANTARROWS | DLGC_WANTCHARS
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            wnd_set_focus(h);
            int x = int16_t(lp & 0xFFFF), y = int16_t(lp >> 16);
            if (needs_scroll(h, wi) && x >= wi.w - kScrollW - (border ? 1 : 0)) {
                int rows = visible_rows(h, inner);
                int top = border ? 1 : 0;
                if (y < top + kScrollW) l.top -= 1;
                else if (y >= wi.h - top - kScrollW) l.top += 1;
                else if (y < top + inner / 2) l.top -= rows;
                else l.top += rows;
                l.top = std::max(0, std::min(l.top, int(l.items.size()) - rows));
                wnd_invalidate(h);
                return 0;
            }
            int row = row_at(h, y, border);
            if (row >= 0 && row < int(l.items.size())) {
                lb_select(h, row, true);
                if (msg == WM_LBUTTONDBLCLK && (wi.style & LBS_NOTIFY)) notify(h, 2);   // LBN_DBLCLK
            }
            return 0;
        }
        case WM_KEYDOWN: {
            int rows = visible_rows(h, inner);
            int n = int(l.items.size());
            int c = l.cur;
            switch (wp) {
                case 0x26: c = std::max(0, c - 1); break;           // up
                case 0x28: c = std::min(n - 1, c + 1); break;       // down
                case 0x21: c = std::max(0, c - rows); break;        // page up
                case 0x22: c = std::min(n - 1, c + rows); break;    // page down
                case 0x24: c = 0; break;                            // home
                case 0x23: c = n - 1; break;                        // end
                case 0x0D: if (wi.style & LBS_NOTIFY) notify(h, 2); return 0;
                default: return 0;
            }
            if (n > 0 && c != l.cur) lb_select(h, c, true);
            return 0;
        }
        case WM_CHAR: {
            if (wp < 0x20 || l.items.empty()) return 0;
            int i = find_item(l.items, l.cur, std::string(1, char(wp)), false);
            if (i >= 0) lb_select(h, i, true);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            wnd_invalidate(h);
            if (wi.style & LBS_NOTIFY) notify(h, msg == WM_SETFOCUS ? 4 : 5);   // LBN_SETFOCUS / KILLFOCUS
            return 0;
        case WM_SIZE: wnd_invalidate(h); return 0;
        default: return wnd_default_proc(h, msg, wp, lp);
    }
}

// ---- COMBOBOX (drop-down list shown inside the control's parent) ------------

constexpr uint16_t CB_GETEDITSEL = 0x400, CB_LIMITTEXT = 0x401, CB_SETEDITSEL = 0x402, CB_ADDSTRING = 0x403,
                   CB_DELETESTRING = 0x404, CB_DIR = 0x405, CB_GETCOUNT = 0x406, CB_GETCURSEL = 0x407,
                   CB_GETLBTEXT = 0x408, CB_GETLBTEXTLEN = 0x409, CB_INSERTSTRING = 0x40A,
                   CB_RESETCONTENT = 0x40B, CB_FINDSTRING = 0x40C, CB_SELECTSTRING = 0x40D, CB_SETCURSEL = 0x40E,
                   CB_SHOWDROPDOWN = 0x40F, CB_GETITEMDATA = 0x410, CB_SETITEMDATA = 0x411,
                   CB_GETDROPPEDCONTROLRECT = 0x412, CB_SETITEMHEIGHT = 0x413, CB_GETITEMHEIGHT = 0x414,
                   CB_SETEXTENDEDUI = 0x415, CB_GETEXTENDEDUI = 0x416, CB_GETDROPPEDSTATE = 0x417,
                   CB_FINDSTRINGEXACT = 0x418;

uint16_t cb_to_lb(uint16_t msg) {
    switch (msg) {
        case CB_ADDSTRING: return LB_ADDSTRING;
        case CB_DELETESTRING: return LB_DELETESTRING;
        case CB_GETCOUNT: return LB_GETCOUNT;
        case CB_GETCURSEL: return LB_GETCURSEL;
        case CB_GETLBTEXT: return LB_GETTEXT;
        case CB_GETLBTEXTLEN: return LB_GETTEXTLEN;
        case CB_INSERTSTRING: return LB_INSERTSTRING;
        case CB_RESETCONTENT: return LB_RESETCONTENT;
        case CB_FINDSTRING: return LB_FINDSTRING;
        case CB_FINDSTRINGEXACT: return LB_FINDSTRINGEXACT;
        case CB_SELECTSTRING: return LB_SELECTSTRING;
        case CB_SETCURSEL: return LB_SETCURSEL;
        case CB_GETITEMDATA: return LB_GETITEMDATA;
        case CB_SETITEMDATA: return LB_SETITEMDATA;
        default: return 0;
    }
}

// combo style bits mapped to list box ones for the shared code
uint32_t cb_list_style(uint32_t style) {
    uint32_t s = LBS_NOTIFY;
    if (style & 0x0100) s |= LBS_SORT;            // CBS_SORT
    if (style & 0x0010) s |= LBS_OWNERDRAWFIXED;  // CBS_OWNERDRAWFIXED
    if (style & 0x0020) s |= LBS_OWNERDRAWVARIABLE;
    if (style & 0x0200) s |= LBS_HASSTRINGS;      // CBS_HASSTRINGS
    return s;
}

constexpr int kDropRows = 8;

int cb_field_h(uint16_t h, const WndInfo& wi) {
    Ctl& l = ctl(h);
    return l.dropped ? std::min(wi.h, item_height(h) + 6) : wi.h;
}

void paint_combo(uint16_t h, uint16_t wp) {
    WndInfo wi;
    if (!wnd_info(h, wi)) return;
    Ctl& l = ctl(h);
    PaintDC pdc(h, wp);
    DC* dc = pdc.get();
    if (!dc) return;
    uint16_t br = ctl_color(h, pdc.hdc, CTLCOLOR_LISTBOX);
    int fh = l.dropped ? item_height(h) + 6 : wi.h;
    Rect field{0, 0, wi.w, fh};
    draw_fill_rect(*dc, field, br);
    frame(*dc, field, 0x000000);
    Rect btn{wi.w - kScrollW - 1, 1, wi.w - 1, fh - 1};
    fill(*dc, btn, 0xC0C0C0);
    bevel(*dc, btn, l.dropped);
    int cx = btn.left + kScrollW / 2, cy = fh / 2 - 1;
    for (int i = 0; i < 4; ++i) fill(*dc, Rect{cx - 3 + i, cy + i, cx + 4 - i, cy + i + 1}, 0x000000);
    Rect text{2, 2, btn.left - 1, fh - 2};
    uint32_t ls = cb_list_style(wi.style);
    if (l.cur >= 0 && l.cur < int(l.items.size())) {
        if (ls & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) {
            draw_item(h, pdc.hdc, ODT_COMBOBOX, l.cur, text, uint16_t(wnd_focus() == h ? ODS_SELECTED | ODS_FOCUS : 0),
                      l.items[size_t(l.cur)].data);
        } else {
            bool f = wnd_focus() == h;
            if (f) fill(*dc, text, 0x000080);
            text_at(*dc, text.left + 1, text.top + (text.h() - line_height(h)) / 2, l.items[size_t(l.cur)].text,
                    f ? 0xFFFFFF : dc->textColor);
        }
    }
    if (l.dropped) {
        Rect list{0, fh, wi.w, wi.h};
        paint_items(h, *dc, pdc.hdc, l, Rect{1, fh, wi.w - 1, wi.h - 1}, ls, ODT_COMBOBOX, br, true);
        frame(*dc, list, 0x000000);
    }
}

void cb_set_dropped(uint16_t h, bool drop) {
    // The drop-down grows the control itself (no separate popup window).
    WndInfo wi;
    if (!wnd_info(h, wi)) return;
    Ctl& l = ctl(h);
    if (l.dropped == drop) return;
    int ih = item_height(h);
    (void)ih;
    if (drop) {
        notify(h, 7);   // CBN_DROPDOWN
        l.dropped = true;
        int rows = std::max(1, std::min(kDropRows, int(l.items.size())));
        l.top = std::max(0, std::min(l.cur, int(l.items.size()) - rows));
    } else {
        l.dropped = false;
        if (wnd_capture() == h) wnd_set_capture(0);
        notify(h, 8);   // CBN_CLOSEUP
    }
    wnd_invalidate(h);
}

uint32_t combo_proc(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp) {
    uint32_t r = 0;
    if (common(h, msg, wp, lp, r)) return r;
    WndInfo wi;
    wnd_info(h, wi);
    Ctl& l = ctl(h);
    if (msg == WM_CREATE) {
        if (wi.style & 0x0010) l.itemH = measure_item(h, ODT_COMBOBOX, line_height(h) + 1);
        return wnd_default_proc(h, msg, wp, lp);
    }
    if (uint16_t lbm = cb_to_lb(msg)) {
        bool handled = false;
        int before = l.cur;
        r = list_message(h, cb_list_style(wi.style), lbm, wp, lp, handled);
        if (msg == CB_SETCURSEL || msg == CB_SELECTSTRING) {
            if (l.cur != before) wnd_set_text(h, l.cur >= 0 ? l.items[size_t(l.cur)].text : std::string());
        }
        return r;
    }
    switch (msg) {
        case WM_PAINT: paint_combo(h, wp); return 0;
        case CB_GETDROPPEDSTATE: return l.dropped ? 1 : 0;
        case CB_SHOWDROPDOWN: cb_set_dropped(h, wp != 0); return 1;
        case CB_SETITEMHEIGHT:
            if (int16_t(wp) >= 0) l.itemH = int(lp & 0xFFFF);
            wnd_invalidate(h);
            return 0;
        case CB_GETITEMHEIGHT: return uint32_t(item_height(h));
        case CB_LIMITTEXT: case CB_SETEXTENDEDUI: return 1;
        case CB_GETEXTENDEDUI: return 0;
        case CB_GETEDITSEL: return 0;
        case CB_SETEDITSEL: return 1;
        case CB_GETDROPPEDCONTROLRECT: {
            Cpu& c = Runtime::get().cpu();
            int x = 0, y = 0;
            screen_to_client(h, &x, &y);
            uint16_t s = uint16_t(lp >> 16), o = uint16_t(lp);
            c.wr16(s, o, uint16_t(-x));
            c.wr16(s, uint16_t(o + 2), uint16_t(-y));
            c.wr16(s, uint16_t(o + 4), uint16_t(-x + wi.w));
            c.wr16(s, uint16_t(o + 6), uint16_t(-y + wi.h));
            return 1;
        }
        case CB_DIR: {
            bool handled = false;
            return list_message(h, cb_list_style(wi.style), LB_DIR, wp, lp, handled);
        }
        case WM_GETTEXT: {
            if (!wp) return 0;
            std::string t = l.cur >= 0 && l.cur < int(l.items.size()) ? l.items[size_t(l.cur)].text : wnd_text(h);
            recomp::write_cstr(Runtime::get().cpu(), lp, t, wp);
            return uint32_t(std::min<size_t>(t.size(), wp - 1));
        }
        case WM_GETTEXTLENGTH:
            return uint32_t((l.cur >= 0 && l.cur < int(l.items.size()) ? l.items[size_t(l.cur)].text : wnd_text(h)).size());
        case WM_GETDLGCODE: return 0x0001 | 0x0004;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            wnd_set_focus(h);
            if (l.items.empty()) return 0;
            // Without a popup window: cycle to the next item (the drop-down
            // list is rarely more than one or two drives here).
            int next = (l.cur + 1) % int(l.items.size());
            lb_select(h, next, false);
            wnd_set_text(h, l.items[size_t(next)].text);
            notify(h, 1);   // CBN_SELCHANGE
            notify(h, 9);   // CBN_SELENDOK
            return 0;
        }
        case WM_KEYDOWN: {
            int n = int(l.items.size());
            int c = l.cur;
            if (wp == 0x26) c = std::max(0, c - 1);
            else if (wp == 0x28) c = std::min(n - 1, c + 1);
            else return 0;
            if (n > 0 && c != l.cur) {
                lb_select(h, c, false);
                wnd_set_text(h, l.items[size_t(c)].text);
                notify(h, 1);
            }
            return 0;
        }
        case WM_SETFOCUS: wnd_invalidate(h); notify(h, 3); return 0;
        case WM_KILLFOCUS: wnd_invalidate(h); notify(h, 4); return 0;
        default: return wnd_default_proc(h, msg, wp, lp);
    }
}

// ---- EDIT -----------------------------------------------------------------

constexpr uint16_t EM_GETSEL = 0x400, EM_SETSEL = 0x401, EM_GETRECT = 0x402, EM_SCROLL = 0x405,
                   EM_LINESCROLL = 0x406, EM_GETMODIFY = 0x408, EM_SETMODIFY = 0x409,
                   EM_GETLINECOUNT = 0x40A, EM_LINEINDEX = 0x40B, EM_LINELENGTH = 0x411, EM_REPLACESEL = 0x412,
                   EM_GETLINE = 0x414, EM_LIMITTEXT = 0x415, EM_CANUNDO = 0x416, EM_UNDO = 0x417,
                   EM_LINEFROMCHAR = 0x419, EM_SETPASSWORDCHAR = 0x41C, EM_EMPTYUNDOBUFFER = 0x41D,
                   EM_GETFIRSTVISIBLELINE = 0x41E, EM_SETREADONLY = 0x41F;

void paint_edit(uint16_t h, uint16_t wp) {
    WndInfo wi;
    if (!wnd_info(h, wi)) return;
    Ctl& e = ctl(h);
    PaintDC pdc(h, wp);
    DC* dc = pdc.get();
    if (!dc) return;
    uint16_t br = ctl_color(h, pdc.hdc, CTLCOLOR_EDIT);
    Rect r{0, 0, wi.w, wi.h};
    draw_fill_rect(*dc, r, br);
    bool border = (wi.style & WS_BORDER) != 0;
    if (border) frame(*dc, r, 0x000000);
    std::string t = wnd_text(h);
    if (e.password) t.assign(t.size(), e.password);
    int th = line_height(h);
    int x0 = border ? 3 : 1;
    int y0 = (wi.h - th) / 2;
    size_t a = std::min(e.selStart, e.selEnd), b = std::max(e.selStart, e.selEnd);
    a = std::min(a, t.size());
    b = std::min(b, t.size());
    bool focused = wnd_focus() == h;
    if (focused && a < b) {
        int xa = x0 + text_width(e.font, t.substr(0, a)), xb = x0 + text_width(e.font, t.substr(0, b));
        fill(*dc, Rect{xa, y0, xb, y0 + th}, 0x000080);
        text_at(*dc, x0, y0, t.substr(0, a), wi.enabled ? dc->textColor : 0x808080);
        text_at(*dc, xa, y0, t.substr(a, b - a), 0xFFFFFF);
        text_at(*dc, xb, y0, t.substr(b), wi.enabled ? dc->textColor : 0x808080);
    } else {
        text_at(*dc, x0, y0, t, wi.enabled ? dc->textColor : 0x808080);
    }
    if (focused && a == b) {
        int xc = x0 + text_width(e.font, t.substr(0, e.selEnd));
        fill(*dc, Rect{xc, y0, xc + 1, y0 + th}, 0x000000);
    }
}

void edit_replace(uint16_t h, const std::string& s) {
    Ctl& e = ctl(h);
    std::string t = wnd_text(h);
    size_t a = std::min(std::min(e.selStart, e.selEnd), t.size()), b = std::min(std::max(e.selStart, e.selEnd), t.size());
    std::string ins = s;
    if (e.limit > 0 && t.size() - (b - a) + ins.size() > size_t(e.limit))
        ins.resize(size_t(std::max(0, e.limit - int(t.size() - (b - a)))));
    t = t.substr(0, a) + ins + t.substr(b);
    wnd_set_text(h, t);
    e.selStart = e.selEnd = a + ins.size();
    e.modified = true;
    wnd_invalidate(h);
    notify(h, 0x400);   // EN_UPDATE
    notify(h, 0x300);   // EN_CHANGE
}

uint32_t edit_proc(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp) {
    uint32_t r = 0;
    if (common(h, msg, wp, lp, r)) return r;
    Ctl& e = ctl(h);
    Cpu& c = Runtime::get().cpu();
    switch (msg) {
        case WM_PAINT: paint_edit(h, wp); return 0;
        case WM_SETTEXT: {
            wnd_default_proc(h, msg, wp, lp);
            e.selStart = e.selEnd = 0;
            e.modified = false;
            wnd_invalidate(h);
            notify(h, 0x300);
            return 1;
        }
        case WM_GETDLGCODE: return 0x0001 | 0x0002 | 0x0004 | 0x0080;   // arrows, tab?, chars, hasSetSel
        case EM_GETSEL: return mklong(int(std::min(e.selStart, e.selEnd)), int(std::max(e.selStart, e.selEnd)));
        case EM_SETSEL: {
            size_t n = wnd_text(h).size();
            int a = int16_t(lp & 0xFFFF), b = int16_t(lp >> 16);
            e.selStart = a < 0 ? n : std::min(size_t(a), n);
            e.selEnd = b < 0 ? n : std::min(size_t(b), n);
            wnd_invalidate(h);
            return 1;
        }
        case EM_REPLACESEL: edit_replace(h, lp ? recomp::read_cstr(c, lp, 4096) : ""); return 0;
        case EM_GETMODIFY: return e.modified ? 1 : 0;
        case EM_SETMODIFY: e.modified = wp != 0; return 0;
        case EM_LIMITTEXT: e.limit = wp; return 0;
        case EM_SETREADONLY: e.readOnly = wp != 0; return 1;
        case EM_SETPASSWORDCHAR: e.password = char(wp); wnd_invalidate(h); return 0;
        case EM_GETLINECOUNT: return 1;
        case EM_LINEINDEX: return 0;
        case EM_LINELENGTH: return uint32_t(wnd_text(h).size());
        case EM_LINEFROMCHAR: case EM_GETFIRSTVISIBLELINE: return 0;
        case EM_GETLINE: {
            std::string t = wnd_text(h);
            uint16_t max = c.rd16(uint16_t(lp >> 16), uint16_t(lp));
            size_t n = std::min<size_t>(t.size(), max);
            for (size_t i = 0; i < n; ++i) c.wr8(uint16_t(lp >> 16), uint16_t(uint16_t(lp) + i), uint8_t(t[i]));
            return uint32_t(n);
        }
        case EM_GETRECT: {
            WndInfo wi;
            wnd_info(h, wi);
            uint16_t s = uint16_t(lp >> 16), o = uint16_t(lp);
            c.wr16(s, o, 2); c.wr16(s, uint16_t(o + 2), 2);
            c.wr16(s, uint16_t(o + 4), uint16_t(wi.w - 2)); c.wr16(s, uint16_t(o + 6), uint16_t(wi.h - 2));
            return 0;
        }
        case EM_CANUNDO: case EM_UNDO: return 0;
        case EM_EMPTYUNDOBUFFER: case EM_SCROLL: case EM_LINESCROLL: return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            wnd_set_focus(h);
            std::string t = wnd_text(h);
            WndInfo wi;
            wnd_info(h, wi);
            int x = int16_t(lp & 0xFFFF) - ((wi.style & WS_BORDER) ? 3 : 1);
            size_t pos = 0;
            while (pos < t.size() && text_width(e.font, t.substr(0, pos + 1)) <= x) ++pos;
            if (msg == WM_LBUTTONDBLCLK) { e.selStart = 0; e.selEnd = t.size(); }
            else e.selStart = e.selEnd = pos;
            wnd_invalidate(h);
            return 0;
        }
        case WM_CHAR: {
            if (e.readOnly) return 0;
            if (wp == 8) {   // backspace
                if (e.selStart == e.selEnd && e.selEnd > 0) e.selStart = e.selEnd - 1;
                edit_replace(h, "");
            } else if (wp >= 0x20 && wp != 0x7F) {
                edit_replace(h, std::string(1, char(wp)));
            }
            return 0;
        }
        case WM_KEYDOWN: {
            std::string t = wnd_text(h);
            size_t n = t.size();
            switch (wp) {
                case 0x25: if (e.selEnd > 0) --e.selEnd; e.selStart = e.selEnd; break;   // left
                case 0x27: if (e.selEnd < n) ++e.selEnd; e.selStart = e.selEnd; break;   // right
                case 0x24: e.selStart = e.selEnd = 0; break;                              // home
                case 0x23: e.selStart = e.selEnd = n; break;                              // end
                case 0x2E:                                                                // delete
                    if (e.readOnly) return 0;
                    if (e.selStart == e.selEnd && e.selEnd < n) e.selEnd = e.selStart + 1;
                    edit_replace(h, "");
                    return 0;
                default: return 0;
            }
            wnd_invalidate(h);
            return 0;
        }
        case WM_SETFOCUS:
            e.selStart = 0;
            e.selEnd = wnd_text(h).size();
            wnd_invalidate(h);
            notify(h, 0x100);   // EN_SETFOCUS
            return 0;
        case WM_KILLFOCUS:
            wnd_invalidate(h);
            notify(h, 0x200);   // EN_KILLFOCUS
            return 0;
        default: return wnd_default_proc(h, msg, wp, lp);
    }
}

// ---- STATIC ---------------------------------------------------------------

uint32_t static_proc(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp) {
    uint32_t r = 0;
    if (common(h, msg, wp, lp, r)) return r;
    if (msg == WM_SETTEXT) {
        wnd_default_proc(h, msg, wp, lp);
        wnd_invalidate(h);
        return 1;
    }
    if (msg != WM_PAINT) return wnd_default_proc(h, msg, wp, lp);
    WndInfo wi;
    if (!wnd_info(h, wi)) return 0;
    PaintDC pdc(h, wp);
    DC* dc = pdc.get();
    if (!dc) return 0;
    uint16_t br = ctl_color(h, pdc.hdc, CTLCOLOR_STATIC);
    draw_fill_rect(*dc, Rect{0, 0, wi.w, wi.h}, br);
    std::string t = wnd_text(h);
    t.erase(std::remove(t.begin(), t.end(), '&'), t.end());
    int kind = int(wi.style & 0x0F);
    int tw = text_width(ctl(h).font, t);
    int x = kind == 1 ? (wi.w - tw) / 2 : kind == 2 ? wi.w - tw : 0;   // SS_CENTER / SS_RIGHT
    text_at(*dc, x, 0, t, dc->textColor);
    return 0;
}

// ---- registration ---------------------------------------------------------

template <uint32_t (*Proc)(uint16_t, uint16_t, uint16_t, uint32_t)>
void thunk(Cpu& c) {
    Args a(c, 10);
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); uint16_t hwnd = a.w();
    a.ret32(Proc(hwnd, msg, wp, lp));
}

}  // namespace

void controls_init() {
    g_mem = Runtime::get().alloc_segment(0x400, "controls");
    auto& rt = Runtime::get();
    rt.register_api("HOST", 2, &thunk<button_proc>, "ButtonWndProc");
    rt.register_api("HOST", 3, &thunk<listbox_proc>, "ListBoxWndProc");
    rt.register_api("HOST", 4, &thunk<edit_proc>, "EditWndProc");
    rt.register_api("HOST", 5, &thunk<combo_proc>, "ComboBoxWndProc");
    rt.register_api("HOST", 6, &thunk<static_proc>, "StaticWndProc");
    g_procs["BUTTON"] = rt.import_far("HOST", 2);
    g_procs["LISTBOX"] = rt.import_far("HOST", 3);
    g_procs["EDIT"] = rt.import_far("HOST", 4);
    g_procs["COMBOBOX"] = rt.import_far("HOST", 5);
    g_procs["STATIC"] = rt.import_far("HOST", 6);
}

uint32_t control_proc(const std::string& cls) {
    auto it = g_procs.find(cls);
    return it == g_procs.end() ? 0 : it->second;
}

}  // namespace win16
