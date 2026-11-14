// Virtual DOS file system: maps the game's DOS paths onto the extracted CD.
//
// Drive C: is the extracted ISO folder, so C:\MYULK\MYULK.EXE is the game and
// C:\MYULK\..\PICTDLL\NBAE.DLL resolves like on the CD. Name lookup is
// case-insensitive (the ISO uses upper case).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace win16::vfs {

void init(const std::string& hostRoot, const std::string& gameSubdir);

// DOS path (absolute, or relative to the current directory) -> host path.
// Returns empty if a path component doesn't exist (except the last one when
// `forCreate` is set).
std::string to_host(const std::string& dosPath, bool forCreate = false);
std::string cwd();                      // e.g. "C:\MYULK"
bool chdir(const std::string& dosPath);
std::string exe_dos_path();             // C:\MYULK\MYULK.EXE
std::string normalize(const std::string& dosPath);   // absolute, upper-cased

// File handles shared by DOS calls and _lread/_hread/_llseek.
int open(const std::string& dosPath, int mode, int* error);    // mode: 0 read, 1 write, 2 rw
int create(const std::string& dosPath, int* error);
bool close(int h);
long read(int h, void* buf, long n);
long write(int h, const void* buf, long n);
long seek(int h, long off, int whence);
long size(int h);
bool remove(const std::string& dosPath);
bool rename(const std::string& from, const std::string& to);

// FindFirst/FindNext over one directory with a DOS wildcard pattern.
struct FindEntry {
    std::string name;   // 8.3 upper case
    uint32_t size = 0;
    bool dir = false;
};
std::vector<FindEntry> find(const std::string& dosPattern, int attrMask);

}  // namespace win16::vfs
