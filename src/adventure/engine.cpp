#include "adventure/engine.hpp"

namespace myulk::adv {
namespace {

// Rect(Left, Top, Right, Bottom) values from 2:0B42.
constexpr Rect kForward{148, 76, 300, 172};
constexpr Rect kLeft{0, 0, 53, 249};
constexpr Rect kRight{396, 0, 449, 249};
constexpr Rect kDown{54, 196, 395, 249};
constexpr Rect kUp{54, 0, 395, 53};
// 2:2489 parks a hotspot at (1500,1500): never hit inside the 450x250 image.
constexpr Rect kOff{1500, 0, 1500, 0};

}  // namespace

Engine::Engine(Host& host) : host_(host) { newGame(); }

Engine::Slot Engine::slotForAddr(std::uint16_t addr) {
    return static_cast<Slot>((addr - 0x4F3E) / 4);
}

void Engine::newGame() {
    exits_.fill(nullptr);
    rects_.fill(kOff);
    rects_[Forward] = kForward;
    rects_[Left] = kLeft;
    rects_[Right] = kRight;
    rects_[Down] = kDown;
    rects_[Up] = kUp;
    dll_.clear();
    music_.clear();
    movieOpen_ = false;
    movieStopMs_ = 0;
    current_ = previous_ = imageClick_ = nullptr;
    // The chapter-1 state block (DS:502E..) is reset by the chapter start;
    // a zeroed data segment matches the original's initial values for the
    // flags used so far.
}

void Engine::loadDll(const std::string& name) { dll_ = name; }

void Engine::showPicture(const std::string& name) { host_.showPicture(dll_, name); }

void Engine::showCompass(const std::string& name) { host_.showCompass(dll_, name); }

void Engine::playMusic(const std::string& file) {
    if (music_ == file) return;
    music_ = file;
    host_.playMusic(file);
}

void Engine::stopSound() {
    music_.clear();
    host_.stopSound();
}

void Engine::playSound(const std::string& file, Done done) {
    host_.playSound(file, std::move(done));
}

void Engine::openMovie(const std::string& file, int x, int y) {
    movieOpen_ = true;
    host_.openMovie(file, x, y);
}

void Engine::playMovie(Done done) {
    host_.playMovie([this, done = std::move(done)] {
        movieOpen_ = false;
        if (done) done();
    });
}

void Engine::playMovieFor(int ms) {
    playMovie();
    movieStopMs_ = ms;
}

void Engine::stopMovie() {
    movieStopMs_ = 0;
    if (movieOpen_) host_.stopMovie();
    movieOpen_ = false;
}

void Engine::setHotspot(Slot s, int left, int top, int width, int height) {
    rects_[s] = Rect{left, top, left + width, top + height};
}

void Engine::clearHotspot(Slot s) { rects_[s] = kOff; }

std::optional<Rect> Engine::hotspot(Slot s) const {
    if (rects_[s].left >= 1500) return std::nullopt;
    return rects_[s];
}

// Test order from GImageMouseMove / GImageMouseUp: extra rects, scene
// hotspots, then the five direction zones; a slot only counts when its
// procedure pointer is set.
std::optional<Engine::Slot> Engine::slotAt(int x, int y) const {
    static constexpr Slot order[] = {Extra0, Extra1, Hot0, Hot1, Hot2, Hot3, Hot4, Hot5,
                                     Hot6, Hot7, Hot8, Hot9, Forward, Left, Right, Down, Up};
    for (Slot s : order)
        if (exits_[s] && rects_[s].contains(x, y)) return s;
    if (exits_[Back]) return Back;
    return std::nullopt;
}

Cursor Engine::cursorAt(int x, int y) const {
    if (imageClick_) return Cursor::Forward;     // TAdvForm.FirstMouseMove
    auto s = slotAt(x, y);
    if (!s) return Cursor::Flat;
    switch (*s) {
        case Forward: return Cursor::Forward;
        case Left: return Cursor::Left;
        case Right: return Cursor::Right;
        case Down: return Cursor::Down;
        case Up: return Cursor::Up;
        case Back: return Cursor::Backward;
        default: return Cursor::Forward;   // hotspots use the "forward" hand
    }
}

void Engine::click(int x, int y) {
    if (imageClick_) {
        go(imageClick_);
        return;
    }
    auto s = slotAt(x, y);
    if (!s) return;
    Scene target = exits_[*s];
    // Location history as in GImageMouseUp: left/right set the current
    // location (DS:57CC); hotspots 0-2 and "back" also shift it to the
    // previous one (DS:57D0). Forward/up/down and other hotspots leave it.
    switch (*s) {
        case Left: case Right:
            current_ = target;
            break;
        case Hot0: case Hot1: case Hot2: case Back:
            previous_ = current_;
            current_ = target;
            break;
        default:
            break;
    }
    go(target);
}

void Engine::go(Scene scene) {
    if (scene) scene(*this);
}

void Engine::update(int elapsedMs) {
    if (movieStopMs_ > 0) {
        movieStopMs_ -= elapsedMs;
        if (movieStopMs_ <= 0) stopMovie();   // MovieTimer (2:545C)
    }
}

}  // namespace myulk::adv
