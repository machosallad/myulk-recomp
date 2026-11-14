// KERNEL: tasks, global memory, modules/resources, files, profile strings;
// plus DOS INT 21h, WIN87EM, TOOLHELP and KEYBOARD.
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/modules.hpp"
#include "recomp/win16/vfs.hpp"
#include "recomp/win16/win16.hpp"

using recomp::Args;
using recomp::Cpu;
using recomp::Runtime;

namespace win16 {

namespace {

uint16_t g_psp = 0;
std::map<uint16_t, uint32_t> g_globalSize;   // handle -> bytes (huge blocks span selectors)

uint16_t sel_of(uint32_t fp) { return uint16_t(fp >> 16); }
uint16_t off_of(uint32_t fp) { return uint16_t(fp); }

// Copy bytes between host memory and emulated huge memory (sel:off, 64K chunks
// at consecutive selectors spaced by __AHINCR = 8).
void huge_copy_out(Cpu& c, uint32_t fp, const uint8_t* src, uint32_t n) {
    uint16_t sel = sel_of(fp);
    uint32_t off = off_of(fp);
    for (uint32_t i = 0; i < n; ++i, ++off) {
        if (off >= 0x10000) { off -= 0x10000; sel = uint16_t(sel + 8); }
        c.wr8(sel, uint16_t(off), src[i]);
    }
}

void huge_copy_in(Cpu& c, uint32_t fp, uint8_t* dst, uint32_t n) {
    uint16_t sel = sel_of(fp);
    uint32_t off = off_of(fp);
    for (uint32_t i = 0; i < n; ++i, ++off) {
        if (off >= 0x10000) { off -= 0x10000; sel = uint16_t(sel + 8); }
        dst[i] = c.rd8(sel, uint16_t(off));
    }
}

uint16_t global_alloc(uint32_t size, const char* tag) {
    auto& rt = Runtime::get();
    if (size == 0) size = 1;
    if (size <= 0x10000) {
        uint16_t s = rt.alloc_segment(size, tag);
        g_globalSize[s] = size;
        return s;
    }
    // huge block: consecutive selectors
    size_t chunks = (size + 0xFFFF) / 0x10000;
    auto& segs = rt.segments();
    for (size_t i = 64; i + chunks < segs.size(); ++i) {
        bool free_ = true;
        for (size_t k = 0; k < chunks; ++k)
            if (segs[i + k].used || (i + k) == (recomp::kImportSel >> 3)) { free_ = false; break; }
        if (!free_) continue;
        for (size_t k = 0; k < chunks; ++k) {
            segs[i + k].used = true;
            segs[i + k].alias = 0;
            segs[i + k].name = tag;
            uint32_t chunk = (k + 1 == chunks) ? size - uint32_t(k) * 0x10000 : 0x10000;
            segs[i + k].data.assign(chunk, 0);
        }
        uint16_t s = uint16_t((i << 3) | 7);
        g_globalSize[s] = size;
        return s;
    }
    return 0;
}

void global_free(uint16_t h) {
    auto it = g_globalSize.find(h);
    uint32_t size = it == g_globalSize.end() ? 1 : it->second;
    size_t chunks = (size + 0xFFFF) / 0x10000;
    for (size_t k = 0; k < chunks; ++k) Runtime::get().free_segment(uint16_t(h + 8 * k));
    if (it != g_globalSize.end()) g_globalSize.erase(it);
}

// ---- DOS ----------------------------------------------------------------

uint32_t g_dta = 0;
struct FindState { std::vector<vfs::FindEntry> entries; size_t next = 0; };
std::map<uint32_t, FindState> g_find;   // keyed by DTA address
int g_lastError = 0;

void dos_error(Cpu& c, int code) {
    g_lastError = code;
    c.set_ax(uint16_t(code));
    c.cf = true;
}

void fill_dta(Cpu& c, const vfs::FindEntry& e) {
    uint16_t s = sel_of(g_dta), o = off_of(g_dta);
    c.wr8(s, uint16_t(o + 0x15), e.dir ? 0x10 : 0x20);
    c.wr16(s, uint16_t(o + 0x16), 0);
    c.wr16(s, uint16_t(o + 0x18), uint16_t(((1996 - 1980) << 9) | (1 << 5) | 1));
    c.wr32(s, uint16_t(o + 0x1A), e.size);
    for (int i = 0; i < 13; ++i)
        c.wr8(s, uint16_t(o + 0x1E + i), i < int(e.name.size()) ? uint8_t(e.name[i]) : 0);
}

void dos_int21(Cpu& c) {
    c.cf = false;
    uint32_t dsdx = (uint32_t(c.ds) << 16) | c.dx();
    switch (c.ah()) {
        case 0x0D: break;                                      // disk reset
        case 0x0E: c.set_al(26); break;                        // select drive
        case 0x19: c.set_al(2); break;                         // current drive = C:
        case 0x1A: g_dta = dsdx; break;                        // set DTA
        case 0x2F: c.es = sel_of(g_dta); c.set_bx(off_of(g_dta)); break;
        case 0x25: break;                                      // set interrupt vector
        case 0x35: c.es = 0; c.set_bx(0); break;               // get interrupt vector
        case 0x2A: {                                           // date
            std::time_t t = std::time(nullptr);
            std::tm* tm = std::localtime(&t);
            c.set_cx(uint16_t(tm->tm_year + 1900));
            c.set_dh(uint8_t(tm->tm_mon + 1));
            c.set_dl(uint8_t(tm->tm_mday));
            c.set_al(uint8_t(tm->tm_wday));
            break;
        }
        case 0x2C: {                                           // time
            std::time_t t = std::time(nullptr);
            std::tm* tm = std::localtime(&t);
            c.set_ch(uint8_t(tm->tm_hour));
            c.set_cl(uint8_t(tm->tm_min));
            c.set_dh(uint8_t(tm->tm_sec));
            c.set_dl(0);
            break;
        }
        case 0x30: c.set_ax(0x0005); c.set_bx(0); c.set_cx(0); break;   // DOS 5.0
        case 0x36: c.set_ax(8); c.set_bx(0x7FFF); c.set_cx(512); c.set_dx(0xFFFF); break;
        case 0x39: case 0x3A: break;                           // mkdir / rmdir: ignore
        case 0x3B:                                             // chdir
            if (!vfs::chdir(recomp::read_cstr(c, dsdx))) dos_error(c, 3);
            break;
        case 0x3C: {                                           // create
            int err = 0;
            int h = vfs::create(recomp::read_cstr(c, dsdx), &err);
            if (h < 0) dos_error(c, err); else c.set_ax(uint16_t(h));
            break;
        }
        case 0x3D: {                                           // open
            int err = 0;
            int h = vfs::open(recomp::read_cstr(c, dsdx), c.al(), &err);
            if (h < 0) dos_error(c, err); else c.set_ax(uint16_t(h));
            break;
        }
        case 0x3E:                                             // close
            if (!vfs::close(c.bx())) dos_error(c, 6);
            break;
        case 0x3F: {                                           // read
            std::vector<uint8_t> buf(c.cx());
            long n = vfs::read(c.bx(), buf.data(), c.cx());
            if (n < 0) { dos_error(c, 6); break; }
            for (long i = 0; i < n; ++i) c.wr8(c.ds, uint16_t(c.dx() + i), buf[size_t(i)]);
            c.set_ax(uint16_t(n));
            break;
        }
        case 0x40: {                                           // write
            std::vector<uint8_t> buf(c.cx());
            for (size_t i = 0; i < buf.size(); ++i) buf[i] = c.rd8(c.ds, uint16_t(c.dx() + i));
            long n = vfs::write(c.bx(), buf.data(), c.cx());
            if (n < 0) dos_error(c, 6); else c.set_ax(uint16_t(n));
            break;
        }
        case 0x41:                                             // delete
            if (!vfs::remove(recomp::read_cstr(c, dsdx))) dos_error(c, 2);
            break;
        case 0x42: {                                           // seek
            long off = long(int32_t((uint32_t(c.cx()) << 16) | c.dx()));
            long pos = vfs::seek(c.bx(), off, c.al());
            if (pos < 0) { dos_error(c, 6); break; }
            c.set_ax(uint16_t(pos));
            c.set_dx(uint16_t(uint32_t(pos) >> 16));
            break;
        }
        case 0x43:                                             // attributes
            if (c.al() == 0) {
                std::string h = vfs::to_host(recomp::read_cstr(c, dsdx));
                if (h.empty()) dos_error(c, 2); else c.set_cx(0x20);
            }
            break;
        case 0x44:                                             // ioctl
            if (c.al() == 0) c.set_dx(c.bx() < 5 ? 0x80 : 0x02);
            else if (c.al() == 8) c.set_ax(1);                 // fixed media
            else if (c.al() == 9) c.set_dx(0);                 // local drive
            break;
        case 0x47: {                                           // getcwd (no "C:\")
            std::string cwd = vfs::cwd().substr(3);
            recomp::write_cstr(c, (uint32_t(c.ds) << 16) | c.si(), cwd, 64);
            break;
        }
        case 0x4C:
            throw ExitProgram{c.al()};
        case 0x4E: {                                           // findfirst
            auto& st = g_find[g_dta];
            st.entries = vfs::find(recomp::read_cstr(c, dsdx), c.cx());
            st.next = 0;
            if (st.entries.empty()) { dos_error(c, 18); break; }
            fill_dta(c, st.entries[st.next++]);
            break;
        }
        case 0x4F: {                                           // findnext
            auto& st = g_find[g_dta];
            if (st.next >= st.entries.size()) { dos_error(c, 18); break; }
            fill_dta(c, st.entries[st.next++]);
            break;
        }
        case 0x56:                                             // rename
            if (!vfs::rename(recomp::read_cstr(c, dsdx),
                             recomp::read_cstr(c, (uint32_t(c.es) << 16) | c.di())))
                dos_error(c, 5);
            break;
        case 0x57:                                             // file date/time
            if (c.al() == 0) { c.set_cx(0); c.set_dx(uint16_t(((1996 - 1980) << 9) | (1 << 5) | 1)); }
            break;
        case 0x59: c.set_ax(uint16_t(g_lastError)); c.set_bx(0); c.set_cx(0); break;
        default:
            recomp::log("unhandled DOS function AH=%02X AL=%02X", c.ah(), c.al());
            dos_error(c, 1);
            break;
    }
}

void interrupt_handler(Cpu& c, int n) {
    switch (n) {
        case 0x21: dos_int21(c); return;
        case 0x25: case 0x26:
            // absolute disk read/write: fail, and like DOS leave the flags on the stack
            c.cf = true;
            c.set_ax(0x8002);
            c.push16(c.get_flags());
            return;
        case 0x2F:
            if (c.ax() == 0x1500) c.set_bx(0);   // MSCDEX: no CD-ROM drives
            return;
        case 0x00: recomp::log("divide error"); return;
        case 0x03: recomp::log("breakpoint"); return;
        default: recomp::log("ignoring INT %02X (AX=%04X)", n, c.ax()); return;
    }
}

}  // namespace

void huge_read(Cpu& c, uint32_t fp, uint8_t* dst, uint32_t n) { huge_copy_in(c, fp, dst, n); }
void huge_write(Cpu& c, uint32_t fp, const uint8_t* src, uint32_t n) { huge_copy_out(c, fp, src, n); }
uint16_t kernel_global_alloc(uint32_t size, const char* tag) { return global_alloc(size, tag); }
void kernel_global_free(uint16_t h) { global_free(h); }
uint32_t kernel_global_size(uint16_t h) {
    auto it = g_globalSize.find(h);
    if (it != g_globalSize.end()) return it->second;
    auto* s = Runtime::get().segment(h);
    return s ? uint32_t(s->data.size()) : 0;
}

void kernel_setup(Runtime& rt, const std::string& gameDir) {
    recomp::g_interrupt_handler = &interrupt_handler;
    (void)gameDir;
    // PSP with an empty command line
    g_psp = rt.alloc_segment(0x100, "psp");
    rt.cpu().wr8(g_psp, 0x80, 0);
    rt.cpu().wr8(g_psp, 0x81, 0x0D);
    set_exe_module(rt.data_sel(), vfs::exe_dos_path(), rt.exe_path());
}

}  // namespace win16

using namespace win16;

// ===========================================================================
// KERNEL
// ===========================================================================

// InitTask: no stack arguments; returns task info in registers.
API(KERNEL, 91, InitTask, 0) {
    auto& rt = Runtime::get();
    uint16_t ds = rt.data_sel();
    // instance data: stack top/min/bottom used by the RTL stack check
    uint16_t sp = c.sp();
    uint16_t stackSize = c.bx() ? c.bx() : 0x2000;
    c.wr16(ds, 0x0A, uint16_t(sp - stackSize + 0x100));   // pStackTop (lowest allowed)
    c.wr16(ds, 0x0C, uint16_t(sp - stackSize + 0x100));   // pStackMin
    c.wr16(ds, 0x0E, sp);                                 // pStackBottom
    c.set_ax(1);
    c.set_bx(0x81);
    c.set_cx(stackSize);
    c.set_dx(1);              // SW_SHOWNORMAL
    c.set_di(ds);             // hInstance
    c.set_si(0);              // hPrevInstance
    c.es = g_psp;
    a.ret(1);
}

API(KERNEL, 30, WaitEvent, 2) { a.w(); a.ret(1); }
API(KERNEL, 3, GetVersion, 0) { c.set_dx(0x0500); a.ret(0x0A03); }     // Windows 3.10, DOS 5
API(KERNEL, 132, GetWinFlags, 0) { a.ret32(0x0425); }
API(KERNEL, 36, GetCurrentTask, 0) { a.ret32((uint32_t(0x0F00) << 16) | 0x0F00); }
API(KERNEL, 107, SetErrorMode, 2) { a.w(); a.ret(0); }
API(KERNEL, 199, SetHandleCount, 2) { uint16_t n = a.w(); a.ret(n); }
API(KERNEL, 136, GetDriveType, 2) { uint16_t d = a.w(); a.ret(d == 2 ? 3 : 0); }   // C: fixed
API(KERNEL, 169, GetFreeSpace, 2) { a.w(); a.ret32(0x00F00000); }
API(KERNEL, 335, IsBadWritePtr, 6) {
    uint16_t n = a.w(); uint32_t p = a.p();
    auto* s = Runtime::get().segment(uint16_t(p >> 16));
    bool bad = !s || uint32_t(uint16_t(p)) + n > s->data.size();
    a.ret(bad ? 1 : 0);
}

// ---- global memory ----
API(KERNEL, 15, GlobalAlloc, 6) {
    uint32_t size = a.l(); a.w();
    a.ret(global_alloc(size, "global"));
}
API(KERNEL, 16, GlobalReAlloc, 8) {
    uint16_t flags = a.w(); uint32_t size = a.l(); uint16_t h = a.w();
    (void)flags;
    uint32_t old = kernel_global_size(h);
    if (size <= 0x10000 && old <= 0x10000) {
        Runtime::get().resize_segment(h, size ? size : 1);
        g_globalSize[h] = size;
        a.ret(h);
        return;
    }
    uint16_t n = global_alloc(size, "global");
    std::vector<uint8_t> tmp(std::min(old, size));
    huge_copy_in(c, uint32_t(h) << 16, tmp.data(), uint32_t(tmp.size()));
    huge_copy_out(c, uint32_t(n) << 16, tmp.data(), uint32_t(tmp.size()));
    global_free(h);
    a.ret(n);
}
API(KERNEL, 17, GlobalFree, 2) { global_free(a.w()); a.ret(0); }
API(KERNEL, 18, GlobalLock, 2) {
    uint16_t h = a.w();
    if (!Runtime::get().segment(h)) { a.ret32(0); return; }
    a.ret32(uint32_t(h) << 16);
}
API(KERNEL, 19, GlobalUnlock, 2) { a.w(); a.ret(0); }
API(KERNEL, 20, GlobalSize, 2) { a.ret32(kernel_global_size(a.w())); }
API(KERNEL, 21, GlobalHandle, 2) { uint16_t s = a.w(); c.set_dx(s); a.ret(s); c.set_dx(s); }
API(KERNEL, 25, GlobalCompact, 4) { a.l(); a.ret32(0x00F00000); }
API(KERNEL, 170, AllocCStoDSAlias, 2) { a.ret(Runtime::get().alias_segment(a.w())); }
API(KERNEL, 176, FreeSelector, 2) { Runtime::get().free_segment(a.w()); a.ret(0); }
API(KERNEL, 177, PrestoChangoSelector, 4) {
    uint16_t dst = a.w(); uint16_t src = a.w();
    Runtime::get().set_alias(dst, src);
    a.ret(dst);
}
API(KERNEL, 348, hmemcpy, 12) {
    uint32_t n = a.l(); uint32_t src = a.p(); uint32_t dst = a.p();
    std::vector<uint8_t> tmp(n);
    huge_copy_in(c, src, tmp.data(), n);
    huge_copy_out(c, dst, tmp.data(), n);
    a.ret();
}

// ---- modules ----
API(KERNEL, 49, GetModuleFileName, 8) {
    uint16_t size = a.w(); uint32_t buf = a.p(); uint16_t h = a.w();
    Module* m = module_by_handle(h);
    std::string p = m ? m->dosPath : vfs::exe_dos_path();
    recomp::write_cstr(c, buf, p, size);
    a.ret(uint16_t(std::min<size_t>(p.size(), size ? size - 1 : 0)));
}
API(KERNEL, 95, LoadLibrary, 4) {
    std::string name = recomp::read_cstr(c, a.p());
    Module* m = load_module(name);
    if (!m) {
        recomp::log("LoadLibrary(%s): not found", name.c_str());
        a.ret(2);   // file not found
        return;
    }
    a.ret(m->handle);
}
API(KERNEL, 96, FreeLibrary, 2) { free_module(a.w()); a.ret(); }
API(KERNEL, 50, GetProcAddress, 6) {
    uint32_t name = a.p(); uint16_t h = a.w();
    recomp::log("GetProcAddress(%04X, %s): not available", h,
                (name >> 16) ? recomp::read_cstr(c, name).c_str() : "ordinal");
    a.ret32(0);
}
API(KERNEL, 51, MakeProcInstance, 6) { a.w(); uint32_t p = a.p(); a.ret32(p); }
API(KERNEL, 52, FreeProcInstance, 4) { a.p(); a.ret(); }

// ---- resources ----
API(KERNEL, 60, FindResource, 10) {
    uint32_t type = a.p(); uint32_t name = a.p(); uint16_t h = a.w();
    Module* m = module_by_handle(h);
    const Resource* r = m ? find_resource(*m, read_reskey(c, name), read_reskey(c, type)) : nullptr;
    a.ret(r ? resource_handle(*m, *r) : 0);
}
API(KERNEL, 65, SizeofResource, 4) {
    uint16_t hr = a.w(); a.w();
    Module* m; const Resource* r;
    a.ret32(resolve_resource_handle(hr, &m, &r) ? r->length : 0);
}
API(KERNEL, 64, AccessResource, 4) {
    uint16_t hr = a.w(); a.w();
    Module* m; const Resource* r;
    if (!resolve_resource_handle(hr, &m, &r)) { a.ret(0xFFFF); return; }
    int err = 0;
    int fh = vfs::open(m->dosPath, 0, &err);
    if (fh < 0) { a.ret(0xFFFF); return; }
    vfs::seek(fh, long(r->fileOffset), 0);
    a.ret(uint16_t(fh));
}
API(KERNEL, 62, LockResource, 2) {
    // hResData from LoadResource is not used by this program; accept a
    // resource-info handle and return a copy of the data.
    uint16_t hr = a.w();
    Module* m; const Resource* r;
    if (!resolve_resource_handle(hr, &m, &r)) { a.ret32(0); return; }
    uint16_t s = global_alloc(r->length, "resource");
    if (const uint8_t* d = resource_data(*m, *r)) huge_copy_out(c, uint32_t(s) << 16, d, r->length);
    a.ret32(uint32_t(s) << 16);
}

// ---- files ----
API(KERNEL, 81, _lclose, 2) { a.ret(vfs::close(a.w()) ? 0 : 0xFFFF); }
API(KERNEL, 84, _llseek, 8) {
    uint16_t whence = a.w(); int32_t off = int32_t(a.l()); uint16_t h = a.w();
    long pos = vfs::seek(h, off, whence);
    a.ret32(uint32_t(pos));
}
API(KERNEL, 349, _hread, 10) {
    uint32_t n = a.l(); uint32_t buf = a.p(); uint16_t h = a.w();
    std::vector<uint8_t> tmp(n);
    long got = vfs::read(h, tmp.data(), long(n));
    if (got > 0) huge_copy_out(c, buf, tmp.data(), uint32_t(got));
    a.ret32(uint32_t(got));
}
API(KERNEL, 350, _hwrite, 10) {
    uint32_t n = a.l(); uint32_t buf = a.p(); uint16_t h = a.w();
    std::vector<uint8_t> tmp(n);
    huge_copy_in(c, buf, tmp.data(), n);
    a.ret32(uint32_t(vfs::write(h, tmp.data(), long(n))));
}

// ---- WIN.INI ----
API(KERNEL, 57, GetProfileInt, 10) { int16_t def = a.i(); a.p(); a.p(); a.ret(uint16_t(def)); }
API(KERNEL, 58, GetProfileString, 18) {
    uint16_t size = a.w(); uint32_t buf = a.p(); uint32_t def = a.p(); a.p(); a.p();
    std::string v = def ? recomp::read_cstr(c, def) : std::string();
    recomp::write_cstr(c, buf, v, size);
    a.ret(uint16_t(std::min<size_t>(v.size(), size ? size - 1 : 0)));
}

// ===========================================================================
// WIN87EM, TOOLHELP, KEYBOARD
// ===========================================================================

// __fpMath: BX = function. 0 init, 1 reset, 2 terminate, 3 set error handler,
// 8 get/set control word ... return success.
API(WIN87EM, 1, __FPMATH, 0) {
    switch (c.bx()) {
        case 0: c.set_ax(0); break;
        case 8: c.set_ax(c.fpu_control); break;
        case 9: c.fpu_control = c.ax(); break;
        case 10: c.set_ax(c.fpu_status()); break;
        default: c.set_ax(0); break;
    }
    a.ret();
}

API(TOOLHELP, 75, InterruptRegister, 6) { a.p(); a.w(); a.ret(1); }
API(TOOLHELP, 76, InterruptUnregister, 2) { a.w(); a.ret(1); }
API(TOOLHELP, 77, TerminateApp, 4) { a.w(); a.w(); throw ExitProgram{0}; }
API(TOOLHELP, 54, GlobalEntryHandle, 6) { a.w(); a.p(); a.ret(0); }
API(TOOLHELP, 62, ModuleFindHandle, 6) { a.w(); a.p(); a.ret(0); }

// KEYBOARD.5 AnsiToOem / .6 OemToAnsi (src, dst): plain copy
API(KEYBOARD, 5, AnsiToOem, 8) {
    uint32_t dst = a.p(); uint32_t src = a.p();
    recomp::write_cstr(c, dst, recomp::read_cstr(c, src), 4096);
    a.ret(1);
}
API(KEYBOARD, 6, OemToAnsi, 8) {
    uint32_t dst = a.p(); uint32_t src = a.p();
    recomp::write_cstr(c, dst, recomp::read_cstr(c, src), 4096);
    a.ret(1);
}
