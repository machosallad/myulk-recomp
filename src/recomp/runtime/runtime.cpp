#include "recomp/runtime/runtime.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <set>

#include "recomp/runtime/generated_support.hpp"

namespace recomp {

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[recomp] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}

TraceRing g_trace;

static std::string backtrace_text() {
    Runtime& rt = Runtime::get();
    Cpu& c = rt.cpu();
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "\n  regs: ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X ds=%04X es=%04X ss=%04X",
                  c.ax(), c.bx(), c.cx(), c.dx(), c.si(), c.di(), c.bp(), c.sp(), c.ds, c.es, c.ss);
    std::string s = buf;
    s += "\n  backtrace (BP chain, return addresses):";
    uint16_t bp = c.bp();
    Segment* st = rt.segment(c.ss);
    for (int i = 0; i < 24 && st; ++i) {
        uint16_t b = uint16_t(bp & ~1);
        if (b == 0 || size_t(b) + 6 > st->data.size()) break;
        uint16_t old = uint16_t(st->data[b] | (st->data[b + 1] << 8));
        uint16_t ip = uint16_t(st->data[b + 2] | (st->data[b + 3] << 8));
        uint16_t cs = uint16_t(st->data[b + 4] | (st->data[b + 5] << 8));
        uint16_t a0 = 0, a1 = 0;
        if (size_t(b) + 10 <= st->data.size()) {
            a0 = uint16_t(st->data[b + 6] | (st->data[b + 7] << 8));
            a1 = uint16_t(st->data[b + 8] | (st->data[b + 9] << 8));
        }
        std::snprintf(buf, sizeof buf, " %d:%04X(%04X:%04X)", cs >> 3, ip, a1, a0);
        s += buf;
        if (old <= bp) break;
        bp = old;
    }
    return s;
}

std::string watch_text();

void arg_size_mismatch(const char* name, int declared, int read) {
    static std::set<std::string> seen;
    if (seen.insert(name).second) log("API %s: declared %d argument bytes but read %d", name, declared, read);
}

static std::string trace_text() {
    std::string s = backtrace_text() + "\n  recent functions (oldest first):";
    // start shortly before the first entry that ran with a foreign DS, else show the last 64
    int start = TraceRing::kSize - 64;
    unsigned want = 0;
    if (const char* e = std::getenv("MYULK_DS_SEARCH")) std::sscanf(e, "%x", &want);
    for (int i = 0; i < TraceRing::kSize; ++i) {
        int k = (g_trace.pos + i) & (TraceRing::kSize - 1);
        if (want && g_trace.entries[k] && g_trace.ds[k] == want) {
            start = std::min(start, std::max(0, i - 24));
            break;
        }
    }
    for (int i = start; i < std::min(TraceRing::kSize, start + 80); ++i) {
        int k = (g_trace.pos + i) & (TraceRing::kSize - 1);
        uint32_t e = g_trace.entries[k];
        if (!e) continue;
        char buf[40];
        uint16_t d = g_trace.ds[k];
        if (d != Runtime::get().data_sel()) std::snprintf(buf, sizeof buf, " %u:%04X[ds=%04X]", e >> 16, e & 0xFFFF, d);
        else std::snprintf(buf, sizeof buf, " %u:%04X", e >> 16, e & 0xFFFF);
        s += buf;
    }
    return s + watch_text();
}

std::string recent_trace() {
    std::string s = trace_text();
    // the last game-code routines (segments 1-22), oldest first
    std::vector<uint32_t> game;
    for (int i = TraceRing::kSize - 1; i >= 0 && game.size() < 48; --i) {
        uint32_t e = g_trace.entries[(g_trace.pos + i) & (TraceRing::kSize - 1)];
        if (e && (e >> 16) >= 1 && (e >> 16) <= 22 && std::find(game.begin(), game.end(), e) == game.end())
            game.push_back(e);
    }
    s += "\n  recent game routines:";
    for (auto it = game.rbegin(); it != game.rend(); ++it) {
        char b[16];
        std::snprintf(b, sizeof b, " %u:%04X", *it >> 16, *it & 0xFFFF);
        s += b;
    }
    return s;
}

struct FatalError : std::runtime_error {
    explicit FatalError(const std::string& m) : std::runtime_error(m + trace_text()) {}
};

Runtime& Runtime::get() {
    static Runtime rt;
    return rt;
}

Runtime::Runtime() {
    segs_.resize(8192);
    cpu_.segs = &segs_;
    for (int i = 0; i < kFuncCount; ++i)
        funcs_[(uint32_t(kFuncs[i].sel) << 16) | kFuncs[i].off] = kFuncs[i].fn;
    segs_[kImportSel >> 3].used = true;
    segs_[kImportSel >> 3].name = "imports";
    segs_[kImportSel >> 3].data.resize(16);
}

// ---------------------------------------------------------------------------
// NE loading
// ---------------------------------------------------------------------------
static bool is_value_import(const std::string& module, int ordinal);

namespace {

uint16_t u16(const std::vector<uint8_t>& d, size_t o) { return uint16_t(d[o] | (d[o + 1] << 8)); }
uint32_t u32(const std::vector<uint8_t>& d, size_t o) { return u16(d, o) | (uint32_t(u16(d, o + 2)) << 16); }

}  // namespace

bool Runtime::load(const std::string& exePath) {
    exePath_ = exePath;
    std::ifstream in(exePath, std::ios::binary);
    if (!in) {
        log("cannot open %s", exePath.c_str());
        return false;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    size_t ne = u32(d, 0x3C);
    if (d[ne] != 'N' || d[ne + 1] != 'E') {
        log("%s is not an NE executable", exePath.c_str());
        return false;
    }
    int autoData = u16(d, ne + 0x0E);
    uint16_t heap = u16(d, ne + 0x10), stack = u16(d, ne + 0x12);
    int nseg = u16(d, ne + 0x1C), nmod = u16(d, ne + 0x1E);
    size_t segtab = ne + u16(d, ne + 0x22);
    size_t modref = ne + u16(d, ne + 0x28), imptab = ne + u16(d, ne + 0x2A);
    size_t enttab = ne + u16(d, ne + 0x04);
    size_t entlen = u16(d, ne + 0x06);
    int shift = u16(d, ne + 0x32);
    if (!shift) shift = 9;

    std::vector<std::string> modules;
    for (int i = 0; i < nmod; ++i) {
        size_t o = imptab + u16(d, modref + 2 * i);
        modules.emplace_back(reinterpret_cast<const char*>(&d[o + 1]), d[o]);
    }
    // entry table: ordinal -> (seg, off), for moveable-segment references
    std::unordered_map<int, std::pair<int, int>> entries;
    {
        size_t p = enttab, end = enttab + entlen;
        int ord = 1;
        while (p < end) {
            int count = d[p], type = d[p + 1];
            p += 2;
            if (!count) break;
            if (!type) { ord += count; continue; }
            for (int i = 0; i < count; ++i) {
                if (type == 0xFF) { entries[ord] = {d[p + 3], u16(d, p + 4)}; p += 6; }
                else { entries[ord] = {type, u16(d, p + 1)}; p += 3; }
                ++ord;
            }
        }
    }

    struct SegInfo { size_t off, len; uint16_t flags; size_t minAlloc; };
    std::vector<SegInfo> infos;
    for (int i = 0; i < nseg; ++i) {
        size_t o = segtab + 8 * i;
        size_t off = size_t(u16(d, o)) << shift;
        size_t len = u16(d, o + 2);
        if (!len && off) len = 0x10000;
        size_t minAlloc = u16(d, o + 6);
        if (!minAlloc) minAlloc = 0x10000;
        infos.push_back({off, len, u16(d, o + 4), minAlloc});
        Segment& s = segs_[sel_of(i + 1) >> 3];
        s.used = true;
        s.name = "seg" + std::to_string(i + 1);
        size_t size = std::max(len, minAlloc);
        if (i + 1 == autoData) {
            size = std::min<size_t>(0x10000, minAlloc + heap + stack + 0x100);
            dataSel_ = sel_of(i + 1);
        }
        s.data.assign(size, 0);
        if (off) std::memcpy(s.data.data(), &d[off], std::min(len, d.size() - off));
    }

    // relocations
    int nrel = 0;
    for (int i = 0; i < nseg; ++i) {
        const SegInfo& si = infos[i];
        if (!si.off || !(si.flags & 0x0100)) continue;
        Segment& s = segs_[sel_of(i + 1) >> 3];
        size_t p = si.off + si.len;
        int count = u16(d, p);
        p += 2;
        for (int k = 0; k < count; ++k, p += 8) {
            int src = d[p], flags = d[p + 1];
            uint16_t loc = u16(d, p + 2), a = u16(d, p + 4), b = u16(d, p + 6);
            bool additive = flags & 4;
            uint16_t tsel = 0, toff = 0;
            switch (flags & 3) {
                case 0:   // internal reference
                    if ((a & 0xFF) == 0xFF) {
                        auto e = entries[b];
                        tsel = sel_of(e.first);
                        toff = uint16_t(e.second);
                    } else {
                        tsel = sel_of(a & 0xFF);
                        toff = b;
                    }
                    break;
                case 1: {  // import by ordinal
                    const std::string& mod = modules[a - 1];
                    if (is_value_import(mod, b)) {
                        tsel = toff = import_value(mod, b);
                    } else {
                        uint32_t fp = import_far(mod, b);
                        tsel = uint16_t(fp >> 16);
                        toff = uint16_t(fp);
                    }
                    break;
                }
                case 2: {  // import by name: not used by this EXE
                    log("import by name in seg %d not supported", i + 1);
                    continue;
                }
                default:   // OS fixup (floating point emulation): keep the x87 opcodes
                    continue;
            }
            uint16_t at = loc;
            for (int guard = 0; guard < 0x8000; ++guard) {
                if (size_t(at) + 4 > s.data.size()) break;
                uint16_t next = uint16_t(s.data[at] | (s.data[at + 1] << 8));
                switch (src) {
                    case 0: s.data[at] = uint8_t(toff); break;
                    case 2: {
                        uint16_t v = additive ? uint16_t(next + tsel) : tsel;
                        s.data[at] = uint8_t(v); s.data[at + 1] = uint8_t(v >> 8);
                        break;
                    }
                    case 3: {
                        uint16_t o2 = additive ? uint16_t(next + toff) : toff;
                        s.data[at] = uint8_t(o2); s.data[at + 1] = uint8_t(o2 >> 8);
                        s.data[at + 2] = uint8_t(tsel); s.data[at + 3] = uint8_t(tsel >> 8);
                        break;
                    }
                    case 5: {
                        uint16_t o2 = additive ? uint16_t(next + toff) : toff;
                        s.data[at] = uint8_t(o2); s.data[at + 1] = uint8_t(o2 >> 8);
                        break;
                    }
                    default: break;
                }
                ++nrel;
                if (additive || next == 0xFFFF) break;
                at = next;
            }
        }
    }

    if (const char* w = std::getenv("MYULK_WATCH")) {
        unsigned s = 0, o = 0;
        if (std::sscanf(w, "%x:%x", &s, &o) == 2) { cpu_.watch_sel = uint16_t(s); cpu_.watch_off = uint16_t(o); }
        cpu_.watch_reads = std::getenv("MYULK_WATCH_READ") != nullptr;
    }
    // registers as Windows sets them up at task start
    cpu_.ds = cpu_.ss = dataSel_;
    Segment& dg = segs_[dataSel_ >> 3];
    cpu_.set_sp(uint16_t((dg.data.size() - 2) & ~1u));
    cpu_.set_bp(0);
    cpu_.set_bx(stack);
    cpu_.set_cx(heap);
    log("loaded %s: %d segments, %d relocations applied, DGROUP %zu bytes",
        exePath.c_str(), nseg, nrel, dg.data.size());
    return true;
}

// ---------------------------------------------------------------------------
// segments
// ---------------------------------------------------------------------------
uint16_t Runtime::alloc_segment(size_t size, const std::string& name) {
    for (size_t i = 64; i < segs_.size(); ++i) {
        if (i == (kImportSel >> 3) || segs_[i].used) continue;
        segs_[i].used = true;
        segs_[i].alias = 0;
        segs_[i].name = name;
        segs_[i].data.assign(std::max<size_t>(size, 1), 0);
        return uint16_t((i << 3) | 7);
    }
    throw FatalError("out of selectors");
}

void Runtime::free_segment(uint16_t sel) {
    size_t i = sel >> 3;
    if (i >= 64 && i < segs_.size() && i != (kImportSel >> 3)) {
        segs_[i].used = false;
        segs_[i].alias = 0;
        segs_[i].data.clear();
        segs_[i].data.shrink_to_fit();
    }
}

void Runtime::resize_segment(uint16_t sel, size_t size) {
    if (Segment* s = segment(sel)) s->data.resize(std::max<size_t>(size, 1), 0);
}

Segment* Runtime::segment(uint16_t sel) {
    size_t i = sel >> 3;
    if (i < segs_.size() && segs_[i].used) {
        if (segs_[i].alias) return &segs_[segs_[i].alias];
        return &segs_[i];
    }
    return nullptr;
}

uint16_t Runtime::alias_segment(uint16_t sel) {
    size_t target = sel >> 3;
    if (segs_[target].alias) target = segs_[target].alias;
    uint16_t a = alloc_segment(1, "alias");
    segs_[a >> 3].data.clear();
    segs_[a >> 3].alias = uint32_t(target);
    return a;
}

void Runtime::set_alias(uint16_t dst, uint16_t src) {
    if ((dst >> 3) == (src >> 3)) return;   // type change in place: nothing to do here
    size_t target = src >> 3;
    if (segs_[target].alias) target = segs_[target].alias;
    segs_[dst >> 3].used = true;
    segs_[dst >> 3].data.clear();
    segs_[dst >> 3].alias = uint32_t(target);
}

// ---------------------------------------------------------------------------
// imports
// ---------------------------------------------------------------------------
void Runtime::register_api(const std::string& module, int ordinal, ApiFn fn, const char* name) {
    apis_[module + "." + std::to_string(ordinal)] = Api{fn, name};
}

void Runtime::call_import(Cpu& c, const std::string& module, int ordinal, const char* name) {
    static const char* traceFilter = std::getenv("MYULK_TRACE_API");   // "1" = all, else module name
    auto it = apis_.find(module + "." + std::to_string(ordinal));
    if (it != apis_.end() && std::getenv("MYULK_API_STATS")) ++apiCounts_[it->second.name];
    if (traceFilter && (std::string(traceFilter) == "1" || module == traceFilter)) {
        std::string args;
        Segment* st = segment(c.ss);
        for (int i = 0; i < 6; ++i) {
            size_t o = size_t(c.sp()) + 4 + 2 * i;
            if (!st || o + 2 > st->data.size()) break;
            char b[8];
            std::snprintf(b, sizeof b, " %04X", c.rd16(c.ss, uint16_t(o)));
            args += b;
        }
        log("API %s.%d %s  stack:%s", module.c_str(), ordinal,
            it != apis_.end() ? it->second.name.c_str() : (name ? name : "?"), args.c_str());
    }
    if (it == apis_.end()) {
        throw FatalError("unimplemented Windows function " + module + "." + std::to_string(ordinal) +
                         " (" + (name ? name : "?") + ")");
    }
    it->second.fn(c);
}

uint32_t Runtime::import_far(const std::string& module, int ordinal) {
    for (size_t i = 0; i < importSlots_.size(); ++i)
        if (importSlots_[i].first == module && importSlots_[i].second == ordinal)
            return (uint32_t(kImportSel) << 16) | uint32_t(i);
    importSlots_.emplace_back(module, ordinal);
    return (uint32_t(kImportSel) << 16) | uint32_t(importSlots_.size() - 1);
}

void Runtime::call_import_index(Cpu& c, uint16_t index) {
    if (index >= importSlots_.size()) throw FatalError("bad import thunk call");
    call_import(c, importSlots_[index].first, importSlots_[index].second, nullptr);
}

// KERNEL exports that are values, not functions.
ApiFn Runtime::find_api(const std::string& module, int ordinal) const {
    auto it = apis_.find(module + "." + std::to_string(ordinal));
    return it == apis_.end() ? nullptr : it->second.fn;
}

void Runtime::print_api_stats() {
    std::vector<std::pair<int, std::string>> v;
    for (auto& [n, k] : apiCounts_) v.emplace_back(k, n);
    std::sort(v.rbegin(), v.rend());
    std::string s;
    for (auto& [k, n] : v) s += " " + n + "=" + std::to_string(k);
    log("API call counts:%s", s.c_str());
}

uint16_t Runtime::import_value(const std::string& module, int ordinal) {
    if (module == "KERNEL") {
        switch (ordinal) {
            case 113: return 3;        // __AHSHIFT
            case 114: return 8;        // __AHINCR
            case 178: return 0x0425;   // __WINFLAGS: PMODE | CPU386 | ENHANCED | 80x87
            default: break;
        }
    }
    log("import value %s.%d used: returning 0", module.c_str(), ordinal);
    return 0;
}

Fn Runtime::lookup(uint16_t sel, uint16_t off) const {
    auto it = funcs_.find((uint32_t(sel) << 16) | off);
    return it == funcs_.end() ? nullptr : it->second;
}

void Runtime::call(uint16_t sel, uint16_t off) {
    cpu_.push16(0xFFFF);   // fake far return address
    cpu_.push16(0xFFFF);
    cpu_.call_far(sel, off);
}

// ---------------------------------------------------------------------------
// Cpu hooks implemented by the runtime
// ---------------------------------------------------------------------------
void Cpu::call_far(uint16_t sel, uint16_t off) {
    if (sel == kImportSel) {
        Runtime::get().call_import_index(*this, off);
        return;
    }
    if (Fn fn = Runtime::get().lookup(sel, off)) {
        fn(*this);
        return;
    }
    // Code generated at runtime by VCL's MakeObjectInstance:
    //   instance: E8 rel16 (call stub) + 8-byte method pointer
    //   stub:     8C CB (mov bx,cs) 8E C3 (mov es,bx) 5B (pop bx) EA off seg (jmp far)
    // Effect: ES:BX = address of the method record, then jump to StdWndProc.
    if (Segment* s = Runtime::get().segment(sel)) {
        const auto& d = s->data;
        if (size_t(off) + 3 <= d.size() && d[off] == 0xE8) {
            uint16_t stub = uint16_t(off + 3 + (d[off + 1] | (d[off + 2] << 8)));
            if (size_t(stub) + 10 <= d.size() && d[stub] == 0x8C && d[stub + 1] == 0xCB && d[stub + 2] == 0x8E &&
                d[stub + 3] == 0xC3 && d[stub + 4] == 0x5B && d[stub + 5] == 0xEA) {
                uint16_t toff = uint16_t(d[stub + 6] | (d[stub + 7] << 8));
                uint16_t tsel = uint16_t(d[stub + 8] | (d[stub + 9] << 8));
                set_bx(uint16_t(off + 3));
                es = sel;
                call_far(tsel, toff);
                return;
            }
        }
    }
    std::string bytes;
    if (Segment* s = Runtime::get().segment(sel)) {
        for (unsigned i = 0; i < 24 && size_t(off) + i < s->data.size(); ++i) {
            char b[4];
            std::snprintf(b, sizeof b, "%02X ", s->data[off + i]);
            bytes += b;
        }
        bytes += "| block start: ";
        for (unsigned i = 0; i < 32 && i < s->data.size(); ++i) {
            char b[4];
            std::snprintf(b, sizeof b, "%02X ", s->data[i]);
            bytes += b;
        }
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "call to unknown code %04X:%04X (seg %d) bytes: ", sel, off, sel >> 3);
    throw FatalError(buf + bytes);
}

uint8_t* Cpu::fault(uint16_t sel, uint16_t off, unsigned size, bool write) {
    char buf[256];
    size_t idx = sel >> 3;
    const Segment* s = idx < segs->size() ? &(*segs)[idx] : nullptr;
    if (s && s->alias) s = &(*segs)[s->alias];
    std::snprintf(buf, sizeof buf, "general protection fault: %s %u bytes at %04X:%04X (segment '%s', %zu bytes, %s)",
                  write ? "write" : "read", size, sel, off, s ? s->name.c_str() : "?",
                  s ? s->data.size() : size_t(0), s && s->used ? "allocated" : "not allocated");
    throw FatalError(buf);
}

// MYULK_WATCH_QUIET: keep the last watch hits in memory (no logging, so the
// timing stays the same) and print them with a fatal error.
namespace {
struct WatchRec { uint16_t old, sp, bp; uint32_t trace[4]; };
constexpr unsigned kWatchRing = 8192;
WatchRec g_watchRing[kWatchRing];
unsigned g_watchPos = 0;
}

// Prints the last 32 hits, or with MYULK_WATCH_VALUE=hex the 24 hits up to
// the first one that found that value in the watched word.
std::string watch_text() {
    std::string out;
    unsigned first = kWatchRing - 32, last = kWatchRing;
    if (const char* v = std::getenv("MYULK_WATCH_VALUE")) {
        unsigned want = 0;
        std::sscanf(v, "%x", &want);
        for (unsigned i = 0; i < kWatchRing; ++i)
            if (g_watchRing[(g_watchPos + i) % kWatchRing].sp && g_watchRing[(g_watchPos + i) % kWatchRing].old == want) {
                first = i >= 24 ? i - 24 : 0;
                last = i + 1;
                break;
            }
    }
    for (unsigned i = first; i < last; ++i) {
        const WatchRec& w = g_watchRing[(g_watchPos + i) % kWatchRing];
        if (!w.sp) continue;
        char b[128];
        std::snprintf(b, sizeof b, "\n  watch: old=%04X sp=%04X bp=%04X in", w.old, w.sp, w.bp);
        out += b;
        for (uint32_t e : w.trace) {
            std::snprintf(b, sizeof b, " %u:%04X", e >> 16, e & 0xFFFF);
            out += b;
        }
    }
    return out;
}

void Cpu::watch_hit(uint16_t sel, uint16_t off, unsigned size) {
    static const bool quiet = std::getenv("MYULK_WATCH_QUIET") != nullptr;
    if (quiet) {
        static const bool firstOnly = std::getenv("MYULK_WATCH_FIRST") != nullptr;
        if (firstOnly && g_watchPos >= kWatchRing) return;
        WatchRec& w = g_watchRing[g_watchPos++ % kWatchRing];
        uint16_t saved = watch_sel;
        watch_sel = 0;
        w.old = rd16(sel, off);
        watch_sel = saved;
        w.sp = sp(); w.bp = bp();
        for (int i = 0; i < 4; ++i)
            w.trace[i] = g_trace.entries[(g_trace.pos + TraceRing::kSize - 4 + i) & (TraceRing::kSize - 1)];
        return;
    }
    std::string recent;
    for (int i = TraceRing::kSize - 6; i < TraceRing::kSize; ++i) {
        uint32_t e = g_trace.entries[(g_trace.pos + i) & (TraceRing::kSize - 1)];
        char b[16];
        std::snprintf(b, sizeof b, " %u:%04X", e >> 16, e & 0xFFFF);
        recent += b;
    }
    log("watch: access %u bytes at %04X:%04X%s\n  last entered:%s", size, sel, off, backtrace_text().c_str(),
        recent.c_str());
}

void (*g_interrupt_handler)(Cpu&, int) = nullptr;

void Cpu::interrupt(int n) {
    if (g_interrupt_handler) {
        g_interrupt_handler(*this, n);
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "unhandled interrupt %02X", n);
    throw FatalError(buf);
}

void Cpu::bad_function(int seg, int off, const char* why) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "entered undecodable function %d:%04X (%s)", seg, off, why);
    throw FatalError(buf);
}

void Cpu::unsupported(int seg, int off, const char* what) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "unsupported instruction at %d:%04X: %s", seg, off, what);
    throw FatalError(buf);
}

void Cpu::fell_off(int seg, int off) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "execution reached data at %d:%04X", seg, off);
    throw FatalError(buf);
}

void Cpu::bad_jump(int seg, uint16_t target) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "jump table in seg %d to unknown target %04X", seg, target);
    throw FatalError(buf);
}

void imp_call(Cpu& c, int id) {
    // resolve each import id once; fall back to the slow path for tracing/stats
    static std::vector<ApiFn> cache(size_t(kImportCount), nullptr);
    static const bool slow = std::getenv("MYULK_TRACE_API") || std::getenv("MYULK_API_STATS");
    ApiFn fn = slow ? nullptr : cache[size_t(id)];
    if (fn) { fn(c); return; }
    const ImportInfo& i = kImports[id];
    if (!slow) cache[size_t(id)] = Runtime::get().find_api(i.module, i.ordinal);
    Runtime::get().call_import(c, i.module, i.ordinal, i.name);
}

static bool is_value_import(const std::string& module, int ordinal) {
    return module == "KERNEL" && (ordinal == 113 || ordinal == 114 || ordinal == 178);
}

uint16_t imp_seg(int id) {
    const ImportInfo& i = kImports[id];
    if (is_value_import(i.module, i.ordinal)) return Runtime::get().import_value(i.module, i.ordinal);
    return uint16_t(Runtime::get().import_far(i.module, i.ordinal) >> 16);
}

uint16_t imp_off(int id) {
    const ImportInfo& i = kImports[id];
    if (is_value_import(i.module, i.ordinal)) return Runtime::get().import_value(i.module, i.ordinal);
    return uint16_t(Runtime::get().import_far(i.module, i.ordinal));
}

// ---------------------------------------------------------------------------
std::string read_cstr(Cpu& c, uint32_t fp, size_t max) {
    std::string s;
    uint16_t sel = uint16_t(fp >> 16), off = uint16_t(fp);
    if (!sel) return s;
    for (size_t i = 0; i < max; ++i) {
        uint8_t ch = c.rd8(sel, uint16_t(off + i));
        if (!ch) break;
        s += char(ch);
    }
    return s;
}

void write_cstr(Cpu& c, uint32_t fp, const std::string& s, size_t max) {
    if (!max) return;
    uint16_t sel = uint16_t(fp >> 16), off = uint16_t(fp);
    size_t n = std::min(s.size(), max - 1);
    for (size_t i = 0; i < n; ++i) c.wr8(sel, uint16_t(off + i), uint8_t(s[i]));
    c.wr8(sel, uint16_t(off + n), 0);
}

}  // namespace recomp
