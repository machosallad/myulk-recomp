// Loaded modules (the EXE and LoadLibrary'd DLLs) and their resources.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "recomp/runtime/cpu.hpp"

namespace win16 {

struct Resource {
    int typeId = 0;          // ordinal type (RT_BITMAP=2...) or 0 if named
    std::string typeName;
    int id = 0;              // ordinal id or 0 if named
    std::string name;        // upper case
    uint32_t fileOffset = 0;
    uint32_t length = 0;
};

struct Module {
    uint16_t handle = 0;
    std::string dosPath;     // C:\MYULK\MYULK.EXE
    std::string hostPath;
    std::vector<uint8_t> file;
    std::vector<Resource> resources;
    int refs = 1;
};

// Resource name/type argument: far pointer that is either MAKEINTRESOURCE
// (selector 0, offset = id) or a C string ("#123" or a name).
struct ResKey {
    int id = 0;
    std::string name;
};
ResKey read_reskey(recomp::Cpu& c, uint32_t farptr);

Module* module_by_handle(uint16_t h);   // accepts hInstance or hModule
Module* load_module(const std::string& dosPath);   // nullptr if missing
void free_module(uint16_t h);
Module& exe_module();
void set_exe_module(uint16_t hInstance, const std::string& dosPath, const std::string& hostPath);

const Resource* find_resource(Module& m, const ResKey& name, const ResKey& type);
// bytes of a resource (pointer into the module file)
const uint8_t* resource_data(const Module& m, const Resource& r);

// Resource handles (FindResource result) are indices into a global table.
uint16_t resource_handle(Module& m, const Resource& r);
bool resolve_resource_handle(uint16_t h, Module** m, const Resource** r);

constexpr int RT_CURSOR = 1, RT_BITMAP = 2, RT_ICON = 3, RT_MENU = 4, RT_DIALOG = 5,
              RT_STRING = 6, RT_RCDATA = 10, RT_GROUP_CURSOR = 12, RT_GROUP_ICON = 14;

}  // namespace win16
