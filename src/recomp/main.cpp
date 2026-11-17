// Entry point of the recompiled game: loads MYULK.EXE into emulated memory,
// installs the Windows 3.1 API layer and runs the original program entry.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <fstream>
#include <iterator>

#include "funcs.hpp"
#include "recomp/enhance/watchdog.hpp"
#include "recomp/enhance/setup.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/webdata.hpp"
#include "recomp/win16/win16.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Saves live in IndexedDB: mount it at /saves and load what is stored there
// before the game starts (the VFS overlay writes into /saves/drive_c).
EM_ASYNC_JS(void, mount_saves, (), {
    try { FS.mkdir('/saves'); } catch (e) {}
    FS.mount(IDBFS, {}, '/saves');
    await new Promise((resolve) => FS.syncfs(true, (err) => {
        if (err) console.warn('loading saves failed', err);
        resolve();
    }));
});
#endif

namespace fs = std::filesystem;

// The game files from the CD: a folder holding MYULK, PICTDLL, ART, ... (the
// CD's root). Looked for in this order: a path to MYULK.EXE on the command
// line, MYULK_GAME_DIR, "game" next to the program (release packages), the
// program's own folder, then the repository layout (extracted/iso).
// The files imported from the player's CD on first start (setup.cpp) come
// after the program's own folder. MYULK_FORCE_SETUP=1 looks only at
// MYULK_GAME_DIR and the imported files (tests of the first-start screen).
static std::string find_exe(int argc, char** argv) {
    if (argc > 1 && fs::exists(argv[1])) return argv[1];
    std::vector<fs::path> roots;
    if (const char* d = std::getenv("MYULK_GAME_DIR")) roots.emplace_back(d);
#ifndef __EMSCRIPTEN__
    const bool onlyImported = std::getenv("MYULK_FORCE_SETUP") != nullptr;
    const char* base = SDL_GetBasePath();
    if (base && !onlyImported) {
        roots.emplace_back(fs::path(base) / "game");
        roots.emplace_back(fs::path(base));
    }
    if (std::string imported = win16::setup::imported_dir(); !imported.empty()) roots.emplace_back(fs::u8path(imported));
    if (base && !onlyImported) roots.emplace_back(fs::path(base) / ".." / ".." / "extracted" / "iso");   // build/native
    if (!onlyImported)
#endif
        for (const char* p : {"extracted/iso", "../extracted/iso", "../../extracted/iso", "/game"}) roots.emplace_back(p);
    for (const fs::path& r : roots) {
        std::error_code ec;
        fs::path exe = r / "MYULK" / "MYULK.EXE";
        if (fs::exists(exe, ec)) return exe.lexically_normal().string();
    }
    return {};
}

static void error_box(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
#ifndef __EMSCRIPTEN__
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Myulk", msg, nullptr);
#endif
}

static bool exe_ok(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    return win16::setup::exe_matches(data);
}

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    mount_saves();
    setenv("MYULK_SAVE_DIR", "/saves/drive_c", 1);
    // game files from the server, or imported earlier into this browser;
    // otherwise the player chooses their CD image now
    if (!win16::webdata::init("/game")) {
        win16::init_host();
        if (!win16::setup::run() || !win16::webdata::init("/game")) return 0;
    }
#endif
    std::string exe = find_exe(argc, argv);
#ifndef __EMSCRIPTEN__
    if (exe.empty()) {   // first start: "choose your Myulk disc"
        win16::init_host();
        if (!win16::setup::run()) return 0;
        exe = (fs::u8path(win16::setup::imported_dir()) / "MYULK" / "MYULK.EXE").string();
    }
#endif
    win16::webdata::ensure(exe);
    if (exe.empty()) {
        error_box("Spelfilerna hittades inte / The game files were not found.");
        return 1;
    }
    if (!exe_ok(exe)) {
        error_box("Den h\xC3\xA4r versionen av MYULK.EXE passar inte programmet (en annan utg\xC3\xA5va av spelet, "
                  "eller en skadad fil).\n\nThis MYULK.EXE does not match the program (another edition of the "
                  "game, or a damaged file).");
        return 1;
    }
    auto& rt = recomp::Runtime::get();
    if (!rt.load(exe)) return 1;
    win16::install(rt, fs::path(exe).parent_path().string());
    win16::watchdog::start();
    try {
        recomp::f_01_0002(rt.cpu());
        recomp::log("program returned normally");
    } catch (const win16::ExitProgram& e) {
        recomp::log("program exited with code %d", e.code);
        if (std::getenv("MYULK_EXIT_TRACE")) recomp::log("at exit:%s", recomp::recent_trace().c_str());
        if (std::getenv("MYULK_API_STATS")) rt.print_api_stats();
        win16::shutdown();
        return e.code;
    } catch (const recomp::DelphiJump& j) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "exception handler %04X:%04X has no active frame", j.sel, j.off);
        recomp::log("FATAL: %s", buf);
        win16::fatal(buf);
        win16::shutdown();
        return 3;
    } catch (const std::exception& e) {
        recomp::log("FATAL: %s", e.what());
        win16::fatal(e.what());
        win16::shutdown();
        return 3;
    }
    win16::shutdown();
    return 0;
}
