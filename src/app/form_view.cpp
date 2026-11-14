#include "app/form_view.hpp"

#include <algorithm>
#include <vector>

#include <SDL3/SDL.h>

#include "app/assets.hpp"

namespace myulk {
namespace {

void setColor(SDL_Renderer* r, unsigned rgb, Uint8 a = 255) {
    SDL_SetRenderDrawColor(r, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

void fillRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_FRect fr{float(x), float(y), float(w), float(h)};
    SDL_RenderFillRect(r, &fr);
}

// Delphi paints graphic controls (TImage, TLabel...) on the parent's canvas
// before its child windows, so windowed children are always on top.
bool isGraphicControl(const Component& c) {
    return c.cls == "TImage" || c.cls == "TLabel" || c.cls == "TShape" ||
           c.cls == "TautoButton" || c.cls == "TGauge" || c.cls == "TBevel";
}

bool isNonVisual(const Component& c) {
    return c.cls == "TTimer" || c.cls == "TMediaPlayer" || c.cls == "TMainMenu" ||
           c.cls == "TPopupMenu" || c.cls == "TOpenDialog" || c.cls == "TSaveDialog";
}

// The debug font is ASCII only: map Swedish letters to their base letter
// until real fonts are in (UTF-8 input).
std::string asciiFold(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = s[i];
        if (ch < 0x80) { out += char(ch); continue; }
        if (ch == 0xC3 && i + 1 < s.size()) {
            unsigned char n = s[++i];
            switch (n) {
                case 0x85: case 0x84: out += 'A'; break;   // Å Ä
                case 0x96: out += 'O'; break;              // Ö
                case 0xA5: case 0xA4: out += 'a'; break;   // å ä
                case 0xB6: out += 'o'; break;              // ö
                case 0xA9: out += 'e'; break;              // é
                default: out += '?';
            }
            continue;
        }
        out += '?';
    }
    return out;
}

}  // namespace

FormView::FormView(SDL_Renderer* renderer, TextureCache& textures)
    : renderer_(renderer), textures_(textures) {}

FormView::Rect FormView::formOrigin(const Form& form) const {
    int w = std::min(form.clientWidth, kScreenW);
    int h = std::min(form.clientHeight, kScreenH);
    return {(kScreenW - w) / 2, (kScreenH - h) / 2, w, h};
}

unsigned FormView::backgroundOf(const Form& form, const Component& c) const {
    const Component* p = &c;
    while (p) {
        if (p->color && !p->parentColor) return *p->color;
        p = p->parent;
    }
    return form.root->color.value_or(0xC0C0C0);
}

void FormView::draw(const Form& form, const FormViewOptions& opt) {
    setColor(renderer_, 0x000000);
    SDL_RenderClear(renderer_);
    Rect o = formOrigin(form);
    setColor(renderer_, form.root->color.value_or(0xC0C0C0));
    fillRect(renderer_, o.x, o.y, o.w, o.h);

    std::vector<const Component*> order;
    for (const auto& ch : form.root->children) if (isGraphicControl(*ch)) order.push_back(ch.get());
    for (const auto& ch : form.root->children) if (!isGraphicControl(*ch)) order.push_back(ch.get());
    for (const Component* ch : order) drawComponent(form, *ch, o.x, o.y, o, opt);
    SDL_SetRenderClipRect(renderer_, nullptr);
}

void FormView::drawComponent(const Form& form, const Component& c, int ox, int oy, Rect clip,
                             const FormViewOptions& opt) {
    if (!c.visible || isNonVisual(c)) return;
    int x = ox + c.left, y = oy + c.top;
    int w = c.width, h = c.height;

    SDL_Rect cr{clip.x, clip.y, clip.w, clip.h};
    SDL_SetRenderClipRect(renderer_, &cr);

    if (c.cls == "TPanel" || c.cls == "TNotebook" || c.cls == "TPage") {
        setColor(renderer_, backgroundOf(form, c));
        fillRect(renderer_, x, y, w, h);
        if (c.bevelOuter != 0 && c.cls == "TPanel") {
            unsigned hi = c.bevelOuter == 1 ? 0xFFFFFF : 0x808080;
            unsigned lo = c.bevelOuter == 1 ? 0x808080 : 0xFFFFFF;
            setColor(renderer_, hi);
            fillRect(renderer_, x, y, w, 1);
            fillRect(renderer_, x, y, 1, h);
            setColor(renderer_, lo);
            fillRect(renderer_, x, y + h - 1, w, 1);
            fillRect(renderer_, x + w - 1, y, 1, h);
        }
    } else if (c.cls == "TImage" || c.cls == "TautoButton") {
        const std::string& file = c.cls == "TImage" ? c.picture : c.bitmapUp;
        if (!file.empty()) {
            if (SDL_Texture* tex = textures_.get("MYULK/forms/" + file)) {
                float tw = 0, th = 0;
                SDL_GetTextureSize(tex, &tw, &th);
                SDL_FRect dst{float(x), float(y), tw, th};
                if (c.stretch) { dst.w = float(w); dst.h = float(h); }
                SDL_Rect inner{std::max(x, clip.x), std::max(y, clip.y), 0, 0};
                inner.w = std::min(x + w, clip.x + clip.w) - inner.x;
                inner.h = std::min(y + h, clip.y + clip.h) - inner.y;
                if (inner.w > 0 && inner.h > 0) {
                    SDL_SetRenderClipRect(renderer_, &inner);
                    SDL_RenderTexture(renderer_, tex, nullptr, &dst);
                    SDL_SetRenderClipRect(renderer_, &cr);
                }
            }
        }
    } else if (c.cls == "TShape") {
        setColor(renderer_, 0xFFFFFF);
        fillRect(renderer_, x, y, w, h);
        setColor(renderer_, 0x000000);
        SDL_FRect fr{float(x), float(y), float(w), float(h)};
        SDL_RenderRect(renderer_, &fr);
    } else if (c.cls == "TLabel" || c.cls == "TButton" || c.cls == "TRadioButton") {
        if (c.cls == "TLabel" && !c.transparent) {
            setColor(renderer_, backgroundOf(form, c));
            fillRect(renderer_, x, y, w, h);
        }
        if (c.cls == "TButton") {
            setColor(renderer_, 0xC0C0C0);
            fillRect(renderer_, x, y, w, h);
            setColor(renderer_, 0x000000);
            SDL_FRect fr{float(x), float(y), float(w), float(h)};
            SDL_RenderRect(renderer_, &fr);
        }
        const Font& f = c.font ? *c.font : form.font;
        std::string text = asciiFold(c.caption);
        float tw = float(text.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
        float tx = float(x);
        if (c.alignment == Align::Center || c.cls == "TButton") tx = x + (w - tw) / 2;
        else if (c.alignment == Align::Right) tx = x + w - tw;
        float ty = float(y) + std::max(0.0f, (std::min(h, f.height + 4) - 8) / 2.0f);
        setColor(renderer_, f.color);
        SDL_RenderDebugText(renderer_, tx, ty, text.c_str());
    } else if (c.cls == "TGauge") {
        setColor(renderer_, 0xFFFFFF);
        fillRect(renderer_, x, y, w, h);
        setColor(renderer_, 0x000000);
        SDL_FRect fr{float(x), float(y), float(w), float(h)};
        SDL_RenderRect(renderer_, &fr);
    }

    if (opt.outlines) {
        setColor(renderer_, c.events.empty() ? 0x00A0FF : 0xFF4040, 200);
        SDL_FRect fr{float(x), float(y), float(w), float(h)};
        SDL_RenderRect(renderer_, &fr);
    }

    if (!c.children.empty()) {
        Rect inner{std::max(x, clip.x), std::max(y, clip.y), 0, 0};
        inner.w = std::min(x + w, clip.x + clip.w) - inner.x;
        inner.h = std::min(y + h, clip.y + clip.h) - inner.y;
        if (inner.w <= 0 || inner.h <= 0) return;
        std::vector<const Component*> order;
        for (const auto& ch : c.children) if (isGraphicControl(*ch)) order.push_back(ch.get());
        for (const auto& ch : c.children) if (!isGraphicControl(*ch)) order.push_back(ch.get());
        for (const Component* ch : order) drawComponent(form, *ch, x, y, inner, opt);
    }
}

const Component* FormView::hitTest(const Form& form, float x, float y) const {
    Rect o = formOrigin(form);
    if (x < o.x || y < o.y || x >= o.x + o.w || y >= o.y + o.h) return nullptr;
    const Component* found = nullptr;
    // windowed children are on top, so test them first (reverse paint order)
    for (int pass = 0; pass < 2 && !found; ++pass)
        for (auto it = form.root->children.rbegin(); it != form.root->children.rend() && !found; ++it)
            if (isGraphicControl(**it) == (pass == 1)) found = hit(**it, o.x, o.y, x, y);
    return found ? found : form.root.get();
}

const Component* FormView::hit(const Component& c, int ox, int oy, float x, float y) const {
    if (!c.visible || isNonVisual(c)) return nullptr;
    int cx = ox + c.left, cy = oy + c.top;
    if (x < cx || y < cy || x >= cx + c.width || y >= cy + c.height) return nullptr;
    for (int pass = 0; pass < 2; ++pass)
        for (auto it = c.children.rbegin(); it != c.children.rend(); ++it)
            if (isGraphicControl(**it) == (pass == 1))
                if (auto* h = hit(**it, cx, cy, x, y)) return h;
    return &c;
}

}  // namespace myulk
