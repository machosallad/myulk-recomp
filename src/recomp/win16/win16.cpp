#include "recomp/win16/win16.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "recomp/win16/gdi.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/vfs.hpp"

namespace fs = std::filesystem;

namespace win16 {

bool display_init();
void display_shutdown();
void mm_poll();
namespace detail {

struct Entry {
    const char* module;
    int ordinal;
    recomp::ApiFn fn;
    const char* name;
};

static std::vector<Entry>& registry() {
    static std::vector<Entry> r;
    return r;
}

void add(const char* module, int ordinal, recomp::ApiFn fn, const char* name) {
    registry().push_back({module, ordinal, fn, name});
}

void install_all(recomp::Runtime& rt) {
    for (const auto& e : registry()) rt.register_api(e.module, e.ordinal, e.fn, e.name);
    recomp::log("registered %zu Windows API functions", registry().size());
}

}  // namespace detail

void install(recomp::Runtime& rt, const std::string& gameDir) {
    fs::path g = fs::absolute(gameDir);
    vfs::init(g.parent_path().string(), g.filename().string());
    // saves and other files the game writes: MYULK_SAVE_DIR or the user's
    // application data folder (the game data is never modified)
    std::string overlay;
    if (const char* d = std::getenv("MYULK_SAVE_DIR")) overlay = d;
    else if (char* p = SDL_GetPrefPath("myulk-recomp", "Myulk")) { overlay = std::string(p) + "drive_c"; SDL_free(p); }
    if (!overlay.empty()) {
        vfs::set_overlay(overlay);
        recomp::log("writable files go to %s", overlay.c_str());
    }
    kernel_setup(rt, gameDir);
    display_init();
    gdi_init();
    user_init();
    detail::install_all(rt);
}

void init_host() {
    display_init();
    gdi_init();
}

void shutdown() { display_shutdown(); }

void fatal(const std::string& message) { std::fprintf(stderr, "Myulk: %s\n", message.c_str()); }

}  // namespace win16
