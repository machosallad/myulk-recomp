// The adventure screen: TAdvForm's layout with the scene picture and compass,
// driven by adv::Engine. Implements the engine's Host with SDL.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "adventure/engine.hpp"
#include "core/form.hpp"

struct SDL_Renderer;
union SDL_Event;

namespace myulk {

class Audio;
class Cursors;
class FormView;
class TextureCache;

class AdventureScreen : public adv::Host {
public:
    AdventureScreen(SDL_Renderer* renderer, TextureCache& textures, FormView& view,
                    Audio& audio, Cursors& cursors, std::string isoRoot);
    ~AdventureScreen() override;

    void start();
    void handleEvent(const SDL_Event& ev);   // event already in render coordinates
    void update(int elapsedMs);
    void draw();

    adv::Engine& engine() { return *engine_; }

    // adv::Host
    void showPicture(const std::string& dll, const std::string& name) override;
    void showCompass(const std::string& dll, const std::string& name) override;
    void playMusic(const std::string& file) override;
    void stopSound() override;
    void playSound(const std::string& file, adv::Done done) override;
    void openMovie(const std::string& file, int x, int y) override;
    void playMovie(adv::Done done) override;
    void stopMovie() override;
    void log(const std::string& msg) override;

private:
    std::string soundPath(const std::string& file) const;
    std::string picturePath(const std::string& dll, const std::string& name) const;
    void later(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

    SDL_Renderer* renderer_;
    TextureCache& textures_;
    FormView& view_;
    Audio& audio_;
    Cursors& cursors_;
    std::string isoRoot_;
    Form form_;
    std::unique_ptr<adv::Engine> engine_;

    int sceneX_ = 95, sceneY_ = 72;        // GImage position on screen
    int compassX_ = 550, compassY_ = 197;  // KImage position on screen
    std::string picture_, compass_;        // texture paths currently shown
    float mouseX_ = -1, mouseY_ = -1;
    std::deque<std::function<void()>> deferred_;
};

}  // namespace myulk
