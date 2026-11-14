// USER: window classes and windows, messages, painting, timers, input,
// cursors/icons/bitmaps/strings from resources, and the USER exports.
#include "recomp/win16/user.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <vector>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/modules.hpp"
#include "recomp/win16/win16.hpp"

using recomp::Args;
using recomp::Cpu;
using recomp::Runtime;

namespace win16 {
namespace {

// ---- data ------------------------------------------------------------------

struct WndClass {
    std::string name;   // upper case
    uint16_t style = 0;
    uint32_t proc = 0;
    int clsExtra = 0, wndExtra = 0;
    uint16_t hInst = 0, hIcon = 0, hCursor = 0, hbrBackground = 0;
    uint32_t menuName = 0;
};

struct Window {
    uint16_t hwnd = 0;
    std::string cls;
    uint32_t proc = 0;
    uint32_t style = 0, exStyle = 0;
    uint16_t parent = 0, owner = 0, id = 0, hInst = 0;
    Rect rect;                         // in parent client coordinates (screen for top level)
    std::string text;
    std::vector<uint8_t> extra;
    std::map<std::string, uint16_t> props;
    std::vector<uint16_t> children;    // front (top of z-order) first
    Rect update;                       // client coordinates
    bool erase = false;
    bool visible = false, enabled = true;
    bool destroyed = false;
};

struct Timer {
    uint16_t hwnd, id;
    uint32_t proc;
    uint32_t interval;
    uint64_t due;
    bool pending = false;
};

struct Msg {
    uint16_t hwnd, msg, wParam;
    uint32_t lParam;
};

std::map<std::string, WndClass> g_classes;
std::map<uint16_t, Window> g_wnds;
std::vector<uint16_t> g_top;           // top-level windows, front first
uint16_t g_nextHwnd = 0x2000;
std::deque<Msg> g_queue;
std::vector<Timer> g_timers;
uint16_t g_nextTimerId = 0x100;
bool g_quit = false;
int g_quitCode = 0;
uint16_t g_capture = 0, g_focus = 0, g_active = 0;
int g_mouseX = 0, g_mouseY = 0;
bool g_buttons[3] = {false, false, false};
bool g_keys[256] = {};
uint16_t g_hoverWnd = 0;

std::map<uint16_t, std::unique_ptr<CursorImage>> g_cursors;   // HCURSOR / HICON
uint16_t g_nextCursor = 0x7000;
uint16_t g_cursor = 0;
uint16_t g_arrow = 0;

std::map<std::string, uint16_t> g_atomsByName;
std::map<uint16_t, std::string> g_atomsById;
uint16_t g_nextAtom = 0xC000;

uint16_t g_scratch = 0;     // selector for structures passed to callbacks
uint16_t g_scratchTop = 0;

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

Window* wnd(uint16_t h) {
    auto it = g_wnds.find(h);
    return (it == g_wnds.end() || it->second.destroyed) ? nullptr : &it->second;
}

uint16_t scratch_alloc(uint16_t n) {
    uint16_t o = g_scratchTop;
    g_scratchTop = uint16_t(g_scratchTop + ((n + 15) & ~15));
    return o;
}

// ---- geometry --------------------------------------------------------------

void screen_origin(uint16_t h, int* x, int* y) {
    *x = *y = 0;
    for (Window* w = wnd(h); w; w = w->parent ? wnd(w->parent) : nullptr) {
        *x += w->rect.left;
        *y += w->rect.top;
    }
}

bool really_visible(uint16_t h) {
    for (Window* w = wnd(h); w; w = w->parent ? wnd(w->parent) : nullptr)
        if (!w->visible) return false;
    return true;
}

// visible part of a window's client area, in screen coordinates
Rect visible_rect(uint16_t h) {
    Window* w = wnd(h);
    if (!w || !really_visible(h)) return Rect{};
    int ox, oy;
    screen_origin(h, &ox, &oy);
    Rect r{ox, oy, ox + w->rect.w(), oy + w->rect.h()};
    for (Window* p = w->parent ? wnd(w->parent) : nullptr; p; p = p->parent ? wnd(p->parent) : nullptr) {
        int px, py;
        screen_origin(p->hwnd, &px, &py);
        r = r.intersect(Rect{px, py, px + p->rect.w(), py + p->rect.h()});
    }
    return r.intersect(Rect{0, 0, 640, 480});
}

std::vector<uint16_t>& siblings(uint16_t h) {
    Window* w = wnd(h);
    if (w && w->parent) {
        if (Window* p = wnd(w->parent)) return p->children;
    }
    return g_top;
}

// invalidate a window area (client coords) and every visible child under it
void invalidate(uint16_t h, Rect r, bool erase) {
    Window* w = wnd(h);
    if (!w) return;
    Rect client{0, 0, w->rect.w(), w->rect.h()};
    r = r.intersect(client);
    if (r.empty()) return;
    w->update = w->update.unite(r);
    w->erase = w->erase || erase;
    for (uint16_t ch : w->children) {
        Window* cw = wnd(ch);
        if (!cw || !cw->visible) continue;
        invalidate(ch, r.offset(-cw->rect.left, -cw->rect.top), true);
    }
}

void invalidate_screen_rect(const Rect& sr) {
    for (uint16_t t : g_top) {
        Window* w = wnd(t);
        if (!w || !w->visible) continue;
        invalidate(t, sr.offset(-w->rect.left, -w->rect.top), true);
    }
}

// first window (paint order: parents before children, back to front) needing paint
uint16_t find_paint(uint16_t h) {
    Window* w = wnd(h);
    if (!w || !w->visible) return 0;
    if (!w->update.empty()) return h;
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it)
        if (uint16_t f = find_paint(*it)) return f;
    return 0;
}

uint16_t next_paint() {
    for (auto it = g_top.rbegin(); it != g_top.rend(); ++it)
        if (uint16_t f = find_paint(*it)) return f;
    return 0;
}

uint16_t child_from_point(uint16_t h, int x, int y) {   // x,y in h's client coords
    Window* w = wnd(h);
    for (uint16_t ch : w->children) {
        Window* cw = wnd(ch);
        if (!cw || !cw->visible || !cw->rect.contains(x, y)) continue;
        return child_from_point(ch, x - cw->rect.left, y - cw->rect.top);
    }
    return h;
}

uint16_t window_from_point(int x, int y) {
    for (uint16_t t : g_top) {
        Window* w = wnd(t);
        if (!w || !w->visible || !w->rect.contains(x, y)) continue;
        return child_from_point(t, x - w->rect.left, y - w->rect.top);
    }
    return 0;
}

bool enabled_chain(uint16_t h) {
    for (Window* w = wnd(h); w; w = w->parent ? wnd(w->parent) : nullptr)
        if (!w->enabled) return false;
    return true;
}

// ---- cursors ---------------------------------------------------------------

uint16_t add_cursor(std::unique_ptr<CursorImage> img) {
    uint16_t h = g_nextCursor;
    g_nextCursor = uint16_t(g_nextCursor + 2);
    g_cursors[h] = std::move(img);
    return h;
}

std::unique_ptr<CursorImage> arrow_cursor() {
    static const char* rows[] = {
        "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
        "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
        "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ", "      XX    "};
    auto c = std::make_unique<CursorImage>();
    c->image = Surface(12, 17, 32);
    for (int y = 0; y < 17; ++y)
        for (int x = 0; x < 12; ++x)
            c->image.set(x, y, rows[y][x] == 'X' ? 0xFF000000 : rows[y][x] == '.' ? 0xFFFFFFFF : 0);
    return c;
}

// RT_CURSOR / RT_ICON resource data -> image
std::unique_ptr<CursorImage> decode_cursor(const uint8_t* d, size_t n, bool cursor) {
    auto c = std::make_unique<CursorImage>();
    size_t o = 0;
    if (cursor) {
        c->hotX = d[0] | (d[1] << 8);
        c->hotY = d[2] | (d[3] << 8);
        o = 4;
    }
    if (o + 40 > n) return c;
    auto u32 = [&](size_t p) { return uint32_t(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (uint32_t(d[p + 3]) << 24)); };
    int hs = int(u32(o)), w = int(u32(o + 4)), h = int(u32(o + 8)) / 2;
    int bpp = d[o + 14] | (d[o + 15] << 8);
    int ncol = bpp <= 8 ? (1 << bpp) : 0;
    size_t pal = o + hs;
    size_t xorOff = pal + 4 * ncol;
    int xstride = ((w * bpp + 31) / 32) * 4, astride = ((w + 31) / 32) * 4;
    size_t andOff = xorOff + size_t(xstride) * h;
    if (andOff + size_t(astride) * h > n || w <= 0 || h <= 0) return c;
    c->image = Surface(w, h, 32);
    for (int y = 0; y < h; ++y) {
        int row = h - 1 - y;
        for (int x = 0; x < w; ++x) {
            bool mask = d[andOff + size_t(row) * astride + x / 8] & (0x80 >> (x % 8));
            uint32_t col = 0;
            const uint8_t* r = d + xorOff + size_t(row) * xstride;
            int idx = 0;
            if (bpp == 1) idx = (r[x / 8] >> (7 - x % 8)) & 1;
            else if (bpp == 4) idx = (x & 1) ? (r[x / 2] & 15) : (r[x / 2] >> 4);
            else if (bpp == 8) idx = r[x];
            if (bpp <= 8) {
                const uint8_t* q = d + pal + 4 * idx;
                col = (uint32_t(q[2]) << 16) | (uint32_t(q[1]) << 8) | q[0];
            }
            if (!mask) c->image.set(x, y, 0xFF000000 | col);
            else if (idx) c->image.set(x, y, 0xFF000000);   // inverted pixels: draw black
            else c->image.set(x, y, 0);
        }
    }
    return c;
}

uint16_t load_group(Module& m, const ResKey& key, bool cursor) {
    ResKey type{cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON, ""};
    const Resource* g = find_resource(m, key, type);
    if (!g) return 0;
    const uint8_t* gd = resource_data(m, *g);
    if (!gd) return 0;
    int count = gd[4] | (gd[5] << 8);
    if (count < 1) return 0;
    int nid = gd[6 + 12] | (gd[6 + 13] << 8);   // first entry's resource id
    ResKey member{nid, ""};
    ResKey mtype{cursor ? RT_CURSOR : RT_ICON, ""};
    const Resource* r = find_resource(m, member, mtype);
    if (!r) return 0;
    return add_cursor(decode_cursor(resource_data(m, *r), r->length, cursor));
}

// ---- message plumbing ------------------------------------------------------

struct SavedRegs {
    uint32_t r[8];
    uint16_t ds, es, fs, gs;
};

SavedRegs save(Cpu& c) {
    SavedRegs s;
    std::copy(std::begin(c.r), std::end(c.r), s.r);
    s.ds = c.ds; s.es = c.es; s.fs = c.fs; s.gs = c.gs;
    return s;
}

void restore(Cpu& c, const SavedRegs& s) {
    std::copy(std::begin(s.r), std::end(s.r), c.r);
    c.ds = s.ds; c.es = s.es; c.fs = s.fs; c.gs = s.gs;
}

uint32_t default_proc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);

}  // namespace

uint32_t call_app(uint32_t proc, const uint16_t* words, int n) {
    Cpu& c = Runtime::get().cpu();
    SavedRegs s = save(c);
    uint16_t savedScratch = g_scratchTop;
    c.ds = Runtime::get().data_sel();
    for (int i = 0; i < n; ++i) c.push16(words[i]);
    Runtime::get().call(uint16_t(proc >> 16), uint16_t(proc));
    uint32_t result = (uint32_t(c.dx()) << 16) | c.ax();
    restore(c, s);
    g_scratchTop = savedScratch;
    return result;
}

static uint32_t call_wndproc(uint32_t proc, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (!proc) return default_proc(hwnd, msg, wp, lp);
    uint16_t words[5] = {hwnd, msg, wp, uint16_t(lp >> 16), uint16_t(lp)};
    return call_app(proc, words, 5);
}

uint32_t send_message(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    Window* w = wnd(hwnd);
    if (!w) return 0;
    return call_wndproc(w->proc, hwnd, msg, wp, lp);
}

void post_message(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    g_queue.push_back(Msg{hwnd, msg, wp, lp});
}

namespace {

uint16_t host_proc_far_off = 0;   // import slot offset of the default window procedure
uint32_t default_proc_far() { return Runtime::get().import_far("HOST", 1); }

void do_destroy(uint16_t h);
void move_window(uint16_t h, Rect nr, bool repaint);

uint32_t default_proc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    Cpu& c = Runtime::get().cpu();
    Window* w = wnd(hwnd);
    switch (msg) {
        case WM_NCCREATE: {
            if (w && lp) {
                uint32_t name = c.rd32(uint16_t(lp >> 16), uint16_t(uint16_t(lp) + 22));   // lpszName
                if (name >> 16) w->text = recomp::read_cstr(c, name, 512);
            }
            return 1;
        }
        case WM_NCHITTEST: return 1;   // HTCLIENT
        case WM_SETTEXT:
            if (w) w->text = lp ? recomp::read_cstr(c, lp, 4096) : std::string();
            return 1;
        case WM_GETTEXT:
            if (!w || !wp) return 0;
            recomp::write_cstr(c, lp, w->text, wp);
            return uint32_t(std::min<size_t>(w->text.size(), wp - 1));
        case WM_GETTEXTLENGTH: return w ? uint32_t(w->text.size()) : 0;
        case WM_CLOSE: do_destroy(hwnd); return 0;
        case WM_ERASEBKGND: {
            if (!w) return 0;
            auto it = g_classes.find(w->cls);
            uint16_t br = it == g_classes.end() ? 0 : it->second.hbrBackground;
            if (!br) return 0;
            DC* dc = dc_get(wp);
            if (!dc) return 0;
            uint16_t brush = br;
            if (br <= 31) {   // COLOR_xxx + 1
                auto b = std::make_unique<GdiObject>();
                b->type = ObjType::Brush;
                b->color = 0xC0C0C0;
                brush = gdi_add(std::move(b));
            }
            draw_fill_rect(*dc, Rect{0, 0, w->rect.w(), w->rect.h()}, brush);
            return 1;
        }
        case WM_SETCURSOR: {
            if (w && w->parent && send_message(w->parent, WM_SETCURSOR, wp, lp)) return 1;
            if (w && (lp & 0xFFFF) == 1) {
                auto it = g_classes.find(w->cls);
                if (it != g_classes.end() && it->second.hCursor) {
                    g_cursor = it->second.hCursor;
                    return 1;
                }
            }
            return 0;
        }
        case WM_MOUSEACTIVATE: return 1;
        case WM_PAINT: {
            if (w) { w->update = Rect{}; w->erase = false; }
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == 0xF060) post_message(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case 0x0011: return 1;   // WM_QUERYENDSESSION
        case 0x0013: return 1;   // WM_QUERYOPEN
        case WM_CTLCOLOR: return gdi_stock(0);
        case WM_GETDLGCODE: return 0;
        default: return 0;
    }
}

// Default window procedure called by the program (DefWindowProc import or a
// class with our default procedure).
void api_default_proc_thunk(Cpu& c) {
    Args a(c, 10);
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); uint16_t hwnd = a.w();
    a.ret32(default_proc(hwnd, msg, wp, lp));
}

void do_destroy(uint16_t h) {
    Window* w = wnd(h);
    if (!w) return;
    if (w->visible) {
        Rect vr = visible_rect(h);
        w->visible = false;
        invalidate_screen_rect(vr);
    }
    send_message(h, WM_DESTROY, 0, 0);
    auto kids = w->children;
    for (uint16_t ch : kids) do_destroy(ch);
    w = wnd(h);
    if (!w) return;
    send_message(h, WM_NCDESTROY, 0, 0);
    auto& sib = siblings(h);
    sib.erase(std::remove(sib.begin(), sib.end(), h), sib.end());
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [h](const Timer& t) { return t.hwnd == h; }),
                   g_timers.end());
    if (g_capture == h) g_capture = 0;
    if (g_focus == h) g_focus = 0;
    if (g_active == h) g_active = 0;
    w->destroyed = true;
}

void show(uint16_t h, bool vis) {
    Window* w = wnd(h);
    if (!w || w->visible == vis) return;
    if (!vis) {
        Rect vr = visible_rect(h);
        w->visible = false;
        invalidate_screen_rect(vr);
        if (w->parent) {
            int px, py;
            screen_origin(w->parent, &px, &py);
            invalidate(w->parent, vr.offset(-px, -py), true);
        }
    } else {
        w->visible = true;
        // WS_MAXIMIZE top-level windows fill the 640x480 screen
        if (!w->parent && (w->style & 0x01000000u) && w->rect != Rect{0, 0, 640, 480}) {
            move_window(h, Rect{0, 0, 640, 480}, true);
            if (!(w = wnd(h))) return;
        }
        invalidate(h, Rect{0, 0, w->rect.w(), w->rect.h()}, true);
    }
    send_message(h, WM_SHOWWINDOW, vis ? 1 : 0, 0);
}

void move_window(uint16_t h, Rect nr, bool repaint) {
    Window* w = wnd(h);
    if (!w) return;
    Rect old = visible_rect(h);
    bool moved = nr.left != w->rect.left || nr.top != w->rect.top;
    bool sized = nr.w() != w->rect.w() || nr.h() != w->rect.h();
    w->rect = nr;
    if (old != Rect{} || w->visible) {
        invalidate_screen_rect(old);
        if (w->parent) {
            int px, py;
            screen_origin(w->parent, &px, &py);
            invalidate(w->parent, old.offset(-px, -py), true);
        }
        if (repaint || sized) invalidate(h, Rect{0, 0, nr.w(), nr.h()}, true);
    }
    if (sized) send_message(h, WM_SIZE, 0, (uint32_t(uint16_t(nr.h())) << 16) | uint16_t(nr.w()));
    if (moved) send_message(h, WM_MOVE, 0, (uint32_t(uint16_t(nr.top)) << 16) | uint16_t(nr.left));
}

bool g_appActive = false;

// Activate a top-level window like Windows does when it is shown/clicked.
void activate(uint16_t h) {
    Window* w = wnd(h);
    if (!w || w->parent || g_active == h) return;
    uint16_t old = g_active;
    g_active = h;
    if (!g_appActive) {
        g_appActive = true;
        send_message(h, WM_ACTIVATEAPP, 1, 0);
    }
    if (old && wnd(old)) send_message(old, WM_ACTIVATE, 0, h);
    if (wnd(h)) send_message(h, WM_ACTIVATE, 1, old);
}

void bring_to_top(uint16_t h) {
    auto& sib = siblings(h);
    auto it = std::find(sib.begin(), sib.end(), h);
    if (it == sib.end() || it == sib.begin()) return;
    sib.erase(it);
    sib.insert(sib.begin(), h);
    if (Window* w = wnd(h)) invalidate(h, Rect{0, 0, w->rect.w(), w->rect.h()}, true);
}

// ---- timers and queue --------------------------------------------------------

uint64_t now_ms() { return SDL_GetTicks(); }

bool timers_due() {
    uint64_t t = now_ms();
    bool any = false;
    for (auto& tm : g_timers) {
        if (!tm.pending && t >= tm.due) {
            tm.pending = true;
            tm.due = t + std::max<uint32_t>(tm.interval, 1);
            post_message(tm.hwnd, WM_TIMER, tm.id, tm.proc);
            any = true;
        }
    }
    return any;
}

int ms_to_next_timer() {
    uint64_t t = now_ms(), best = t + 50;
    for (auto& tm : g_timers)
        if (!tm.pending) best = std::min(best, tm.due);
    return int(best > t ? best - t : 0);
}

bool matches(const Msg& m, uint16_t hwnd, uint16_t mn, uint16_t mx) {
    if (hwnd && m.hwnd != hwnd) return false;
    if (mn || mx) return m.msg >= mn && m.msg <= mx;
    return true;
}

void write_msg(Cpu& c, uint32_t p, const Msg& m) {
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    c.wr16(s, o, m.hwnd);
    c.wr16(s, uint16_t(o + 2), m.msg);
    c.wr16(s, uint16_t(o + 4), m.wParam);
    c.wr32(s, uint16_t(o + 6), m.lParam);
    c.wr32(s, uint16_t(o + 10), uint32_t(now_ms()));
    c.wr16(s, uint16_t(o + 14), uint16_t(g_mouseX));
    c.wr16(s, uint16_t(o + 16), uint16_t(g_mouseY));
}

// Fetch the next message (posted, timer, paint) without blocking.
bool get_next(Msg& out, uint16_t hwnd, uint16_t mn, uint16_t mx, bool remove) {
    if (g_quit) {
        out = Msg{0, WM_QUIT, uint16_t(g_quitCode), 0};
        if (remove) g_quit = false;
        return true;
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
            if (!matches(*it, hwnd, mn, mx)) continue;
            out = *it;
            if (remove) {
                if (out.msg == WM_TIMER)
                    for (auto& tm : g_timers)
                        if (tm.hwnd == out.hwnd && tm.id == out.wParam) tm.pending = false;
                g_queue.erase(it);
            }
            return true;
        }
        if (pass == 0 && !timers_due()) break;
    }
    if (!mn || (mn <= WM_PAINT && mx >= WM_PAINT)) {
        uint16_t p = next_paint();
        if (p && (!hwnd || p == hwnd)) {
            out = Msg{p, WM_PAINT, 0, 0};
            return true;
        }
    }
    return false;
}

}  // namespace

// ---- input from the host ----------------------------------------------------

void input_mouse_move(int x, int y) {
    g_mouseX = x;
    g_mouseY = y;
    uint16_t target = g_capture ? g_capture : window_from_point(x, y);
    if (!target || (!g_capture && !enabled_chain(target))) return;
    int ox, oy;
    screen_origin(target, &ox, &oy);
    if (!g_capture) send_message(target, WM_SETCURSOR, target, (uint32_t(WM_MOUSEMOVE) << 16) | 1);
    uint16_t keys = uint16_t((g_buttons[0] ? 1 : 0) | (g_buttons[1] ? 2 : 0) | (g_keys[0x10] ? 4 : 0) |
                             (g_keys[0x11] ? 8 : 0));
    // coalesce: drop an older unprocessed move for the same window
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
        if (it->msg == WM_MOUSEMOVE && it->hwnd == target) { g_queue.erase(it); break; }
    post_message(target, WM_MOUSEMOVE, keys, (uint32_t(uint16_t(y - oy)) << 16) | uint16_t(x - ox));
}

void input_mouse_button(int x, int y, int button, bool down) {
    g_mouseX = x;
    g_mouseY = y;
    if (button >= 0 && button < 3) g_buttons[button] = down;
    uint16_t target = g_capture ? g_capture : window_from_point(x, y);
    if (!target || (!g_capture && !enabled_chain(target))) return;
    int ox, oy;
    screen_origin(target, &ox, &oy);
    uint16_t msg = button == 0 ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP) : (down ? WM_RBUTTONDOWN : WM_RBUTTONUP);
    uint16_t keys = uint16_t((g_buttons[0] ? 1 : 0) | (g_buttons[1] ? 2 : 0) | (g_keys[0x10] ? 4 : 0) |
                             (g_keys[0x11] ? 8 : 0));
    post_message(target, msg, keys, (uint32_t(uint16_t(y - oy)) << 16) | uint16_t(x - ox));
}

void input_key(int vk, bool down) {
    if (vk > 0 && vk < 256) g_keys[vk] = down;
    uint16_t target = g_focus ? g_focus : g_active;
    if (target) post_message(target, down ? WM_KEYDOWN : WM_KEYUP, uint16_t(vk), 1);
}

void input_char(int ch) {
    uint16_t target = g_focus ? g_focus : g_active;
    if (target) post_message(target, WM_CHAR, uint16_t(ch), 1);
}

void input_quit() { throw ExitProgram{0}; }

static void dump_window(uint16_t h, int depth) {
    Window* w = wnd(h);
    if (!w) return;
    recomp::log("%*s%04X %s '%s' rect=(%d,%d)-(%d,%d) %s%s style=%08X", depth * 2, "", h, w->cls.c_str(),
                w->text.substr(0, 24).c_str(), w->rect.left, w->rect.top, w->rect.right, w->rect.bottom,
                w->visible ? "visible" : "hidden", w->enabled ? "" : " disabled", w->style);
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) dump_window(*it, depth + 1);
}

void dump_windows() {
    for (auto it = g_top.rbegin(); it != g_top.rend(); ++it) {
        Window* w = wnd(*it);
        if (w && w->visible) dump_window(*it, 0);
    }
}

const CursorImage* current_cursor() {
    auto it = g_cursors.find(g_cursor ? g_cursor : g_arrow);
    return it == g_cursors.end() ? nullptr : it->second.get();
}

void cursor_position(int* x, int* y) {
    *x = g_mouseX;
    *y = g_mouseY;
}

void user_init() {
    g_scratch = Runtime::get().alloc_segment(0x10000, "user-scratch");
    g_arrow = add_cursor(arrow_cursor());
    g_cursor = g_arrow;
    Runtime::get().register_api("HOST", 1, &api_default_proc_thunk, "DefWindowProc");
    (void)host_proc_far_off;
    controls_init();
    // predefined control classes: native procedures from controls.cpp
    for (const char* n : {"BUTTON", "EDIT", "LISTBOX", "COMBOBOX", "STATIC", "SCROLLBAR", "MDICLIENT"}) {
        WndClass wc;
        wc.name = n;
        wc.proc = control_proc(n);
        if (!wc.proc) wc.proc = default_proc_far();
        wc.hbrBackground = 0;   // controls paint their own background
        g_classes[n] = wc;
    }
}

// ---- window internals for controls.cpp ----
bool wnd_info(uint16_t h, WndInfo& out) {
    Window* w = wnd(h);
    if (!w) return false;
    out.parent = w->parent;
    out.id = w->id;
    out.style = w->style;
    out.w = w->rect.w();
    out.h = w->rect.h();
    out.enabled = enabled_chain(h);
    out.visible = really_visible(h);
    return true;
}
std::string wnd_text(uint16_t h) { Window* w = wnd(h); return w ? w->text : std::string(); }
void wnd_set_text(uint16_t h, const std::string& text) { if (Window* w = wnd(h)) w->text = text; }
void wnd_invalidate(uint16_t h) {
    if (Window* w = wnd(h)) invalidate(h, Rect{0, 0, w->rect.w(), w->rect.h()}, false);
}
uint16_t wnd_paint_begin(uint16_t h) {
    Window* w = wnd(h);
    if (!w) return 0;
    int ox, oy;
    screen_origin(h, &ox, &oy);
    w->update = Rect{};
    w->erase = false;
    return dc_create_window(h, ox, oy, visible_rect(h));
}
void wnd_paint_end(uint16_t hdc) { dc_delete(hdc); }
uint16_t wnd_focus() { return g_focus; }
void wnd_set_focus(uint16_t h) {
    uint16_t old = g_focus;
    if (h == old) return;
    g_focus = h;
    if (old && wnd(old)) send_message(old, WM_KILLFOCUS, h, 0);
    if (h && wnd(h)) send_message(h, WM_SETFOCUS, old, 0);
}
uint16_t wnd_capture() { return g_capture; }
void wnd_set_capture(uint16_t h) { g_capture = h; }
std::vector<uint16_t> wnd_children(uint16_t h) {
    Window* w = wnd(h);
    return w ? w->children : std::vector<uint16_t>{};
}
uint32_t wnd_default_proc(uint16_t h, uint16_t msg, uint16_t wp, uint32_t lp) { return default_proc(h, msg, wp, lp); }
void screen_to_client(uint16_t h, int* x, int* y) {
    int ox, oy;
    screen_origin(h, &ox, &oy);
    *x -= ox;
    *y -= oy;
}

}  // namespace win16

// ============================================================================
// USER exports
// ============================================================================
using namespace win16;

namespace {

Rect read_rect(Cpu& c, uint32_t p) {
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    return Rect{int16_t(c.rd16(s, o)), int16_t(c.rd16(s, uint16_t(o + 2))),
                int16_t(c.rd16(s, uint16_t(o + 4))), int16_t(c.rd16(s, uint16_t(o + 6)))};
}
void write_rect(Cpu& c, uint32_t p, const Rect& r) {
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    c.wr16(s, o, uint16_t(r.left)); c.wr16(s, uint16_t(o + 2), uint16_t(r.top));
    c.wr16(s, uint16_t(o + 4), uint16_t(r.right)); c.wr16(s, uint16_t(o + 6), uint16_t(r.bottom));
}
std::string prop_key(Cpu& c, uint32_t p) {
    if ((p >> 16) == 0) return "#" + std::to_string(p & 0xFFFF);
    return upper(recomp::read_cstr(c, p, 256));
}
uint32_t make_lparam(int lo, int hi) { return (uint32_t(uint16_t(hi)) << 16) | uint16_t(lo); }

}  // namespace

API(USER, 5, InitApp, 2) { a.w(); a.ret(1); }
API(USER, 266, SetMessageQueue, 2) { a.w(); a.ret(1); }
API(USER, 1, MessageBox, 12) {
    uint16_t type = a.w(); uint32_t cap = a.p(); uint32_t text = a.p(); a.w();
    recomp::log("MessageBox [%s]: %s", cap ? recomp::read_cstr(c, cap).c_str() : "",
                text ? recomp::read_cstr(c, text).c_str() : "");
    int t = type & 0xF;
    a.ret(uint16_t(t == 3 || t == 4 ? 6 : 1));   // IDYES / IDOK
}
API(USER, 171, WinHelp, 10) { a.l(); a.w(); a.p(); a.w(); a.ret(1); }

// ---- classes ----
API(USER, 57, RegisterClass, 4) {
    uint32_t p = a.p(); uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    WndClass wc;
    wc.style = c.rd16(s, o);
    wc.proc = c.rd32(s, uint16_t(o + 2));
    wc.clsExtra = int16_t(c.rd16(s, uint16_t(o + 6)));
    wc.wndExtra = int16_t(c.rd16(s, uint16_t(o + 8)));
    wc.hInst = c.rd16(s, uint16_t(o + 10));
    wc.hIcon = c.rd16(s, uint16_t(o + 12));
    wc.hCursor = c.rd16(s, uint16_t(o + 14));
    wc.hbrBackground = c.rd16(s, uint16_t(o + 16));
    wc.menuName = c.rd32(s, uint16_t(o + 18));
    uint32_t name = c.rd32(s, uint16_t(o + 22));
    wc.name = (name >> 16) ? upper(recomp::read_cstr(c, name)) : "#" + std::to_string(name & 0xFFFF);
    g_classes[wc.name] = wc;
    a.ret(uint16_t(0xC000 + g_classes.size()));
}
API(USER, 404, GetClassInfo, 10) {
    uint32_t p = a.p(); uint32_t name = a.p(); a.w();
    std::string n = (name >> 16) ? upper(recomp::read_cstr(c, name)) : "#" + std::to_string(name & 0xFFFF);
    auto it = g_classes.find(n);
    if (it == g_classes.end()) { a.ret(0); return; }
    const WndClass& wc = it->second;
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    c.wr16(s, o, wc.style); c.wr32(s, uint16_t(o + 2), wc.proc);
    c.wr16(s, uint16_t(o + 6), uint16_t(wc.clsExtra)); c.wr16(s, uint16_t(o + 8), uint16_t(wc.wndExtra));
    c.wr16(s, uint16_t(o + 10), wc.hInst); c.wr16(s, uint16_t(o + 12), wc.hIcon);
    c.wr16(s, uint16_t(o + 14), wc.hCursor); c.wr16(s, uint16_t(o + 16), wc.hbrBackground);
    c.wr32(s, uint16_t(o + 18), wc.menuName); c.wr32(s, uint16_t(o + 22), name);
    a.ret(1);
}

// ---- windows ----
static uint16_t create_window(Cpu& c, uint32_t exStyle, uint32_t clsName, uint32_t title, uint32_t style,
                              int x, int y, int w, int h, uint16_t parent, uint16_t menu, uint16_t inst,
                              uint32_t param) {
    std::string cn = (clsName >> 16) ? upper(recomp::read_cstr(c, clsName)) : "#" + std::to_string(clsName & 0xFFFF);
    auto it = g_classes.find(cn);
    if (it == g_classes.end()) {
        recomp::log("CreateWindow: unknown class %s", cn.c_str());
        return 0;
    }
    const WndClass& wc = it->second;
    if (x == int16_t(0x8000)) { x = 0; y = 0; }
    if (w == int16_t(0x8000)) { w = 640; h = 480; }
    uint16_t hwnd = g_nextHwnd;
    g_nextHwnd = uint16_t(g_nextHwnd + 4);
    Window& wn = g_wnds[hwnd];
    wn.hwnd = hwnd;
    wn.cls = cn;
    wn.proc = wc.proc;
    wn.style = style & ~0x10000000u;   // WS_VISIBLE applied after creation
    wn.exStyle = exStyle;
    bool child = (style & 0x40000000u) != 0;
    wn.parent = child ? parent : 0;
    wn.owner = child ? 0 : parent;
    wn.id = child ? menu : 0;
    wn.hInst = inst;
    wn.rect = Rect{x, y, x + w, y + h};
    wn.extra.assign(size_t(std::max(wc.wndExtra, 0)), 0);
    if (wn.parent) {
        if (Window* p = wnd(wn.parent)) p->children.insert(p->children.begin(), hwnd);
    } else {
        g_top.insert(g_top.begin(), hwnd);
    }
    // CREATESTRUCT in scratch memory
    uint16_t cs = scratch_alloc(34);
    Cpu& cc = c;
    cc.wr32(g_scratch, cs, param);
    cc.wr16(g_scratch, uint16_t(cs + 4), inst);
    cc.wr16(g_scratch, uint16_t(cs + 6), menu);
    cc.wr16(g_scratch, uint16_t(cs + 8), parent);
    cc.wr16(g_scratch, uint16_t(cs + 10), uint16_t(h));
    cc.wr16(g_scratch, uint16_t(cs + 12), uint16_t(w));
    cc.wr16(g_scratch, uint16_t(cs + 14), uint16_t(y));
    cc.wr16(g_scratch, uint16_t(cs + 16), uint16_t(x));
    cc.wr32(g_scratch, uint16_t(cs + 18), style);
    cc.wr32(g_scratch, uint16_t(cs + 22), title);
    cc.wr32(g_scratch, uint16_t(cs + 26), clsName);
    cc.wr32(g_scratch, uint16_t(cs + 30), exStyle);
    uint32_t lpcs = (uint32_t(g_scratch) << 16) | cs;
    if (!send_message(hwnd, WM_NCCREATE, 0, lpcs) || int32_t(send_message(hwnd, WM_CREATE, 0, lpcs)) == -1) {
        do_destroy(hwnd);
        return 0;
    }
    if (!wnd(hwnd)) return 0;
    send_message(hwnd, WM_SIZE, 0, make_lparam(w, h));
    send_message(hwnd, WM_MOVE, 0, make_lparam(x, y));
    if (style & 0x10000000u) {
        if (Window* wp = wnd(hwnd)) wp->style |= 0x10000000u;
        show(hwnd, true);
    }
    return hwnd;
}

API(USER, 41, CreateWindow, 30) {
    uint32_t param = a.p(); uint16_t inst = a.w(); uint16_t menu = a.w(); uint16_t parent = a.w();
    int16_t h = a.i(); int16_t w = a.i(); int16_t y = a.i(); int16_t x = a.i(); uint32_t style = a.l();
    uint32_t title = a.p(); uint32_t cls = a.p();
    a.ret(create_window(c, 0, cls, title, style, x, y, w, h, parent, menu, inst, param));
}
API(USER, 452, CreateWindowEx, 34) {
    uint32_t param = a.p(); uint16_t inst = a.w(); uint16_t menu = a.w(); uint16_t parent = a.w();
    int16_t h = a.i(); int16_t w = a.i(); int16_t y = a.i(); int16_t x = a.i(); uint32_t style = a.l();
    uint32_t title = a.p(); uint32_t cls = a.p(); uint32_t ex = a.l();
    a.ret(create_window(c, ex, cls, title, style, x, y, w, h, parent, menu, inst, param));
}
API(USER, 53, DestroyWindow, 2) { do_destroy(a.w()); a.ret(1); }
API(USER, 47, IsWindow, 2) { a.ret(wnd(a.w()) ? 1 : 0); }
API(USER, 42, ShowWindow, 4) {
    int16_t cmd = a.i(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    bool was = w->visible;
    bool vis = cmd != 0 && cmd != 6 && cmd != 7;   // SW_HIDE / MINIMIZE
    if (vis) w->style |= 0x10000000u; else w->style &= ~0x10000000u;
    show(h, vis);
    if (cmd == 3 && !w->parent) move_window(h, Rect{0, 0, 640, 480}, true);
    if (vis && (cmd == 1 || cmd == 3 || cmd == 5 || cmd == 9)) {
        if (!w->parent) { bring_to_top(h); activate(h); }
    }
    a.ret(was ? 1 : 0);
}
API(USER, 49, IsWindowVisible, 2) { a.ret(really_visible(a.w()) ? 1 : 0); }
API(USER, 31, IsIconic, 2) { a.w(); a.ret(0); }
API(USER, 272, IsZoomed, 2) { a.w(); a.ret(0); }
API(USER, 34, EnableWindow, 4) {
    uint16_t en = a.w(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    bool was = !w->enabled;
    if (w->enabled != (en != 0)) {
        w->enabled = en != 0;
        send_message(h, WM_ENABLE, en, 0);
    }
    a.ret(was ? 1 : 0);
}
API(USER, 35, IsWindowEnabled, 2) { Window* w = wnd(a.w()); a.ret(w && w->enabled ? 1 : 0); }
API(USER, 32, GetWindowRect, 6) {
    uint32_t p = a.p(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { write_rect(c, p, Rect{0, 0, 640, 480}); a.ret(); return; }
    int ox, oy;
    screen_origin(h, &ox, &oy);
    write_rect(c, p, Rect{ox, oy, ox + w->rect.w(), oy + w->rect.h()});
    a.ret();
}
API(USER, 33, GetClientRect, 6) {
    uint32_t p = a.p(); uint16_t h = a.w();
    Window* w = wnd(h);
    write_rect(c, p, w ? Rect{0, 0, w->rect.w(), w->rect.h()} : Rect{0, 0, 640, 480});
    a.ret();
}
API(USER, 232, SetWindowPos, 14) {
    uint16_t flags = a.w(); int16_t cy = a.i(); int16_t cx = a.i(); int16_t y = a.i(); int16_t x = a.i();
    uint16_t after = a.w(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    Rect nr = w->rect;
    if (!(flags & 0x0002)) nr = nr.offset(x - nr.left, y - nr.top);                 // SWP_NOMOVE
    if (!(flags & 0x0001)) { nr.right = nr.left + cx; nr.bottom = nr.top + cy; }     // SWP_NOSIZE
    if (!(flags & 0x0004) && after == 0) bring_to_top(h);                            // HWND_TOP
    move_window(h, nr, !(flags & 0x0008));
    if (flags & 0x0040) show(h, true);                                               // SWP_SHOWWINDOW
    if (flags & 0x0080) show(h, false);                                              // SWP_HIDEWINDOW
    if ((flags & 0x0040) && !(flags & 0x0010)) activate(h);                          // not SWP_NOACTIVATE
    a.ret(1);
}
API(USER, 45, BringWindowToTop, 2) { bring_to_top(a.w()); a.ret(1); }
API(USER, 46, GetParent, 2) {
    Window* w = wnd(a.w());
    a.ret(w ? (w->parent ? w->parent : w->owner) : 0);
}
API(USER, 262, GetWindow, 4) {
    uint16_t cmd = a.w(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    auto& sib = siblings(h);
    auto it = std::find(sib.begin(), sib.end(), h);
    switch (cmd) {
        case 0: a.ret(sib.empty() ? 0 : sib.front()); return;                     // GW_HWNDFIRST
        case 1: a.ret(sib.empty() ? 0 : sib.back()); return;                      // GW_HWNDLAST
        case 2: a.ret(it != sib.end() && it + 1 != sib.end() ? *(it + 1) : 0); return;   // NEXT
        case 3: a.ret(it != sib.end() && it != sib.begin() ? *(it - 1) : 0); return;     // PREV
        case 4: a.ret(w->owner); return;
        case 5: a.ret(w->children.empty() ? 0 : w->children.front()); return;
        default: a.ret(0); return;
    }
}
API(USER, 54, EnumWindows, 8) {
    uint32_t lp = a.l(); uint32_t proc = a.p();
    auto tops = g_top;
    for (uint16_t h : tops) {
        if (!wnd(h)) continue;
        uint16_t words[3] = {h, uint16_t(lp >> 16), uint16_t(lp)};
        if (!(call_app(proc, words, 3) & 0xFFFF)) break;
    }
    a.ret(1);
}
API(USER, 225, EnumTaskWindows, 10) {
    uint32_t lp = a.l(); uint32_t proc = a.p(); a.w();
    auto tops = g_top;
    for (uint16_t h : tops) {
        if (!wnd(h)) continue;
        uint16_t words[3] = {h, uint16_t(lp >> 16), uint16_t(lp)};
        if (!(call_app(proc, words, 3) & 0xFFFF)) break;
    }
    a.ret(1);
}
API(USER, 36, GetWindowText, 8) {
    uint16_t n = a.w(); uint32_t buf = a.p(); uint16_t h = a.w();
    a.ret(uint16_t(send_message(h, WM_GETTEXT, n, buf)));
}
API(USER, 37, SetWindowText, 6) {
    uint32_t t = a.p(); uint16_t h = a.w();
    send_message(h, WM_SETTEXT, 0, t);
    a.ret();
}
static uint32_t* long_field(Window* w, int idx) { (void)w; (void)idx; return nullptr; }
API(USER, 133, GetWindowWord, 4) {
    int16_t idx = a.i(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    (void)long_field;
    switch (idx) {
        case -6: a.ret(w->hInst); return;     // GWW_HINSTANCE
        case -8: a.ret(w->parent ? w->parent : w->owner); return;
        case -12: a.ret(w->id); return;       // GWW_ID
        default:
            if (idx >= 0 && size_t(idx) + 2 <= w->extra.size()) { a.ret(uint16_t(w->extra[idx] | (w->extra[idx + 1] << 8))); return; }
            a.ret(0);
    }
}
API(USER, 134, SetWindowWord, 6) {
    uint16_t v = a.w(); int16_t idx = a.i(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    uint16_t old = 0;
    if (idx == -12) { old = w->id; w->id = v; }
    else if (idx == -6) { old = w->hInst; w->hInst = v; }
    else if (idx >= 0 && size_t(idx) + 2 <= w->extra.size()) {
        old = uint16_t(w->extra[idx] | (w->extra[idx + 1] << 8));
        w->extra[idx] = uint8_t(v); w->extra[idx + 1] = uint8_t(v >> 8);
    }
    a.ret(old);
}
API(USER, 135, GetWindowLong, 4) {
    int16_t idx = a.i(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret32(0); return; }
    switch (idx) {
        case -4: a.ret32(w->proc ? w->proc : default_proc_far()); return;
        case -16: a.ret32(w->style); return;
        case -20: a.ret32(w->exStyle); return;
        default:
            if (idx >= 0 && size_t(idx) + 4 <= w->extra.size()) {
                uint32_t v = w->extra[idx] | (w->extra[idx + 1] << 8) | (w->extra[idx + 2] << 16) | (uint32_t(w->extra[idx + 3]) << 24);
                a.ret32(v); return;
            }
            a.ret32(0);
    }
}
API(USER, 136, SetWindowLong, 8) {
    uint32_t v = a.l(); int16_t idx = a.i(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret32(0); return; }
    uint32_t old = 0;
    switch (idx) {
        case -4: old = w->proc ? w->proc : default_proc_far(); w->proc = v; break;
        case -16: old = w->style; w->style = v; break;
        case -20: old = w->exStyle; w->exStyle = v; break;
        default:
            if (idx >= 0 && size_t(idx) + 4 <= w->extra.size()) {
                old = w->extra[idx] | (w->extra[idx + 1] << 8) | (w->extra[idx + 2] << 16) | (uint32_t(w->extra[idx + 3]) << 24);
                for (int k = 0; k < 4; ++k) w->extra[size_t(idx + k)] = uint8_t(v >> (8 * k));
            }
    }
    a.ret32(old);
}

// ---- properties and atoms ----
API(USER, 26, SetProp, 8) {
    uint16_t v = a.w(); uint32_t key = a.p(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    w->props[prop_key(c, key)] = v;
    a.ret(1);
}
API(USER, 25, GetProp, 6) {
    uint32_t key = a.p(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    auto it = w->props.find(prop_key(c, key));
    a.ret(it == w->props.end() ? 0 : it->second);
}
API(USER, 24, RemoveProp, 6) {
    uint32_t key = a.p(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    auto k = prop_key(c, key);
    auto it = w->props.find(k);
    uint16_t v = it == w->props.end() ? 0 : it->second;
    if (it != w->props.end()) w->props.erase(it);
    a.ret(v);
}
API(USER, 268, GlobalAddAtom, 4) {
    uint32_t p = a.p();
    if (!(p >> 16)) { a.ret(uint16_t(p)); return; }
    std::string n = upper(recomp::read_cstr(c, p, 256));
    auto it = g_atomsByName.find(n);
    if (it != g_atomsByName.end()) { a.ret(it->second); return; }
    uint16_t id = g_nextAtom++;
    g_atomsByName[n] = id;
    g_atomsById[id] = n;
    a.ret(id);
}
API(USER, 269, GlobalDeleteAtom, 2) { a.w(); a.ret(0); }
API(USER, 118, RegisterWindowMessage, 4) {
    std::string n = upper(recomp::read_cstr(c, a.p(), 256));
    auto it = g_atomsByName.find(n);
    if (it != g_atomsByName.end()) { a.ret(it->second); return; }
    uint16_t id = g_nextAtom++;
    g_atomsByName[n] = id;
    a.ret(id);
}

// ---- messages ----
API(USER, 111, SendMessage, 10) {
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); uint16_t h = a.w();
    uint32_t r = 0;
    if (h == 0xFFFF) {   // HWND_BROADCAST to top-level windows
        auto tops = g_top;
        for (uint16_t t : tops) send_message(t, msg, wp, lp);
    } else {
        r = send_message(h, msg, wp, lp);
    }
    a.ret32(r);
}
API(USER, 110, PostMessage, 10) {
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); uint16_t h = a.w();
    post_message(h, msg, wp, lp);
    a.ret(1);
}
API(USER, 6, PostQuitMessage, 2) { g_quitCode = a.i(); g_quit = true; a.ret(); }
API(USER, 109, PeekMessage, 12) {
    uint16_t flags = a.w(); uint16_t mx = a.w(); uint16_t mn = a.w(); uint16_t h = a.w(); uint32_t p = a.p();
    Msg m;
    bool got = get_next(m, h, mn, mx, flags & 1);
    if (!got) {
        host_pump(0);
        got = get_next(m, h, mn, mx, flags & 1);
    }
    if (got) write_msg(c, p, m);
    a.ret(got ? 1 : 0);
}
API(USER, 112, WaitMessage, 0) {
    for (;;) {
        Msg m;
        if (get_next(m, 0, 0, 0, false)) break;
        host_pump(std::min(ms_to_next_timer(), 16));
    }
    a.ret();
}
API(USER, 113, TranslateMessage, 4) { a.p(); a.ret(0); }
API(USER, 114, DispatchMessage, 4) {
    uint32_t p = a.p(); uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    uint16_t h = c.rd16(s, o), msg = c.rd16(s, uint16_t(o + 2)), wp = c.rd16(s, uint16_t(o + 4));
    uint32_t lp = c.rd32(s, uint16_t(o + 6));
    uint32_t r = 0;
    if (msg == WM_TIMER && lp) {
        uint16_t words[4] = {h, msg, wp, 0};
        uint32_t t = uint32_t(SDL_GetTicks());
        words[3] = uint16_t(t >> 16);
        uint16_t w5[5] = {h, msg, wp, uint16_t(t >> 16), uint16_t(t)};
        (void)words;
        r = call_app(lp, w5, 5);
    } else if (msg == WM_PAINT) {
        r = send_message(h, msg, wp, lp);
        if (Window* w = wnd(h)) if (!w->update.empty()) { w->update = Rect{}; w->erase = false; }   // not validated
    } else {
        r = send_message(h, msg, wp, lp);
    }
    a.ret32(r);
}
API(USER, 90, IsDialogMessage, 6) { a.p(); a.w(); a.ret(0); }
API(USER, 107, DefWindowProc, 10) {
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); uint16_t h = a.w();
    a.ret32(default_proc(h, msg, wp, lp));
}
API(USER, 122, CallWindowProc, 14) {
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); uint16_t h = a.w(); uint32_t proc = a.p();
    a.ret32(call_wndproc(proc, h, msg, wp, lp));
}
API(USER, 445, DefFrameProc, 12) {
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); a.w(); uint16_t h = a.w();
    a.ret32(default_proc(h, msg, wp, lp));
}
API(USER, 447, DefMDIChildProc, 10) {
    uint32_t lp = a.l(); uint16_t wp = a.w(); uint16_t msg = a.w(); uint16_t h = a.w();
    a.ret32(default_proc(h, msg, wp, lp));
}
API(USER, 451, TranslateMDISysAccel, 6) { a.p(); a.w(); a.ret(0); }
API(USER, 106, GetKeyState, 2) {
    int16_t vk = a.i();
    bool down = false;
    if (vk == 1) down = g_buttons[0];
    else if (vk == 2) down = g_buttons[1];
    else if (vk > 0 && vk < 256) down = g_keys[vk];
    a.ret(down ? 0x8000 : 0);
}

// ---- timers ----
API(USER, 10, SetTimer, 10) {
    uint32_t proc = a.p(); uint16_t ms = a.w(); uint16_t id = a.w(); uint16_t h = a.w();
    if (!h) id = g_nextTimerId++;
    for (auto& t : g_timers)
        if (t.hwnd == h && t.id == id) {
            t.interval = ms; t.proc = proc; t.due = now_ms() + ms;
            a.ret(id ? id : 1); return;
        }
    g_timers.push_back(Timer{h, id, proc, ms, now_ms() + ms});
    a.ret(id ? id : 1);
}
API(USER, 12, KillTimer, 4) {
    uint16_t id = a.w(); uint16_t h = a.w();
    auto before = g_timers.size();
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(),
                                  [&](const Timer& t) { return t.hwnd == h && t.id == id; }), g_timers.end());
    for (auto it = g_queue.begin(); it != g_queue.end();)
        it = (it->msg == WM_TIMER && it->hwnd == h && it->wParam == id) ? g_queue.erase(it) : it + 1;
    a.ret(before != g_timers.size() ? 1 : 0);
}

// ---- painting ----
API(USER, 125, InvalidateRect, 8) {
    uint16_t erase = a.w(); uint32_t pr = a.p(); uint16_t h = a.w();
    if (!h) { invalidate_screen_rect(Rect{0, 0, 640, 480}); a.ret(); return; }
    Window* w = wnd(h);
    if (w) invalidate(h, pr ? read_rect(c, pr) : Rect{0, 0, w->rect.w(), w->rect.h()}, erase != 0);
    a.ret();
}
static void paint_now(uint16_t h) {
    Window* w = wnd(h);
    if (w && !w->update.empty() && really_visible(h)) {
        send_message(h, WM_PAINT, 0, 0);
        if ((w = wnd(h))) { w->update = Rect{}; w->erase = false; }
    }
}
API(USER, 124, UpdateWindow, 2) { paint_now(a.w()); a.ret(); }
API(USER, 39, BeginPaint, 6) {
    uint32_t ps = a.p(); uint16_t h = a.w();
    Window* w = wnd(h);
    if (!w) { a.ret(0); return; }
    int ox, oy;
    screen_origin(h, &ox, &oy);
    Rect upd = w->update.empty() ? Rect{} : w->update;
    Rect clip = visible_rect(h).intersect(upd.offset(ox, oy));
    uint16_t hdc = dc_create_window(h, ox, oy, clip);
    bool erase = w->erase;
    w->update = Rect{};
    w->erase = false;
    uint16_t s = uint16_t(ps >> 16), o = uint16_t(ps);
    c.wr16(s, o, hdc);
    c.wr16(s, uint16_t(o + 2), erase ? 1 : 0);
    write_rect(c, (uint32_t(s) << 16) | uint16_t(o + 4), upd);
    for (int i = 12; i < 32; ++i) c.wr8(s, uint16_t(o + i), 0);
    if (erase && !clip.empty()) {
        if (send_message(h, WM_ERASEBKGND, hdc, 0)) c.wr16(s, uint16_t(o + 2), 0);
    }
    a.ret(hdc);
}
API(USER, 40, EndPaint, 6) {
    uint32_t ps = a.p(); a.w();
    dc_delete(c.rd16(uint16_t(ps >> 16), uint16_t(ps)));
    a.ret();
}
API(USER, 66, GetDC, 2) {
    uint16_t h = a.w();
    if (!h) { a.ret(dc_create_window(0, 0, 0, Rect{0, 0, 640, 480})); return; }
    int ox, oy;
    screen_origin(h, &ox, &oy);
    a.ret(dc_create_window(h, ox, oy, visible_rect(h)));
}
API(USER, 359, GetDCEx, 8) {
    a.w(); a.w(); uint16_t h = a.w();
    if (!h) { a.ret(dc_create_window(0, 0, 0, Rect{0, 0, 640, 480})); return; }
    int ox, oy;
    screen_origin(h, &ox, &oy);
    a.ret(dc_create_window(h, ox, oy, visible_rect(h)));
}
API(USER, 68, ReleaseDC, 4) { uint16_t hdc = a.w(); a.w(); dc_delete(hdc); a.ret(1); }
API(USER, 61, ScrollWindow, 14) {
    a.p(); a.p(); a.i(); a.i(); uint16_t h = a.w();
    if (Window* w = wnd(h)) invalidate(h, Rect{0, 0, w->rect.w(), w->rect.h()}, true);
    a.ret();
}
API(USER, 62, SetScrollPos, 8) { a.w(); a.w(); a.w(); a.w(); a.ret(0); }
API(USER, 63, GetScrollPos, 4) { a.w(); a.w(); a.ret(0); }
API(USER, 64, SetScrollRange, 10) { a.w(); a.w(); a.w(); a.w(); a.w(); a.ret(); }

// ---- focus, capture, activation ----
API(USER, 18, SetCapture, 2) { uint16_t old = g_capture; g_capture = a.w(); a.ret(old); }
API(USER, 19, ReleaseCapture, 0) { g_capture = 0; a.ret(); }
API(USER, 236, GetCapture, 0) { a.ret(g_capture); }
API(USER, 22, SetFocus, 2) {
    uint16_t h = a.w();
    uint16_t old = g_focus;
    if (h == old) { a.ret(old); return; }
    g_focus = h;
    if (old && wnd(old)) send_message(old, WM_KILLFOCUS, h, 0);
    if (h && wnd(h)) send_message(h, WM_SETFOCUS, old, 0);
    a.ret(old);
}
API(USER, 23, GetFocus, 0) { a.ret(g_focus); }
API(USER, 59, SetActiveWindow, 2) { uint16_t old = g_active; activate(a.w()); a.ret(old); }
API(USER, 60, GetActiveWindow, 0) { a.ret(g_active); }
API(USER, 287, GetLastActivePopup, 2) { a.ret(a.w()); }
API(USER, 265, ShowOwnedPopups, 4) { a.w(); a.w(); a.ret(); }

// ---- coordinates ----
API(USER, 17, GetCursorPos, 4) {
    uint32_t p = a.p();
    c.wr16(uint16_t(p >> 16), uint16_t(p), uint16_t(g_mouseX));
    c.wr16(uint16_t(p >> 16), uint16_t(uint16_t(p) + 2), uint16_t(g_mouseY));
    a.ret();
}
API(USER, 28, ClientToScreen, 6) {
    uint32_t p = a.p(); uint16_t h = a.w();
    int ox, oy; screen_origin(h, &ox, &oy);
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    c.wr16(s, o, uint16_t(int16_t(c.rd16(s, o)) + ox));
    c.wr16(s, uint16_t(o + 2), uint16_t(int16_t(c.rd16(s, uint16_t(o + 2))) + oy));
    a.ret();
}
API(USER, 29, ScreenToClient, 6) {
    uint32_t p = a.p(); uint16_t h = a.w();
    int ox, oy; screen_origin(h, &ox, &oy);
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    c.wr16(s, o, uint16_t(int16_t(c.rd16(s, o)) - ox));
    c.wr16(s, uint16_t(o + 2), uint16_t(int16_t(c.rd16(s, uint16_t(o + 2))) - oy));
    a.ret();
}
API(USER, 30, WindowFromPoint, 4) {
    uint16_t x = a.w(); uint16_t y = a.w();   // POINT by value
    a.ret(window_from_point(int16_t(x), int16_t(y)));
}

// ---- rectangles ----
API(USER, 72, SetRect, 12) {
    int16_t b = a.i(); int16_t r = a.i(); int16_t t = a.i(); int16_t l = a.i(); uint32_t p = a.p();
    write_rect(c, p, Rect{l, t, r, b}); a.ret();
}
API(USER, 75, IsRectEmpty, 4) { a.ret(read_rect(c, a.p()).empty() ? 1 : 0); }
API(USER, 76, PtInRect, 8) {
    uint16_t x = a.w(); uint16_t y = a.w(); uint32_t p = a.p();   // POINT by value: x at the lower address
    Rect r = read_rect(c, p);
    a.ret(r.contains(int16_t(x), int16_t(y)) ? 1 : 0);
}
API(USER, 77, OffsetRect, 8) {
    int16_t dy = a.i(); int16_t dx = a.i(); uint32_t p = a.p();
    write_rect(c, p, read_rect(c, p).offset(dx, dy)); a.ret();
}
API(USER, 78, InflateRect, 8) {
    int16_t dy = a.i(); int16_t dx = a.i(); uint32_t p = a.p();
    Rect r = read_rect(c, p);
    write_rect(c, p, Rect{r.left - dx, r.top - dy, r.right + dx, r.bottom + dy}); a.ret();
}
API(USER, 81, FillRect, 8) {
    uint16_t br = a.w(); uint32_t p = a.p(); uint16_t hdc = a.w();
    if (DC* dc = dc_get(hdc)) draw_fill_rect(*dc, read_rect(c, p), br);
    a.ret(1);
}
API(USER, 83, FrameRect, 8) {
    uint16_t br = a.w(); uint32_t p = a.p(); uint16_t hdc = a.w();
    if (DC* dc = dc_get(hdc)) draw_frame_rect(*dc, read_rect(c, p), br);
    a.ret(1);
}
API(USER, 466, DrawFocusRect, 6) { a.p(); a.w(); a.ret(); }
API(USER, 85, DrawText, 14) {
    uint16_t fmt = a.w(); uint32_t pr = a.p(); int16_t n = a.i(); uint32_t str = a.p(); uint16_t hdc = a.w();
    DC* dc = dc_get(hdc);
    std::string s = n < 0 ? recomp::read_cstr(c, str) : std::string();
    if (n >= 0) for (int i = 0; i < n; ++i) s += char(c.rd8(uint16_t(str >> 16), uint16_t(uint16_t(str) + i)));
    Rect r = read_rect(c, pr);
    int th = dc ? text_height(dc->font) : 13;
    if (fmt & 0x400) {   // DT_CALCRECT
        int lines = 1, width = 0, cur = 0;
        std::string line;
        for (char ch : s) { if (ch == '\n') { ++lines; width = std::max(width, cur); cur = 0; } else cur += dc ? text_width(dc->font, std::string(1, ch)) : 7; }
        width = std::max(width, cur);
        if (fmt & 0x10) {   // DT_WORDBREAK: estimate wrapped lines
            int avail = std::max(1, r.w());
            lines = std::max(lines, (width + avail - 1) / avail);
            width = std::min(width, avail);
        }
        r.right = r.left + width;
        r.bottom = r.top + lines * th;
        write_rect(c, pr, r);
        a.ret(uint16_t(lines * th));
        return;
    }
    if (!dc) { a.ret(0); return; }
    // word wrap into lines
    std::vector<std::string> lines;
    if (fmt & 0x10) {
        std::string cur, word;
        auto flush_word = [&]() {
            std::string trial = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && text_width(dc->font, trial) > r.w()) { lines.push_back(cur); cur = word; }
            else cur = trial;
            word.clear();
        };
        for (char ch : s) {
            if (ch == ' ') flush_word();
            else if (ch == '\n') { flush_word(); lines.push_back(cur); cur.clear(); }
            else if (ch != '\r') word += ch;
        }
        flush_word();
        if (!cur.empty()) lines.push_back(cur);
    } else {
        lines.push_back(s);
    }
    int y = r.top;
    if (fmt & 0x20) {   // DT_SINGLELINE
        if (fmt & 0x04) y = r.top + (r.h() - th) / 2;   // DT_VCENTER
        else if (fmt & 0x08) y = r.bottom - th;         // DT_BOTTOM
    }
    for (const auto& ln : lines) {
        std::string t;
        for (size_t i = 0; i < ln.size(); ++i) {   // '&' marks an accelerator unless DT_NOPREFIX
            if (ln[i] == '&' && !(fmt & 0x800)) { if (i + 1 < ln.size() && ln[i + 1] == '&') t += '&', ++i; continue; }
            t += ln[i];
        }
        int tw = text_width(dc->font, t);
        int x = r.left;
        if (fmt & 0x01) x = r.left + (r.w() - tw) / 2;
        else if (fmt & 0x02) x = r.right - tw;
        int oldMode = dc->bkMode;
        draw_text(*dc, x, y, t, nullptr, 0);
        dc->bkMode = oldMode;
        y += th;
    }
    a.ret(uint16_t(y - r.top));
}

// ---- system information ----
API(USER, 179, GetSystemMetrics, 2) {
    int16_t i = a.i();
    int v = 0;
    switch (i) {
        case 0: v = 640; break; case 1: v = 480; break;          // CX/CYSCREEN
        case 2: case 3: v = 16; break;                           // scroll bar sizes
        case 4: v = 20; break;                                   // CYCAPTION
        case 5: case 6: v = 1; break;                            // borders
        case 7: case 8: v = 4; break;                            // dialog frame
        case 11: case 12: v = 32; break;                         // icon size
        case 13: case 14: v = 32; break;                         // cursor size
        case 15: v = 18; break;                                  // menu
        case 16: v = 640; break; case 17: v = 460; break;        // full screen client
        case 20: v = 16; break; case 21: v = 16; break;
        case 28: v = 100; break; case 29: v = 30; break;
        case 32: case 33: v = 4; break;                          // frame
        case 34: v = 100; break;
        case 36: case 37: v = 4; break;                          // double click
        default: v = 0; break;
    }
    a.ret(uint16_t(v));
}
API(USER, 180, GetSysColor, 2) {
    static const uint32_t cols[21] = {0xC0C0C0, 0x808000, 0x800000, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x000000,
                                      0x000000, 0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0xC0C0C0, 0x800000,
                                      0xFFFFFF, 0xC0C0C0, 0x808080, 0x808080, 0x000000, 0x000000, 0xFFFFFF};
    int16_t i = a.i();
    a.ret32((i >= 0 && i < 21) ? cols[i] : 0);   // already COLORREF (BGR) order
}
API(USER, 512, WNetGetConnection, 12) { a.p(); a.p(); a.p(); a.ret(1); }   // WN_NOT_SUPPORTED

// ---- cursors, icons, bitmaps, strings ----
API(USER, 173, LoadCursor, 6) {
    uint32_t name = a.p(); uint16_t inst = a.w();
    if (!inst) { a.ret(g_arrow); return; }   // system cursors: arrow for all
    Module* m = module_by_handle(inst);
    uint16_t h = m ? load_group(*m, read_reskey(c, name), true) : 0;
    a.ret(h ? h : g_arrow);
}
API(USER, 174, LoadIcon, 6) {
    uint32_t name = a.p(); uint16_t inst = a.w();
    if (!inst) { a.ret(g_arrow); return; }
    Module* m = module_by_handle(inst);
    uint16_t h = m ? load_group(*m, read_reskey(c, name), false) : 0;
    a.ret(h);
}
API(USER, 69, SetCursor, 2) { uint16_t old = g_cursor; g_cursor = a.w(); a.ret(old); }
API(USER, 247, GetCursor, 0) { a.ret(g_cursor); }
API(USER, 407, CreateIcon, 18) { a.p(); a.p(); a.w(); a.w(); a.w(); a.w(); a.w(); a.ret(add_cursor(arrow_cursor())); }
API(USER, 457, DestroyIcon, 2) { uint16_t h = a.w(); if (h != g_arrow) g_cursors.erase(h); a.ret(1); }
API(USER, 458, DestroyCursor, 2) { uint16_t h = a.w(); if (h != g_arrow) g_cursors.erase(h); a.ret(1); }
API(USER, 84, DrawIcon, 8) {
    uint16_t hi = a.w(); int16_t y = a.i(); int16_t x = a.i(); uint16_t hdc = a.w();
    DC* dc = dc_get(hdc);
    auto it = g_cursors.find(hi);
    if (dc && it != g_cursors.end()) {
        Surface* s = dc_surface(*dc);
        const Surface& img = it->second->image;
        for (int yy = 0; yy < img.h; ++yy)
            for (int xx = 0; xx < img.w; ++xx) {
                uint32_t p = img.get(xx, yy);
                int px = dc->originX + x + xx, py = dc->originY + y + yy;
                if ((p >> 24) && s && px >= 0 && py >= 0 && px < s->w && py < s->h) s->set(px, py, p & 0xFFFFFF);
            }
    }
    a.ret(1);
}
API(USER, 175, LoadBitmap, 6) {
    uint32_t name = a.p(); uint16_t inst = a.w();
    Module* m = module_by_handle(inst);
    const Resource* r = m ? find_resource(*m, read_reskey(c, name), ResKey{RT_BITMAP, ""}) : nullptr;
    if (!r) { a.ret(0); return; }
    const uint8_t* d = resource_data(*m, *r);
    auto u32 = [&](size_t p) { return uint32_t(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (uint32_t(d[p + 3]) << 24)); };
    int hs = int(u32(0)), w = int(u32(4)), h = int(u32(8)), bpp = d[14] | (d[15] << 8);
    int clr = int(u32(32));
    int ncol = clr ? clr : (bpp <= 8 ? 1 << bpp : 0);
    std::vector<Color> pal;
    for (int i = 0; i < ncol; ++i) pal.push_back((uint32_t(d[hs + 4 * i + 2]) << 16) | (uint32_t(d[hs + 4 * i + 1]) << 8) | d[hs + 4 * i]);
    const uint8_t* bits = d + hs + 4 * ncol;
    int stride = ((w * bpp + 31) / 32) * 4;
    auto s = std::make_shared<Surface>(w, std::abs(h), bpp == 1 ? 1 : 8);
    for (int y = 0; y < s->h; ++y) {
        const uint8_t* row = bits + size_t(s->h - 1 - y) * stride;
        for (int x = 0; x < w; ++x) {
            int idx = bpp == 8 ? row[x] : bpp == 4 ? ((x & 1) ? (row[x / 2] & 15) : (row[x / 2] >> 4))
                                                  : ((row[x / 8] >> (7 - x % 8)) & 1);
            Color col = size_t(idx) < pal.size() ? pal[size_t(idx)] : 0;
            if (bpp == 1) col = idx ? 0xFFFFFF : 0;
            s->set(x, y, col);
        }
    }
    a.ret(gdi_create_bitmap(s));
}
API(USER, 176, LoadString, 10) {
    uint16_t size = a.w(); uint32_t buf = a.p(); uint16_t id = a.w(); uint16_t inst = a.w();
    Module* m = module_by_handle(inst);
    std::string out;
    if (m) {
        const Resource* r = find_resource(*m, ResKey{id / 16 + 1, ""}, ResKey{RT_STRING, ""});
        if (r) {
            const uint8_t* d = resource_data(*m, *r);
            size_t p = 0;
            for (int i = 0; i < 16 && p < r->length; ++i) {
                int n = d[p];
                if (i == id % 16) { out.assign(reinterpret_cast<const char*>(d + p + 1), n); break; }
                p += 1 + n;
            }
        }
    }
    recomp::write_cstr(c, buf, out, size);
    a.ret(uint16_t(std::min<size_t>(out.size(), size ? size - 1 : 0)));
}

// ---- palettes (exported by USER in Windows 3.1) ----
API(USER, 282, SelectPalette, 6) {
    a.w(); uint16_t pal = a.w(); uint16_t hdc = a.w();
    DC* dc = dc_get(hdc);
    if (!dc) { a.ret(0); return; }
    uint16_t old = dc->palette;
    dc->palette = pal;
    a.ret(old);
}
API(USER, 283, RealizePalette, 2) {
    uint16_t hdc = a.w();
    DC* dc = dc_get(hdc);
    if (dc) {
        if (GdiObject* p = gdi_get(dc->palette)) {
            if (p->entries.size() > 20) {
                auto& sp = system_palette();
                for (size_t i = 0; i < p->entries.size() && i < 256; ++i) sp[i] = p->entries[i];
            }
        }
    }
    a.ret(0);
}

// ---- menus: the game has none; keep handles valid ----
API(USER, 151, CreateMenu, 0) { a.ret(0x7F00); }
API(USER, 415, CreatePopupMenu, 0) { a.ret(0x7F02); }
API(USER, 152, DestroyMenu, 2) { a.w(); a.ret(1); }
API(USER, 155, EnableMenuItem, 6) { a.w(); a.w(); a.w(); a.ret(0); }
API(USER, 156, GetSystemMenu, 4) { a.w(); a.w(); a.ret(0x7F04); }
API(USER, 157, GetMenu, 2) { a.w(); a.ret(0); }
API(USER, 158, SetMenu, 4) { a.w(); a.w(); a.ret(1); }
API(USER, 160, DrawMenuBar, 2) { a.w(); a.ret(); }
API(USER, 161, GetMenuString, 12) { a.w(); a.w(); a.p(); a.w(); a.w(); a.ret(0); }
API(USER, 250, GetMenuState, 6) { a.w(); a.w(); a.w(); a.ret(0xFFFF); }
API(USER, 263, GetMenuItemCount, 2) { a.w(); a.ret(0); }
API(USER, 410, InsertMenu, 12) { a.p(); a.w(); a.w(); a.w(); a.w(); a.ret(1); }
API(USER, 412, RemoveMenu, 6) { a.w(); a.w(); a.w(); a.ret(1); }
API(USER, 413, DeleteMenu, 6) { a.w(); a.w(); a.w(); a.ret(1); }
API(USER, 416, TrackPopupMenu, 16) { a.p(); a.w(); a.w(); a.w(); a.w(); a.w(); a.w(); a.ret(0); }
API(USER, 370, GetWindowPlacement, 6) {
    uint32_t p = a.p(); uint16_t h = a.w();
    uint16_t s = uint16_t(p >> 16), o = uint16_t(p);
    for (int i = 0; i < 22; ++i) c.wr8(s, uint16_t(o + i), 0);
    c.wr16(s, o, 22);
    c.wr16(s, uint16_t(o + 4), 1);
    if (Window* w = wnd(h)) write_rect(c, (uint32_t(s) << 16) | uint16_t(o + 14), w->rect);
    a.ret(1);
}
API(USER, 371, SetWindowPlacement, 6) { a.p(); a.w(); a.ret(1); }
API(USER, 292, UnhookWindowsHookEx, 4) { a.l(); a.ret(1); }

// ---- strings ----
API(USER, 432, AnsiLower, 4) {
    uint32_t p = a.p();
    if (p >> 16) {
        std::string s = recomp::read_cstr(c, p);
        for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        recomp::write_cstr(c, p, s, s.size() + 1);
    }
    a.ret32(p);
}
API(USER, 437, AnsiUpperBuff, 6) {
    uint16_t n = a.w(); uint32_t p = a.p();
    for (int i = 0; i < n; ++i) {
        uint16_t s = uint16_t(p >> 16), o = uint16_t(uint16_t(p) + i);
        uint8_t ch = c.rd8(s, o);
        if (ch >= 'a' && ch <= 'z') ch = uint8_t(ch - 32);
        else if (ch == 0xE5 || ch == 0xE4 || ch == 0xF6 || ch == 0xE9) ch = uint8_t(ch - 32);
        c.wr8(s, o, ch);
    }
    a.ret(n);
}
API(USER, 438, AnsiLowerBuff, 6) {
    uint16_t n = a.w(); uint32_t p = a.p();
    for (int i = 0; i < n; ++i) {
        uint16_t s = uint16_t(p >> 16), o = uint16_t(uint16_t(p) + i);
        uint8_t ch = c.rd8(s, o);
        if (ch >= 'A' && ch <= 'Z') ch = uint8_t(ch + 32);
        else if (ch == 0xC5 || ch == 0xC4 || ch == 0xD6 || ch == 0xC9) ch = uint8_t(ch + 32);
        c.wr8(s, o, ch);
    }
    a.ret(n);
}
API(USER, 471, lstrcmpi, 8) {
    std::string b = upper(recomp::read_cstr(c, a.p())); std::string s = upper(recomp::read_cstr(c, a.p()));
    a.ret(uint16_t(s < b ? -1 : s > b ? 1 : 0));
}
