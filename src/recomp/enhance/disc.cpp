// Reading the CD's files (see disc.hpp).
//
// ISO 9660: 2048-byte logical sectors; the primary volume descriptor at
// sector 16 ("CD001") holds the root directory record; directory records give
// each entry's first sector, size and flags. Raw images store 2352-byte
// sectors with the 2048 data bytes at offset 16 (mode 1) or 24 (mode 2 form 1).
#include "recomp/enhance/disc.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace win16::disc {
namespace fs = std::filesystem;

namespace {

// a path as UTF-8 in a std::string (u8string() is std::u8string in C++20)
std::string utf8(const fs::path& p) {
    auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

class Image : public Source {
public:
    Image(ReadAt read, uint32_t sectorSize, uint32_t dataOffset)
        : read_(std::move(read)), sectorSize_(sectorSize), dataOffset_(dataOffset) {}

    bool sector(uint32_t lba, uint8_t* out) {   // one 2048-byte logical sector
        return read_(uint64_t(lba) * sectorSize_ + dataOffset_, out, 2048);
    }

    bool read(const File& f, uint64_t offset, void* buf, size_t n) override {
        if (offset + n > f.size) return false;
        uint8_t* out = static_cast<uint8_t*>(buf);
        if (sectorSize_ == 2048)   // plain ISO: one contiguous read
            return read_(uint64_t(f.lba) * 2048 + offset, out, n);
        uint8_t sec[2048];
        while (n) {
            uint32_t lba = f.lba + uint32_t(offset / 2048);
            size_t in = size_t(offset % 2048), take = std::min(n, size_t(2048) - in);
            if (!sector(lba, sec)) return false;
            std::memcpy(out, sec + in, take);
            out += take; offset += take; n -= take;
        }
        return true;
    }

    // Walk a directory (its extent) and add its files, depth-first.
    bool walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth) {
        if (depth > 8) return true;
        std::vector<uint8_t> dir(size_t((size + 2047) / 2048) * 2048);
        for (size_t s = 0; s * 2048 < dir.size(); ++s)
            if (!sector(lba + uint32_t(s), dir.data() + s * 2048)) return false;
        for (size_t o = 0; o < size;) {
            uint8_t len = dir[o];
            if (len == 0) { o = (o / 2048 + 1) * 2048; continue; }   // records don't cross sectors
            if (o + 33 > dir.size() || len < 34) break;
            uint32_t elba = dir[o + 2] | dir[o + 3] << 8 | dir[o + 4] << 16 | uint32_t(dir[o + 5]) << 24;
            uint32_t esize = dir[o + 10] | dir[o + 11] << 8 | dir[o + 12] << 16 | uint32_t(dir[o + 13]) << 24;
            uint8_t flags = dir[o + 25], nameLen = dir[o + 32];
            std::string name(reinterpret_cast<const char*>(&dir[o + 33]), std::min<size_t>(nameLen, len - 33));
            o += len;
            if (nameLen == 1 && (name[0] == 0 || name[0] == 1)) continue;   // "." and ".."
            name = upper(name.substr(0, name.find(';')));
            if (!name.empty() && name.back() == '.') name.pop_back();
            std::string path = prefix.empty() ? name : prefix + "/" + name;
            if (flags & 2) {
                if (!walk(elba, esize, path, depth + 1)) return false;
            } else {
                files_.push_back({path, esize, elba});
            }
        }
        return true;
    }

private:
    ReadAt read_;
    uint32_t sectorSize_, dataOffset_;
};

class Folder : public Source {
public:
    explicit Folder(fs::path root) : root_(std::move(root)) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string rel = upper(utf8(fs::relative(it->path(), root_, ec)));
            if (!ec) {
                files_.push_back({rel, uint64_t(it->file_size(ec)), 0});
                real_.push_back(it->path());
            }
        }
    }
    bool read(const File& f, uint64_t offset, void* buf, size_t n) override {
        size_t i = size_t(&f - files_.data());
        if (i >= real_.size()) return false;
        if (!in_ || current_ != i) {
            in_ = std::make_unique<std::ifstream>(real_[i], std::ios::binary);
            current_ = i;
        }
        in_->clear();
        in_->seekg(std::streamoff(offset));
        in_->read(static_cast<char*>(buf), std::streamsize(n));
        return size_t(in_->gcount()) == n;
    }

private:
    fs::path root_;
    std::vector<fs::path> real_;
    std::unique_ptr<std::ifstream> in_;
    size_t current_ = SIZE_MAX;
};

// The .bin a .cue sheet points to (FILE "name.bin" BINARY), next to the sheet.
fs::path cue_target(const fs::path& cue) {
    std::ifstream in(cue);
    std::string line;
    while (std::getline(in, line)) {
        size_t f = line.find("FILE");
        if (f == std::string::npos) continue;
        size_t a = line.find('"', f), b = a == std::string::npos ? a : line.find('"', a + 1);
        std::string name = a != std::string::npos && b != std::string::npos ? line.substr(a + 1, b - a - 1)
                                                                            : line.substr(f + 5, line.rfind(' ') - f - 5);
        return cue.parent_path() / fs::u8path(name);
    }
    return {};
}

}  // namespace

const File* Source::find(const std::string& path) const {
    std::string want = upper(path);
    for (const File& f : files_)
        if (f.path == want) return &f;
    return nullptr;
}

bool is_game_file(const std::string& path) {
    static const char* folders[] = {"MYULK/", "PICTDLL/", "ART/", "SOUNDS/", "LJUD/", "MOVIES/", "IMAGES/"};
    for (const char* f : folders)
        if (path.compare(0, std::strlen(f), f) == 0) return true;
    return false;
}

std::unique_ptr<Source> open_image(ReadAt read, std::string& error) {
    struct Layout { uint32_t sector, offset; };
    for (Layout l : {Layout{2048, 0}, Layout{2352, 16}, Layout{2352, 24}}) {
        uint8_t pvd[2048];
        if (!read(uint64_t(16) * l.sector + l.offset, pvd, sizeof pvd)) continue;
        if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) continue;
        const uint8_t* root = pvd + 156;   // root directory record
        uint32_t lba = root[2] | root[3] << 8 | root[4] << 16 | uint32_t(root[5]) << 24;
        uint32_t size = root[10] | root[11] << 8 | root[12] << 16 | uint32_t(root[13]) << 24;
        auto img = std::make_unique<Image>(read, l.sector, l.offset);
        if (!img->walk(lba, size, "", 0)) {
            error = "Skivavbilden kunde inte l\xE4sas till slut.";
            return nullptr;
        }
        return img;
    }
    error = "Det h\xE4r ser inte ut som en cd-avbild (ISO 9660).";
    return nullptr;
}

std::unique_ptr<Source> open_path(const std::string& path, std::string& error) {
    fs::path p = fs::u8path(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        auto f = std::make_unique<Folder>(p);
        if (f->files().empty()) error = "Mappen \xE4r tom eller kan inte l\xE4sas.";
        return f->files().empty() ? nullptr : std::move(f);
    }
    std::string ext = upper(utf8(p.extension()));
    if (ext == ".CUE") {
        fs::path bin = cue_target(p);
        if (bin.empty() || !fs::exists(bin, ec)) {
            error = "Cue-filen pekar p\xE5 en bin-fil som inte finns bredvid den.";
            return nullptr;
        }
        p = bin;
    }
    auto in = std::make_shared<std::ifstream>(p, std::ios::binary);
    if (!*in) {
        error = "Filen kunde inte \xF6ppnas.";
        return nullptr;
    }
    return open_image(
        [in](uint64_t off, void* buf, size_t n) {
            in->clear();
            in->seekg(std::streamoff(off));
            in->read(static_cast<char*>(buf), std::streamsize(n));
            return size_t(in->gcount()) == n;
        },
        error);
}

}  // namespace win16::disc
