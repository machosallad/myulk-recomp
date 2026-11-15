#include "recomp/enhance/quick.hpp"

#include <SDL3/SDL.h>

#include <climits>
#include <cstdlib>
#include <string>

#include "recomp/enhance/settings.hpp"
#include "recomp/enhance/ui.hpp"
#include "recomp/runtime/generated_support.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"

namespace win16::quick {
namespace {

constexpr uint16_t kMode = 0x53C0;      // 1-3 adventure chapter, 4 farm
constexpr uint16_t kBusy = 0x53C3;      // adventure scene busy
constexpr uint16_t kMenuForm = 0x57F8;  // TMenuForm instance
constexpr uint16_t kSpara = 0x3AA0, kLadda = 0x3C2F;   // TMenuForm.SparaBtnClick / LaddaBtnClick (seg 6)
constexpr uint64_t kAutosaveEvery = 10 * 60 * 1000;
const char* kQuickFile = "SNABB.MLK";

enum class Action { None, Save, Load };
Action g_pending = Action::None;
int g_lastMode = INT_MIN;
uint64_t g_autoAt = 0, g_lastAuto = 0;

int mode() {
    auto& rt = recomp::Runtime::get();
    return int16_t(rt.cpu().rd16(rt.data_sel(), kMode));
}

bool game_ready() {
    auto& rt = recomp::Runtime::get();
    std::string cls = active_window_class();
    if (cls == "TFORM1") return true;
    return cls == "TADVFORM" && rt.cpu().rd8(rt.data_sel(), kBusy) == 0;
}

// Run a TMenuForm button handler (Self and Sender = MenuForm).
void call_menu(uint16_t off) {
    auto& rt = recomp::Runtime::get();
    uint32_t menu = rt.cpu().rd32(rt.data_sel(), kMenuForm);
    uint16_t words[4] = {uint16_t(menu >> 16), uint16_t(menu), uint16_t(menu >> 16), uint16_t(menu)};
    call_app((uint32_t(recomp::sel_of(6)) << 16) | off, words, 4);
}

std::string where() {
    int m = mode();
    return m >= 1 && m <= 3 ? "Kapitel " + std::to_string(m) : std::string("G\xE5rden");
}

void save(const std::string& file, const std::string& name) {
    ui::set_game_frame(screen());   // picture for the save card
    ui::auto_answer(file, name);
    call_menu(kSpara);
    ui::auto_answer("", "");
}

void autosave() {
    // three rotating files, the oldest is replaced
    std::string pick = "AUTO1.MLK", oldest = "\x7F";
    for (const char* f : {"AUTO1.MLK", "AUTO2.MLK", "AUTO3.MLK"}) {
        std::string t = ui::save_exists(f) ? ui::saved_at(f) : std::string();
        if (t < oldest) { oldest = t; pick = f; }
    }
    save(pick, "Autospar: " + where());
    g_lastAuto = SDL_GetTicks();
}

void idle() {
    uint64_t now = SDL_GetTicks();
    int m = mode();
    static const bool debugDay = std::getenv("MYULK_DEBUG_DAY") != nullptr;   // farm day once a second
    static uint64_t lastDayLog = 0;
    if (debugDay && now - lastDayLog >= 1000) {
        lastDayLog = now;
        auto& rt = recomp::Runtime::get();
        recomp::log("day %d", int16_t(rt.cpu().rd16(rt.data_sel(), 0x3F2D)));
    }
    if (m != g_lastMode) {
        // a chapter starts or the farm opens: autosave once things have settled
        if (g_lastMode != INT_MIN && g_lastMode != 0 && m >= 1 && m <= 4) g_autoAt = now + 5000;
        g_lastMode = m;
    }
    bool autoOn = settings::get_int("autosave", 1) != 0;
    if (autoOn && m >= 1 && m <= 4 && g_lastAuto && now - g_lastAuto > kAutosaveEvery && !g_autoAt) g_autoAt = now;
    if (!g_lastAuto) g_lastAuto = now;   // first periodic autosave ten minutes into the session
    if (g_pending == Action::None && !(autoOn && g_autoAt && now >= g_autoAt)) return;
    static const bool debug = std::getenv("MYULK_DEBUG_QUICK") != nullptr;
    if (debug) {
        auto& rt = recomp::Runtime::get();
        recomp::log("quick: pending %d mode %d active %s busy %d", int(g_pending), m, active_window_class().c_str(),
                    rt.cpu().rd8(rt.data_sel(), kBusy));
    }
    if (!game_ready() || m < 1 || m > 4) return;
    Action a = g_pending;
    g_pending = Action::None;
    if (a == Action::Save) {
        save(kQuickFile, "Snabbsparning: " + where());
        show_note("Snabbsparat (F9 laddar)");
        return;
    }
    if (a == Action::Load) {
        if (!ui::save_exists(kQuickFile)) { show_note("Ingen snabbsparning \xE4nnu (F5 sparar)"); return; }
        ui::auto_answer(kQuickFile, "");
        call_menu(kLadda);
        ui::auto_answer("", "");
        g_lastMode = mode();   // loading is not a new chapter
        g_autoAt = 0;
        show_note("Snabbsparning laddad");
        return;
    }
    g_autoAt = 0;
    autosave();
    show_note("Autosparat");
}

const bool g_registered = (set_idle_hook(&idle), true);

}  // namespace

void request_save() { g_pending = Action::Save; }
void request_load() { g_pending = Action::Load; }

}  // namespace win16::quick
