// Runtime services for recompiled code: loading the NE image into segmented
// memory (with relocations applied), dispatching indirect calls, imported
// functions and DOS interrupts.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "recomp/runtime/cpu.hpp"

namespace recomp {

// Selector range used for import thunks (far pointers to imported functions
// stored in data point here; offset = import index).
constexpr uint16_t kImportSel = (511 << 3) | 7;

using ApiFn = void (*)(Cpu&);

class Runtime {
public:
    static Runtime& get();

    // Load MYULK.EXE: code and data segments, relocations; set DS/SS/SP.
    bool load(const std::string& exePath);
    Cpu& cpu() { return cpu_; }

    // Segment management (GlobalAlloc etc.).
    uint16_t alloc_segment(size_t size, const std::string& name);
    void free_segment(uint16_t sel);
    void resize_segment(uint16_t sel, size_t size);
    Segment* segment(uint16_t sel);           // resolves aliases
    uint16_t alias_segment(uint16_t sel);     // new selector sharing sel's memory
    void set_alias(uint16_t dst, uint16_t src);
    std::vector<Segment>& segments() { return segs_; }

    // Windows API registry.
    void register_api(const std::string& module, int ordinal, ApiFn fn, const char* name);
    void call_import(Cpu& c, const std::string& module, int ordinal, const char* name);
    uint16_t import_value(const std::string& module, int ordinal);
    // Far pointer to an import (for GetProcAddress / relocations in data).
    uint32_t import_far(const std::string& module, int ordinal);
    void call_import_index(Cpu& c, uint16_t index);

    // Run recompiled code at sel:off from the host (callbacks such as window
    // procedures): pushes a return address and dispatches.
    void call(uint16_t sel, uint16_t off);
    Fn lookup(uint16_t sel, uint16_t off) const;

    uint16_t data_sel() const { return dataSel_; }
    void print_api_stats();
    ApiFn find_api(const std::string& module, int ordinal) const;
    const std::string& exe_path() const { return exePath_; }

private:
    Runtime();
    Cpu cpu_;
    std::vector<Segment> segs_;
    std::unordered_map<uint32_t, Fn> funcs_;
    struct Api { ApiFn fn; std::string name; };
    std::unordered_map<std::string, Api> apis_;          // "MODULE.ordinal"
    std::vector<std::pair<std::string, int>> importSlots_;  // kImportSel offsets
    std::unordered_map<std::string, int> apiCounts_;
    uint16_t dataSel_ = 0;
    std::string exePath_;
};

// Helper for API implementations: reads Pascal-convention arguments. Pascal
// pushes the first declared argument first, so it sits at the highest
// address; reading starts just above the far return address, i.e. with the
// LAST declared argument, and moves towards the first.
// Reports (once per API) a declared argument size that differs from what the
// implementation read: popping the wrong amount unbalances the program's stack.
void arg_size_mismatch(const char* name, int declared, int read);

class Args {
public:
    Args(Cpu& c, int bytes, const char* name = nullptr) : c_(c), bytes_(bytes), pos_(4), name_(name) {}
    uint16_t w() { uint16_t v = c_.rd16(c_.ss, uint16_t(c_.sp() + pos_)); pos_ += 2; return v; }
    int16_t i() { return int16_t(w()); }
    uint32_t l() { uint32_t v = c_.rd32(c_.ss, uint16_t(c_.sp() + pos_)); pos_ += 4; return v; }
    // far pointer: returns (selector << 16) | offset
    uint32_t p() { return l(); }
    // Pop return address + arguments (callee cleans up, as in Pascal).
    void ret() {
        if (name_ && pos_ != 4 && pos_ - 4 != bytes_) arg_size_mismatch(name_, bytes_, pos_ - 4);
        c_.ret_far(bytes_);
    }
    void ret(uint16_t ax) { c_.set_ax(ax); ret(); }
    void ret32(uint32_t v) { c_.set_ax(uint16_t(v)); c_.set_dx(uint16_t(v >> 16)); ret(); }

private:
    Cpu& c_;
    int bytes_;
    int pos_;
    const char* name_;
};

// Pascal string / C string helpers on emulated memory.
std::string read_cstr(Cpu& c, uint32_t farptr, size_t max = 4096);
void write_cstr(Cpu& c, uint32_t farptr, const std::string& s, size_t max);

void log(const char* fmt, ...);

// Installed by the Windows layer: handles INT n (DOS INT 21h etc.).
extern void (*g_interrupt_handler)(Cpu&, int);

}  // namespace recomp
