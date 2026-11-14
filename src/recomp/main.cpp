// Entry point of the recompiled game: loads MYULK.EXE into emulated memory,
// installs the Windows 3.1 API layer and runs the original program entry.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#include "funcs.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/win16.hpp"

namespace fs = std::filesystem;

static std::string find_exe(int argc, char** argv) {
    if (argc > 1 && fs::exists(argv[1])) return argv[1];
    for (const char* p : {"extracted/iso/MYULK/MYULK.EXE", "../extracted/iso/MYULK/MYULK.EXE",
                          "../../extracted/iso/MYULK/MYULK.EXE", "/data/iso/MYULK/MYULK.EXE"})
        if (fs::exists(p)) return p;
    return {};
}

int main(int argc, char** argv) {
    std::string exe = find_exe(argc, argv);
    if (exe.empty()) {
        std::fprintf(stderr, "MYULK.EXE not found (pass its path as the first argument)\n");
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
        if (std::getenv("MYULK_API_STATS")) rt.print_api_stats();
        win16::shutdown();
        return e.code;
    } catch (const std::exception& e) {
        recomp::log("FATAL: %s", e.what());
        win16::fatal(e.what());
        win16::shutdown();
        return 3;
    }
    win16::shutdown();
    return 0;
}
