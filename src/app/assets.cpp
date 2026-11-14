#include "app/assets.hpp"

#include <filesystem>
#include <vector>

#include <SDL3/SDL.h>

namespace fs = std::filesystem;

namespace myulk {

std::optional<std::string> findDataRoot() {
    std::vector<fs::path> candidates;
    if (const char* env = SDL_getenv("MYULK_DATA")) candidates.emplace_back(env);
    candidates.emplace_back("extracted/resources");
    candidates.emplace_back("../extracted/resources");
    candidates.emplace_back("../../extracted/resources");
    if (const char* base = SDL_GetBasePath()) {
        fs::path b(base);
        for (int up = 0; up < 4; ++up) {
            candidates.push_back(b / "extracted" / "resources");
            b = b.parent_path();
        }
    }
    candidates.emplace_back("/data/resources");

    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::is_directory(c / "MYULK" / "forms", ec))
            return fs::absolute(c, ec).string();
    }
    return std::nullopt;
}

TextureCache::TextureCache(SDL_Renderer* renderer, std::string root)
    : renderer_(renderer), root_(std::move(root)) {}

TextureCache::~TextureCache() {
    for (auto& [_, tex] : cache_)
        if (tex) SDL_DestroyTexture(tex);
}

SDL_Texture* TextureCache::get(const std::string& relPath) {
    if (auto it = cache_.find(relPath); it != cache_.end()) return it->second;
    SDL_Texture* tex = nullptr;
    std::string full = (fs::path(root_) / relPath).string();
    if (SDL_Surface* s = SDL_LoadBMP(full.c_str())) {
        tex = SDL_CreateTextureFromSurface(renderer_, s);
        SDL_DestroySurface(s);
        if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    } else {
        SDL_Log("cannot load %s: %s", full.c_str(), SDL_GetError());
    }
    cache_[relPath] = tex;
    return tex;
}

}  // namespace myulk
