// Ported scene procedures. Comments give the original routine and what the
// decompiled code does; see decomp/annotated/segNN.c for the source.
#include "adventure/scenes.hpp"

namespace myulk::adv::scenes {
namespace {

using S = Engine::Slot;

// Common to most scenes in this area: park both scene hotspots (2:2489).
void clearHotspots(Engine& e) {
    e.clearHotspot(S::Hot0);
    e.clearHotspot(S::Hot1);
}

// Area entry: picture DLL and music only change when different
// (the PStrCmp against DS:4C3C / DS:4D3E before 2:14F5 / 2:17B0).
void enterAe(Engine& e) {
    e.loadDll("nbae.dll");
    e.playMusic("mu_ae1.wav");
}

// ---- intro (segment 2) ------------------------------------------------------

// 2:43D5 TAdvForm.GImageClick4: postcard done, play vykort.avi; its end
// (MoviePlayer1Notify2, 2:5326) enables the scene handlers and enters 18:2EC0.
void introVykort(Engine& e) {
    e.setImageClick(nullptr);
    e.openMovie("vykort.avi");
    e.playMovie([&e] { s18_2EC0(e); });
}

// 2:4393 GImageClick3: third postcard picture.
void introTE1C(Engine& e) {
    e.showPicture("TE1C");
    e.setImageClick(&introVykort);
}

// 2:4357 GImageClick2: second postcard picture.
void introTE1B(Engine& e) {
    e.showPicture("TE1B");
    e.setImageClick(&introTE1C);
}

// 2:52E0 MoviePlayer1Notify1: title movie finished, first postcard.
void introTE1A(Engine& e) {
    e.loadDll("nb0.dll");
    e.showPicture("TE1A");
    e.setImageClick(&introTE1B);
}

}  // namespace

// 2:3CED TAdvForm.FormActivate (first activation): intro.avi plays with
// Wait=True (a click skips it), then tintro.avi with the title music.
void startIntro(Engine& e) {
    e.newGame();
    e.openMovie("intro.avi");
    e.playMovie([&e] {
        e.openMovie("tintro.avi");
        e.playMusic("mu_testa.wav");
        e.playMovie([&e] { introTE1A(e); });
    });
}

// ---- chapter 1 opening area, nbae.dll (segment 18) --------------------------

// 18:2EC0 AE1. First visit (DS:5032 = 0) shows AE1 with ae1.avi ready to
// play; afterwards AE1AA.
void s18_2EC0(Engine& e) {
    if (e.moviePlaying()) e.stopMovie();
    enterAe(e);
    e.showPicture(e.ds8(0x5032) == 0 ? "AE1" : "AE1AA");
    e.setHotspot(S::Hot0, 75, 85, 110, 100);
    e.setHotspot(S::Hot1, 350, 56, 44, 97);
    e.setExit(S::Left, &s18_306F);
    e.setExit(S::Right, &s18_3105);
    e.setExit(S::Hot0, e.ds8(0x5032) == 0 ? &s18_3001 : &s18_31F8);
    e.setExit(S::Hot1, &s18_3162);
    e.clearExit(S::Back);
    if (e.ds8(0x5032) == 0) e.openMovie("ae1.avi");
}

// 18:3001 first click on the AE1 hotspot: mark done, play ae1.avi, go to AE4.
void s18_3001(Engine& e) {
    e.ds8(0x5032) = 1;
    clearHotspots(e);
    e.clearExit(S::Forward);
    e.clearExit(S::Left);
    e.clearExit(S::Right);
    e.playMovie([&e] { s18_31F8(e); });   // 2:234C with Wait=True
}

// 18:306F AE1B, with ae1b.avi playing for 12 seconds.
void s18_306F(Engine& e) {
    clearHotspots(e);
    e.clearExit(S::Left);
    e.clearExit(S::Right);
    e.showPicture("AE1B");
    e.openMovie("ae1b.avi", 68, 0);
    e.playMovieFor(12000);
    e.setExit(S::Left, &s18_3258);
    e.setExit(S::Right, &s18_2EC0);
}

// 18:3105 AE1C.
void s18_3105(Engine& e) {
    e.showPicture("AE1C");
    clearHotspots(e);
    e.setExit(S::Left, &s18_2EC0);
    e.setExit(S::Right, &s18_3258);
}

// 18:3162 AE2: close-up; any click goes back to AE1.
void s18_3162(Engine& e) {
    e.showPicture("AE2");
    clearHotspots(e);
    e.clearExit(S::Forward);
    e.clearExit(S::Left);
    e.clearExit(S::Right);
    e.setExit(S::Back, &s18_2EC0);
}

// 18:31CA AE3.
void s18_31CA(Engine& e) {
    e.showPicture("AE3");
    e.setExit(S::Forward, &s18_3258);
    e.clearExit(S::Left);
}

// 18:31F8 AE4.
void s18_31F8(Engine& e) {
    e.showPicture("AE4");
    clearHotspots(e);
    e.setExit(S::Forward, &s18_32AD);
    e.clearExit(S::Left);
    e.clearExit(S::Right);
}

// 18:3258 AE5.
void s18_3258(Engine& e) {
    if (e.moviePlaying()) e.stopMovie();
    e.showPicture("AE5");
    e.clearExit(S::Forward);
    e.setExit(S::Left, &s18_3105);
    e.setExit(S::Right, &s18_306F);
}

// 18:32AD AE6. Forward plays ae6.avi the first time (DS:5033), then leads on
// to segment 6.
void s18_32AD(Engine& e) {
    e.showPicture("AE6");
    e.setExit(S::Forward, e.ds8(0x5033) == 0 ? &s18_3319 : &s06_009A);
    e.clearExit(S::Left);
    e.setExit(S::Right, &s18_33DC);
    if (e.ds8(0x5033) == 0) e.openMovie("ae6.avi");
}

// 18:3319 first time forward from AE6.
void s18_3319(Engine& e) {
    e.ds8(0x5033) = 1;
    e.clearExit(S::Forward);
    e.clearExit(S::Right);
    e.playMovie([&e] { s06_009A(e); });
}

// 18:335F AE7.
void s18_335F(Engine& e) {
    enterAe(e);
    e.showPicture("AE7");
    e.setExit(S::Forward, &s18_31CA);
    e.setExit(S::Left, &s18_33DC);
    e.clearExit(S::Right);
}

// 18:33DC AE9.
void s18_33DC(Engine& e) {
    e.showPicture("AE9");
    e.clearExit(S::Forward);
    e.setExit(S::Left, &s18_32AD);
    e.setExit(S::Right, &s18_335F);
}

// 6:009A: next area of chapter 1, not ported yet.
void s06_009A(Engine& e) {
    e.host().log("scene 6:009A not ported yet");
}

}  // namespace myulk::adv::scenes
