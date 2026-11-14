// Locating the user's extracted game data and caching textures loaded from it.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

struct SDL_Renderer;
struct SDL_Texture;

namespace myulk {

// Find extracted/resources (the folder holding MYULK/forms). Checks the
// MYULK_DATA environment variable, then paths relative to the working
// directory and the executable, then /data/resources (web build).
std::optional<std::string> findDataRoot();

class TextureCache {
public:
    TextureCache(SDL_Renderer* renderer, std::string root);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // Path relative to the data root, e.g. "MYULK/forms/TMAIN/x.bmp".
    // Returns nullptr if the file can't be loaded (logged once).
    SDL_Texture* get(const std::string& relPath);
    const std::string& root() const { return root_; }

private:
    SDL_Renderer* renderer_;
    std::string root_;
    std::unordered_map<std::string, SDL_Texture*> cache_;
};

}  // namespace myulk
