// Farm helper (see farm.hpp). The farm status lives at DS:3F24 (balance,
// 32-bit kronor) and DS:3F2D (day number; 360-day years of 30-day months,
// year 1 = 1997), the same block the save files store.
#include "recomp/enhance/farm.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <string>

#include "recomp/enhance/settings.hpp"
#include "recomp/runtime/generated_support.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"

namespace win16::farm {
namespace {

constexpr uint16_t kBalance = 0x3F24, kDay = 0x3F2D;
constexpr int kClockX = 200, kClockY = 422, kClockR = 40;   // "Ett dygn" on the farm screen

long balance() {
    auto& rt = recomp::Runtime::get();
    return long(int32_t(rt.cpu().rd32(rt.data_sel(), kBalance)));
}
int day() {
    auto& rt = recomp::Runtime::get();
    return int16_t(rt.cpu().rd16(rt.data_sel(), kDay));
}

int month_of(int d) {
    int m = (d % 360 - 1) / 30 + 1;
    return m < 1 ? 1 : m > 12 ? 12 : m;
}
int year_of(int d) { return 1996 + d / 360 + 1; }

std::string date_text(int d) {
    static const char* months[] = {"jan", "feb", "mar", "apr", "maj", "jun",
                                   "jul", "aug", "sep", "okt", "nov", "dec"};
    int dom = d % 30 == 0 ? 30 : d % 30;
    return std::to_string(dom) + " " + months[month_of(d) - 1] + " " + std::to_string(year_of(d));
}

std::string kronor(long v) {
    std::string digits = std::to_string(v < 0 ? -v : v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ' ';
        out += digits[i];
    }
    return out + " kr";
}

bool active() {
    return settings::helper("farm_helper") && farm_open() && active_window_class() == "TFORM1" &&
           !input_overlay_active();
}

bool g_watching = false, g_stepRequested = false;
long g_lastBalance = 0, g_pendingDelta = 0;
int g_lastDay = 0;
uint64_t g_deltaSince = 0;

}  // namespace

bool next_day() {
    static const bool debug = std::getenv("MYULK_DEBUG_FARM") != nullptr;
    if (debug)
        recomp::log("farm: next day? helper %d open %d active %s overlay %d", settings::helper("farm_helper"),
                    farm_open(), active_window_class().c_str(), input_overlay_active());
    if (!active()) return false;
    bool fired = farm_next_day();
    if (debug) recomp::log("farm: day clock fired %d", fired);
    if (fired) g_stepRequested = true;
    return true;
}

bool click(int x, int y) {
    if (!active()) return false;
    int dx = x - kClockX, dy = y - kClockY;
    if (dx * dx + dy * dy > kClockR * kClockR) return false;
    return next_day();
}

void pump() {
    if (!settings::helper("farm_helper") || !farm_open()) { g_watching = false; return; }
    long b = balance();
    int d = day();
    uint64_t now = SDL_GetTicks();
    if (!g_watching) {   // the farm just opened (or a save was loaded): start from here
        g_watching = true;
        g_lastBalance = b;
        g_lastDay = d;
        g_pendingDelta = 0;
        return;
    }
    if (d != g_lastDay) {
        bool step = g_stepRequested;
        g_stepRequested = false;
        if (d < g_lastDay || d > g_lastDay + 31) {   // a different game was loaded
            g_lastDay = d;
            g_lastBalance = b;
            g_pendingDelta = 0;
            return;
        }
        static const char* seasons[] = {"Vintern", "V\xE5ren", "Sommaren", "H\xF6sten"};
        int m = month_of(d), pm = month_of(g_lastDay);
        if (m != pm && m % 3 == 0) {   // March, June, September, December
            show_note(std::string(seasons[m / 3 % 4]) + " " + std::to_string(year_of(d)) + " har b\xF6rjat");
        } else if (step) {
            show_note("N\xE4sta dag: " + date_text(d));
        }
        g_lastDay = d;
    }
    // balance changes are collected for a moment, so a purchase with fees is one note
    if (b != g_lastBalance) {
        g_pendingDelta += b - g_lastBalance;
        g_lastBalance = b;
        g_deltaSince = now;
    }
    if (g_pendingDelta && now - g_deltaSince > 700) {
        show_note(std::string(g_pendingDelta > 0 ? "+" : "\x96") + kronor(g_pendingDelta < 0 ? -g_pendingDelta : g_pendingDelta) +
                  "  (saldo " + (b < 0 ? "\x96" : "") + kronor(b < 0 ? -b : b) + ")");
        g_pendingDelta = 0;
    }
}

}  // namespace win16::farm
