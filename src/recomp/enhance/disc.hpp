// Reading the game files straight from the player's copy of the CD: an ISO
// 9660 image (.iso), a raw image (.bin, 2352-byte sectors, with or without its
// .cue sheet), or a folder (a CD drive, or the CD's files copied to disk).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace win16::disc {

struct File {
    std::string path;    // "MYULK/MYULK.EXE" (upper case, '/' separated)
    uint64_t size = 0;
    uint32_t lba = 0;    // images: first sector
};

class Source {
public:
    virtual ~Source() = default;
    const std::vector<File>& files() const { return files_; }
    const File* find(const std::string& path) const;
    // Reads n bytes of a file from offset; false on a read error.
    virtual bool read(const File& f, uint64_t offset, void* buf, size_t n) = 0;

protected:
    std::vector<File> files_;
};

// Raw access to an image: read n bytes at an absolute offset.
using ReadAt = std::function<bool(uint64_t offset, void* buf, size_t n)>;

// An image read through `read` (any of the sector layouts); null if it is no
// ISO 9660 disc. `error` says why.
std::unique_ptr<Source> open_image(ReadAt read, std::string& error);

// A path the player chose: an image file, a .cue sheet or a folder.
std::unique_ptr<Source> open_path(const std::string& path, std::string& error);

// The folders of the CD the game uses (MYULK, PICTDLL, ART, ...).
bool is_game_file(const std::string& path);

}  // namespace win16::disc
