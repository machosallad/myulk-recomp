// SDL front end: window, presenting the 640x480 screen, input translation.
#include <SDL3/SDL.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "recomp/enhance/dedither.hpp"
#include "recomp/enhance/farm.hpp"
#include "recomp/enhance/nav.hpp"
#include "recomp/enhance/quick.hpp"
#include "recomp/enhance/screenshot.hpp"
#include "recomp/enhance/settings.hpp"
#include "recomp/runtime/generated_support.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/win16.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// On-screen keyboard (web/recomp_shell.html): open/close it and collect the
// text typed there ("\b" backspace, "\r" enter), as UTF-8.
EM_JS(void, web_show_keyboard, (int on), {
    if (Module.showKeyboard) Module.showKeyboard(!!on);
});
EM_JS(char*, web_take_text, (), {
    const s = Module.textQueue || '';
    if (!s) return 0;
    Module.textQueue = '';
    return stringToNewUTF8(s);
});
#endif

namespace win16 {
namespace {

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_tex = nullptr;
SDL_Texture* g_cursorTex = nullptr;
const CursorImage* g_cursorFor = nullptr;
uint64_t g_lastPresent = 0;

int vk_from_sdl(SDL_Keycode k) {
    if (k >= SDLK_A && k <= SDLK_Z) return int('A' + (k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9) return int('0' + (k - SDLK_0));
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + int(k - SDLK_F1);
    switch (k) {
        case SDLK_RETURN: return 0x0D; case SDLK_ESCAPE: return 0x1B; case SDLK_SPACE: return 0x20;
        case SDLK_BACKSPACE: return 0x08; case SDLK_TAB: return 0x09; case SDLK_DELETE: return 0x2E;
        case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27;
        case SDLK_DOWN: return 0x28; case SDLK_HOME: return 0x24; case SDLK_END: return 0x23;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
        case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
        case SDLK_LALT: case SDLK_RALT: return 0x12;
        default: return 0;
    }
}

// ---- on-screen note (F7/F8 settings) -------------------------------------------
std::string g_toast;
uint64_t g_toastUntil = 0;
std::shared_ptr<Surface> g_toastSurf;
uint16_t g_toastDC = 0, g_toastFont = 0;

void show_toast(const std::string& text) {
    g_toast = text;
    g_toastUntil = SDL_GetTicks() + 1800;
}

void draw_hints(Surface& t);
bool hints_active();

// The screen with the note (and hotspot hints) drawn on top, into a copy so
// the game's picture is untouched.
const Surface& compose_toast(const Surface& s, bool toast, bool hints) {
    if (!g_toastSurf) {
        g_toastSurf = std::make_shared<Surface>(s.w, s.h, 32);
        g_toastDC = dc_create_memory();
        if (DC* dc = dc_get(g_toastDC)) dc->bitmap = gdi_create_bitmap(g_toastSurf);
        auto f = std::make_unique<GdiObject>();
        f->type = ObjType::Font; f->face = "Arial"; f->height = -16; f->weight = 700;
        g_toastFont = gdi_add(std::move(f));
    }
    Surface& t = *g_toastSurf;
    t.px = s.px;
    if (hints) draw_hints(t);
    if (!toast) return t;
    int w = text_width(g_toastFont, g_toast) + 28, h = 30;
    Rect r{(t.w - w) / 2, t.h - h - 18, (t.w + w) / 2, t.h - 18};
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x) {
            Color c = t.get(x, y);
            t.set(x, y, ((c >> 2) & 0x3F3F3F) + 0x101418);   // darkened backdrop
        }
    if (DC* dc = dc_get(g_toastDC)) {
        dc->font = g_toastFont;
        dc->textColor = 0xF0F0F0;
        dc->bkMode = 1;
        draw_text(*dc, r.left + 14, r.top + (h - text_height(g_toastFont)) / 2, g_toast, nullptr, 0);
    }
    return t;
}

// ---- hotspot hints (hold H; the web page's eye button) -----------------------
bool g_hintsHeld = false;
uint64_t g_hintsUntil = 0;

bool hints_active() {
    if (!g_hintsHeld && SDL_GetTicks() >= g_hintsUntil) return false;
    if (!settings::helper("hints")) return false;
    return active_window_class() == "TADVFORM";
}

void blend_rect(Surface& t, Rect r, Color c, unsigned alpha) {
    r = r.intersect(Rect{0, 0, t.w, t.h});
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x) {
            Color d = t.get(x, y);
            auto mix = [&](int sh) { return ((((c >> sh) & 255) * alpha + ((d >> sh) & 255) * (255 - alpha)) / 255) << sh; };
            t.set(x, y, mix(16) | mix(8) | mix(0));
        }
}

void outline(Surface& t, Rect r, Color c, int w) {
    blend_rect(t, Rect{r.left, r.top, r.right, r.top + w}, c, 255);
    blend_rect(t, Rect{r.left, r.bottom - w, r.right, r.bottom}, c, 255);
    blend_rect(t, Rect{r.left, r.top, r.left + w, r.bottom}, c, 255);
    blend_rect(t, Rect{r.right - w, r.top, r.right, r.bottom}, c, 255);
}

// filled arrow pointing dir (0 up, 1 right, 2 down, 3 left) centred at cx, cy
void arrow(Surface& t, int cx, int cy, int dir, Color c) {
    for (int i = 0; i < 12; ++i)
        for (int j = -i; j <= i; ++j) {
            int x = cx, y = cy;
            switch (dir) {
                case 0: x += j; y += i - 6; break;
                case 1: x += 6 - i; y += j; break;
                case 2: x += j; y += 6 - i; break;
                default: x += i - 6; y += j; break;
            }
            if (x >= 0 && y >= 0 && x < t.w && y < t.h) t.set(x, y, c);
        }
}

// Exit slots DS:4F3E + 4*i (forward, left, right, down, up, hotspots 0-9,
// back, extra 0-1); hotspot rectangles at TAdvForm+0x251 / +0x2A1; the
// direction zones are fixed (2:0B42). Scene picture at 95,72 on the screen.
void draw_hints(Surface& t) {
    auto& rt = recomp::Runtime::get();
    recomp::Cpu& c = rt.cpu();
    uint16_t ds = rt.data_sel();
    auto slot_set = [&](int i) { return c.rd16(ds, uint16_t(0x4F3E + 4 * i)) || c.rd16(ds, uint16_t(0x4F40 + 4 * i)); };
    const int ox = 95, oy = 72;
    static const Rect zones[5] = {{148, 76, 300, 172}, {0, 0, 53, 249}, {396, 0, 449, 249}, {54, 196, 395, 249}, {54, 0, 395, 53}};
    static const int dirs[5] = {0, 3, 1, 2, 0};
    for (int i = 0; i < 5; ++i) {
        if (!slot_set(i)) continue;
        Rect r = zones[i].offset(ox, oy);
        blend_rect(t, r, 0x40C0FF, 50);
        outline(t, r, 0x40C0FF, 2);
        arrow(t, (r.left + r.right) / 2, (r.top + r.bottom) / 2, dirs[i], 0xFFFFFF);
    }
    uint16_t fo = c.rd16(ds, 0x4C36), fs = c.rd16(ds, 0x4C38);   // TAdvForm
    if (!fs) return;
    auto rect_at = [&](uint16_t off) {
        uint16_t o = uint16_t(fo + off);
        return Rect{int16_t(c.rd16(fs, o)), int16_t(c.rd16(fs, uint16_t(o + 2))),
                    int16_t(c.rd16(fs, uint16_t(o + 4))), int16_t(c.rd16(fs, uint16_t(o + 6)))};
    };
    for (int i = 5; i < 18; ++i) {
        if (i == 15 || !slot_set(i)) continue;
        Rect r = rect_at(uint16_t(i < 15 ? 0x251 + 8 * (i - 5) : 0x2A1 + 8 * (i - 16)));
        if (r.left >= 1500 || r.empty()) continue;
        r = r.intersect(Rect{0, 0, 450, 250}).offset(ox, oy);
        blend_rect(t, r, 0xF0C040, 70);
        outline(t, r, 0xF0C040, 2);
    }
}

// F7: picture scaling, F8: text style (saved in the settings file)
void cycle_scaling() {
    int m = (settings::scaling() + 1) % settings::kScalingCount;
    settings::set_int("scaling", m);
    static const char* names[] = {"Bild: skarpa pixlar", "Bild: mjuk skalning", "Bild: utj\xE4mnad",
                                  "Bild: heltalsskalning"};
    show_toast(names[m]);
}
void toggle_text() {
    bool crisp = !settings::crisp_text();
    settings::set_int("crisp_text", crisp ? 1 : 0);
    screen_invalidate();   // redraw all text in the new style
    show_toast(crisp ? "Text: skarp (som originalet)" : "Text: mjuk");
}

void toggle_pause() {
    if (!settings::helper("farm_helper")) return;   // part of the farm helper
    if (!farm_open()) { show_toast("Paus finns p\xE5 g\xE5rden"); return; }
    set_farm_paused(!farm_paused());
    show_toast(farm_paused() ? "G\xE5rden pausad (P forts\xE4tter)" : "G\xE5rden forts\xE4tter");
}

// The settings menu (F2, or the web page's gear button) opens at the next
// host_pump, outside the event loop.
bool g_settingsRequested = false;
void open_settings() { g_settingsRequested = true; }
bool g_helpRequested = false;   // F1 or the settings menu: the handbook
std::string g_dropped;          // a file dropped on the window (first-start screen)
bool g_systemCursor = false;    // the system arrow instead of the game's cursor

Surface shot_surface(const Surface& shown);

// F12: the picture as shown (de-dithered when that is on), without notes
void take_screenshot() {
    std::string where = save_screenshot(screen().hd ? shot_surface(screen())
                                     : settings::dedither() ? dedither(screen()) : screen());
    if (where.empty()) { show_toast("Sk\xE4rmbilden kunde inte sparas"); return; }
    recomp::log("screenshot: %s", where.c_str());
    size_t slash = where.find_last_of("\\/");
    show_toast("Sk\xE4rmbild: " + (slash == std::string::npos ? where : where.substr(slash + 1)));
}

// Window size and position are saved a second after they last changed
// (not in fullscreen, and not in the browser, where the page sizes the canvas).
uint64_t g_windowChanged = 0;
void remember_window(uint64_t now) {
#ifndef __EMSCRIPTEN__
    if (!g_windowChanged || now - g_windowChanged < 1000) return;
    g_windowChanged = 0;
    static const bool test = SDL_getenv("MYULK_WINDOW") != nullptr;
    SDL_WindowFlags f = SDL_GetWindowFlags(g_window);
    if (test || (f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) return;
    bool max = f & SDL_WINDOW_MAXIMIZED;
    if (settings::get_int("window_max", 0) != int(max)) settings::set_int("window_max", max ? 1 : 0);
    if (max) return;   // keep the size to restore to
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    if (settings::get_int("window_x", INT_MIN) != x) settings::set_int("window_x", x);
    if (settings::get_int("window_y", INT_MIN) != y) settings::set_int("window_y", y);
    if (settings::get_int("window_w", 0) != w) settings::set_int("window_w", w);
    if (settings::get_int("window_h", 0) != h) settings::set_int("window_h", h);
#else
    (void)now;
#endif
}

SDL_Texture* g_mid = nullptr;   // integer-scaled copy for "mjuk skalning"
int g_midScale = 0;
SDL_Texture* g_texHd = nullptr;   // texture packs: the screen's HD layer

// Texture packs: the screen's HD layer, up to date, with whatever `shown`
// adds to the screen (notes, hints) enlarged into it. Null without a pack.
const HdLayer* screen_hd(const Surface& shown) {
    Surface& scr = screen();
    if (!scr.hd) return nullptr;
    hd_sync(scr);
    const HdLayer& L = *scr.hd;
    if (&shown == &scr) return &L;
    static HdLayer out;
    out.k = L.k;
    out.px = L.px;
    const int k = L.k, W = scr.w * k;
    for (int y = 0; y < scr.h; ++y)
        for (int x = 0; x < scr.w; ++x) {
            Color c = shown.get(x, y);
            if (c == scr.get(x, y)) continue;
            for (int v = 0; v < k; ++v) std::fill_n(&out.px[size_t(y * k + v) * W + size_t(x) * k], k, c);
        }
    return &out;
}

// What a screenshot saves: the HD picture when a texture pack is in use.
Surface shot_surface(const Surface& shown) {
    const HdLayer* hd = screen_hd(shown);
    if (!hd) return shown;
    Surface s(shown.w * hd->k, shown.h * hd->k, 8);
    s.px = hd->px;
    return s;
}

void present() {
    bool toast = SDL_GetTicks() < g_toastUntil, hints = hints_active();
    // de-dithering is for the original art; a texture pack replaces it
    const bool hdOn = screen().hd != nullptr;
    const Surface& base = settings::dedither() && !hdOn ? dedither(screen()) : screen();
    const Surface& shown = toast || hints ? compose_toast(base, toast, hints) : base;
    SDL_Texture* tex = g_tex;
    int texW = shown.w, texH = shown.h;
    if (const HdLayer* hd = screen_hd(shown)) {
        texW = shown.w * hd->k;
        texH = shown.h * hd->k;
        if (!g_texHd)
            g_texHd = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, texW, texH);
        if (g_texHd) {
            tex = g_texHd;
            SDL_UpdateTexture(tex, nullptr, hd->px.data(), texW * 4);
        }
    }
    if (tex == g_tex) {
        texW = shown.w;
        texH = shown.h;
        SDL_UpdateTexture(g_tex, nullptr, shown.px.data(), shown.w * 4);
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    int mode = settings::scaling();
    // pure integer scaling: only whole multiples, a border around the rest
    static SDL_RendererLogicalPresentation presentation = SDL_LOGICAL_PRESENTATION_LETTERBOX;
    SDL_RendererLogicalPresentation want =
        mode == settings::kInteger ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    if (want != presentation) {
        SDL_SetRenderLogicalPresentation(g_renderer, 640, 480, want);
        presentation = want;
    }
    if (tex == g_texHd) {
        // an HD picture is enlarged or reduced smoothly in every mode
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        SDL_RenderTexture(g_renderer, tex, nullptr, nullptr);
    } else if (mode == settings::kSharpSmooth) {
        // "sharp bilinear": enlarge by the largest whole factor with sharp pixels,
        // then smoothly to the window, so pixels stay crisp without uneven sizes
        int ow = 0, oh = 0;
        SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
        int k = std::max(1, std::min(ow / texW, oh / texH));
        if (k != g_midScale || !g_mid) {
            if (g_mid) SDL_DestroyTexture(g_mid);
            g_mid = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET, texW * k, texH * k);
            SDL_SetTextureScaleMode(g_mid, SDL_SCALEMODE_LINEAR);
            g_midScale = k;
        }
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        if (g_mid && SDL_SetRenderTarget(g_renderer, g_mid)) {
            SDL_RenderTexture(g_renderer, tex, nullptr, nullptr);
            SDL_SetRenderTarget(g_renderer, nullptr);
            SDL_RenderTexture(g_renderer, g_mid, nullptr, nullptr);
        } else {
            SDL_RenderTexture(g_renderer, tex, nullptr, nullptr);
        }
    } else {
        SDL_SetTextureScaleMode(tex, mode == settings::kSmooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_RenderTexture(g_renderer, tex, nullptr, nullptr);
    }
    if (const CursorImage* cur = g_systemCursor ? nullptr : current_cursor()) {
        if (cur != g_cursorFor) {
            if (g_cursorTex) SDL_DestroyTexture(g_cursorTex);
            g_cursorTex = nullptr;
            if (cur->image.w > 0) {
                g_cursorTex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                                cur->image.w, cur->image.h);
                SDL_UpdateTexture(g_cursorTex, nullptr, cur->image.px.data(), cur->image.w * 4);
                SDL_SetTextureBlendMode(g_cursorTex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(g_cursorTex, SDL_SCALEMODE_NEAREST);
            }
            g_cursorFor = cur;
        }
        if (g_cursorTex) {
            int x, y;
            cursor_position(&x, &y);
            SDL_FRect dst{float(x - cur->hotX), float(y - cur->hotY), float(cur->image.w), float(cur->image.h)};
            SDL_RenderTexture(g_renderer, g_cursorTex, nullptr, &dst);
        }
    }
    SDL_RenderPresent(g_renderer);
}

// Test automation through environment variables:
//   MYULK_CLICKS="ms:x,y;ms:x,y"  left clicks at 640x480 coordinates
//   MYULK_SHOT=out.bmp MYULK_SHOT_AT=ms   save the screen and exit
// MYULK_ROUTE="seg:off,seg:off,...": follow a route through adventure scenes.
// For the next routine on the route, find the exit slot (DS:4F3E + 4*i) that
// holds it and click that slot's zone; a routine counts as reached once the
// trace ring shows it was entered. Steps reached by timers need no click.
struct RouteState {
    std::vector<std::pair<int, uint16_t>> steps;
    std::vector<std::string> items;   // per step: inventory item to use ("seg:off@ITEM")
    std::vector<bool> warp;           // per step: "!seg:off" = put it in the forward slot (debug warp)
    std::vector<int> talk;            // per step: "#N:seg:off" = click dialogue line N (1-4)
    std::vector<bool> game;           // per step: "*seg:off" = play the TMain click game until reached
    size_t next = 0;
    uint64_t lastClick = 0, waitingSince = 0;
    uint32_t tracePos = 0;
    bool reported = false, init = false;
};
RouteState g_route;

bool entered_since(uint32_t fromPos, int seg, uint16_t off) {
    uint32_t want = (uint32_t(seg) << 16) | off;
    for (uint32_t p = fromPos; p != recomp::g_trace.pos; p = (p + 1) & (recomp::TraceRing::kSize - 1))
        if (recomp::g_trace.entries[p] == want) return true;
    return false;
}

void route_step(uint64_t now) {
    RouteState& r = g_route;
    if (!r.init) {
        r.init = true;
        if (const char* s = SDL_getenv("MYULK_ROUTE")) {
            std::string str = s;
            size_t p = 0;
            while (p < str.size()) {
                size_t comma = str.find(',', p);
                std::string item = str.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
                int seg = 0;
                unsigned off = 0;
                bool warp = !item.empty() && item[0] == '!';
                bool game = !item.empty() && item[0] == '*';
                int talk = 0;
                size_t skip = (warp || game) ? 1 : 0;
                if (!item.empty() && item[0] == '#') {
                    talk = std::atoi(item.c_str() + 1);
                    skip = item.find(':') + 1;
                }
                if (std::sscanf(item.c_str() + skip, "%d:%x", &seg, &off) == 2) {
                    r.steps.push_back({seg, uint16_t(off)});
                    r.warp.push_back(warp);
                    r.talk.push_back(talk);
                    r.game.push_back(game);
                    size_t at = item.find('@');
                    r.items.push_back(at == std::string::npos ? std::string() : item.substr(at + 1));
                }
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
            r.tracePos = recomp::g_trace.pos;
        }
    }
    if (r.next >= r.steps.size()) return;
    // furthest of the next few steps that has run (timer chains advance on their own)
    size_t hit = r.steps.size();
    for (size_t k = std::min(r.steps.size(), r.next + 8); k-- > r.next;)
        if (entered_since(r.tracePos, r.steps[k].first, r.steps[k].second)) { hit = k; break; }
    if (hit < r.steps.size()) {
        recomp::log("route: reached %d:%04X (%zu/%zu)", r.steps[hit].first, r.steps[hit].second, hit + 1, r.steps.size());
        r.tracePos = recomp::g_trace.pos;
        r.next = hit + 1;
        r.waitingSince = now;
        r.reported = false;
        return;
    }
    if (r.next >= r.steps.size()) return;
    auto [seg, off] = r.steps[r.next];
    if (!r.waitingSince) r.waitingSince = now;
    recomp::Cpu& c = recomp::Runtime::get().cpu();
    uint16_t ds = recomp::Runtime::get().data_sel();
    uint16_t sel = uint16_t((seg << 3) | 7);
    if (!r.game[r.next] && (c.rd8(ds, 0x53C3) != 0 || now - r.lastClick < 800)) return;   // scene busy / just clicked
    if (r.game[r.next]) {
        // TMain (DS:57D4): target rectangle at Self + idx + 0x343 (left, top, right, bottom),
        // idx as computed by the hit test 5:0EB3
        if (now - r.lastClick < 700) return;
        uint16_t mo = c.rd16(ds, 0x57D4), ms = c.rd16(ds, 0x57D6);
        if (!ms) return;
        auto w = [&](int o) { return int(int16_t(c.rd16(ms, uint16_t(mo + o)))); };
        int idx = w(0x1A6) * 8 + w(0x1A0) * 0x205 + w(0x1A2) * 0x60F + w(0x1A4) * 0x1E4B;
        int l = w(idx + 0x343), t = w(idx + 0x345), rr = w(idx + 0x347), b = w(idx + 0x349);
        if (rr <= l || b <= t || l < 0 || rr > 450 || b > 250) return;
        int x = 95 + (l + rr) / 2, y = 72 + (t + b) / 2;
        recomp::log("route: click game target (%d,%d)-(%d,%d) at %d,%d", l, t, rr, b, x, y);
        input_mouse_move(x, y);
        input_mouse_button(x, y, 0, true);
        input_mouse_button(x, y, 0, false);
        r.lastClick = now;
        return;
    }
    if (int line = r.talk[r.next]) {   // dialogue choice: TalkLabel1-4 below the scene
        static const int top[4] = {2, 36, 70, 104};
        int x = 150, y = 329 + top[std::clamp(line, 1, 4) - 1] + 7;
        recomp::log("route: dialogue line %d -> %d:%04X, click %d,%d", line, seg, off, x, y);
        input_mouse_move(x, y);
        input_mouse_button(x, y, 0, true);
        input_mouse_button(x, y, 0, false);
        r.lastClick = now;
        return;
    }
    if (r.warp[r.next]) {
        c.wr16(ds, 0x4F3E, off);
        c.wr16(ds, 0x4F40, sel);
        recomp::log("route: warp %d:%04X via the forward slot", seg, off);
        r.warp[r.next] = false;
    }
    int slot = -1;
    for (int i = 0; i < 26 && slot < 0; ++i)
        if (c.rd16(ds, uint16_t(0x4F3E + 4 * i)) == off && c.rd16(ds, uint16_t(0x4F40 + 4 * i)) == sel) slot = i;
    if (slot < 0) {
        // a later step already clickable: we are past this one (or the trace ring wrapped)
        for (size_t k = r.next + 1; k < r.steps.size() && k <= r.next + 3; ++k) {
            uint16_t ksel = uint16_t((r.steps[k].first << 3) | 7);
            for (int i = 0; i < 18; ++i)
                if (c.rd16(ds, uint16_t(0x4F3E + 4 * i)) == r.steps[k].second && c.rd16(ds, uint16_t(0x4F40 + 4 * i)) == ksel) {
                    recomp::log("route: skipping to %d:%04X", r.steps[k].first, r.steps[k].second);
                    r.next = k;
                    r.waitingSince = now;
                    return;
                }
        }
        if (!r.reported && now - r.waitingSince > 15000) {
            r.reported = true;
            std::string slots;
            for (int i = 0; i < 18; ++i) {
                uint16_t o = c.rd16(ds, uint16_t(0x4F3E + 4 * i)), s = c.rd16(ds, uint16_t(0x4F40 + 4 * i));
                if (o || s) {
                    char b[32];
                    std::snprintf(b, sizeof b, " [%d]=%d:%04X", i, s >> 3, o);
                    slots += b;
                }
            }
            recomp::log("route: waiting for %d:%04X; slots:%s", seg, off, slots.c_str());
        }
        return;
    }
    static const int zone[5][2] = {{224, 124}, {26, 124}, {422, 124}, {224, 222}, {224, 26}};
    int x = 0, y = 0;
    if (slot < 5) { x = zone[slot][0]; y = zone[slot][1]; }
    else if (slot == 15) { x = 70; y = 70; }   // "anywhere else"
    else {
        uint16_t fo = c.rd16(ds, 0x4C36), fs = c.rd16(ds, 0x4C38);   // AdvForm instance
        if (slot >= 18) {
            // item use: hold the named inventory item (AdvForm+0x309, item mode +0x224)
            const std::string& want = r.items[r.next];
            uint16_t io = c.rd16(fs, uint16_t(fo + 0x301)), is = c.rd16(fs, uint16_t(fo + 0x303));
            bool found = want.empty();
            for (int k = 0; k < 24 && !found && is; ++k) {
                uint16_t eo = c.rd16(is, uint16_t(io + 4 * k)), es = c.rd16(is, uint16_t(io + 4 * k + 2));
                if (!es) continue;
                std::string name;
                int len = c.rd8(es, uint16_t(eo + 4));
                for (int j = 0; j < len && j < 8; ++j) name += char(c.rd8(es, uint16_t(eo + 5 + j)));
                if (name == want) {
                    c.wr16(fs, uint16_t(fo + 0x309), eo);
                    c.wr16(fs, uint16_t(fo + 0x30B), es);
                    found = true;
                }
            }
            if (!found) {
                if (!r.reported) recomp::log("route: item %s not in the inventory", want.c_str());
                r.reported = true;
                return;
            }
            c.wr8(fs, uint16_t(fo + 0x224), 1);
        }
        uint16_t ro = uint16_t(fo + (slot < 15 ? 0x251 + 8 * (slot - 5) :
                                     slot < 18 ? 0x2A1 + 8 * (slot - 16) : 0x2B1 + 8 * (slot - 18)));
        int l = int16_t(c.rd16(fs, ro)), t = int16_t(c.rd16(fs, uint16_t(ro + 2)));
        int rr = int16_t(c.rd16(fs, uint16_t(ro + 4))), b = int16_t(c.rd16(fs, uint16_t(ro + 6)));
        x = (l + rr) / 2;
        y = (t + b) / 2;
    }
    x += 95;   // scene picture (GPanel) position on the screen
    y += 72;
    recomp::log("route: slot %d -> %d:%04X, click %d,%d", slot, seg, off, x, y);
    input_mouse_move(x, y);
    input_mouse_button(x, y, 0, true);
    input_mouse_button(x, y, 0, false);
    r.lastClick = now;
}

// A mouse button from the player (or MYULK_CLICKS): the shortcuts get a look
// first. Right-click in the scene goes back, a click on the farm's day clock
// gives the next day; the matching release is swallowed too.
void mouse_button(int x, int y, int b, bool down) {
    nav::mouse_at(x, y);
    static bool rightTaken = false, leftTaken = false;
    if (b == 1 && down && nav::right_click(x, y)) { rightTaken = true; return; }
    if (b == 1 && !down && rightTaken) { rightTaken = false; return; }
    if (b == 0 && down && farm::click(x, y)) { leftTaken = true; return; }
    if (b == 0 && !down && leftTaken) { leftTaken = false; return; }
    input_mouse_button(x, y, b, down);
}

struct Click { uint64_t at; int x, y; bool done; };
std::vector<Click> g_clicks;
bool g_autoInit = false;

void automation(uint64_t now) {
    if (!g_autoInit) {
        g_autoInit = true;
        if (const char* cl = SDL_getenv("MYULK_CLICKS")) {
            std::string s = cl;
            size_t p = 0;
            while (p < s.size()) {
                size_t semi = s.find(';', p);
                std::string item = s.substr(p, semi == std::string::npos ? std::string::npos : semi - p);
                int ms = 0, x = 0, y = 0;
                if (std::sscanf(item.c_str(), "%d:%d,%d", &ms, &x, &y) == 3) g_clicks.push_back({uint64_t(ms), x, y, false});
                if (semi == std::string::npos) break;
                p = semi + 1;
            }
        }
    }
    // MYULK_CLICK_HOLD=ms: release the button that much later (like a finger tap)
    static const int hold = SDL_getenv("MYULK_CLICK_HOLD") ? std::atoi(SDL_getenv("MYULK_CLICK_HOLD")) : 0;
    static std::vector<Click> releases;
    for (auto& c : g_clicks) {
        if (!c.done && now >= c.at) {
            c.done = true;
            input_mouse_move(c.x, c.y);
            mouse_button(c.x, c.y, 0, true);
            if (hold > 0) releases.push_back({now + uint64_t(hold), c.x, c.y, false});
            else mouse_button(c.x, c.y, 0, false);
        }
    }
    for (auto& rl : releases)
        if (!rl.done && now >= rl.at) { rl.done = true; mouse_button(rl.x, rl.y, 0, false); }
    // MYULK_KEYS="ms:text;ms:text": typed characters; {enter} {esc} {bs} {tab}
    struct Keys { uint64_t at; std::string text; bool done; };
    static std::vector<Keys> keys;
    static bool keysInit = false;
    if (!keysInit) {
        keysInit = true;
        if (const char* kl = SDL_getenv("MYULK_KEYS")) {
            std::string s = kl;
            size_t p = 0;
            while (p < s.size()) {
                size_t semi = s.find(';', p);
                std::string item = s.substr(p, semi == std::string::npos ? std::string::npos : semi - p);
                size_t colon = item.find(':');
                if (colon != std::string::npos) keys.push_back({uint64_t(std::atoi(item.c_str())), item.substr(colon + 1), false});
                if (semi == std::string::npos) break;
                p = semi + 1;
            }
        }
    }
    for (auto& k : keys) {
        if (k.done || now < k.at) continue;
        k.done = true;
        for (size_t i = 0; i < k.text.size(); ++i) {
            if (k.text.compare(i, 4, "{f7}") == 0) { cycle_scaling(); i += 3; continue; }
            if (k.text.compare(i, 4, "{f8}") == 0) { toggle_text(); i += 3; continue; }
            if (k.text.compare(i, 4, "{f5}") == 0) { quick::request_save(); i += 3; continue; }
            if (k.text.compare(i, 4, "{f9}") == 0) { quick::request_load(); i += 3; continue; }
            if (k.text.compare(i, 4, "{f2}") == 0) { open_settings(); i += 3; continue; }
            if (k.text.compare(i, 3, "{p}") == 0) { toggle_pause(); i += 2; continue; }
            if (k.text.compare(i, 5, "{f12}") == 0) { take_screenshot(); i += 4; continue; }
            if (k.text.compare(i, 4, "{f1}") == 0) { g_helpRequested = true; i += 3; continue; }
            if (k.text.compare(i, 7, "{nextd}") == 0) { recomp::log("farm next day: %d", farm::next_day()); i += 6; continue; }
            // navigation shortcuts: {up} {down} {left} {right} {back} {rclick} {wheelup} {wheeldown}
            if (k.text.compare(i, 4, "{up}") == 0) { recomp::log("nav up: %d", nav::key(SDLK_UP)); i += 3; continue; }
            if (k.text.compare(i, 6, "{down}") == 0) { recomp::log("nav down: %d", nav::key(SDLK_DOWN)); i += 5; continue; }
            if (k.text.compare(i, 6, "{left}") == 0) { recomp::log("nav left: %d", nav::key(SDLK_LEFT)); i += 5; continue; }
            if (k.text.compare(i, 7, "{right}") == 0) { recomp::log("nav right: %d", nav::key(SDLK_RIGHT)); i += 6; continue; }
            if (k.text.compare(i, 6, "{back}") == 0) { recomp::log("nav back: %d", nav::key(SDLK_BACKSPACE)); i += 5; continue; }
            if (k.text.compare(i, 8, "{rclick}") == 0) { recomp::log("nav rclick: %d", nav::right_click(320, 200)); i += 7; continue; }
            if (k.text.compare(i, 9, "{wheelup}") == 0) { recomp::log("nav wheel up: %d", nav::wheel(1)); i += 8; continue; }
            if (k.text.compare(i, 11, "{wheeldown}") == 0) { recomp::log("nav wheel down: %d", nav::wheel(-1)); i += 10; continue; }
            if (k.text.compare(i, 7, "{hints}") == 0) { g_hintsUntil = now + 3000; i += 6; continue; }
            static const std::pair<const char*, int> named[] = {{"{enter}", 0x0D}, {"{esc}", 0x1B}, {"{bs}", 0x08}, {"{tab}", 0x09}};
            bool special = false;
            for (auto& [name, vk] : named) {
                size_t n = std::strlen(name);
                if (k.text.compare(i, n, name) == 0) {
                    input_key(vk, true);
                    input_char(vk);
                    input_key(vk, false);
                    i += n - 1;
                    special = true;
                    break;
                }
            }
            if (!special) input_char(static_cast<unsigned char>(k.text[i]));
        }
    }
    // MYULK_POKE="ms:OFF=VAL;OFF=VAL|ms:...": write data-segment bytes (more than two hex
    // digits: a word) at the given times (debug: set game flags, warp via exit slots)
    static std::vector<std::pair<uint64_t, std::string>> pokes;
    static bool pokesInit = false;
    if (!pokesInit) {
        pokesInit = true;
        if (const char* pk = SDL_getenv("MYULK_POKE")) {
            std::string all = pk;
            size_t p = 0;
            while (p <= all.size()) {
                size_t bar = all.find('|', p);
                std::string g = all.substr(p, bar == std::string::npos ? std::string::npos : bar - p);
                if (!g.empty()) pokes.push_back({uint64_t(std::atoi(g.c_str())), g});
                if (bar == std::string::npos) break;
                p = bar + 1;
            }
        }
    }
    for (auto& pg : pokes) {
        {
            if (!pg.second.empty() && now >= pg.first) {
                std::string s = pg.second;
                pg.second.clear();
                size_t p = s.find(':');
                while (p != std::string::npos && p + 1 < s.size()) {
                    unsigned off = 0, val = 0;
                    int n0 = 0, n1 = 0;
                    if (std::sscanf(s.c_str() + p + 1, "%x=%n%x%n", &off, &n0, &val, &n1) == 2) {
                        auto& cpu = recomp::Runtime::get().cpu();
                        uint16_t ds = recomp::Runtime::get().data_sel();
                        if (n1 - n0 > 2) cpu.wr16(ds, uint16_t(off), uint16_t(val));   // more than 2 digits: word
                        else cpu.wr8(ds, uint16_t(off), uint8_t(val));
                        recomp::log("poke DS:%04X = %X", off, val);
                    }
                    p = s.find(';', p + 1);
                }
            }
        }
    }
    route_step(now);
    // MYULK_CALL="ms:seg:off": call a parameterless routine of the game once
    // (debugging shortcut, e.g. 7:2B03 = end of chapter 1 -> farm)
    static bool called = false;
    static bool inCall = false;
    if (!called && !inCall) {
        if (const char* cl = SDL_getenv("MYULK_CALL")) {
            int ms = 0;
            unsigned seg = 0, off = 0;
            if (std::sscanf(cl, "%d:%u:%x", &ms, &seg, &off) == 3 && now >= uint64_t(ms)) {
                called = true;
                inCall = true;
                recomp::log("debug: calling %u:%04X", seg, off);
                recomp::Cpu& cpu = recomp::Runtime::get().cpu();
                {   // Delphi exception frame chain (head at DGROUP:3500)
                    uint16_t ds = recomp::Runtime::get().data_sel();
                    std::string chain;
                    uint16_t f = cpu.rd16(ds, 0x3500);
                    for (int i = 0; i < 12 && f; ++i) {
                        char b[48];
                        std::snprintf(b, sizeof b, " %04X(tab %04X:%04X)", f, cpu.rd16(cpu.ss, uint16_t(f + 6)),
                                      cpu.rd16(cpu.ss, uint16_t(f + 4)));
                        chain += b;
                        f = cpu.rd16(cpu.ss, f);
                    }
                    recomp::log("debug: sp=%04X frames:%s", cpu.sp(), chain.c_str());
                }
                recomp::Runtime::get().cpu().ds = recomp::Runtime::get().data_sel();
                recomp::Runtime::get().call(uint16_t((seg << 3) | 7), uint16_t(off));
                inCall = false;
            }
        }
    }
    // MYULK_SHOT_AT="t1,t2,...": a screenshot at each time (out.bmp, out_1.bmp, ...);
    // the program exits after the last one.
    const char* shot = SDL_getenv("MYULK_SHOT");
    const char* at = SDL_getenv("MYULK_SHOT_AT");
    static std::vector<uint64_t> shotTimes;
    static size_t shotNext = 0;
    static bool shotInit = false;
    if (shot && at && !shotInit) {
        shotInit = true;
        for (const char* p = at; *p;) {
            shotTimes.push_back(uint64_t(std::atoi(p)));
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
    }
    if (shot && shotNext < shotTimes.size() && now >= shotTimes[shotNext]) {
        std::string path = shot;
        if (shotNext > 0) {
            size_t dot = path.rfind('.');
            std::string suffix = "_" + std::to_string(shotNext);
            path = dot == std::string::npos ? path + suffix : path.substr(0, dot) + suffix + path.substr(dot);
        }
        Surface s = shot_surface(screen());   // HD with a texture pack
        SDL_Surface* surf = nullptr;
        if (SDL_getenv("MYULK_SHOT_WINDOW")) {   // what the window shows (scaling, notes)
            present();
            int lw, lh;
            SDL_RendererLogicalPresentation mode;
            SDL_GetRenderLogicalPresentation(g_renderer, &lw, &lh, &mode);
            SDL_SetRenderLogicalPresentation(g_renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
            surf = SDL_RenderReadPixels(g_renderer, nullptr);
            SDL_SetRenderLogicalPresentation(g_renderer, lw, lh, mode);
        } else {
            surf = SDL_CreateSurfaceFrom(s.w, s.h, SDL_PIXELFORMAT_XRGB8888, s.px.data(), s.w * 4);
        }
        bool ok = surf && SDL_SaveBMP(surf, path.c_str());
        if (surf) SDL_DestroySurface(surf);
        recomp::log("screenshot %s: %s", path.c_str(), ok ? "saved" : SDL_GetError());
        if (SDL_getenv("MYULK_DUMP_WINDOWS")) dump_windows();
        if (++shotNext == shotTimes.size()) throw ExitProgram{0};
    }
}

}  // namespace

bool display_init() {
    if (g_window) return true;   // already open (the first-start screen)
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        recomp::log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    // the window opens as the player left it (desktop); MYULK_WINDOW=WxH for tests
    int ww = settings::get_int("window_w", 1280), wh = settings::get_int("window_h", 960);
    const char* testSize = SDL_getenv("MYULK_WINDOW");
    if (testSize) std::sscanf(testSize, "%dx%d", &ww, &wh);
    ww = std::max(ww, 320);
    wh = std::max(wh, 240);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
#ifndef __EMSCRIPTEN__
    if (!testSize && settings::get_int("window_max", 0)) flags |= SDL_WINDOW_MAXIMIZED;
#endif
    if (!SDL_CreateWindowAndRenderer("Myulk", ww, wh, flags, &g_window, &g_renderer)) {
        recomp::log("window: %s", SDL_GetError());
        return false;
    }
#ifndef __EMSCRIPTEN__
    if (!testSize) {
        int wx = settings::get_int("window_x", INT_MIN), wy = settings::get_int("window_y", INT_MIN);
        if (wx != INT_MIN && wy != INT_MIN) {
            // only where a display still shows the title bar
            SDL_Rect r{wx, wy + 8, 64, 8};
            if (SDL_GetDisplayForRect(&r)) {
                SDL_Rect b;
                if (SDL_GetDisplayBounds(SDL_GetDisplayForRect(&r), &b) && SDL_HasRectIntersection(&r, &b))
                    SDL_SetWindowPosition(g_window, wx, wy);
            }
        }
        if (settings::get_int("fullscreen", 0)) SDL_SetWindowFullscreen(g_window, true);
    }
#endif
    SDL_SetRenderLogicalPresentation(g_renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(g_renderer, 1);
    g_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);
    SDL_HideCursor();
    SDL_StartTextInput(g_window);
    return true;
}

void host_text_input(bool active) {
#ifdef __EMSCRIPTEN__
    web_show_keyboard(active ? 1 : 0);
#else
    (void)active;
#endif
}

void display_shutdown() {
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_renderer = nullptr;
    g_window = nullptr;
}

void mm_poll();

void host_pump(int blockMs) {
    if (!g_renderer) return;
    mm_poll();
    SDL_Event ev;
#ifdef __EMSCRIPTEN__
    // never block the browser thread: yield (JSPI) and then poll
    emscripten_sleep(blockMs > 0 ? unsigned(blockMs) : 0);
    bool have = SDL_PollEvent(&ev);
#else
    bool have = blockMs > 0 ? SDL_WaitEventTimeout(&ev, blockMs) : SDL_PollEvent(&ev);
#endif
    // scripted runs ignore the real mouse so it cannot disturb them
    static const bool scripted = SDL_getenv("MYULK_ROUTE") || SDL_getenv("MYULK_CLICKS");
    while (have) {
        SDL_ConvertEventToRenderCoordinates(g_renderer, &ev);
        if (scripted && (ev.type == SDL_EVENT_MOUSE_MOTION || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                         ev.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            have = SDL_PollEvent(&ev);
            continue;
        }
        switch (ev.type) {
            case SDL_EVENT_QUIT: input_quit(); break;
            case SDL_EVENT_MOUSE_MOTION:
                nav::mouse_at(int(ev.motion.x), int(ev.motion.y));
                input_mouse_move(int(ev.motion.x), int(ev.motion.y));
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int b = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                mouse_button(int(ev.button.x), int(ev.button.y), b, down);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                float dy = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y;
                if (!input_wheel(dy)) nav::wheel(dy);
                break;
            }
            case SDL_EVENT_DROP_FILE:
                if (ev.drop.data) g_dropped = ev.drop.data;
                break;
            case SDL_EVENT_WINDOW_MOVED:
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_MAXIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                g_windowChanged = SDL_GetTicks();
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_F11) {
                    host_set_fullscreen(!host_fullscreen());
                    break;
                }
                if (ev.key.key == SDLK_F12) { if (!ev.key.repeat) take_screenshot(); break; }
                if (ev.key.key == SDLK_F1 && !input_overlay_active()) { if (!ev.key.repeat) g_helpRequested = true; break; }
                if (nav::key(ev.key.key)) break;
                if (ev.key.key == SDLK_N && farm::next_day()) break;
                if (ev.key.key == SDLK_F7) { if (!ev.key.repeat) cycle_scaling(); break; }
                if (ev.key.key == SDLK_F8) { if (!ev.key.repeat) toggle_text(); break; }
                if (ev.key.key == SDLK_F5) { if (!ev.key.repeat) quick::request_save(); break; }
                if (ev.key.key == SDLK_F9) { if (!ev.key.repeat) quick::request_load(); break; }
                if (ev.key.key == SDLK_F2) { if (!ev.key.repeat) open_settings(); break; }
                if ((ev.key.key == SDLK_P || ev.key.key == SDLK_PAUSE) && !ev.key.repeat) { toggle_pause(); break; }
                if (ev.key.key == SDLK_H) { g_hintsHeld = true; break; }
                if (int vk = vk_from_sdl(ev.key.key)) input_key(vk, true);
                if (ev.key.key == SDLK_RETURN) input_char(13);
                if (ev.key.key == SDLK_BACKSPACE) input_char(8);
                if (ev.key.key == SDLK_ESCAPE) input_char(27);
                break;
            case SDL_EVENT_KEY_UP:
                if (ev.key.key == SDLK_H) { g_hintsHeld = false; break; }
                if (int vk = vk_from_sdl(ev.key.key)) input_key(vk, false);
                break;
            case SDL_EVENT_TEXT_INPUT:
                for (const char* p = ev.text.text; *p; ++p) {
                    unsigned char ch = static_cast<unsigned char>(*p);
                    if (ch == 0xC3 && p[1]) {   // UTF-8 Latin-1 -> cp1252
                        input_char(0x40 + static_cast<unsigned char>(*++p));
                    } else if (ch < 0x80) {
                        input_char(ch);
                    }
                }
                break;
            default: break;
        }
        have = SDL_PollEvent(&ev);
    }
#ifdef __EMSCRIPTEN__
    if (char* typed = web_take_text()) {   // on-screen keyboard
        for (const unsigned char* p = reinterpret_cast<unsigned char*>(typed); *p; ++p) {
            if (*p == '\b' || *p == '\r') {
                int vk = *p == '\b' ? 0x08 : 0x0D;
                input_key(vk, true);
                input_char(vk);
                input_key(vk, false);
            } else if (*p == 0xC3 && p[1]) {
                input_char(0x40 + *++p);   // UTF-8 Latin-1 letters (å ä ö ...) -> cp1252
            } else if (*p < 0x80) {
                input_char(*p);
            }
        }
        std::free(typed);
    }
#endif
    uint64_t now = SDL_GetTicks();
    if (now - g_lastPresent >= 15) {
        present();
        g_lastPresent = now;
    }
    automation(now);
    quick::pump();
    farm::pump();
    remember_window(now);
    static bool inSettings = false;
    if (g_settingsRequested && !inSettings) {
        g_settingsRequested = false;
        inSettings = true;
        run_settings_screen();
        inSettings = false;
    }
    static bool inHelp = false;
    if (g_helpRequested && !inHelp && !input_overlay_active()) {
        g_helpRequested = false;
        inHelp = true;
        run_help_screen("", 3, 0);   // HELP_CONTENTS
        inHelp = false;
    }
}

void request_help() { g_helpRequested = true; }

::SDL_Window* host_window() { return g_window; }
std::string host_take_dropped() {
    std::string d;
    d.swap(g_dropped);
    return d;
}
void host_system_cursor(bool show) {
    g_systemCursor = show;
    if (show) SDL_ShowCursor();
    else SDL_HideCursor();
}

void show_note(const std::string& text) { show_toast(text); }

bool host_fullscreen() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN); }
void host_set_fullscreen(bool on) {
    if (!g_window) return;
    SDL_SetWindowFullscreen(g_window, on);
#ifndef __EMSCRIPTEN__
    settings::set_int("fullscreen", on ? 1 : 0);   // the browser needs a click for it each visit
#endif
}

}  // namespace win16

#ifdef __EMSCRIPTEN__
// Called by the web page (gear button)
extern "C" EMSCRIPTEN_KEEPALIVE void myulk_open_settings() { win16::open_settings(); }
// Eye button: show the hotspot hints for a few seconds
extern "C" EMSCRIPTEN_KEEPALIVE void myulk_show_hints() { win16::g_hintsUntil = SDL_GetTicks() + 3000; }
#endif
