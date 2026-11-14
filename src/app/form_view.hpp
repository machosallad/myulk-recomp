// Draws a Delphi form with SDL and maps mouse positions to components.
#pragma once

#include <string>

#include "core/form.hpp"

struct SDL_Renderer;

namespace myulk {

class TextureCache;

struct FormViewOptions {
    bool outlines = false;    // draw component boxes (debug)
};

class FormView {
public:
    FormView(SDL_Renderer* renderer, TextureCache& textures);

    // Forms are drawn into a 640x480 screen; smaller dialogs are centred.
    void draw(const Form& form, const FormViewOptions& opt);

    // Topmost visible component under a screen point, or nullptr.
    const Component* hitTest(const Form& form, float x, float y) const;

    static constexpr int kScreenW = 640;
    static constexpr int kScreenH = 480;

private:
    struct Rect { int x, y, w, h; };
    Rect formOrigin(const Form& form) const;
    void drawComponent(const Form& form, const Component& c, int ox, int oy, Rect clip,
                       const FormViewOptions& opt);
    const Component* hit(const Component& c, int ox, int oy, float x, float y) const;
    unsigned backgroundOf(const Form& form, const Component& c) const;

    SDL_Renderer* renderer_;
    TextureCache& textures_;
};

}  // namespace myulk
