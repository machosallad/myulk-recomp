#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "adventure/engine.hpp"
#include "adventure/scenes.hpp"
#include "test.hpp"

using namespace myulk::adv;
using S = Engine::Slot;

namespace {

// Records what the engine asks for; movies and sounds finish when run().
struct FakeHost : Host {
    std::string picture, music, movie;
    std::vector<std::string> logs;
    std::deque<Done> pending;

    void showPicture(const std::string& dll, const std::string& name) override { picture = dll + ":" + name; }
    void showCompass(const std::string&, const std::string&) override {}
    void playMusic(const std::string& f) override { music = f; }
    void stopSound() override { music.clear(); }
    void playSound(const std::string&, Done d) override { if (d) pending.push_back(std::move(d)); }
    void openMovie(const std::string& f, int, int) override { movie = f; }
    void playMovie(Done d) override { if (d) pending.push_back(std::move(d)); }
    void stopMovie() override { movie.clear(); }
    void log(const std::string& m) override { logs.push_back(m); }

    void run() {
        while (!pending.empty()) {
            auto d = std::move(pending.front());
            pending.pop_front();
            d();
        }
    }
};

// Click in the middle of a slot's rectangle (scene coordinates).
void clickSlot(Engine& e, S slot) {
    auto r = e.hotspot(slot);
    static const Rect zones[] = {{148, 76, 300, 172}, {0, 0, 53, 249}, {396, 0, 449, 249},
                                 {54, 196, 395, 249}, {54, 0, 395, 53}};
    Rect z = r ? *r : zones[slot];
    e.click((z.left + z.right) / 2, (z.top + z.bottom) / 2);
}

}  // namespace

TEST(adventure_intro_reaches_first_scene) {
    FakeHost h;
    Engine e(h);
    scenes::startIntro(e);
    h.run();                                   // intro.avi, tintro.avi
    CHECK_EQ(h.picture, std::string("nb0.dll:TE1A"));
    CHECK_EQ(h.music, std::string("mu_testa.wav"));
    CHECK(e.cursorAt(10, 10) == Cursor::Forward);
    e.click(10, 10);
    CHECK_EQ(h.picture, std::string("nb0.dll:TE1B"));
    e.click(10, 10);
    CHECK_EQ(h.picture, std::string("nb0.dll:TE1C"));
    e.click(10, 10);
    h.run();                                   // vykort.avi
    CHECK_EQ(h.picture, std::string("nbae.dll:AE1"));
    CHECK_EQ(h.music, std::string("mu_ae1.wav"));
    CHECK_EQ(h.movie, std::string("ae1.avi"));
}

TEST(adventure_direction_zones_and_cursors) {
    FakeHost h;
    Engine e(h);
    scenes::s18_2EC0(e);
    CHECK(e.cursorAt(10, 120) == Cursor::Left);
    CHECK(e.cursorAt(420, 120) == Cursor::Right);
    CHECK(e.cursorAt(225, 220) == Cursor::Flat);   // no down exit here
    CHECK(e.cursorAt(100, 120) == Cursor::Forward); // hotspot 0 (75,85)-(185,185)
    clickSlot(e, S::Left);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE1B"));
    clickSlot(e, S::Right);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE1"));
}

TEST(adventure_first_hotspot_plays_once) {
    FakeHost h;
    Engine e(h);
    scenes::s18_2EC0(e);
    CHECK_EQ(int(e.ds8(0x5032)), 0);
    clickSlot(e, S::Hot0);                       // 18:3001 plays ae1.avi
    CHECK_EQ(int(e.ds8(0x5032)), 1);
    h.run();
    CHECK_EQ(h.picture, std::string("nbae.dll:AE4"));
    clickSlot(e, S::Forward);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE6"));
    clickSlot(e, S::Right);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE9"));
    clickSlot(e, S::Right);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE7"));
    clickSlot(e, S::Forward);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE3"));
    clickSlot(e, S::Forward);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE5"));
    clickSlot(e, S::Right);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE1B"));
    clickSlot(e, S::Right);                      // back at the start: now AE1AA
    CHECK_EQ(h.picture, std::string("nbae.dll:AE1AA"));
}

TEST(adventure_back_slot_and_unported_scene) {
    FakeHost h;
    Engine e(h);
    scenes::s18_2EC0(e);
    e.ds8(0x5032) = 1;
    scenes::s18_2EC0(e);
    clickSlot(e, S::Hot1);                       // close-up AE2
    CHECK_EQ(h.picture, std::string("nbae.dll:AE2"));
    CHECK(e.cursorAt(5, 5) == Cursor::Backward);
    e.click(5, 5);
    CHECK_EQ(h.picture, std::string("nbae.dll:AE1AA"));
    scenes::s18_32AD(e);
    e.ds8(0x5033) = 1;
    scenes::s18_32AD(e);
    clickSlot(e, S::Forward);
    CHECK_EQ(h.logs.size(), size_t(1));
}
