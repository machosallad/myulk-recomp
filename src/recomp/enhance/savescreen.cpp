// A modern save/load screen in place of the original Windows file dialogs.
//
// TMenuForm.SparaBtnClick / LaddaBtnClick (6:3AA0 / 6:3C2F) call
// SaveForm.ShowModal / LoadForm.ShowModal and, on mrOk, read the file name at
// Self+0x1A4 and write or read the 4199-byte record themselves. Hooking
// TForm.ShowModal (29:575A, tools/recomp/hooks.txt) for those two forms keeps
// all of that: this screen only chooses the file, so saves stay compatible
// with the original game. Each save can have two side files next to it:
// NAME.BMP (160x120 picture of the game when the menu was opened) and NAME.TXT
// (display name and the time it was saved). MYULK_CLASSIC_SAVE=1 brings back
// the original dialogs.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "funcs.hpp"
#include "recomp/runtime/generated_support.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/gdi.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/vfs.hpp"

using recomp::Cpu;
using recomp::Runtime;

namespace win16 {
namespace {

constexpr int kThumbW = 160, kThumbH = 120;
constexpr int kRecordSize = 4199;
const char* kSaveDir = "C:\\MYULK\\";

Surface g_gameFrame;   // the game's picture when the menu was opened

// ---- save files ---------------------------------------------------------------

struct SaveInfo {
    std::string file;     // 8.3 file name without directory, e.g. SPAR001.MLK
    std::string name;     // display name (Windows-1252)
    std::string savedAt;  // "2026-11-15 12:30" (empty for saves without a side file)
    std::string where;    // "Kapitel 2" / "Gården · 12 maj 1998"
    std::string money;    // "125 000 kr" (farm only)
    Surface thumb;        // empty when there is no picture
};

std::string stem(const std::string& file) {
    size_t dot = file.rfind('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

bool read_file(const std::string& dos, std::vector<uint8_t>& out) {
    int err = 0;
    int h = vfs::open(dos, 0, &err);
    if (h < 0) return false;
    long n = vfs::size(h);
    out.resize(size_t(std::max(n, 0L)));
    long got = n > 0 ? vfs::read(h, out.data(), n) : 0;
    vfs::close(h);
    return got == n;
}

bool write_file(const std::string& dos, const std::vector<uint8_t>& data) {
    int err = 0;
    int h = vfs::create(dos, &err);
    if (h < 0) return false;
    bool ok = vfs::write(h, data.data(), long(data.size())) == long(data.size());
    vfs::close(h);
    return ok;
}

std::string group_thousands(long v) {
    std::string s = std::to_string(v < 0 ? -v : v), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += ' ';
        out += s[i];
    }
    return (v < 0 ? "-" : "") + out;
}

// The farm's calendar: 360-day years of 30-day months, year 1 = 1997.
std::string farm_date(int day) {
    static const char* months[] = {"jan", "feb", "mar", "apr", "maj", "jun",
                                   "jul", "aug", "sep", "okt", "nov", "dec"};
    int year = day / 360 + 1;
    int month = (day % 360 - 1) / 30 + 1;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    int dom = day % 30 == 0 ? 30 : day % 30;
    return std::to_string(dom) + " " + months[month - 1] + " " + std::to_string(1996 + year);
}

void describe(const std::vector<uint8_t>& rec, SaveInfo& s) {
    if (rec.size() < size_t(kRecordSize)) return;
    auto i16 = [&](size_t o) { return int(int16_t(rec[o] | (rec[o + 1] << 8))); };
    auto i32 = [&](size_t o) { return long(int32_t(rec[o] | (rec[o + 1] << 8) | (rec[o + 2] << 16) | (uint32_t(rec[o + 3]) << 24))); };
    int mode = i16(0);
    constexpr size_t status = 864 + (0x3F24 - 0x36A4);   // farm status record in the save
    if (mode >= 1 && mode <= 3) {
        s.where = "Kapitel " + std::to_string(mode);
    } else {
        s.where = "G\xE5rden, " + farm_date(i16(status + 9));
        s.money = group_thousands(i32(status)) + " kr";
    }
}

Surface read_bmp(const std::vector<uint8_t>& d) {
    Surface s;
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return s;
    auto u32 = [&](size_t o) { return uint32_t(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (uint32_t(d[o + 3]) << 24)); };
    uint32_t off = u32(10);
    int w = int(int32_t(u32(18))), h = int(int32_t(u32(22)));
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || (d[28] | (d[29] << 8)) != 24) return s;
    size_t stride = (size_t(w) * 3 + 3) & ~size_t(3);
    if (off + stride * size_t(h) > d.size()) return s;
    s = Surface(w, h, 32);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = d.data() + off + stride * size_t(h - 1 - y);
        for (int x = 0; x < w; ++x) s.set(x, y, (uint32_t(row[x * 3 + 2]) << 16) | (row[x * 3 + 1] << 8) | row[x * 3]);
    }
    return s;
}

std::vector<uint8_t> write_bmp(const Surface& s) {
    size_t stride = (size_t(s.w) * 3 + 3) & ~size_t(3);
    uint32_t size = uint32_t(54 + stride * size_t(s.h));
    std::vector<uint8_t> d(size, 0);
    auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) d[o + i] = uint8_t(v >> (8 * i)); };
    d[0] = 'B'; d[1] = 'M';
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, uint32_t(s.w)); put32(22, uint32_t(s.h));
    d[26] = 1; d[28] = 24;
    put32(34, uint32_t(stride * size_t(s.h)));
    for (int y = 0; y < s.h; ++y) {
        uint8_t* row = d.data() + 54 + stride * size_t(s.h - 1 - y);
        for (int x = 0; x < s.w; ++x) {
            Color c = s.get(x, y);
            row[x * 3] = uint8_t(c); row[x * 3 + 1] = uint8_t(c >> 8); row[x * 3 + 2] = uint8_t(c >> 16);
        }
    }
    return d;
}

Surface make_thumb(const Surface& src) {
    Surface t(kThumbW, kThumbH, 32);
    if (src.w <= 0) return Surface();
    int fx = src.w / kThumbW, fy = src.h / kThumbH;   // 4 x 4 boxes for 640 x 480
    for (int y = 0; y < kThumbH; ++y)
        for (int x = 0; x < kThumbW; ++x) {
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < fy; ++j)
                for (int i = 0; i < fx; ++i) {
                    Color c = src.get(x * fx + i, y * fy + j);
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255;
                }
            uint32_t n = uint32_t(fx * fy);
            t.set(x, y, ((r / n) << 16) | ((g / n) << 8) | (b / n));
        }
    return t;
}

std::vector<SaveInfo> list_saves() {
    std::vector<SaveInfo> out;
    for (const auto& e : vfs::find(std::string(kSaveDir) + "*.MLK", 0)) {
        if (e.dir) continue;
        SaveInfo s;
        s.file = e.name;
        s.name = stem(e.name);
        std::vector<uint8_t> data;
        if (read_file(kSaveDir + e.name, data)) describe(data, s);
        if (read_file(kSaveDir + stem(e.name) + ".TXT", data)) {
            std::string t(data.begin(), data.end());
            size_t nl = t.find('\n');
            std::string first = t.substr(0, nl);
            if (!first.empty()) s.name = first;
            if (nl != std::string::npos) {
                std::string second = t.substr(nl + 1);
                second = second.substr(0, second.find('\n'));
                s.savedAt = second;
            }
        }
        if (read_file(kSaveDir + stem(e.name) + ".BMP", data)) s.thumb = read_bmp(data);
        out.push_back(std::move(s));
    }
    // newest first; saves without a time (older or original ones) after them
    std::stable_sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.savedAt > b.savedAt; });
    return out;
}

std::string new_file_name() {
    for (int i = 1; i < 1000; ++i) {
        char b[16];
        std::snprintf(b, sizeof b, "SPAR%03d.MLK", i);
        int err = 0;
        int h = vfs::open(kSaveDir + std::string(b), 0, &err);
        if (h < 0) return b;
        vfs::close(h);
    }
    return "SPAR999.MLK";
}

// "2026-11-15 12:20" -> "15 nov 12:20"
std::string short_time(const std::string& t) {
    static const char* months[] = {"jan", "feb", "mar", "apr", "maj", "jun",
                                   "jul", "aug", "sep", "okt", "nov", "dec"};
    if (t.size() < 16) return t;
    int m = std::atoi(t.substr(5, 2).c_str()), d = std::atoi(t.substr(8, 2).c_str());
    if (m < 1 || m > 12) return t;
    return std::to_string(d) + " " + months[m - 1] + " " + t.substr(11, 5);
}

std::string now_text() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", tm);
    return b;
}

// ---- drawing ------------------------------------------------------------------

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

// ---- the screen -----------------------------------------------------------------

struct Element { Rect r; int id; };
enum Id { kCancel = 1, kPrev, kNext, kOk, kNo, kNew, kCardBase = 100, kDeleteBase = 200 };

class SaveScreen : public InputOverlay {
public:
    explicit SaveScreen(bool save) : save_(save) {}

    // Runs until the player picks a save (returns its 8.3 file name) or cancels ("").
    std::string run() {
        Surface& scr = screen();
        under_ = scr;
        backdrop_ = g_gameFrame.w == scr.w && g_gameFrame.h == scr.h ? g_gameFrame : scr;
        for (Color& c : backdrop_.px) c = ((c >> 2) & 0x3F3F3F);   // dimmed game picture
        saves_ = list_saves();
        painter_ = std::make_unique<Painter>();
        set_input_overlay(this);
        while (!done_) {
            draw();
            host_pump(16);
        }
        set_input_overlay(nullptr);
        host_text_input(false);
        painter_.reset();
        scr = under_;
        screen_invalidate();
        return result_;
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
        if (vk == 0x1B) activate(mode_ == List ? kCancel : kNo);          // Esc
        else if (vk == 0x0D && mode_ != List) activate(kOk);               // Enter
        else if (mode_ == List && vk == 0x25) activate(kPrev);             // left
        else if (mode_ == List && vk == 0x27) activate(kNext);             // right
    }
    void character(int ch) override {
        if (mode_ != Name) return;
        if (ch == 8) { if (!name_.empty()) name_.pop_back(); }
        else if (ch >= 0x20 && ch != 0x7F && name_.size() < 32) name_ += char(ch);
    }

private:
    enum Mode { List, Name, ConfirmLoad, ConfirmDelete };
    bool save_;
    bool done_ = false;
    std::string result_;
    Surface under_, backdrop_;
    std::vector<SaveInfo> saves_;
    std::vector<Element> elements_;
    Mode mode_ = List;
    int page_ = 0, selected_ = -1;   // selected_: save index, -1 = new save
    std::string name_;
    int mx_ = -1, my_ = -1, pressed_ = 0;
    std::unique_ptr<Painter> painter_;

    int perPage() const { return save_ ? 5 : 6; }   // the save screen's first card is "new"
    int pages() const { return std::max(1, int((saves_.size() + size_t(perPage()) - 1) / size_t(perPage()))); }

    int hit(int x, int y) const {
        for (auto it = elements_.rbegin(); it != elements_.rend(); ++it)
            if (it->r.contains(x, y)) return it->id;
        return 0;
    }
    bool hover(const Rect& r) const { return r.contains(mx_, my_); }

    std::string default_name() const {
        if (g_gameFrame.w == 0) return "Sparat spel";
        return "Sparat " + now_text();
    }

    void activate(int id) {
        if (mode_ == List) {
            if (id == kCancel) { done_ = true; return; }
            if (id == kPrev && page_ > 0) --page_;
            if (id == kNext && page_ + 1 < pages()) ++page_;
            if (id == kNew) { selected_ = -1; name_ = default_name(); enter(Name); }
            if (id >= kDeleteBase) { selected_ = id - kDeleteBase; enter(ConfirmDelete); return; }
            if (id >= kCardBase) {
                selected_ = id - kCardBase;
                if (save_) { name_ = saves_[size_t(selected_)].name; enter(Name); }
                else enter(ConfirmLoad);
            }
            return;
        }
        if (id == kNo) { enter(List); return; }
        if (id != kOk) return;
        if (mode_ == Name) {
            std::string file = selected_ >= 0 ? saves_[size_t(selected_)].file : new_file_name();
            std::string nm = name_.empty() ? default_name() : name_;
            std::string txt = nm + "\n" + now_text() + "\n";
            write_file(kSaveDir + stem(file) + ".TXT", std::vector<uint8_t>(txt.begin(), txt.end()));
            if (g_gameFrame.w > 0) write_file(kSaveDir + stem(file) + ".BMP", write_bmp(make_thumb(g_gameFrame)));
            result_ = file;
            done_ = true;
        } else if (mode_ == ConfirmLoad) {
            result_ = saves_[size_t(selected_)].file;
            done_ = true;
        } else if (mode_ == ConfirmDelete) {
            const std::string& f = saves_[size_t(selected_)].file;
            vfs::remove(kSaveDir + f);
            vfs::remove(kSaveDir + stem(f) + ".TXT");
            vfs::remove(kSaveDir + stem(f) + ".BMP");
            saves_ = list_saves();
            page_ = std::min(page_, pages() - 1);
            enter(List);
        }
    }

    void enter(Mode m) {
        mode_ = m;
        host_text_input(m == Name);
    }

    void button(Painter& p, Rect r, const std::string& label, int id, Color accent = kBorder) {
        bool h = hover(r);
        p.fill(r, h ? kCardHover : kCard);
        p.frame(r, accent);
        p.text(r.left + (r.w() - p.width(label)) / 2, r.top + (r.h() - 16) / 2, label, kText);
        elements_.push_back({r, id});
    }

    void draw_card(Painter& p, Rect r, int index) {
        bool h = hover(r) && mode_ == List;
        p.fill(r, h ? kCardHover : kCard);
        p.frame(r, h ? kAccent : kBorder);
        Rect img{r.left + 4, r.top + 4, r.left + 4 + kThumbW, r.top + 4 + kThumbH};
        if (index < 0) {   // new save
            p.fill(img, 0x18202A);
            std::string plus = "+";
            p.text(img.left + (kThumbW - p.width(plus, true)) / 2, img.top + 36, plus, kAccent, true);
            std::string t = "Ny sparning";
            p.text(img.left + (kThumbW - p.width(t)) / 2, img.top + 80, t, kText);
            p.text(r.left + 6, r.top + 130, p.fit(g_gameFrame.w ? "Spara spelet h\xE4r" : "", kThumbW), kDim);
            elements_.push_back({r, kNew});
            return;
        }
        const SaveInfo& s = saves_[size_t(index)];
        if (s.thumb.w == kThumbW && s.thumb.h == kThumbH) p.image(img.left, img.top, s.thumb);
        else {
            p.fill(img, 0x18202A);
            std::string t = "Ingen bild";
            p.text(img.left + (kThumbW - p.width(t)) / 2, img.top + 52, t, kDim);
        }
        p.text(r.left + 6, r.top + 128, p.fit(s.name, kThumbW - 2), kText);
        p.text(r.left + 6, r.top + 144, p.fit(s.where, kThumbW - 2), kDim);
        if (!s.money.empty()) p.text(r.left + 6, r.top + 160, s.money, kAccent);
        if (!s.savedAt.empty()) {
            std::string when = short_time(s.savedAt);
            p.text(r.right - 6 - p.width(when), r.top + 160, when, kDim);
        }
        elements_.push_back({r, kCardBase + index});
        // delete button in the picture's corner
        Rect del{img.right - 22, img.top + 2, img.right - 2, img.top + 22};
        p.fill(del, hover(del) ? kDanger : Color(0x303840));
        p.text(del.left + 6, del.top + 2, "x", kText);
        elements_.push_back({del, kDeleteBase + index});
    }

    void dialog(Painter& p, const std::string& title, const std::string& detail, const std::string& ok, bool danger,
                const std::string& note = std::string()) {
        Rect d{120, 150, 520, 330};
        p.fill(d, kPanel);
        p.frame(d, danger ? kDanger : kAccent);
        p.text(d.left + 20, d.top + 18, p.fit(title, d.w() - 40, true), kText, true);
        if (!detail.empty()) p.text(d.left + 20, d.top + 64, p.fit(detail, d.w() - 40), mode_ == Name ? kDim : kText);
        if (!note.empty()) p.text(d.left + 20, d.top + 88, p.fit(note, d.w() - 40), kDim);
        if (mode_ == Name) {
            Rect field{d.left + 20, d.top + 88, d.right - 20, d.top + 116};
            p.fill(field, 0x101820);
            p.frame(field, kAccent);
            std::string shown = name_;
            while (!shown.empty() && p.width(shown) > field.w() - 16) shown.erase(0, 1);
            p.text(field.left + 6, field.top + 6, shown, kText);
            int cx = field.left + 6 + p.width(shown);
            if ((SDL_GetTicks() / 500) % 2) p.fill(Rect{cx + 1, field.top + 5, cx + 2, field.bottom - 5}, kText);
        }
        button(p, Rect{d.right - 230, d.bottom - 50, d.right - 130, d.bottom - 16}, ok, kOk, danger ? kDanger : kAccent);
        button(p, Rect{d.right - 120, d.bottom - 50, d.right - 20, d.bottom - 16}, "Avbryt", kNo);
    }

    void draw() {
        Painter& p = *painter_;
        elements_.clear();
        p.s.px = backdrop_.px;
        Rect panel{24, 20, 616, 460};
        p.fill(panel, kPanel);
        p.frame(panel, kBorder);
        p.text(44, 34, save_ ? "Spara spel" : "Ladda spel", kText, true);
        if (mode_ == List) {
            button(p, Rect{496, 32, 596, 64}, "Tillbaka", kCancel);
            if (pages() > 1) {
                button(p, Rect{344, 32, 376, 64}, "<", kPrev);
                std::string pg = std::to_string(page_ + 1) + "/" + std::to_string(pages());
                p.text(380 + (36 - p.width(pg)) / 2, 40, pg, kDim);
                button(p, Rect{420, 32, 452, 64}, ">", kNext);
            }
        }
        // cards: 3 x 2, the save screen's first card is "new save"
        int slot = 0;
        auto place = [&](int index) {
            int col = slot % 3, row = slot / 3;
            Rect r{44 + col * 184, 84 + row * 186, 44 + col * 184 + 168, 84 + row * 186 + 178};
            draw_card(p, r, index);
            ++slot;
        };
        if (save_) place(-1);
        int first = page_ * perPage();
        for (int i = first; i < int(saves_.size()) && i < first + perPage(); ++i) place(i);
        if (!save_ && saves_.empty()) p.text(44, 120, "Inga sparade spel \xE4nnu.", kDim);
        if (mode_ != List) {
            elements_.clear();   // only the dialog reacts now
            for (Color& c : p.s.px) c = (c >> 1) & 0x7F7F7F;
            const std::string nm = selected_ >= 0 ? saves_[size_t(selected_)].name : std::string();
            if (mode_ == Name)
                dialog(p, selected_ >= 0 ? "Skriv \xF6ver sparningen?" : "Ny sparning", "Namn:", "Spara", selected_ >= 0);
            else if (mode_ == ConfirmLoad)
                dialog(p, "Ladda sparningen?", nm, "Ladda", false, "Spel som inte sparats g\xE5r f\xF6rlorat.");
            else
                dialog(p, "Ta bort sparningen?", nm, "Ta bort", true, "Det g\xE5r inte att \xE5ngra.");
        }
    }
};

// ---- the hook -------------------------------------------------------------------

void show_modal_hook(Cpu& c) {
    // function TForm.ShowModal(Self): Self far pointer above the return address,
    // result in AX, callee pops 4 bytes
    uint16_t ds = Runtime::get().data_sel();
    uint32_t self = c.rd32(c.ss, uint16_t(c.sp() + 4));
    if (self == c.rd32(ds, 0x57F8)) {   // MenuForm: remember the game's picture
        g_gameFrame = screen();
        recomp::f_29_575A_orig(c);
        return;
    }
    bool save = self == c.rd32(ds, 0x5804), load = self == c.rd32(ds, 0x5800);
    static const bool classic = std::getenv("MYULK_CLASSIC_SAVE") != nullptr;
    if ((!save && !load) || classic) {
        recomp::f_29_575A_orig(c);
        return;
    }
    std::string file = SaveScreen(save).run();
    if (!file.empty()) {
        std::string path = kSaveDir + file;   // Pascal string[79] at Self+0x1A4
        uint16_t s = uint16_t(self >> 16), o = uint16_t(uint16_t(self) + 0x1A4);
        c.wr8(s, o, uint8_t(std::min<size_t>(path.size(), 79)));
        for (size_t i = 0; i < path.size() && i < 79; ++i) c.wr8(s, uint16_t(o + 1 + i), uint8_t(path[i]));
    }
    c.set_ax(file.empty() ? 2 : 1);   // mrCancel / mrOk
    c.set_dx(0);
    c.ret_far(4);
}

const bool g_registered = (recomp::set_hook(recomp::sel_of(29), 0x575A, &show_modal_hook), true);

}  // namespace
}  // namespace win16
