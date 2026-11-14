// Adventure engine: a port of the scene engine in TAdvForm (segment 2).
//
// The original works like this (see docs/game-outline.md):
//  * a scene is a procedure that shows a picture from a picture DLL, sets the
//    compass image and fills an exit table (DS:4F3E..4FA0) with procedure
//    pointers to other scenes;
//  * the 450x250 scene image has fixed direction zones plus up to ten scene
//    hotspots; the mouse cursor shows what a click would do;
//  * a click calls the procedure in the matching slot;
//  * anything that takes time (sounds, movies) continues through callbacks.
//
// This class holds that state and implements the engine calls with their
// original behaviour. Side effects that need SDL (drawing, audio, video) go
// through the Host interface, so the engine and the ported scenes can be
// unit-tested without a window.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace myulk::adv {

class Engine;
using Scene = void (*)(Engine&);
using Done = std::function<void()>;

// Screen.Cursors[] as loaded by TAdvForm.FormCreate (2:3ACD).
enum class Cursor : std::uint8_t {
    Default = 0, Backward = 1, Flat = 2, Forward = 3, Grab = 4,
    Left = 5, Right = 6, Up = 7, Down = 8, Axe = 9, Spade = 10,
};

struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    // Windows PtInRect: right and bottom are exclusive.
    bool contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
};

// What the engine asks the platform layer to do.
class Host {
public:
    virtual ~Host() = default;
    virtual void showPicture(const std::string& dll, const std::string& name) = 0;
    virtual void showCompass(const std::string& dll, const std::string& name) = 0;
    virtual void playMusic(const std::string& file) = 0;            // loops (sounds\)
    virtual void stopSound() = 0;
    virtual void playSound(const std::string& file, Done done) = 0; // sounds\, once
    virtual void openMovie(const std::string& file, int x, int y) = 0;
    virtual void playMovie(Done done) = 0;
    virtual void stopMovie() = 0;
    virtual void log(const std::string&) {}
};

class Engine {
public:
    static constexpr int kSceneW = 450;
    static constexpr int kSceneH = 250;

    explicit Engine(Host& host);

    // ---- data segment -------------------------------------------------
    // Game state is kept at its original data-segment addresses so ported
    // scene code reads like the original (DAT_1108_5032 -> ds8(0x5032)) and
    // save games (*.mlk) can be copied in and out unchanged.
    std::uint8_t& ds8(std::uint16_t addr) { return ds_[addr]; }
    std::uint8_t ds8(std::uint16_t addr) const { return ds_[addr]; }

    // ---- engine calls (original address in brackets) -----------------
    void newGame();                                   // [2:0B42] reset adventure state
    void loadDll(const std::string& name);            // [2:14F5] only when different
    const std::string& dll() const { return dll_; }   // DS:4C3C
    void showPicture(const std::string& name);        // [2:1584]
    void showCompass(const std::string& name);        // [2:1694]
    void playMusic(const std::string& file);          // [2:17B0] only when different
    const std::string& music() const { return music_; }  // DS:4D3E
    void stopSound();                                 // [2:182C]
    void playSound(const std::string& file, Done done = {});  // [2:1940]
    void openMovie(const std::string& file, int x = 0, int y = 0);  // [2:224A]
    void playMovie(Done done = {});                   // [2:234C]
    void playMovieFor(int ms);                        // [2:23BA] play, stop after ms
    void stopMovie();                                 // [2:23F4]
    bool moviePlaying() const { return movieOpen_; }  // DS:4E3E

    // Exit/action slots: the procedure pointers at DS:4F3E + 4*i.
    enum Slot : int {
        Forward = 0, Left, Right, Down, Up,             // 4F3E 4F42 4F46 4F4A 4F4E
        Hot0, Hot1, Hot2, Hot3, Hot4, Hot5, Hot6, Hot7, Hot8, Hot9,  // 4F52..4F76
        Back,                                          // 4F7A (anywhere else)
        Extra0, Extra1,                                // 4F7E 4F82
        kSlots
    };
    static Slot slotForAddr(std::uint16_t addr);      // DS address -> slot
    void setExit(Slot s, Scene scene) { exits_[s] = scene; }
    void clearExit(Slot s) { exits_[s] = nullptr; }
    Scene exit(Slot s) const { return exits_[s]; }

    // Scene hotspot rectangles (TAdvForm+0x251.. for Hot0..9, +0x2A1/2A9 Extra).
    void setHotspot(Slot s, int left, int top, int width, int height);  // [2:245D]
    void clearHotspot(Slot s);                        // [2:2489] moves it off-screen
    std::optional<Rect> hotspot(Slot s) const;

    // GImage.OnClick handler: while set, a click anywhere on the scene calls
    // it and the slot table is ignored (used by the intro, 2:4357..43D5).
    void setImageClick(Scene scene) { imageClick_ = scene; }

    // ---- input ---------------------------------------------------------
    Cursor cursorAt(int x, int y) const;              // [2:44CB] GImageMouseMove
    void click(int x, int y);                         // [2:4975] GImageMouseUp
    void go(Scene scene);                             // run a scene procedure

    // Location history (DS:57CC current, DS:57D0 previous), used by saves.
    Scene current() const { return current_; }
    Scene previous() const { return previous_; }

    // Advance timers (movie stop timer). Call once per frame with elapsed ms.
    void update(int elapsedMs);

    Host& host() { return host_; }

private:
    std::optional<Slot> slotAt(int x, int y) const;

    Host& host_;
    std::array<std::uint8_t, 0x10000> ds_{};
    std::array<Scene, kSlots> exits_{};
    std::array<Rect, kSlots> rects_{};
    std::string dll_, music_;
    Scene current_ = nullptr, previous_ = nullptr;
    Scene imageClick_ = nullptr;
    bool movieOpen_ = false;
    int movieStopMs_ = 0;
};

}  // namespace myulk::adv
