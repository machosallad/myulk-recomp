// Emulated 16-bit x86 CPU state and segmented memory for recompiled code.
//
// Generated code (tools/recomp/recomp.py) calls these members directly:
// register accessors (c.ax(), c.set_ax(v)...), memory (c.rd16(sel, off)),
// flag-setting arithmetic (c.add16(a, b)), stack, string instructions and
// the x87 FPU.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace recomp {

class Cpu;
using Fn = void (*)(Cpu&);

// Thrown when the Delphi RTL jumps to an except handler (sel:off) after
// setting SP/BP to the handler's frame. The recompiled routine owning that
// handler with the matching frame catches it and resumes at the label; the
// C++ frames in between are discarded just like the emulated ones.
struct DelphiJump {
    uint16_t sel, off;
};

// One selector's memory. Code segments come from the EXE; the data segment
// (DGROUP) and dynamic allocations are created by the runtime.
struct Segment {
    std::vector<uint8_t> data;
    bool used = false;
    uint32_t alias = 0;   // nonzero: this selector shares memory with segs[alias]
    std::string name;
};

class Cpu {
public:
    // ---- registers --------------------------------------------------------
    uint32_t r[8] = {};   // eax ecx edx ebx esp ebp esi edi
    uint16_t ds = 0, es = 0, ss = 0, fs = 0, gs = 0;
    bool cf = false, zf = false, sf = false, of = false, pf = false, af = false, df = false;

#define R32(name, i) \
    uint32_t name() const { return r[i]; } \
    void set_##name(uint32_t v) { r[i] = v; }
#define R16(name, i) \
    uint16_t name() const { return uint16_t(r[i]); } \
    void set_##name(uint16_t v) { r[i] = (r[i] & 0xFFFF0000u) | v; }
#define R8(lo, hi, i) \
    uint8_t lo() const { return uint8_t(r[i]); } \
    void set_##lo(uint8_t v) { r[i] = (r[i] & 0xFFFFFF00u) | v; } \
    uint8_t hi() const { return uint8_t(r[i] >> 8); } \
    void set_##hi(uint8_t v) { r[i] = (r[i] & 0xFFFF00FFu) | (uint32_t(v) << 8); }
    R32(eax, 0) R32(ecx, 1) R32(edx, 2) R32(ebx, 3) R32(esp, 4) R32(ebp, 5) R32(esi, 6) R32(edi, 7)
    R16(ax, 0) R16(cx, 1) R16(dx, 2) R16(bx, 3) R16(sp, 4) R16(bp, 5) R16(si, 6) R16(di, 7)
    R8(al, ah, 0) R8(cl, ch, 1) R8(dl, dh, 2) R8(bl, bh, 3)
#undef R32
#undef R16
#undef R8

    uint16_t get_flags() const {
        return uint16_t((cf ? 1 : 0) | 2 | (pf ? 4 : 0) | (af ? 0x10 : 0) | (zf ? 0x40 : 0) |
                        (sf ? 0x80 : 0) | 0x200 | (df ? 0x400 : 0) | (of ? 0x800 : 0));
    }
    void set_flags(uint16_t f) {
        cf = f & 1; pf = f & 4; af = f & 0x10; zf = f & 0x40; sf = f & 0x80;
        df = f & 0x400; of = f & 0x800;
    }

    // ---- memory -----------------------------------------------------------
    std::vector<Segment>* segs = nullptr;   // indexed by selector >> 3

    // debugging: log writes touching watch_sel:watch_off (MYULK_WATCH=sel:off)
    uint16_t watch_sel = 0, watch_off = 0;
    bool watch_reads = false;
    void watch_hit(uint16_t sel, uint16_t off, unsigned size);

    uint8_t* ptr(uint16_t sel, uint16_t off, unsigned size, bool write) {
        if ((write || watch_reads) && watch_sel && (sel >> 3) == (watch_sel >> 3) && off <= watch_off && watch_off < off + size)
            watch_hit(sel, off, size);
        auto idx = size_t(sel >> 3);
        if (idx < segs->size()) {
            Segment* s = &(*segs)[idx];
            if (s->alias) s = &(*segs)[s->alias];
            if (s->used && size_t(off) + size <= s->data.size()) return &s->data[off];
        }
        return fault(sel, off, size, write);
    }
    uint8_t rd8(uint16_t s, uint16_t o) { return *ptr(s, o, 1, false); }
    uint16_t rd16(uint16_t s, uint16_t o) { uint16_t v; std::memcpy(&v, ptr(s, o, 2, false), 2); return v; }
    uint32_t rd32(uint16_t s, uint16_t o) { uint32_t v; std::memcpy(&v, ptr(s, o, 4, false), 4); return v; }
    uint64_t rd64(uint16_t s, uint16_t o) { uint64_t v; std::memcpy(&v, ptr(s, o, 8, false), 8); return v; }
    void wr8(uint16_t s, uint16_t o, uint8_t v) { *ptr(s, o, 1, true) = v; }
    void wr16(uint16_t s, uint16_t o, uint16_t v) { std::memcpy(ptr(s, o, 2, true), &v, 2); }
    void wr32(uint16_t s, uint16_t o, uint32_t v) { std::memcpy(ptr(s, o, 4, true), &v, 4); }
    void wr64(uint16_t s, uint16_t o, uint64_t v) { std::memcpy(ptr(s, o, 8, true), &v, 8); }

    // ---- stack ------------------------------------------------------------
    void push16(uint16_t v) { set_sp(uint16_t(sp() - 2)); wr16(ss, sp(), v); }
    uint16_t pop16() { uint16_t v = rd16(ss, sp()); set_sp(uint16_t(sp() + 2)); return v; }
    void push32(uint32_t v) { set_sp(uint16_t(sp() - 4)); wr32(ss, sp(), v); }
    uint32_t pop32() { uint32_t v = rd32(ss, sp()); set_sp(uint16_t(sp() + 4)); return v; }
    void pusha();
    void popa();
    void pushad();
    void popad();
    void ret_near(int n) { pop16(); set_sp(uint16_t(sp() + n)); }
    void ret_far(int n) { pop16(); pop16(); set_sp(uint16_t(sp() + n)); }
    void iret() { pop16(); pop16(); set_flags(pop16()); }
    void enter(int size, int level);
    void leave() { set_sp(bp()); set_bp(pop16()); }

    // ---- arithmetic (set flags like the real instructions) ----------------
    void szp8(uint8_t v) { zf = v == 0; sf = v & 0x80; pf = parity(v); }
    void szp16(uint16_t v) { zf = v == 0; sf = v & 0x8000; pf = parity(uint8_t(v)); }
    void szp32(uint32_t v) { zf = v == 0; sf = v & 0x80000000u; pf = parity(uint8_t(v)); }
    static bool parity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

#define ARITH(W, T, S, TOP)                                                                       \
    T add##W(T a, T b) { T r_ = T(a + b); cf = r_ < a; of = ((a ^ r_) & (b ^ r_)) & TOP;         \
        af = ((a ^ b ^ r_) & 0x10) != 0; szp##W(r_); return r_; }                                 \
    T adc##W(T a, T b) { T c_ = cf ? 1 : 0; T r_ = T(a + b + c_);                                 \
        cf = c_ ? (r_ <= a) : (r_ < a); of = ((a ^ r_) & (b ^ r_)) & TOP;                         \
        af = ((a ^ b ^ r_) & 0x10) != 0; szp##W(r_); return r_; }                                 \
    T sub##W(T a, T b) { T r_ = T(a - b); cf = a < b; of = ((a ^ b) & (a ^ r_)) & TOP;           \
        af = ((a ^ b ^ r_) & 0x10) != 0; szp##W(r_); return r_; }                                 \
    T sbb##W(T a, T b) { T c_ = cf ? 1 : 0; T r_ = T(a - b - c_);                                 \
        cf = c_ ? (a <= b) : (a < b); of = ((a ^ b) & (a ^ r_)) & TOP;                            \
        af = ((a ^ b ^ r_) & 0x10) != 0; szp##W(r_); return r_; }                                 \
    T and##W(T a, T b) { T r_ = T(a & b); cf = of = false; szp##W(r_); return r_; }               \
    T or##W(T a, T b) { T r_ = T(a | b); cf = of = false; szp##W(r_); return r_; }                \
    T xor##W(T a, T b) { T r_ = T(a ^ b); cf = of = false; szp##W(r_); return r_; }               \
    T inc##W(T a) { T r_ = T(a + 1); of = r_ == TOP; af = (r_ & 0xF) == 0; szp##W(r_); return r_; } \
    T dec##W(T a) { T r_ = T(a - 1); of = a == TOP; af = (a & 0xF) == 0; szp##W(r_); return r_; }  \
    T neg##W(T a) { T r_ = T(0 - a); cf = a != 0; of = a == TOP; szp##W(r_); return r_; }        \
    T not##W(T a) { return T(~a); }                                                               \
    T shl##W(T a, uint8_t n) { n &= 31; if (!n) return a; T r_ = T(n > W ? 0 : (S(a) << n));     \
        cf = n <= W && ((a >> (W - n)) & 1); of = ((r_ & TOP) != 0) != cf; szp##W(r_); return r_; } \
    T shr##W(T a, uint8_t n) { n &= 31; if (!n) return a; T r_ = T(n >= W ? 0 : (a >> n));       \
        cf = n <= W && ((a >> (n - 1)) & 1); of = (a & TOP) != 0; szp##W(r_); return r_; }       \
    T sar##W(T a, uint8_t n) { n &= 31; if (!n) return a; using I = std::make_signed_t<T>;       \
        T r_ = T(I(a) >> (n >= W ? W - 1 : n)); cf = (I(a) >> (n >= W ? W - 1 : n - 1)) & 1;      \
        of = false; szp##W(r_); return r_; }                                                      \
    T rol##W(T a, uint8_t n) { n &= 31; if (!n) return a; n %= W; T r_ = n ? T((a << n) | (a >> (W - n))) : a; \
        cf = r_ & 1; of = ((r_ & TOP) != 0) != cf; return r_; }                                    \
    T ror##W(T a, uint8_t n) { n &= 31; if (!n) return a; n %= W; T r_ = n ? T((a >> n) | (a << (W - n))) : a; \
        cf = (r_ & TOP) != 0; of = ((r_ ^ (r_ << 1)) & TOP) != 0; return r_; }                     \
    T rcl##W(T a, uint8_t n) { n &= 31; while (n--) { bool c_ = (a & TOP) != 0; a = T((a << 1) | (cf ? 1 : 0)); cf = c_; } \
        of = ((a & TOP) != 0) != cf; return a; }                                                   \
    T rcr##W(T a, uint8_t n) { n &= 31; while (n--) { bool c_ = a & 1; a = T((a >> 1) | (cf ? TOP : 0)); cf = c_; } \
        of = ((a ^ (a << 1)) & TOP) != 0; return a; }
    ARITH(8, uint8_t, uint32_t, 0x80u)
    ARITH(16, uint16_t, uint32_t, 0x8000u)
    ARITH(32, uint32_t, uint64_t, 0x80000000u)
#undef ARITH

    uint16_t shld16(uint16_t a, uint16_t b, uint8_t n);
    uint16_t shrd16(uint16_t a, uint16_t b, uint8_t n);
    uint32_t shld32(uint32_t a, uint32_t b, uint8_t n);
    uint32_t shrd32(uint32_t a, uint32_t b, uint8_t n);

    void mul8(uint8_t v);
    void mul16(uint16_t v);
    void mul32(uint32_t v);
    void imul8(uint8_t v);
    void imul16(uint16_t v);
    void imul32(uint32_t v);
    uint16_t imul16_2(uint16_t a, uint16_t b);
    uint32_t imul32_2(uint32_t a, uint32_t b);
    void div8(uint8_t v);
    void div16(uint16_t v);
    void div32(uint32_t v);
    void idiv8(uint8_t v);
    void idiv16(uint16_t v);
    void idiv32(uint32_t v);
    void bt16(uint16_t a, uint16_t b) { cf = (a >> (b & 15)) & 1; }
    void bt32(uint32_t a, uint32_t b) { cf = (a >> (b & 31)) & 1; }
    void daa();
    void das();
    void aaa();
    void aas();
    void aam(int base);
    void aad(int base);

    // ---- string instructions (rep: 0 none, 1 rep/repe, 2 repne) ----------
    void movs8(uint16_t srcseg, int rep);
    void movs16(uint16_t srcseg, int rep);
    void movs32(uint16_t srcseg, int rep);
    void stos8(uint16_t, int rep);
    void stos16(uint16_t, int rep);
    void stos32(uint16_t, int rep);
    void lods8(uint16_t srcseg, int rep);
    void lods16(uint16_t srcseg, int rep);
    void lods32(uint16_t srcseg, int rep);
    void cmps8(uint16_t srcseg, int rep);
    void cmps16(uint16_t srcseg, int rep);
    void scas8(uint16_t, int rep);
    void scas16(uint16_t, int rep);

    // ---- x87 FPU (values held as double) ------------------------------------
    double st[8] = {};
    int top = 0;
    uint16_t fpu_sw = 0;
    uint16_t fpu_control = 0x037F;
    double& ST(int i) { return st[(top + i) & 7]; }
    void fpu_push(double v) { top = (top - 1) & 7; st[top] = v; }
    double fpu_pop() { double v = st[top]; top = (top + 1) & 7; return v; }
    uint16_t fpu_status() const { return uint16_t((fpu_sw & ~0x3800) | ((top & 7) << 11)); }
    void fpu_load_f32(uint16_t s, uint16_t o);
    void fpu_load_f64(uint16_t s, uint16_t o);
    void fpu_load_f80(uint16_t s, uint16_t o);
    void fpu_load_i16(uint16_t s, uint16_t o);
    void fpu_load_i32(uint16_t s, uint16_t o);
    void fpu_load_i64(uint16_t s, uint16_t o);
    void fpu_store_f32(uint16_t s, uint16_t o, bool pop);
    void fpu_store_f64(uint16_t s, uint16_t o, bool pop);
    void fpu_store_f80(uint16_t s, uint16_t o, bool pop);
    void fpu_store_i16(uint16_t s, uint16_t o, bool pop);
    void fpu_store_i32(uint16_t s, uint16_t o, bool pop);
    void fpu_store_i64(uint16_t s, uint16_t o, bool pop);
    void fpu_fbstp(uint16_t s, uint16_t o);   // store 18-digit packed BCD, pop
    void fpu_fbld(uint16_t s, uint16_t o);
    void fpu_fld_st(int i) { double v = ST(i); fpu_push(v); }
    void fpu_fst_st(int i) { ST(i) = ST(0); }
    void fpu_fstp_st(int i) { ST(i) = ST(0); fpu_pop(); }
    void fpu_fxch_st(int i) { double t = ST(0); ST(0) = ST(i); ST(i) = t; }
    void fpu_arith(int op, double& d, double s);
#define FPU_OP(name, op)                                                                       \
    void fpu_##name##_st(int d, int s, bool pop) { fpu_arith(op, ST(d), ST(s)); if (pop) fpu_pop(); } \
    void fpu_##name##_mem_f32(uint16_t sg, uint16_t o) { float f; uint32_t u = rd32(sg, o); std::memcpy(&f, &u, 4); fpu_arith(op, ST(0), f); } \
    void fpu_##name##_mem_f64(uint16_t sg, uint16_t o) { double f; uint64_t u = rd64(sg, o); std::memcpy(&f, &u, 8); fpu_arith(op, ST(0), f); } \
    void fpu_##name##_mem_i16(uint16_t sg, uint16_t o) { fpu_arith(op, ST(0), double(int16_t(rd16(sg, o)))); } \
    void fpu_##name##_mem_i32(uint16_t sg, uint16_t o) { fpu_arith(op, ST(0), double(int32_t(rd32(sg, o)))); }
    FPU_OP(add, 0) FPU_OP(sub, 1) FPU_OP(subr, 2) FPU_OP(mul, 3) FPU_OP(div, 4) FPU_OP(divr, 5)
#undef FPU_OP
    void fpu_compare(double a, double b);
    void fpu_com_st(int d, int s, bool) { fpu_compare(ST(d), ST(s)); }
    void fpu_comp_st(int d, int s, bool) { fpu_compare(ST(d), ST(s)); fpu_pop(); }
    void fpu_com_mem_f32(uint16_t sg, uint16_t o) { float f; uint32_t u = rd32(sg, o); std::memcpy(&f, &u, 4); fpu_compare(ST(0), f); }
    void fpu_com_mem_f64(uint16_t sg, uint16_t o) { double f; uint64_t u = rd64(sg, o); std::memcpy(&f, &u, 8); fpu_compare(ST(0), f); }
    void fpu_com_mem_i16(uint16_t sg, uint16_t o) { fpu_compare(ST(0), double(int16_t(rd16(sg, o)))); }
    void fpu_com_mem_i32(uint16_t sg, uint16_t o) { fpu_compare(ST(0), double(int32_t(rd32(sg, o)))); }
    void fpu_comp_mem_f32(uint16_t sg, uint16_t o) { fpu_com_mem_f32(sg, o); fpu_pop(); }
    void fpu_comp_mem_f64(uint16_t sg, uint16_t o) { fpu_com_mem_f64(sg, o); fpu_pop(); }
    void fpu_comp_mem_i16(uint16_t sg, uint16_t o) { fpu_com_mem_i16(sg, o); fpu_pop(); }
    void fpu_comp_mem_i32(uint16_t sg, uint16_t o) { fpu_com_mem_i32(sg, o); fpu_pop(); }
    void fpu_compp() { fpu_compare(ST(0), ST(1)); fpu_pop(); fpu_pop(); }
    void fpu_ffree(int) {}
    void fpu_fchs() { ST(0) = -ST(0); }
    void fpu_fabs() { ST(0) = std::fabs(ST(0)); }
    void fpu_fsqrt() { ST(0) = std::sqrt(ST(0)); }
    void fpu_frndint();
    void fpu_fld1() { fpu_push(1.0); }
    void fpu_fldz() { fpu_push(0.0); }
    void fpu_fldpi() { fpu_push(3.14159265358979323846); }
    void fpu_fldl2e() { fpu_push(1.44269504088896340736); }
    void fpu_fldln2() { fpu_push(0.693147180559945309417); }
    void fpu_fldlg2() { fpu_push(0.301029995663981195214); }
    void fpu_fldl2t() { fpu_push(3.32192809488736234787); }
    void fpu_ftst() { fpu_compare(ST(0), 0.0); }
    void fpu_fxam();
    void fpu_fscale() { ST(0) = std::ldexp(ST(0), int(std::trunc(ST(1)))); }
    void fpu_fprem() { ST(0) = std::fmod(ST(0), ST(1)); fpu_sw &= ~0x0400; }
    void fpu_fprem1() { ST(0) = std::remainder(ST(0), ST(1)); fpu_sw &= ~0x0400; }
    void fpu_f2xm1() { ST(0) = std::exp2(ST(0)) - 1.0; }
    void fpu_fyl2x() { double v = ST(1) * std::log2(ST(0)); fpu_pop(); ST(0) = v; }
    void fpu_fyl2xp1() { double v = ST(1) * std::log2(ST(0) + 1.0); fpu_pop(); ST(0) = v; }
    void fpu_fptan() { ST(0) = std::tan(ST(0)); fpu_push(1.0); }
    void fpu_fpatan() { double v = std::atan2(ST(1), ST(0)); fpu_pop(); ST(0) = v; }
    void fpu_fsin() { ST(0) = std::sin(ST(0)); }
    void fpu_fcos() { ST(0) = std::cos(ST(0)); }
    void fpu_fsincos() { double v = ST(0); ST(0) = std::sin(v); fpu_push(std::cos(v)); }
    void fpu_fxtract();
    void fpu_fninit() { top = 0; fpu_sw = 0; fpu_control = 0x037F; }
    void fpu_finit() { fpu_fninit(); }
    void fpu_fnclex() { fpu_sw &= 0x7F00; }
    void fpu_fclex() { fpu_fnclex(); }
    void fpu_fincstp() { top = (top + 1) & 7; }
    void fpu_fdecstp() { top = (top - 1) & 7; }
    void fpu_fwait() {}

    // ---- control flow into the runtime ------------------------------------
    void call_far(uint16_t sel, uint16_t off);    // indirect call: dispatch by address
    void jump_near(uint16_t sel, uint16_t off) { call_far(sel, off); }
    void interrupt(int n);
    [[noreturn]] void bad_function(int seg, int off, const char* why);
    [[noreturn]] void unsupported(int seg, int off, const char* what);
    [[noreturn]] void fell_off(int seg, int off);
    [[noreturn]] void bad_jump(int seg, uint16_t target);

private:
    uint8_t* fault(uint16_t sel, uint16_t off, unsigned size, bool write);
};

}  // namespace recomp
