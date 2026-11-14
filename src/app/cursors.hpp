// The game's mouse cursors (.cur files extracted from MYULK.EXE), drawn as a
// software cursor so they scale with the 640x480 screen on every platform.
#pragma once

#include <array>
#include <string>

struct SDL_Renderer;
struct SDL_Texture;

namespace myulk {

class Cursors {
public:
    Cursors(SDL_Renderer* renderer, const std::string& cursorDir);
    ~Cursors();
    Cursors(const Cursors&) = delete;
    Cursors& operator=(const Cursors&) = delete;

    // index: Screen.Cursors[] number (1 BACKWARD .. 10 SPADE); 0 or missing
    // falls back to FLAT (2), the game's default pointer.
    void draw(int index, float x, float y);

private:
    struct Cur { SDL_Texture* tex = nullptr; int hx = 0, hy = 0, w = 0, h = 0; };
    Cur load(const std::string& path);
    SDL_Renderer* renderer_;
    std::array<Cur, 11> cursors_{};
};

}  // namespace myulk
