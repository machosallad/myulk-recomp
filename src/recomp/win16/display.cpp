// SDL front end: window, presenting the 640x480 screen, input translation.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
    while (have) {
        SDL_ConvertEventToRenderCoordinates(g_renderer, &ev);
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
