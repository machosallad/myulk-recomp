// Declarations shared by the generated code and the runtime.
#pragma once

#include <cstdint>

#include "recomp/runtime/cpu.hpp"

namespace recomp {

// Bodies of routines with Delphi exception handlers stay separate from their
// try/catch wrapper: exception pads inside their irreducible control flow
// miscompile for WebAssembly.
#if defined(_MSC_VER)
#define RECOMP_NOINLINE __declspec(noinline)
#else
#define RECOMP_NOINLINE __attribute__((noinline))
#endif

// Selector of NE segment n: (n << 3) | 7 (LDT, ring 3), as the generator emits.
constexpr uint16_t sel_of(int n) { return uint16_t((n << 3) | 7); }
#define RECOMP_SEL(n) constexpr uint16_t SEL##n = sel_of(n);
RECOMP_SEL(1) RECOMP_SEL(2) RECOMP_SEL(3) RECOMP_SEL(4) RECOMP_SEL(5) RECOMP_SEL(6) RECOMP_SEL(7)
RECOMP_SEL(8) RECOMP_SEL(9) RECOMP_SEL(10) RECOMP_SEL(11) RECOMP_SEL(12) RECOMP_SEL(13)
RECOMP_SEL(14) RECOMP_SEL(15) RECOMP_SEL(16) RECOMP_SEL(17) RECOMP_SEL(18) RECOMP_SEL(19)
RECOMP_SEL(20) RECOMP_SEL(21) RECOMP_SEL(22) RECOMP_SEL(23) RECOMP_SEL(24) RECOMP_SEL(25)
RECOMP_SEL(26) RECOMP_SEL(27) RECOMP_SEL(28) RECOMP_SEL(29) RECOMP_SEL(30) RECOMP_SEL(31)
RECOMP_SEL(32) RECOMP_SEL(33) RECOMP_SEL(34)
#undef RECOMP_SEL

// Ring buffer of recently entered functions, printed on fatal errors.
struct TraceRing {
    static constexpr int kSize = 65536;
    uint32_t entries[kSize] = {};
    uint16_t ds[kSize] = {};
    int pos = 0;
    void add(int seg, uint16_t off, uint16_t d) {
        entries[pos] = (uint32_t(seg) << 16) | off;
        ds[pos] = d;
        pos = (pos + 1) & (kSize - 1);
    }
};
extern TraceRing g_trace;
#define RECOMP_TRACE(seg, off) ::recomp::g_trace.add(seg, off, c.ds)

struct FuncEntry {
    uint16_t sel;
    uint16_t off;
    Fn fn;
};
extern const FuncEntry kFuncs[];
extern const int kFuncCount;

struct ImportInfo {
    const char* module;
    int ordinal;
    const char* name;
};
extern const ImportInfo kImports[];   // index = ImportId used by generated code
extern const int kImportCount;

// Call an imported Windows function (return address already pushed).
void imp_call(Cpu& c, int id);
// Segment / offset half of an import used as an immediate. For KERNEL value
// exports (__AHSHIFT, __AHINCR, __WINFLAGS) both return the value; for
// functions they form a far pointer to the import thunk.
uint16_t imp_seg(int id);
uint16_t imp_off(int id);

}  // namespace recomp
