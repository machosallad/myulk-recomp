#include "recomp/win16/modules.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/vfs.hpp"

namespace win16 {
namespace {

std::map<uint16_t, std::unique_ptr<Module>> g_modules;
uint16_t g_exe = 0;
uint16_t g_nextHandle = 0x0100;
struct ResHandle { uint16_t module; size_t index; };
std::vector<ResHandle> g_resHandles(1);   // handle 0 invalid

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

uint16_t u16(const std::vector<uint8_t>& d, size_t o) { return uint16_t(d[o] | (d[o + 1] << 8)); }

void parse_resources(Module& m) {
    const auto& d = m.file;
    if (d.size() < 0x40) return;
    size_t ne = u16(d, 0x3C) | (size_t(u16(d, 0x3E)) << 16);
    if (ne + 0x40 > d.size() || d[ne] != 'N') return;
    size_t rt = ne + u16(d, ne + 0x24), rn = ne + u16(d, ne + 0x26);
    if (rt == rn) return;
    int shift = u16(d, rt);
    size_t p = rt + 2;
    auto name_at = [&](uint16_t off) {
        size_t q = rt + off;
        return upper(std::string(reinterpret_cast<const char*>(&d[q + 1]), d[q]));
    };
    while (p + 8 <= d.size()) {
        uint16_t type = u16(d, p), count = u16(d, p + 2);
        if (!type) break;
        p += 8;
        for (int i = 0; i < count; ++i, p += 12) {
            Resource r;
            if (type & 0x8000) r.typeId = type & 0x7FFF; else r.typeName = name_at(type);
            uint16_t id = u16(d, p + 6);
            if (id & 0x8000) r.id = id & 0x7FFF; else r.name = name_at(id);
            r.fileOffset = uint32_t(u16(d, p)) << shift;
            r.length = uint32_t(u16(d, p + 2)) << shift;
            m.resources.push_back(r);
        }
    }
}

std::unique_ptr<Module> read_module(const std::string& dosPath, const std::string& hostPath) {
    std::ifstream in(hostPath, std::ios::binary);
    if (!in) return nullptr;
    auto m = std::make_unique<Module>();
    m->dosPath = dosPath;
    m->hostPath = hostPath;
    m->file.assign(std::istreambuf_iterator<char>(in), {});
    parse_resources(*m);
    return m;
}

}  // namespace

ResKey read_reskey(recomp::Cpu& c, uint32_t fp) {
    ResKey k;
    if ((fp >> 16) == 0) {
        k.id = int(fp & 0xFFFF);
        return k;
    }
    std::string s = recomp::read_cstr(c, fp, 256);
    if (!s.empty() && s[0] == '#') k.id = std::atoi(s.c_str() + 1);
    else k.name = upper(s);
    return k;
}

void set_exe_module(uint16_t hInstance, const std::string& dosPath, const std::string& hostPath) {
    auto m = read_module(dosPath, hostPath);
    if (!m) return;
    m->handle = hInstance;
    g_exe = hInstance;
    g_modules[hInstance] = std::move(m);
}

Module& exe_module() { return *g_modules[g_exe]; }

Module* module_by_handle(uint16_t h) {
    if (h == 0) return g_modules.count(g_exe) ? g_modules[g_exe].get() : nullptr;
    auto it = g_modules.find(h);
    if (it != g_modules.end()) return it->second.get();
    // hModule vs hInstance of the EXE differ only in the low bits on Windows
    auto it2 = g_modules.find(uint16_t(h | 7));
    return it2 == g_modules.end() ? nullptr : it2->second.get();
}

Module* load_module(const std::string& dosPath) {
    std::string norm = vfs::normalize(dosPath);
    for (auto& [h, m] : g_modules) {
        if (m->dosPath == norm) {
            ++m->refs;
            return m.get();
        }
    }
    std::string host = vfs::to_host(norm);
    if (host.empty()) return nullptr;
    auto m = read_module(norm, host);
    if (!m) return nullptr;
    m->handle = g_nextHandle;
    g_nextHandle = uint16_t(g_nextHandle + 0x10);
    Module* raw = m.get();
    g_modules[raw->handle] = std::move(m);
    return raw;
}

void free_module(uint16_t h) {
    auto it = g_modules.find(h);
    if (it == g_modules.end() || h == g_exe) return;
    if (--it->second->refs <= 0) g_modules.erase(it);
}

const Resource* find_resource(Module& m, const ResKey& name, const ResKey& type) {
    for (const auto& r : m.resources) {
        bool typeOk = type.id ? r.typeId == type.id : r.typeName == type.name;
        bool nameOk = name.id ? r.id == name.id : r.name == name.name;
        if (typeOk && nameOk) return &r;
    }
    return nullptr;
}

const uint8_t* resource_data(const Module& m, const Resource& r) {
    if (r.fileOffset + r.length > m.file.size()) return nullptr;
    return m.file.data() + r.fileOffset;
}

uint16_t resource_handle(Module& m, const Resource& r) {
    size_t idx = size_t(&r - m.resources.data());
    for (size_t i = 1; i < g_resHandles.size(); ++i)
        if (g_resHandles[i].module == m.handle && g_resHandles[i].index == idx) return uint16_t(i);
    g_resHandles.push_back({m.handle, idx});
    return uint16_t(g_resHandles.size() - 1);
}

bool resolve_resource_handle(uint16_t h, Module** m, const Resource** r) {
    if (h == 0 || h >= g_resHandles.size()) return false;
    Module* mod = module_by_handle(g_resHandles[h].module);
    if (!mod || g_resHandles[h].index >= mod->resources.size()) return false;
    *m = mod;
    *r = &mod->resources[g_resHandles[h].index];
    return true;
}

}  // namespace win16
