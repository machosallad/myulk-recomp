// Shared pieces for native screens drawn over the game (save/load, settings):
// a painter on the 640x480 screen and a base class with buttons and a loop.
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <vector>

#include "recomp/win16/gdi.hpp"
#include "recomp/win16/user.hpp"

namespace win16::ui {

constexpr Color kPanel = 0x1E2630, kCard = 0x2A3440, kCardHover = 0x3A4858, kBorder = 0x56687A;
constexpr Color kText = 0xF0F0F0, kDim = 0xA8B4C0, kAccent = 0xF0C040, kDanger = 0xD05050;

struct Painter {
    Surface& s = screen();
    uint16_t hdc = dc_create_window(0, 0, 0, Rect{0, 0, 640, 480});
    uint16_t fontBig = 0, fontNormal = 0;
    Painter() {
        auto f = std::make_unique<GdiObject>();
        f->type = ObjType::Font; f->face = "Arial"; f->height = -26; f->weight = 700;
        fontBig = gdi_add(std::move(f));
        auto g = std::make_unique<GdiObject>();
        g->type = ObjType::Font; g->face = "System"; g->height = 16;
        fontNormal = gdi_add(std::move(g));
    }
    ~Painter() { dc_delete(hdc); }
    void fill(Rect r, Color c) {
        r = r.intersect(Rect{0, 0, s.w, s.h});
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x) s.set(x, y, c);
    }
    void frame(Rect r, Color c) {
        fill(Rect{r.left, r.top, r.right, r.top + 1}, c);
        fill(Rect{r.left, r.bottom - 1, r.right, r.bottom}, c);
        fill(Rect{r.left, r.top, r.left + 1, r.bottom}, c);
        fill(Rect{r.right - 1, r.top, r.right, r.bottom}, c);
    }
    void text(int x, int y, const std::string& t, Color c, bool big = false) {
        DC* dc = dc_get(hdc);
        if (!dc) return;
        dc->font = big ? fontBig : fontNormal;
        dc->textColor = c;
        dc->bkMode = 1;
        draw_text(*dc, x, y, t, nullptr, 0);
    }
    int width(const std::string& t, bool big = false) { return text_width(big ? fontBig : fontNormal, t); }
    // fit text into a width, ending with "..." when it is cut
    std::string fit(std::string t, int w, bool big = false) {
        if (width(t, big) <= w) return t;
        while (!t.empty() && width(t + "...", big) > w) t.pop_back();
        return t + "...";
    }
    void image(int x0, int y0, const Surface& img) {
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
                if (x0 + x >= 0 && y0 + y >= 0 && x0 + x < s.w && y0 + y < s.h) s.set(x0 + x, y0 + y, img.get(x, y));
    }
};

// The game's picture when its menu was last opened (save thumbnails, backdrops).
const Surface& game_frame();
void set_game_frame(const Surface& s);

// savescreen.cpp: the next SaveForm/LoadForm.ShowModal answers with `file`
// (8.3 name in C:\MYULK) without showing a screen; saving names it `name`.
void auto_answer(const std::string& file, const std::string& name);
bool save_exists(const std::string& file);
std::string saved_at(const std::string& file);   // "YYYY-MM-DD HH:MM" or ""
// The start card: offers the newest save; returns its file to continue, or "".
std::string run_start_card();
// settings_screen.cpp: the first-start question (Modern or Original play style).
void run_style_card();

// A modal screen over the game: input goes to it, the game waits until it closes.
class OverlayScreen : public InputOverlay {
public:
    void run() {
        Surface& scr = screen();
        Surface under = scr;
        backdrop_ = scr;
        for (Color& c : backdrop_.px) c = (c >> 2) & 0x3F3F3F;   // dimmed game picture
        painter_ = std::make_unique<Painter>();
        set_input_overlay(this);
        while (!done_) {
            elements_.clear();
            painter_->s.px = backdrop_.px;
            draw(*painter_);
            host_pump(16);
        }
        set_input_overlay(nullptr);
        host_text_input(false);
        painter_.reset();
        scr = under;
        screen_invalidate();
    }

    void mouse(int x, int y, int button, bool down) override {
        mx_ = x; my_ = y;
        if (button != 0) return;
        if (down) { pressed_ = hit(x, y); return; }
        int id = hit(x, y);
        if (id && id == pressed_) activate(id);
        pressed_ = 0;
    }
    void key(int vk, bool down) override {
        if (!down) return;
        if (vk == 0x1B) { close(); return; }   // Esc
        on_key(vk);
    }
    void character(int ch) override { on_char(ch); }

protected:
    virtual void draw(Painter& p) = 0;
    virtual void activate(int id) = 0;
    virtual void on_key(int) {}
    virtual void on_char(int) {}

    void close() { done_ = true; }
    void add(Rect r, int id) { elements_.push_back({r, id}); }
    bool hover(const Rect& r) const { return r.contains(mx_, my_); }
    void button(Painter& p, Rect r, const std::string& label, int id, Color accent = kBorder) {
        p.fill(r, hover(r) ? kCardHover : kCard);
        p.frame(r, accent);
        p.text(r.left + (r.w() - p.width(label)) / 2, r.top + (r.h() - 16) / 2, label, kText);
        add(r, id);
    }
    void panel(Painter& p, Rect r, const std::string& title) {
        p.fill(r, kPanel);
        p.frame(r, kBorder);
        p.text(r.left + 20, r.top + 14, title, kText, true);
    }

private:
    struct Element { Rect r; int id; };
    int hit(int x, int y) const {
        for (auto it = elements_.rbegin(); it != elements_.rend(); ++it)
            if (it->r.contains(x, y)) return it->id;
        return 0;
    }
    std::vector<Element> elements_;
    std::unique_ptr<Painter> painter_;
    Surface backdrop_;
    bool done_ = false;
    int mx_ = -1, my_ = -1, pressed_ = 0;
};

}  // namespace win16::ui
