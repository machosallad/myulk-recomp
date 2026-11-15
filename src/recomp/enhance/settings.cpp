#include "recomp/enhance/settings.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace settings {
namespace {

std::map<std::string, int> g_values;
bool g_loaded = false;

std::string path() {
#ifdef __EMSCRIPTEN__
    return "/saves/settings.txt";   // IndexedDB (mounted by main)
#else
    static std::string p;
    if (p.empty() && std::getenv("MYULK_SETTINGS")) p = std::getenv("MYULK_SETTINGS");   // tests
    if (p.empty()) {
        if (char* dir = SDL_GetPrefPath("myulk-recomp", "Myulk")) {
            p = std::string(dir) + "settings.txt";
            SDL_free(dir);
        } else {
            p = "myulk-settings.txt";
        }
    }
    return p;
#endif
}

void load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(path());
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) g_values[line.substr(0, eq)] = std::atoi(line.c_str() + eq + 1);
    }
}

void save() {
    {
        std::ofstream out(path(), std::ios::trunc);
        for (const auto& [k, v] : g_values) out << k << '=' << v << '\n';
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({ if (typeof IDBFS !== 'undefined') FS.syncfs(false, function (e) { if (e) console.warn(e); }); });
#endif
}

}  // namespace

int get_int(const std::string& key, int def) {
    load();
    auto it = g_values.find(key);
    return it == g_values.end() ? def : it->second;
}

void set_int(const std::string& key, int value) {
    load();
    g_values[key] = value;
    save();
}

int style() {
    bool all = true, none = get_int("farm_speed", 1) == 1;
    for (const char* k : kHelpers) {
        if (helper(k)) none = false;
        else all = false;
    }
    return all ? kModern : none ? kOriginal : kCustom;
}

void set_style(int s) {
    load();
    for (const char* k : kHelpers) g_values[k] = s == kModern ? 1 : 0;
    if (s == kOriginal) g_values["farm_speed"] = 1;
    g_values["style_chosen"] = 1;
    save();
}

bool style_chosen() { return get_int("style_chosen", 0) != 0; }

}  // namespace settings
