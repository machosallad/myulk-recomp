// Entry point of the recompiled game: loads MYULK.EXE into emulated memory,
// installs the Windows 3.1 API layer and runs the original program entry.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "funcs.hpp"
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
static std::string find_exe(int argc, char** argv) {
    if (argc > 1 && fs::exists(argv[1])) return argv[1];
    std::vector<fs::path> roots;
    if (const char* d = std::getenv("MYULK_GAME_DIR")) roots.emplace_back(d);
#ifndef __EMSCRIPTEN__
    if (const char* base = SDL_GetBasePath()) {
        roots.emplace_back(fs::path(base) / "game");
        roots.emplace_back(fs::path(base));
        roots.emplace_back(fs::path(base) / ".." / ".." / "extracted" / "iso");   // build/native
    }
#endif
    for (const char* p : {"extracted/iso", "../extracted/iso", "../../extracted/iso", "/game"}) roots.emplace_back(p);
    for (const fs::path& r : roots) {
        std::error_code ec;
        fs::path exe = r / "MYULK" / "MYULK.EXE";
        if (fs::exists(exe, ec)) return exe.lexically_normal().string();
    }
    return {};
}

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    mount_saves();
    setenv("MYULK_SAVE_DIR", "/saves/drive_c", 1);
    if (!win16::webdata::init("/game")) {
        std::fprintf(stderr, "game data not found (game/manifest.txt)\n");
        return 1;
    }
#endif
    std::string exe = find_exe(argc, argv);
    win16::webdata::ensure(exe);
    if (exe.empty()) {
        const char* msg =
            "Spelfilerna hittades inte / The game files were not found.\n\n"
            "Kopiera inneh\xC3\xA5llet p\xC3\xA5 Myulk-cd:n till mappen \"game\" bredvid programmet, "
            "s\xC3\xA5 att game\\MYULK\\MYULK.EXE finns.\n\n"
            "Copy the contents of the Myulk CD into a folder named \"game\" next to the program, "
            "so that game\\MYULK\\MYULK.EXE exists (or set MYULK_GAME_DIR).";
        std::fprintf(stderr, "%s\n", msg);
#ifndef __EMSCRIPTEN__
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Myulk", msg, nullptr);
#endif
        return 1;
    }
    auto& rt = recomp::Runtime::get();
    if (!rt.load(exe)) return 1;
    win16::install(rt, fs::path(exe).parent_path().string());
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
