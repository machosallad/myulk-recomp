// SDL front end: window, presenting the 640x480 screen, input translation.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recomp/runtime/generated_support.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/win16.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
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

void present() {
    Surface& s = screen();
    SDL_UpdateTexture(g_tex, nullptr, s.px.data(), s.w * 4);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_tex, nullptr, nullptr);
    if (const CursorImage* cur = current_cursor()) {
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
                if (std::sscanf(item.c_str(), "%d:%x", &seg, &off) == 2) r.steps.push_back({seg, uint16_t(off)});
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
            r.tracePos = recomp::g_trace.pos;
        }
    }
    if (r.next >= r.steps.size()) return;
    auto [seg, off] = r.steps[r.next];
    if (entered_since(r.tracePos, seg, off)) {
        recomp::log("route: reached %d:%04X (%zu/%zu)", seg, off, r.next + 1, r.steps.size());
        r.tracePos = recomp::g_trace.pos;
        ++r.next;
        r.waitingSince = now;
        r.reported = false;
        return;
    }
    if (!r.waitingSince) r.waitingSince = now;
    recomp::Cpu& c = recomp::Runtime::get().cpu();
    uint16_t ds = recomp::Runtime::get().data_sel();
    if (c.rd8(ds, 0x53C3) != 0 || now - r.lastClick < 800) return;   // scene busy / just clicked
    uint16_t sel = uint16_t((seg << 3) | 7);
    int slot = -1;
    for (int i = 0; i < 18 && slot < 0; ++i)
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
        uint16_t ro = uint16_t(fo + (slot < 15 ? 0x251 + 8 * (slot - 5) : 0x2A1 + 8 * (slot - 16)));
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
    for (auto& c : g_clicks) {
        if (!c.done && now >= c.at) {
            c.done = true;
            input_mouse_move(c.x, c.y);
            input_mouse_button(c.x, c.y, 0, true);
            input_mouse_button(c.x, c.y, 0, false);
        }
    }
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
    // MYULK_POKE="ms:OFF=VAL;...": write a byte into the data segment (debug: set game flags)
    static bool poked = false;
    if (!poked) {
        if (const char* pk = SDL_getenv("MYULK_POKE")) {
            int ms = std::atoi(pk);
            if (now >= uint64_t(ms)) {
                poked = true;
                std::string s = pk;
                size_t p = s.find(':');
                while (p != std::string::npos && p + 1 < s.size()) {
                    unsigned off = 0, val = 0;
                    if (std::sscanf(s.c_str() + p + 1, "%x=%x", &off, &val) == 2) {
                        recomp::Runtime::get().cpu().wr8(recomp::Runtime::get().data_sel(), uint16_t(off), uint8_t(val));
                        recomp::log("poke DS:%04X = %02X", off, val);
                    }
                    p = s.find(';', p + 1);
                }
            }
        } else {
            poked = true;
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
        Surface& s = screen();
        SDL_Surface* surf = SDL_CreateSurfaceFrom(s.w, s.h, SDL_PIXELFORMAT_XRGB8888, s.px.data(), s.w * 4);
        bool ok = surf && SDL_SaveBMP(surf, path.c_str());
        if (surf) SDL_DestroySurface(surf);
        recomp::log("screenshot %s: %s", path.c_str(), ok ? "saved" : SDL_GetError());
        if (SDL_getenv("MYULK_DUMP_WINDOWS")) dump_windows();
        if (++shotNext == shotTimes.size()) throw ExitProgram{0};
    }
}

}  // namespace

bool display_init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        recomp::log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    if (!SDL_CreateWindowAndRenderer("Myulk", 1280, 960, SDL_WINDOW_RESIZABLE, &g_window, &g_renderer)) {
        recomp::log("window: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderLogicalPresentation(g_renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(g_renderer, 1);
    g_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);
    SDL_HideCursor();
    SDL_StartTextInput(g_window);
    return true;
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
    bool have = blockMs > 0 ? SDL_WaitEventTimeout(&ev, blockMs) : SDL_PollEvent(&ev);
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
            case SDL_EVENT_MOUSE_MOTION: input_mouse_move(int(ev.motion.x), int(ev.motion.y)); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int b = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
                input_mouse_button(int(ev.button.x), int(ev.button.y), b, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_F11) {
                    bool full = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(g_window, !full);
                    break;
                }
                if (int vk = vk_from_sdl(ev.key.key)) input_key(vk, true);
                if (ev.key.key == SDLK_RETURN) input_char(13);
                if (ev.key.key == SDLK_BACKSPACE) input_char(8);
                if (ev.key.key == SDLK_ESCAPE) input_char(27);
                break;
            case SDL_EVENT_KEY_UP:
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
    uint64_t now = SDL_GetTicks();
    if (now - g_lastPresent >= 15) {
        present();
        g_lastPresent = now;
    }
    automation(now);
#ifdef __EMSCRIPTEN__
    emscripten_sleep(0);
#endif
}

}  // namespace win16
