// The first-start screen and the import of the game files (see setup.hpp).
#include "recomp/enhance/setup.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "recomp/enhance/disc.hpp"
#include "recomp/enhance/ui.hpp"
#include "recomp/runtime/generated_support.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// The page's file picker (and drops on the page, web/recomp_shell.html) put
// the chosen file in Module.discFile.
EM_JS(void, web_pick_disc, (), {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.iso,.bin,.img,.cue';
    input.onchange = () => { if (input.files.length) Module.discFile = input.files[0]; };
    input.click();
});
EM_JS(int, web_disc_chosen, (), { return Module.discFile ? 1 : 0; });
EM_JS(char*, web_disc_name, (), { return stringToNewUTF8(Module.discFile ? Module.discFile.name : ''); });
EM_JS(double, web_disc_size, (), { return Module.discFile ? Module.discFile.size : 0; });
EM_JS(void, web_disc_forget, (), { Module.discFile = null; });
EM_ASYNC_JS(int, web_disc_read, (double offset, void* buf, int n), {
    try {
        const f = Module.discFile;
        if (!f || offset + n > f.size) return 0;
        const data = new Uint8Array(await f.slice(offset, offset + n).arrayBuffer());
        HEAPU8.set(data, buf);
        return 1;
    } catch (e) {
        console.warn('reading the disc image failed', e);
        return 0;
    }
});
// A file is collected in pieces, then stored in the browser's cache under the
// address the game would fetch it from ("game/<path>").
EM_JS(void, web_store_begin, (), { Module.storeParts = []; });
EM_JS(void, web_store_part, (const void* buf, int n), { Module.storeParts.push(HEAPU8.slice(buf, buf + n)); });
EM_ASYNC_JS(int, web_store_end, (const char* path), {
    try {
        const cache = await caches.open('myulk-own');
        await cache.put('game/' + UTF8ToString(path), new Response(new Blob(Module.storeParts)));
        Module.storeParts = [];
        return 1;
    } catch (e) {
        console.warn('storing a game file failed', e);
        return 0;
    }
});
EM_ASYNC_JS(int, web_store_text, (const char* path, const char* text), {
    try {
        const cache = await caches.open('myulk-own');
        await cache.put('game/' + UTF8ToString(path), new Response(UTF8ToString(text)));
        if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
        return 1;
    } catch (e) {
        console.warn('storing failed', e);
        return 0;
    }
});
EM_JS(int, web_can_store, (), { return typeof caches !== 'undefined' ? 1 : 0; });
#endif

namespace win16::setup {
namespace fs = std::filesystem;

namespace {

using namespace ui;

// Chosen in a dialog (its callback may come from another thread) or dropped.
std::mutex g_pickMutex;
std::string g_picked;
void picked(void*, const char* const* list, int) {
    std::lock_guard<std::mutex> lock(g_pickMutex);
    g_picked = list && list[0] ? list[0] : "";
}
std::string take_picked() {
    std::lock_guard<std::mutex> lock(g_pickMutex);
    std::string p;
    p.swap(g_picked);
    return p;
}

class SetupScreen : public OverlayScreen {
public:
    bool ok = false;

protected:
    enum { kImage = 1, kFolder, kQuit, kRetry };
    enum class State { Choose, Copying, Failed } state_ = State::Choose;
    std::unique_ptr<disc::Source> src_;
    std::vector<const disc::File*> todo_;
    size_t index_ = 0;
    uint64_t done_ = 0, total_ = 0, fileDone_ = 0;
    std::string error_, current_;
    std::unique_ptr<std::ofstream> out_;
    std::string manifest_;

    void draw(Painter& p) override {
        poll();
        if (state_ == State::Copying) step();
        p.fill(Rect{0, 0, 640, 480}, 0x0C1016);
        Rect d{60, 70, 580, 410};
        p.fill(d, kPanel);
        p.frame(d, kBorder);
        p.text(d.left + 24, d.top + 18, "V\xE4lkommen till Myulk", kText, true);
        int y = d.top + 70;
        auto line = [&](const std::string& t, Color c = kDim) { p.text(d.left + 24, y, p.fit(t, d.w() - 48), c); y += 22; };
        if (state_ == State::Choose) {
            line("Spelet beh\xF6ver filerna fr\xE5n din Myulk-cd.", kText);
#ifdef __EMSCRIPTEN__
            line("V\xE4lj cd-avbilden (iso eller bin), eller dra den hit.");
            line("Den l\xE4ses h\xE4r i webbl\xE4saren och sparas d\xE4r; inget laddas upp.");
            line("Sedan beh\xF6vs filen inte l\xE4ngre.");
            button(p, Rect{d.left + 24, d.bottom - 60, d.left + 224, d.bottom - 24}, "V\xE4lj skivavbild...", kImage, kAccent);
#else
            line("V\xE4lj cd-avbilden (iso, bin eller cue), cd-enheten eller en mapp");
            line("med cd:ns filer, eller dra den hit. Filerna kopieras en g\xE5ng;");
            line("sedan beh\xF6vs skivan inte l\xE4ngre.");
            button(p, Rect{d.left + 24, d.bottom - 60, d.left + 204, d.bottom - 24}, "V\xE4lj skivavbild...", kImage, kAccent);
            button(p, Rect{d.left + 214, d.bottom - 60, d.left + 404, d.bottom - 24}, "V\xE4lj mapp eller cd...", kFolder);
#endif
            button(p, Rect{d.right - 104, d.bottom - 60, d.right - 24, d.bottom - 24}, "Avsluta", kQuit);
        } else if (state_ == State::Copying) {
            line("Kopierar spelfilerna...", kText);
            line(current_);
            Rect bar{d.left + 24, y + 16, d.right - 24, y + 40};
            p.fill(bar, kCard);
            p.frame(bar, kBorder);
            int w = total_ ? int(int64_t(bar.w() - 4) * int64_t(done_) / int64_t(total_)) : 0;
            p.fill(Rect{bar.left + 2, bar.top + 2, bar.left + 2 + w, bar.bottom - 2}, kAccent);
            y += 56;
            line(std::to_string(done_ >> 20) + " av " + std::to_string(total_ >> 20) + " MB");
        } else {
            line("Det gick inte:", kText);
            std::string row, word;   // wrap the message word by word
            for (size_t i = 0; i <= error_.size(); ++i) {
                if (i < error_.size() && error_[i] != ' ') { word += error_[i]; continue; }
                std::string next = row.empty() ? word : row + " " + word;
                if (!row.empty() && p.width(next) > d.w() - 48) { line(row); row = word; }
                else row = next;
                word.clear();
            }
            if (!row.empty()) line(row);
            button(p, Rect{d.left + 24, d.bottom - 60, d.left + 184, d.bottom - 24}, "F\xF6rs\xF6k igen", kRetry, kAccent);
            button(p, Rect{d.right - 104, d.bottom - 60, d.right - 24, d.bottom - 24}, "Avsluta", kQuit);
        }
    }

    void activate(int id) override {
        if (id == kQuit) { close(); return; }
        if (id == kRetry) { state_ = State::Choose; return; }
        if (state_ != State::Choose) return;
#ifdef __EMSCRIPTEN__
        if (id == kImage) web_pick_disc();
#else
        static const SDL_DialogFileFilter filters[] = {{"Myulk-cd (iso, bin, cue)", "iso;bin;img;cue"}, {"Alla filer", "*"}};
        if (id == kImage) SDL_ShowOpenFileDialog(&picked, nullptr, host_window(), filters, 2, nullptr, false);
        if (id == kFolder) SDL_ShowOpenFolderDialog(&picked, nullptr, host_window(), nullptr, false);
#endif
    }

    void fail(const std::string& why) {
        error_ = why;
        state_ = State::Failed;
        src_.reset();
        out_.reset();
#ifdef __EMSCRIPTEN__
        web_disc_forget();
#endif
    }

    // a choice from the dialog, a drop, or MYULK_IMPORT (tests)
    void poll() {
        if (state_ != State::Choose) return;
        std::string path = take_picked();
        if (path.empty()) path = host_take_dropped();
        static bool envUsed = false;
        if (path.empty() && !envUsed && std::getenv("MYULK_IMPORT")) { envUsed = true; path = std::getenv("MYULK_IMPORT"); }
#ifdef __EMSCRIPTEN__
        if (path.empty() && web_disc_chosen()) {
            char* name = web_disc_name();
            std::string n = name;
            std::free(name);
            std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : n;
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".cue") { fail("V\xE4lj bin-filen som h\xF6r till cue-filen."); return; }
            if (!web_can_store()) { fail("Webbl\xE4saren kan inte spara filerna h\xE4r (sidan beh\xF6ver https eller localhost)."); return; }
            std::string err;
            src_ = disc::open_image([](uint64_t off, void* buf, size_t len) { return web_disc_read(double(off), buf, int(len)) != 0; }, err);
            if (!src_) { fail(err); return; }
            start();
        }
#endif
        if (path.empty()) return;
        recomp::log("setup: importing from %s", path.c_str());
        std::string err;
        src_ = disc::open_path(path, err);
        if (!src_) { fail(err); return; }
        start();
    }

    // check the game's version, then list what to copy
    void start() {
        const disc::File* exe = src_->find("MYULK/MYULK.EXE");
        if (!exe) { fail("Hittar inte MYULK\\MYULK.EXE. \xC4r det Myulk-cd:n?"); return; }
        std::vector<uint8_t> data(size_t(exe->size));
        if (!src_->read(*exe, 0, data.data(), data.size())) { fail("MYULK.EXE kunde inte l\xE4sas."); return; }
        if (!exe_matches(data)) {
            fail("Det h\xE4r \xE4r en annan version av Myulk \xE4n den programmet \xE4r gjort f\xF6r, "
                 "eller en skadad kopia.");
            return;
        }
        todo_.clear();
        total_ = done_ = 0;
        for (const disc::File& f : src_->files())
            if (disc::is_game_file(f.path)) { todo_.push_back(&f); total_ += f.size; }
        index_ = 0;
        fileDone_ = 0;
        manifest_.clear();
#ifndef __EMSCRIPTEN__
        std::error_code ec;
        fs::remove_all(fs::u8path(imported_dir() + ".part"), ec);
#endif
        state_ = State::Copying;
    }

    // copy a few megabytes per frame, so the screen keeps up
    void step() {
        std::vector<uint8_t> buf(1 << 20);
        for (int chunks = 0; chunks < 8 && state_ == State::Copying; ++chunks) {
            if (index_ >= todo_.size()) { finish(); return; }
            const disc::File& f = *todo_[index_];
            current_ = f.path;
            if (fileDone_ == 0) {
#ifdef __EMSCRIPTEN__
                web_store_begin();
#else
                fs::path dst = fs::u8path(imported_dir() + ".part") / fs::u8path(f.path);
                std::error_code ec;
                fs::create_directories(dst.parent_path(), ec);
                out_ = std::make_unique<std::ofstream>(dst, std::ios::binary);
                if (!*out_) {
                    auto where = dst.u8string();
                    fail("Kunde inte skriva " + std::string(where.begin(), where.end()));
                    return;
                }
#endif
            }
            size_t n = size_t(std::min<uint64_t>(buf.size(), f.size - fileDone_));
            if (n && !src_->read(f, fileDone_, buf.data(), n)) { fail("L\xE4sfel i " + f.path); return; }
#ifdef __EMSCRIPTEN__
            if (n) web_store_part(buf.data(), int(n));
#else
            out_->write(reinterpret_cast<const char*>(buf.data()), std::streamsize(n));
#endif
            fileDone_ += n;
            done_ += n;
            if (fileDone_ >= f.size) {
#ifdef __EMSCRIPTEN__
                if (!web_store_end(f.path.c_str())) { fail("Webbl\xE4saren har inte plats f\xF6r spelfilerna."); return; }
#else
                out_.reset();
#endif
                manifest_ += f.path + "\t" + std::to_string(f.size) + "\n";
                ++index_;
                fileDone_ = 0;
            }
        }
    }

    void finish() {
#ifdef __EMSCRIPTEN__
        if (!web_store_text("manifest.txt", manifest_.c_str())) { fail("Webbl\xE4saren kunde inte spara spelfilerna."); return; }
        web_disc_forget();
#else
        std::error_code ec;
        fs::remove_all(fs::u8path(imported_dir()), ec);
        fs::rename(fs::u8path(imported_dir() + ".part"), fs::u8path(imported_dir()), ec);
        if (ec) { fail("Kunde inte flytta spelfilerna p\xE5 plats: " + ec.message()); return; }
#endif
        recomp::log("setup: %zu files, %llu bytes imported", todo_.size(), (unsigned long long)total_);
        ok = true;
        close();
    }
};

}  // namespace

std::string imported_dir() {
    if (const char* d = std::getenv("MYULK_IMPORT_DIR")) return d;   // tests
    std::string dir;
    if (char* p = SDL_GetPrefPath("myulk-recomp", "Myulk")) {
        dir = std::string(p) + "game";
        SDL_free(p);
    }
    return dir;
}

bool exe_matches(const std::vector<uint8_t>& exe) {
    return exe.size() == recomp::kExeSize && recomp::fnv1a64(exe.data(), exe.size()) == recomp::kExeHash;
}

bool run() {
    host_system_cursor(true);
    SetupScreen s;
    s.run();
    host_system_cursor(false);
    return s.ok;
}

}  // namespace win16::setup
