#include "app/adventure_screen.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <SDL3/SDL.h>

#include "adventure/scenes.hpp"
#include "app/assets.hpp"
#include "app/audio.hpp"
#include "app/cursors.hpp"
#include "app/form_view.hpp"

namespace fs = std::filesystem;

namespace myulk {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Absolute screen position of a component (sum of parent offsets).
void absPos(const Component* c, int& x, int& y) {
    x = y = 0;
    for (; c && c->parent; c = c->parent) {
        x += c->left;
        y += c->top;
    }
}

}  // namespace

AdventureScreen::AdventureScreen(SDL_Renderer* renderer, TextureCache& textures, FormView& view,
                                 Audio& audio, Cursors& cursors, std::string isoRoot)
    : renderer_(renderer), textures_(textures), view_(view), audio_(audio), cursors_(cursors),
      isoRoot_(std::move(isoRoot)) {
    form_ = Form::load((fs::path(textures_.root()) / "MYULK" / "forms" / "TADVFORM.json").string());
    if (auto* g = form_.root->find("GImage")) absPos(g, sceneX_, sceneY_);
    if (auto* k = form_.root->find("KImage")) absPos(k, compassX_, compassY_);
    engine_ = std::make_unique<adv::Engine>(*this);
}

AdventureScreen::~AdventureScreen() = default;

void AdventureScreen::start() { adv::scenes::startIntro(*engine_); }

// ---- Host ------------------------------------------------------------------

std::string AdventureScreen::picturePath(const std::string& dll, const std::string& name) const {
    // PICTDLL\NBAE.DLL resource AE1 -> extracted/resources/NBAE/bitmaps/AE1.bmp
    std::string mod = upper(fs::path(dll).stem().string());
    return mod + "/bitmaps/" + upper(name) + ".bmp";
}

std::string AdventureScreen::soundPath(const std::string& file) const {
    return (fs::path(isoRoot_) / "SOUNDS" / upper(file)).string();
}

void AdventureScreen::showPicture(const std::string& dll, const std::string& name) {
    picture_ = picturePath(dll, name);
}

void AdventureScreen::showCompass(const std::string& dll, const std::string& name) {
    compass_ = picturePath(dll, name);
}

void AdventureScreen::playMusic(const std::string& file) { audio_.playMusic(soundPath(file)); }

void AdventureScreen::stopSound() { audio_.stopAll(); }

void AdventureScreen::playSound(const std::string& file, adv::Done done) {
    audio_.playSound(soundPath(file), std::move(done));
}

void AdventureScreen::openMovie(const std::string& file, int x, int y) {
    SDL_Log("movie: open %s at (%d,%d) [video playback not implemented yet]", file.c_str(), x, y);
}

// Until video playback exists a movie "finishes" on the next frame, so the
// scene continues exactly as it would after the movie.
void AdventureScreen::playMovie(adv::Done done) {
    if (done) later(std::move(done));
}

void AdventureScreen::stopMovie() {}

void AdventureScreen::log(const std::string& msg) { SDL_Log("%s", msg.c_str()); }

// ---- frame -----------------------------------------------------------------

void AdventureScreen::handleEvent(const SDL_Event& ev) {
    switch (ev.type) {
        case SDL_EVENT_MOUSE_MOTION:
            mouseX_ = ev.motion.x;
            mouseY_ = ev.motion.y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                int x = int(ev.button.x) - sceneX_, y = int(ev.button.y) - sceneY_;
                if (x >= 0 && y >= 0 && x < adv::Engine::kSceneW && y < adv::Engine::kSceneH)
                    engine_->click(x, y);
            }
            break;
        default:
            break;
    }
}

void AdventureScreen::update(int elapsedMs) {
    audio_.update();
    engine_->update(elapsedMs);
    // run callbacks queued last frame (callbacks may queue more for next frame)
    auto pending = std::move(deferred_);
    deferred_.clear();
    for (auto& fn : pending) fn();
}

void AdventureScreen::draw() {
    view_.draw(form_, FormViewOptions{});
    if (!picture_.empty())
        if (SDL_Texture* t = textures_.get(picture_)) {
            SDL_FRect dst{float(sceneX_), float(sceneY_), float(adv::Engine::kSceneW),
                          float(adv::Engine::kSceneH)};
            SDL_RenderTexture(renderer_, t, nullptr, &dst);
        }
    if (!compass_.empty())
        if (SDL_Texture* t = textures_.get(compass_)) {
            float w = 0, h = 0;
            SDL_GetTextureSize(t, &w, &h);
            SDL_FRect dst{float(compassX_), float(compassY_), w, h};   // TAdvForm+0x214 rect
            SDL_RenderTexture(renderer_, t, nullptr, &dst);
        }
    int cursor = int(adv::Cursor::Flat);
    int x = int(mouseX_) - sceneX_, y = int(mouseY_) - sceneY_;
    if (x >= 0 && y >= 0 && x < adv::Engine::kSceneW && y < adv::Engine::kSceneH)
        cursor = int(engine_->cursorAt(x, y));
    if (mouseX_ >= 0) cursors_.draw(cursor, mouseX_, mouseY_);
}

}  // namespace myulk
