// Re-implementation of the Windows 3.1 API used by MYULK.EXE, on SDL.
#pragma once

#include <string>

#include "recomp/runtime/runtime.hpp"

namespace win16 {

struct ExitProgram {
    int code;
};

// Register all implemented API functions and set up the environment.
// gameDir is the folder holding MYULK.EXE (the original MYULK\ directory).
void install(recomp::Runtime& rt, const std::string& gameDir);
void shutdown();
void fatal(const std::string& message);

// KERNEL services shared with other modules
void kernel_setup(recomp::Runtime& rt, const std::string& gameDir);
void huge_read(recomp::Cpu& c, uint32_t farptr, uint8_t* dst, uint32_t n);
void huge_write(recomp::Cpu& c, uint32_t farptr, const uint8_t* src, uint32_t n);
uint16_t kernel_global_alloc(uint32_t size, const char* tag);
void kernel_global_free(uint16_t h);
uint32_t kernel_global_size(uint16_t h);

}  // namespace win16

// Convenience for API definitions:
//   API(KERNEL, 3, GetVersion, 0) { a.ret(0x0A03); }
// declares a function with an Args reader `a` over `bytes` argument bytes.
#define API(MOD, ORD, NAME, BYTES)                                                  \
    static void api_##MOD##_##NAME##_impl(recomp::Cpu& c, recomp::Args& a);        \
    static void api_##MOD##_##NAME(recomp::Cpu& c) {                               \
        recomp::Args a(c, BYTES, #MOD "." #NAME);                                  \
        api_##MOD##_##NAME##_impl(c, a);                                           \
    }                                                                               \
    static const bool reg_##MOD##_##NAME = (::win16::detail::add(#MOD, ORD, &api_##MOD##_##NAME, #NAME), true); \
    static void api_##MOD##_##NAME##_impl([[maybe_unused]] recomp::Cpu& c, [[maybe_unused]] recomp::Args& a)

namespace win16::detail {
void add(const char* module, int ordinal, recomp::ApiFn fn, const char* name);
void install_all(recomp::Runtime& rt);
}
