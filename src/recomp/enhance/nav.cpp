// Moving around with the keyboard and mouse shortcuts (see nav.hpp).
//
// The adventure screen keeps its exits in slots at DS:4F3E + 4*i: 0 forward,
// 1 left, 2 right, 3 down, 4 up, 5-14 hotspots, 15 back ("anywhere else"),
// 16-17 extra hotspots. The direction zones are fixed rectangles (2:0B42),
// hotspot rectangles live in TAdvForm (+0x251 / +0x2A1). A shortcut clicks the
// middle of the zone, or for "back" a spot that is neither a zone nor a
// hotspot, then puts the cursor back where the player has it.
#include "recomp/enhance/nav.hpp"

#include <string>

#include "recomp/runtime/generated_support.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"

namespace win16::nav {
namespace {

constexpr int kSceneX = 95, kSceneY = 72;   // scene picture (GPanel) on the screen
constexpr int kSceneW = 450, kSceneH = 250;
enum Slot { kForward = 0, kLeft, kRight, kDown, kUp, kBack = 15 };
const Rect kZones[5] = {{148, 76, 300, 172}, {0, 0, 53, 249}, {396, 0, 449, 249}, {54, 196, 395, 249}, {54, 0, 395, 53}};

int g_mouseX = 320, g_mouseY = 240;
uint64_t g_lastMove = 0;

recomp::Cpu& cpu() { return recomp::Runtime::get().cpu(); }
uint16_t ds() { return recomp::Runtime::get().data_sel(); }

bool slot_set(int i) {
    return cpu().rd16(ds(), uint16_t(0x4F3E + 4 * i)) || cpu().rd16(ds(), uint16_t(0x4F40 + 4 * i));
}

// adventure screen in front, a chapter running and the scene not busy
bool ready() {
    if (input_overlay_active() || active_window_class() != "TADVFORM") return false;
    int mode = int16_t(cpu().rd16(ds(), 0x53C0));
    return mode >= 1 && mode <= 3 && cpu().rd8(ds(), 0x53C3) == 0;
}

Rect hotspot(int i) {
    uint16_t fo = cpu().rd16(ds(), 0x4C36), fs = cpu().rd16(ds(), 0x4C38);   // TAdvForm
    if (!fs) return Rect{};
    uint16_t o = uint16_t(fo + (i < 15 ? 0x251 + 8 * (i - 5) : 0x2A1 + 8 * (i - 16)));
    return Rect{int16_t(cpu().rd16(fs, o)), int16_t(cpu().rd16(fs, uint16_t(o + 2))),
                int16_t(cpu().rd16(fs, uint16_t(o + 4))), int16_t(cpu().rd16(fs, uint16_t(o + 6)))};
}

// a spot in the scene that is no direction zone and no hotspot: "back"
bool free_spot(int& x, int& y) {
    for (int sy = 12; sy < kSceneH - 6; sy += 8)
        for (int sx = 12; sx < kSceneW - 6; sx += 8) {
            bool taken = false;
            for (int i = 0; i < 5 && !taken; ++i) taken = slot_set(i) && kZones[i].contains(sx, sy);
            for (int i = 5; i < 18 && !taken; ++i) {
                if (i == kBack || !slot_set(i)) continue;
                Rect r = hotspot(i);
                taken = !r.empty() && r.left < 1500 && r.contains(sx, sy);
            }
            if (!taken) { x = sx; y = sy; return true; }
        }
    return false;
}

void click_scene(int sx, int sy) {
    int x = kSceneX + sx, y = kSceneY + sy;
    input_mouse_move(x, y);
    input_mouse_button(x, y, 0, true);
    input_mouse_button(x, y, 0, false);
    input_mouse_move(g_mouseX, g_mouseY);
    g_lastMove = SDL_GetTicks();
}

// Use exit `slot` if the scene has it; returns false otherwise.
bool go(int slot) {
    static const bool debug = SDL_getenv("MYULK_DEBUG_NAV") != nullptr;
    if (debug) {
        std::string s;
        for (int i = 0; i < 18; ++i)
            if (slot_set(i)) {
                s += " " + std::to_string(i);
                if (i >= 5 && i != kBack) {
                    Rect r = hotspot(i);
                    s += "(" + std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) +
                         "," + std::to_string(r.bottom) + ")";
                }
            }
        recomp::log("nav: slot %d ready %d busy %d mode %d slots:%s", slot, ready(), cpu().rd8(ds(), 0x53C3),
                    int16_t(cpu().rd16(ds(), 0x53C0)), s.c_str());
    }
    if (!ready() || !slot_set(slot)) return false;
    if (SDL_GetTicks() - g_lastMove < 300) return true;   // one step at a time
    if (slot == kBack) {
        int x = 0, y = 0;
        if (!free_spot(x, y)) return false;
        click_scene(x, y);
    } else {
        const Rect& z = kZones[slot];
        click_scene((z.left + z.right) / 2, (z.top + z.bottom) / 2);
    }
    return true;
}

}  // namespace

void mouse_at(int x, int y) { g_mouseX = x; g_mouseY = y; }

bool key(SDL_Keycode k) {
    switch (k) {
        case SDLK_UP: case SDLK_W: return go(kForward) || go(kUp);
        case SDLK_DOWN: case SDLK_S: return go(kDown) || go(kBack);
        case SDLK_LEFT: case SDLK_A: return go(kLeft);
        case SDLK_RIGHT: case SDLK_D: return go(kRight);
        case SDLK_PAGEUP: return go(kUp);
        case SDLK_PAGEDOWN: return go(kDown);
        case SDLK_BACKSPACE: return go(kBack);
        default: return false;
    }
}

bool right_click(int x, int y) {
    if (x < kSceneX || y < kSceneY || x >= kSceneX + kSceneW || y >= kSceneY + kSceneH) return false;
    return go(kBack);
}

bool wheel(float dy) {
    if (dy > 0) return go(kForward);
    if (dy < 0) return go(kBack);
    return false;
}

}  // namespace win16::nav
