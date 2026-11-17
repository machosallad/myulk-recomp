// Hang reports (see watchdog.hpp).
#include "recomp/enhance/watchdog.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace recomp {
void log(const char* fmt, ...);
std::string recent_trace();
}

namespace win16::watchdog {
namespace {

std::atomic<uint64_t> g_lastPump{0};
constexpr uint64_t kHangMs = 5000;

#ifdef _WIN32
HANDLE g_main = nullptr;

// Where the game's thread is running, sampled a hundred times: offsets in
// myulk_recomp.exe (look them up in myulk_recomp.map), most frequent first.
std::string native_samples() {
    if (!g_main) return {};
    auto base = uintptr_t(GetModuleHandleW(nullptr));
    std::map<uintptr_t, int> hits;
    for (int i = 0; i < 100; ++i) {
        if (SuspendThread(g_main) == DWORD(-1)) break;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_main, &ctx)) {
#if defined(_M_X64)
            hits[uintptr_t(ctx.Rip)]++;
#elif defined(_M_ARM64)
            hits[uintptr_t(ctx.Pc)]++;
#endif
        }
        ResumeThread(g_main);
        Sleep(10);
    }
    std::multimap<int, uintptr_t, std::greater<int>> order;
    for (auto [a, n] : hits) order.emplace(n, a);
    std::string s = "\n  native (exe offset x samples):";
    int shown = 0;
    for (auto [n, a] : order) {
        char b[48];
        if (a >= base && a - base < 0x10000000) std::snprintf(b, sizeof b, " +%llX x%d", (unsigned long long)(a - base), n);
        else std::snprintf(b, sizeof b, " %llX x%d", (unsigned long long)a, n);
        s += b;
        if (++shown == 24) break;
    }
    return s;
}
#else
std::string native_samples() { return {}; }
#endif

void report(uint64_t stuckMs) {
    std::string text = native_samples();
    // read while the game's thread runs: good enough to see where it loops
    text = recomp::recent_trace() + text;
    char head[96];
    std::time_t t = std::time(nullptr);
    std::strftime(head, sizeof head, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::string out = std::string("hang at ") + head + " (no message pump for " + std::to_string(stuckMs / 1000) +
                      " s):" + text + "\n\n";
    recomp::log("%s", out.c_str());
    if (char* p = SDL_GetPrefPath("myulk-recomp", "Myulk")) {
        std::string path = std::string(p) + "hang.txt";
        SDL_free(p);
        if (FILE* f = std::fopen(path.c_str(), "a")) {
            std::fputs(out.c_str(), f);
            std::fclose(f);
        }
    }
}

}  // namespace

void alive() {
    uint64_t now = SDL_GetTicks();
    g_lastPump.store(now, std::memory_order_relaxed);
    // MYULK_TEST_HANG=ms: stop pumping for 7 s at that time (tests the report)
    static const char* test = std::getenv("MYULK_TEST_HANG");
    static bool tested = false;
    if (test && !tested && now >= uint64_t(std::atoll(test))) {
        tested = true;
        while (SDL_GetTicks() - now < 7000) {
        }
    }
}

void start() {
#ifndef __EMSCRIPTEN__
    static bool started = false;
    if (started) return;
    started = true;
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
#endif
    alive();
    std::thread([] {
        bool reported = false;
        for (;;) {
            SDL_Delay(1000);
            uint64_t last = g_lastPump.load(std::memory_order_relaxed);
            uint64_t stuck = SDL_GetTicks() - last;
            if (stuck < kHangMs) {
                reported = false;
                continue;
            }
            if (!reported) {
                reported = true;
                report(stuck);
            }
        }
    }).detach();
#endif
}

}  // namespace win16::watchdog
