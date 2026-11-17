// HD texture packs (see texture_pack.hpp). A pack is a folder with
//   pack.json        {"version": 1, "scale": 3, "name": "..."}
//   hd/<hash>.png    one picture per replaced bitmap, exactly scale x its size
// where <hash> is pixel_hash() of the original bitmap as 16 hex digits.
#include "recomp/enhance/texture_pack.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "recomp/enhance/settings.hpp"
#include "recomp/runtime/runtime.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace fs = std::filesystem;

namespace win16::texture_pack {
namespace {

int g_scale = 0;
std::unordered_map<uint64_t, fs::path> g_files;
bool g_log = false;

// Decoded pictures, most recently used first; bitmaps for animation frames
// are created again and again, so decoding each PNG only once matters.
struct Cached {
    uint64_t hash;
    std::shared_ptr<const std::vector<Color>> px;
};
std::list<Cached> g_cache;
size_t g_cacheBytes = 0;
constexpr size_t kCacheLimit = size_t(384) << 20;

// ---- the hash (tools/texturepack.py: pixel_hash) ------------------------------
// Over width, height (uint32 LE) and R,G,B of every pixel top-down: CRC-32 in
// the high half, Adler-32 in the low half.
struct Hasher {
    uint32_t crc = 0xFFFFFFFFu, a = 1, b = 0;
    static const uint32_t* table() {
        static uint32_t t[256];
        static bool made = false;
        if (!made) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            made = true;
        }
        return t;
    }
    void byte(uint8_t v, const uint32_t* t) {
        crc = t[(crc ^ v) & 0xFF] ^ (crc >> 8);
        a += v;
        b += a;
    }
    uint64_t done() {
        a %= 65521u;
        b %= 65521u;
        return (uint64_t(crc ^ 0xFFFFFFFFu) << 32) | (uint64_t(b) << 16) | a;
    }
};

uint64_t pixel_hash(const Surface& s) {
    Hasher h;
    const uint32_t* t = Hasher::table();
    // Adler sums are reduced every few thousand bytes to stay within 32 bits
    int pending = 0;
    auto put = [&](uint8_t v) {
        h.byte(v, t);
        if (++pending == 5000) { h.a %= 65521u; h.b %= 65521u; pending = 0; }
    };
    for (uint32_t v : {uint32_t(s.w), uint32_t(s.h)})
        for (int i = 0; i < 4; ++i) put(uint8_t(v >> (8 * i)));
    for (Color c : s.px) {
        put(uint8_t(c >> 16));
        put(uint8_t(c >> 8));
        put(uint8_t(c));
    }
    return h.done();
}

std::shared_ptr<const std::vector<Color>> load_png(const fs::path& path, int w, int h) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    int pw = 0, ph = 0, n = 0;
    unsigned char* rgb = stbi_load_from_memory(data.data(), int(data.size()), &pw, &ph, &n, 3);
    if (!rgb) {
        recomp::log("texture pack: %s: %s", path.string().c_str(), stbi_failure_reason());
        return nullptr;
    }
    std::shared_ptr<std::vector<Color>> out;
    if (pw == w * g_scale && ph == h * g_scale) {
        out = std::make_shared<std::vector<Color>>(size_t(pw) * ph);
        for (size_t i = 0; i < out->size(); ++i)
            (*out)[i] = (Color(rgb[3 * i]) << 16) | (Color(rgb[3 * i + 1]) << 8) | rgb[3 * i + 2];
    } else {
        recomp::log("texture pack: %s is %dx%d, expected %dx%d", path.string().c_str(), pw, ph, w * g_scale,
                    h * g_scale);
    }
    stbi_image_free(rgb);
    return out;
}

std::shared_ptr<const std::vector<Color>> picture(uint64_t hash, int w, int h) {
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
        if (it->hash == hash) {
            g_cache.splice(g_cache.begin(), g_cache, it);
            return it->px;
        }
    auto f = g_files.find(hash);
    if (f == g_files.end()) return nullptr;
    auto px = load_png(f->second, w, h);
    if (!px) {
        g_files.erase(f);   // broken: do not try again
        return nullptr;
    }
    g_cache.push_front({hash, px});
    g_cacheBytes += px->size() * sizeof(Color);
    while (g_cacheBytes > kCacheLimit && g_cache.size() > 1) {
        g_cacheBytes -= g_cache.back().px->size() * sizeof(Color);
        g_cache.pop_back();
    }
    return px;
}

int read_scale(const fs::path& json) {
    std::ifstream in(json);
    std::stringstream ss;
    ss << in.rdbuf();
    std::smatch m;
    std::string text = ss.str();
    if (std::regex_search(text, m, std::regex("\"scale\"\\s*:\\s*(\\d+)"))) return std::atoi(m[1].str().c_str());
    return 0;
}

}  // namespace

void init(const std::string& gameDir) {
    g_log = std::getenv("MYULK_TEXTURE_PACK_LOG") != nullptr;
    if (!settings::get_int("texture_pack", 1)) {
        recomp::log("texture pack: turned off in the settings");
        return;
    }
    std::vector<fs::path> roots;
    if (const char* d = std::getenv("MYULK_TEXTURE_PACK")) roots.emplace_back(fs::u8path(d));
    if (const char* base = SDL_GetBasePath()) roots.emplace_back(fs::u8path(base) / "texturepack");
    if (char* p = SDL_GetPrefPath("myulk-recomp", "Myulk")) {
        roots.emplace_back(fs::u8path(p) / "texturepack");
        SDL_free(p);
    }
    fs::path game = fs::absolute(fs::u8path(gameDir)).parent_path();   // the CD's root
    roots.push_back(game / "texturepack");
    roots.push_back(game.parent_path() / "texturepack");
    for (const fs::path& r : roots) {
        std::error_code ec;
        if (!fs::exists(r / "pack.json", ec)) continue;
        int k = read_scale(r / "pack.json");
        if (k < 2 || k > 4) {
            recomp::log("texture pack %s: scale %d not supported (2-4)", r.string().c_str(), k);
            continue;
        }
        for (const auto& e : fs::directory_iterator(r / "hd", ec)) {
            std::string name = e.path().stem().string();
            if (name.size() != 16 || e.path().extension() != ".png") continue;
            char* end = nullptr;
            uint64_t h = std::strtoull(name.c_str(), &end, 16);
            if (end == name.c_str() + 16) g_files[h] = e.path();
        }
        if (g_files.empty()) continue;
        g_scale = k;
        hd_ensure(screen(), k);
        recomp::log("texture pack %s: %zu pictures at %dx", r.string().c_str(), g_files.size(), k);
        return;
    }
}

int scale() { return g_scale; }

void attach(Surface& s) {
    if (!g_scale || s.bpp == 1 || s.w <= 0 || s.h <= 0) return;
    uint64_t h = pixel_hash(s);
    auto px = picture(h, s.w, s.h);
    if (g_log) recomp::log("texture pack: %dx%d %016llx %s", s.w, s.h, (unsigned long long)h, px ? "HD" : "-");
    if (px) hd_attach(s, g_scale, *px);
}

}  // namespace win16::texture_pack
