#include "recomp/runtime/cpu.hpp"

#include <cfloat>
#include <cstdio>
#include <cstdlib>

namespace recomp {

// ---- stack ------------------------------------------------------------------

void Cpu::pusha() {
    uint16_t s = sp();
    push16(ax()); push16(cx()); push16(dx()); push16(bx());
    push16(s); push16(bp()); push16(si()); push16(di());
}

void Cpu::popa() {
    set_di(pop16()); set_si(pop16()); set_bp(pop16()); pop16();
    set_bx(pop16()); set_dx(pop16()); set_cx(pop16()); set_ax(pop16());
}

void Cpu::pushad() {
    uint32_t s = esp();
    push32(eax()); push32(ecx()); push32(edx()); push32(ebx());
    push32(s); push32(ebp()); push32(esi()); push32(edi());
}

void Cpu::popad() {
    set_edi(pop32()); set_esi(pop32()); set_ebp(pop32()); pop32();
    set_ebx(pop32()); set_edx(pop32()); set_ecx(pop32()); set_eax(pop32());
}

void Cpu::enter(int size, int level) {
    push16(bp());
    uint16_t frame = sp();
    level &= 31;
    if (level > 0) {
        uint16_t b = bp();
        for (int i = 1; i < level; ++i) {
            b = uint16_t(b - 2);
            push16(rd16(ss, b));
        }
        push16(frame);
    }
    set_bp(frame);
    set_sp(uint16_t(sp() - size));
}

// ---- shifts with two operands ---------------------------------------------------

uint16_t Cpu::shld16(uint16_t a, uint16_t b, uint8_t n) {
    n &= 31;
    if (!n) return a;
    uint32_t v = (uint32_t(a) << 16) | b;
    uint16_t r_ = uint16_t((v << n) >> 16);
    cf = (a >> (16 - n)) & 1;
    szp16(r_);
    return r_;
}

uint16_t Cpu::shrd16(uint16_t a, uint16_t b, uint8_t n) {
    n &= 31;
    if (!n) return a;
    uint32_t v = (uint32_t(b) << 16) | a;
    uint16_t r_ = uint16_t(v >> n);
    cf = (a >> (n - 1)) & 1;
    szp16(r_);
    return r_;
}

uint32_t Cpu::shld32(uint32_t a, uint32_t b, uint8_t n) {
    n &= 31;
    if (!n) return a;
    uint64_t v = (uint64_t(a) << 32) | b;
    uint32_t r_ = uint32_t((v << n) >> 32);
    cf = (a >> (32 - n)) & 1;
    szp32(r_);
    return r_;
}

uint32_t Cpu::shrd32(uint32_t a, uint32_t b, uint8_t n) {
    n &= 31;
    if (!n) return a;
    uint64_t v = (uint64_t(b) << 32) | a;
    uint32_t r_ = uint32_t(v >> n);
    cf = (a >> (n - 1)) & 1;
    szp32(r_);
    return r_;
}

// ---- multiply / divide ----------------------------------------------------------

void Cpu::mul8(uint8_t v) {
    uint16_t r_ = uint16_t(al() * v);
    set_ax(r_);
    cf = of = (r_ >> 8) != 0;
}

void Cpu::mul16(uint16_t v) {
    uint32_t r_ = uint32_t(ax()) * v;
    set_ax(uint16_t(r_));
    set_dx(uint16_t(r_ >> 16));
    cf = of = (r_ >> 16) != 0;
}

void Cpu::mul32(uint32_t v) {
    uint64_t r_ = uint64_t(eax()) * v;
    set_eax(uint32_t(r_));
    set_edx(uint32_t(r_ >> 32));
    cf = of = (r_ >> 32) != 0;
}

void Cpu::imul8(uint8_t v) {
    int16_t r_ = int16_t(int8_t(al()) * int8_t(v));
    set_ax(uint16_t(r_));
    cf = of = r_ != int8_t(r_);
}

void Cpu::imul16(uint16_t v) {
    int32_t r_ = int32_t(int16_t(ax())) * int16_t(v);
    set_ax(uint16_t(r_));
    set_dx(uint16_t(uint32_t(r_) >> 16));
    cf = of = r_ != int16_t(r_);
}

void Cpu::imul32(uint32_t v) {
    int64_t r_ = int64_t(int32_t(eax())) * int32_t(v);
    set_eax(uint32_t(r_));
    set_edx(uint32_t(uint64_t(r_) >> 32));
    cf = of = r_ != int32_t(r_);
}

uint16_t Cpu::imul16_2(uint16_t a, uint16_t b) {
    int32_t r_ = int32_t(int16_t(a)) * int16_t(b);
    cf = of = r_ != int16_t(r_);
    return uint16_t(r_);
}

uint32_t Cpu::imul32_2(uint32_t a, uint32_t b) {
    int64_t r_ = int64_t(int32_t(a)) * int32_t(b);
    cf = of = r_ != int32_t(r_);
    return uint32_t(r_);
}

static void divide_error(Cpu& c) {
    // Delphi installs an exception handler for INT 0; report and continue.
    std::fprintf(stderr, "[recomp] divide error\n");
    c.interrupt(0);
}

void Cpu::div8(uint8_t v) {
    if (!v) return divide_error(*this);
    uint16_t a = ax();
    uint16_t q = a / v;
    if (q > 0xFF) return divide_error(*this);
    set_al(uint8_t(q));
    set_ah(uint8_t(a % v));
}

void Cpu::div16(uint16_t v) {
    if (!v) return divide_error(*this);
    uint32_t a = (uint32_t(dx()) << 16) | ax();
    uint32_t q = a / v;
    if (q > 0xFFFF) return divide_error(*this);
    set_ax(uint16_t(q));
    set_dx(uint16_t(a % v));
}

void Cpu::div32(uint32_t v) {
    if (!v) return divide_error(*this);
    uint64_t a = (uint64_t(edx()) << 32) | eax();
    uint64_t q = a / v;
    if (q > 0xFFFFFFFFull) return divide_error(*this);
    set_eax(uint32_t(q));
    set_edx(uint32_t(a % v));
}

void Cpu::idiv8(uint8_t v) {
    if (!v) return divide_error(*this);
    int16_t a = int16_t(ax());
    int q = a / int8_t(v);
    if (q > 127 || q < -128) return divide_error(*this);
    set_al(uint8_t(q));
    set_ah(uint8_t(a % int8_t(v)));
}

void Cpu::idiv16(uint16_t v) {
    if (!v) return divide_error(*this);
    int32_t a = int32_t((uint32_t(dx()) << 16) | ax());
    int32_t q = a / int16_t(v);
    if (q > 32767 || q < -32768) return divide_error(*this);
    set_ax(uint16_t(q));
    set_dx(uint16_t(a % int16_t(v)));
}

void Cpu::idiv32(uint32_t v) {
    if (!v) return divide_error(*this);
    int64_t a = int64_t((uint64_t(edx()) << 32) | eax());
    int64_t q = a / int32_t(v);
    if (q > INT32_MAX || q < INT32_MIN) return divide_error(*this);
    set_eax(uint32_t(q));
    set_edx(uint32_t(a % int32_t(v)));
}

// ---- BCD ----------------------------------------------------------------------------

void Cpu::daa() {
    uint8_t a = al();
    bool c = cf;
    if ((a & 0xF) > 9 || af) { set_al(uint8_t(al() + 6)); af = true; } else af = false;
    if (a > 0x99 || c) { set_al(uint8_t(al() + 0x60)); cf = true; } else cf = false;
    szp8(al());
}

void Cpu::das() {
    uint8_t a = al();
    bool c = cf;
    if ((a & 0xF) > 9 || af) { set_al(uint8_t(al() - 6)); af = true; } else af = false;
    if (a > 0x99 || c) { set_al(uint8_t(al() - 0x60)); cf = true; } else cf = false;
    szp8(al());
}

void Cpu::aaa() {
    if ((al() & 0xF) > 9 || af) { set_ax(uint16_t(ax() + 0x106)); af = cf = true; } else af = cf = false;
    set_al(al() & 0xF);
}

void Cpu::aas() {
    if ((al() & 0xF) > 9 || af) { set_ax(uint16_t(ax() - 6)); set_ah(uint8_t(ah() - 1)); af = cf = true; } else af = cf = false;
    set_al(al() & 0xF);
}

void Cpu::aam(int base) {
    if (!base) return divide_error(*this);
    uint8_t a = al();
    set_ah(uint8_t(a / base));
    set_al(uint8_t(a % base));
    szp8(al());
}

void Cpu::aad(int base) {
    set_al(uint8_t(al() + ah() * base));
    set_ah(0);
    szp8(al());
}

// ---- string instructions ---------------------------------------------------------------

#define STEP(n) (df ? -(n) : (n))
#define REP_LOOP(body)                                                     \
    if (!rep) { body; return; }                                              \
    while (cx()) { body; set_cx(uint16_t(cx() - 1)); }
#define REP_LOOP_COND(body)                                                \
    if (!rep) { body; return; }                                              \
    while (cx()) { body; set_cx(uint16_t(cx() - 1)); if (rep == 1 ? !zf : zf) break; }

void Cpu::movs8(uint16_t s, int rep) { REP_LOOP(wr8(es, di(), rd8(s, si())); set_si(uint16_t(si() + STEP(1))); set_di(uint16_t(di() + STEP(1)))) }
void Cpu::movs16(uint16_t s, int rep) { REP_LOOP(wr16(es, di(), rd16(s, si())); set_si(uint16_t(si() + STEP(2))); set_di(uint16_t(di() + STEP(2)))) }
void Cpu::movs32(uint16_t s, int rep) { REP_LOOP(wr32(es, di(), rd32(s, si())); set_si(uint16_t(si() + STEP(4))); set_di(uint16_t(di() + STEP(4)))) }
void Cpu::stos8(uint16_t, int rep) { REP_LOOP(wr8(es, di(), al()); set_di(uint16_t(di() + STEP(1)))) }
void Cpu::stos16(uint16_t, int rep) { REP_LOOP(wr16(es, di(), ax()); set_di(uint16_t(di() + STEP(2)))) }
void Cpu::stos32(uint16_t, int rep) { REP_LOOP(wr32(es, di(), eax()); set_di(uint16_t(di() + STEP(4)))) }
void Cpu::lods8(uint16_t s, int rep) { REP_LOOP(set_al(rd8(s, si())); set_si(uint16_t(si() + STEP(1)))) }
void Cpu::lods16(uint16_t s, int rep) { REP_LOOP(set_ax(rd16(s, si())); set_si(uint16_t(si() + STEP(2)))) }
void Cpu::lods32(uint16_t s, int rep) { REP_LOOP(set_eax(rd32(s, si())); set_si(uint16_t(si() + STEP(4)))) }
void Cpu::cmps8(uint16_t s, int rep) { REP_LOOP_COND(sub8(rd8(s, si()), rd8(es, di())); set_si(uint16_t(si() + STEP(1))); set_di(uint16_t(di() + STEP(1)))) }
void Cpu::cmps16(uint16_t s, int rep) { REP_LOOP_COND(sub16(rd16(s, si()), rd16(es, di())); set_si(uint16_t(si() + STEP(2))); set_di(uint16_t(di() + STEP(2)))) }
void Cpu::scas8(uint16_t, int rep) { REP_LOOP_COND(sub8(al(), rd8(es, di())); set_di(uint16_t(di() + STEP(1)))) }
void Cpu::scas16(uint16_t, int rep) { REP_LOOP_COND(sub16(ax(), rd16(es, di())); set_di(uint16_t(di() + STEP(2)))) }

#undef STEP
#undef REP_LOOP
#undef REP_LOOP_COND

// ---- FPU ----------------------------------------------------------------------------------

static double from_f80(const uint8_t* p) {
    uint64_t mant;
    uint16_t se;
    std::memcpy(&mant, p, 8);
    std::memcpy(&se, p + 8, 2);
    int exp = se & 0x7FFF;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (exp == 0 && mant == 0) return 0.0 * sign;
    if (exp == 0x7FFF) return (mant << 1) ? NAN : sign * INFINITY;
    return sign * std::ldexp(double(mant), exp - 16383 - 63);
}

static void to_f80(double v, uint8_t* p) {
    uint64_t mant = 0;
    uint16_t se = 0;
    if (std::signbit(v)) se = 0x8000;
    double a = std::fabs(v);
    if (std::isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (std::isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (a != 0.0) {
        int e;
        double m = std::frexp(a, &e);   // a = m * 2^e, m in [0.5, 1)
        mant = uint64_t(std::ldexp(m, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    std::memcpy(p, &mant, 8);
    std::memcpy(p + 8, &se, 2);
}

void Cpu::fpu_load_f32(uint16_t s, uint16_t o) { float f; uint32_t u = rd32(s, o); std::memcpy(&f, &u, 4); fpu_push(f); }
void Cpu::fpu_load_f64(uint16_t s, uint16_t o) { double f; uint64_t u = rd64(s, o); std::memcpy(&f, &u, 8); fpu_push(f); }
void Cpu::fpu_load_f80(uint16_t s, uint16_t o) { fpu_push(from_f80(ptr(s, o, 10, false))); }
void Cpu::fpu_load_i16(uint16_t s, uint16_t o) { fpu_push(double(int16_t(rd16(s, o)))); }
void Cpu::fpu_load_i32(uint16_t s, uint16_t o) { fpu_push(double(int32_t(rd32(s, o)))); }
void Cpu::fpu_load_i64(uint16_t s, uint16_t o) { fpu_push(double(int64_t(rd64(s, o)))); }

void Cpu::fpu_store_f32(uint16_t s, uint16_t o, bool pop) {
    float f = float(ST(0)); uint32_t u; std::memcpy(&u, &f, 4); wr32(s, o, u); if (pop) fpu_pop();
}
void Cpu::fpu_store_f64(uint16_t s, uint16_t o, bool pop) {
    double f = ST(0); uint64_t u; std::memcpy(&u, &f, 8); wr64(s, o, u); if (pop) fpu_pop();
}
void Cpu::fpu_store_f80(uint16_t s, uint16_t o, bool pop) {
    to_f80(ST(0), ptr(s, o, 10, true)); if (pop) fpu_pop();
}

// Integer stores honour the rounding control (bits 10-11 of the control word).
static double fpu_round(double v, uint16_t control) {
    switch ((control >> 10) & 3) {
        case 0: return std::nearbyint(v);    // to nearest (even)
        case 1: return std::floor(v);
        case 2: return std::ceil(v);
        default: return std::trunc(v);
    }
}

void Cpu::fpu_store_i16(uint16_t s, uint16_t o, bool pop) {
    double v = fpu_round(ST(0), fpu_control);
    wr16(s, o, (v >= -32768.0 && v <= 32767.0) ? uint16_t(int16_t(v)) : 0x8000);
    if (pop) fpu_pop();
}
void Cpu::fpu_store_i32(uint16_t s, uint16_t o, bool pop) {
    double v = fpu_round(ST(0), fpu_control);
    wr32(s, o, (v >= -2147483648.0 && v <= 2147483647.0) ? uint32_t(int32_t(v)) : 0x80000000u);
    if (pop) fpu_pop();
}
void Cpu::fpu_store_i64(uint16_t s, uint16_t o, bool pop) {
    double v = fpu_round(ST(0), fpu_control);
    wr64(s, o, uint64_t(int64_t(v)));
    if (pop) fpu_pop();
}

void Cpu::fpu_fbstp(uint16_t s, uint16_t o) {
    double v = fpu_round(ST(0), fpu_control);
    bool neg = v < 0;
    uint64_t n = uint64_t(std::fabs(v));
    for (int i = 0; i < 9; ++i) {
        uint8_t lo = uint8_t(n % 10); n /= 10;
        uint8_t hi = uint8_t(n % 10); n /= 10;
        wr8(s, uint16_t(o + i), uint8_t((hi << 4) | lo));
    }
    wr8(s, uint16_t(o + 9), neg ? 0x80 : 0);
    fpu_pop();
}

void Cpu::fpu_fbld(uint16_t s, uint16_t o) {
    double v = 0, scale = 1;
    for (int i = 0; i < 9; ++i) {
        uint8_t b = rd8(s, uint16_t(o + i));
        v += (b & 15) * scale; scale *= 10;
        v += (b >> 4) * scale; scale *= 10;
    }
    if (rd8(s, uint16_t(o + 9)) & 0x80) v = -v;
    fpu_push(v);
}

void Cpu::fpu_arith(int op, double& d, double s) {
    switch (op) {
        case 0: d = d + s; break;
        case 1: d = d - s; break;
        case 2: d = s - d; break;
        case 3: d = d * s; break;
        case 4: d = d / s; break;
        case 5: d = s / d; break;
    }
}

// C3/C2/C0 = bits 14/10/8 of the status word.
void Cpu::fpu_compare(double a, double b) {
    fpu_sw &= ~0x4500;
    if (std::isnan(a) || std::isnan(b)) fpu_sw |= 0x4500;
    else if (a < b) fpu_sw |= 0x0100;
    else if (a == b) fpu_sw |= 0x4000;
}

void Cpu::fpu_frndint() { ST(0) = fpu_round(ST(0), fpu_control); }

void Cpu::fpu_fxam() {
    double v = ST(0);
    fpu_sw &= ~0x4700;
    if (std::signbit(v)) fpu_sw |= 0x0200;   // C1 = sign
    if (std::isnan(v)) fpu_sw |= 0x0100;
    else if (std::isinf(v)) fpu_sw |= 0x0500;
    else if (v == 0.0) fpu_sw |= 0x4000;
    else fpu_sw |= 0x0400;                    // normal
}

void Cpu::fpu_fxtract() {
    double v = ST(0);
    int e = 0;
    double m = std::frexp(v, &e);
    ST(0) = double(e - 1);
    fpu_push(m * 2.0);
}

}  // namespace recomp
