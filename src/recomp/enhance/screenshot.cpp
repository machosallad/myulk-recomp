// F12: save the game picture (640x480, as shown, without notes) as a PNG.
// Desktop: Pictures/Myulk (or the app-data folder); browser: a download.
#include "recomp/enhance/screenshot.hpp"

#include <SDL3/SDL.h>

#include <ctime>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIW_WINDOWS_UTF8   // file names are UTF-8 (SDL folders)
#include "stb_image_write.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, web_download, (const char* name, const unsigned char* data, int size), {
    const blob = new Blob([HEAPU8.slice(data, data + size)], {type: 'image/png'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = UTF8ToString(name);
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
});
#endif

namespace win16 {

std::string save_screenshot(const Surface& s) {
    std::vector<unsigned char> rgb(size_t(s.w) * s.h * 3);
    for (size_t i = 0; i < s.px.size(); ++i) {
        rgb[i * 3] = (s.px[i] >> 16) & 0xFF;
        rgb[i * 3 + 1] = (s.px[i] >> 8) & 0xFF;
        rgb[i * 3 + 2] = s.px[i] & 0xFF;
    }
    std::time_t t = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof name, "myulk-%Y%m%d-%H%M%S.png", std::localtime(&t));
#ifdef __EMSCRIPTEN__
    std::vector<unsigned char> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<unsigned char>*>(ctx);
            out->insert(out->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
        },
        &png, s.w, s.h, 3, rgb.data(), s.w * 3);
    if (png.empty()) return {};
    web_download(name, png.data(), int(png.size()));
    return name;
#else
    std::string dir;
    if (const char* over = SDL_getenv("MYULK_SCREENSHOT_DIR")) {   // tests
        dir = over;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    } else if (const char* pics = SDL_GetUserFolder(SDL_FOLDER_PICTURES)) {
        std::string base = pics;   // ends with the platform's separator
        char sep = base.empty() ? '/' : base.back();
        dir = base + "Myulk";
        if (!SDL_CreateDirectory(dir.c_str())) dir.clear();
        else dir += sep;
    }
    if (dir.empty()) {
        char* pref = SDL_GetPrefPath("Myulk", "Myulk");
        if (pref) { dir = pref; SDL_free(pref); }
    }
    std::string path = dir + name;
    if (!stbi_write_png(path.c_str(), s.w, s.h, 3, rgb.data(), s.w * 3)) return {};
    return path;
#endif
}

}  // namespace win16
