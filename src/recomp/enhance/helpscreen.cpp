// The handbook: the game's own help file (MYULK.HLP, "Myulk handbok") in a
// modern viewer instead of the Windows 3.1 help program. Opened by the game's
// "Handbok" button (WinHelp), F1, or the settings menu.
//
// Left: the topics (contents and glossary first) with a search field that
// matches titles and text. Right: the topic, laid out for the screen, with
// its pictures; links jump to other topics, definition links open a small
// card. Back, wheel/keys/drag scrolling, Esc closes.
#include <SDL3/SDL.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "recomp/enhance/helpfile.hpp"
#include "recomp/enhance/ui.hpp"
#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/vfs.hpp"

namespace win16 {
namespace {

using namespace ui;
using help::HelpFile;
using help::Item;
using help::Topic;

constexpr Color kLink = 0x7FC8FF, kPaper = 0x18202A, kSide = 0x1E2630, kSel = 0x34465A;
const Rect kPanelR{6, 6, 634, 474};
const Rect kSideR{12, 52, 196, 468};
const Rect kViewR{204, 52, 628, 468};   // the topic
constexpr int kPad = 14;

// ---- loading ------------------------------------------------------------------

HelpFile* help_file(const std::string& path) {
    static std::unique_ptr<HelpFile> cached;
    static bool tried = false;
    if (tried) return cached.get();
    tried = true;
    for (const std::string& p : {path, std::string("C:\\MYULK\\MYULK.HLP")}) {
        if (p.empty()) continue;
        int err = 0;
        int h = vfs::open(p, 0, &err);
        if (h < 0) continue;
        long n = vfs::size(h);
        std::vector<uint8_t> data(size_t(std::max(n, 0L)));
        long got = n > 0 ? vfs::read(h, data.data(), n) : 0;
        vfs::close(h);
        auto hf = std::make_unique<HelpFile>();
        if (got == n && hf->load(data)) {
            recomp::log("help: %s, %zu topics, %zu pictures", p.c_str(), hf->topics.size(), hf->pictures.size());
            cached = std::move(hf);
            break;
        }
    }
    return cached.get();
}

// ---- layout -------------------------------------------------------------------

struct Fonts {
    uint16_t body, bold, heading, title, small;
    static uint16_t make(const char* face, int height, int weight) {
        auto f = std::make_unique<GdiObject>();
        f->type = ObjType::Font;
        f->face = face;
        f->height = height;
        f->weight = weight;
        return gdi_add(std::move(f));
    }
    Fonts()
        : body(make("Arial", -14, 400)), bold(make("Arial", -14, 700)), heading(make("Arial", -19, 700)),
          title(make("Arial", -24, 700)), small(make("Arial", -13, 400)) {}
    uint16_t of(help::Style s) const {
        return s == help::kTitle ? title : s == help::kHeading ? heading : s == help::kBold ? bold : body;
    }
};

struct Op {
    enum Kind { Text, Picture } kind = Text;
    int x = 0, y = 0, w = 0, h = 0;   // relative to the layout origin
    std::string text;
    uint16_t font = 0;
    Color color = kText;
    int link = -1;
    bool popup = false;
    int picture = -1;
};

struct Layout {
    std::vector<Op> ops;
    int height = 0;
};

Layout layout(const HelpFile& hf, const Topic& t, int width, const Fonts& f) {
    Layout L;
    struct Float { int side, top, bottom, w; };
    std::vector<Float> floats;
    int y = 0;
    auto margins = [&](int at, int h, int& left, int& right) {
        left = right = 0;
        for (const Float& fl : floats)
            if (at < fl.bottom && at + h > fl.top) (fl.side == 1 ? left : right) = std::max(fl.side == 1 ? left : right, fl.w + 10);
    };
    bool lastEmpty = true;   // no blank space at the very top
    for (const help::Para& p : t.paras) {
        if (p.items.empty()) {
            if (!lastEmpty) y += 9;   // an empty paragraph: a small gap, never several
            lastEmpty = true;
            continue;
        }
        lastEmpty = false;
        std::vector<Op> line;
        int lineW = 0, lineH = 0;
        auto flush = [&](bool force) {
            if (line.empty() && !force) return;
            if (line.empty()) lineH = text_height(f.body);
            int left, right;
            margins(y, lineH, left, right);
            int avail = width - left - right - p.indent;
            int dx = left + p.indent + (p.align == 1 ? (avail - lineW) / 2 : p.align == 2 ? avail - lineW : 0);
            for (Op& op : line) {
                op.x += dx;
                op.y = y + lineH - op.h;   // bottom-aligned
                L.ops.push_back(op);
            }
            y += lineH + 2;
            line.clear();
            lineW = lineH = 0;
        };
        auto place = [&](Op op, bool breakable) {
            int left, right;
            margins(y, std::max(lineH, op.h), left, right);
            int avail = width - left - right - p.indent;
            if (breakable && !line.empty() && lineW + op.w > avail) {
                flush(false);
                if (op.text == " ") return;   // no space at the start of a line
            }
            op.x = lineW;
            lineW += op.w;
            lineH = std::max(lineH, op.h);
            line.push_back(op);
        };
        for (const Item& it : p.items) {
            if (it.kind == Item::LineBreak) { flush(true); continue; }
            if (it.kind == Item::Tab) { lineW += 28; continue; }
            if (it.kind == Item::Picture) {
                const Surface& pic = hf.pictures[size_t(it.picture)];
                if (it.place == 0) {
                    Op op;
                    op.kind = Op::Picture;
                    op.picture = it.picture;
                    op.w = std::min(pic.w, width);
                    op.h = pic.h;
                    place(op, true);
                } else {
                    // floating picture at the side; the text flows next to it,
                    // pictures on the same side stack
                    int top = y;
                    for (const Float& fl : floats)
                        if (fl.side == it.place && fl.bottom > top) top = std::max(top, fl.bottom + 8);
                    Op op;
                    op.kind = Op::Picture;
                    op.picture = it.picture;
                    op.w = std::min(pic.w, width / 2);
                    op.h = pic.h;
                    op.x = it.place == 1 ? 0 : width - op.w;
                    op.y = top;
                    L.ops.push_back(op);
                    floats.push_back({it.place, top, top + pic.h, op.w});
                }
                continue;
            }
            uint16_t font = f.of(it.style);
            Color color = it.link >= 0 ? kLink : it.accent ? kAccent : kText;
            // words with their spaces, so lines break between words
            const std::string& s = it.text;
            for (size_t a = 0; a < s.size();) {
                size_t b = a;
                while (b < s.size() && s[b] != ' ') ++b;
                while (b < s.size() && s[b] == ' ') ++b;
                Op op;
                op.text = s.substr(a, b - a);
                op.font = font;
                op.color = color;
                op.link = it.link;
                op.popup = it.popup;
                op.w = text_width(font, op.text);
                op.h = text_height(font);
                place(op, true);
                a = b;
            }
        }
        flush(false);
        y += 7;   // between paragraphs
    }
    for (const Float& fl : floats) y = std::max(y, fl.bottom + 8);
    L.height = y;
    return L;
}

// ---- the screen -------------------------------------------------------------------

class HelpScreen : public InputOverlay {
public:
    HelpScreen(HelpFile& hf, int topic) : hf_(hf) {
        go(topic, false);
        filter();
    }

    void run() {
        Surface& scr = screen();
        Surface under = scr;
        painter_ = std::make_unique<Painter>();
        set_input_overlay(this);
        while (!done_) {
            draw();
            host_pump(16);
        }
        set_input_overlay(nullptr);
        host_text_input(false);
        painter_.reset();
        scr = under;
        screen_invalidate();
    }

    void mouse(int x, int y, int button, bool down) override {
        if (button == -1) {   // moved: drag to scroll (touch)
            if (pressed_ && (dragging_ || std::abs(y - pressY_) > 6)) {
                dragging_ = true;
                if (pressIn_ == 1) side_ = std::clamp(pressScroll_ - (y - pressY_), 0, sideMax_);
                else if (pressIn_ == 2) popupScroll_ = std::clamp(pressScroll_ - (y - pressY_), 0, popupMax_);
                else scroll_ = std::clamp(pressScroll_ - (y - pressY_), 0, scrollMax_);
            }
            mx_ = x; my_ = y;
            return;
        }
        if (button != 0) return;
        mx_ = x; my_ = y;
        if (down) {
            pressed_ = true;
            dragging_ = false;
            pressY_ = y;
            pressIn_ = popup_ >= 0 && popupR_.contains(x, y) ? 2 : kSideR.contains(x, y) ? 1 : 0;
            pressScroll_ = pressIn_ == 1 ? side_ : pressIn_ == 2 ? popupScroll_ : scroll_;
            return;
        }
        bool wasDrag = dragging_;
        pressed_ = dragging_ = false;
        if (!wasDrag) click(x, y);
    }
    void key(int vk, bool down) override {
        if (!down) return;
        int* s = popup_ >= 0 ? &popupScroll_ : &scroll_;
        int mx = popup_ >= 0 ? popupMax_ : scrollMax_;
        switch (vk) {
            case 0x1B:   // Esc: the card, then the search, then the screen
                if (popup_ >= 0) popup_ = -1;
                else if (!search_.empty()) { search_.clear(); filter(); }
                else done_ = true;
                break;
            case 0x26: *s = std::max(0, *s - 40); break;
            case 0x28: *s = std::min(mx, *s + 40); break;
            case 0x21: *s = std::max(0, *s - (kViewR.h() - 60)); break;
            case 0x22: *s = std::min(mx, *s + (kViewR.h() - 60)); break;
            case 0x24: *s = 0; break;
            case 0x23: *s = mx; break;
            case 0x70: done_ = true; break;   // F1 again
            default: break;
        }
    }
    void character(int ch) override {
        if (ch == 8) {
            if (!search_.empty()) { search_.pop_back(); filter(); }
            else back();
            return;
        }
        if (ch == 13 && !list_.empty() && !search_.empty()) { go(list_.front(), true); return; }
        if (ch >= 0x20 && ch != 0x7F && search_.size() < 24) {
            search_ += char(ch);
            filter();
        }
    }
    void wheel(float dy) override {
        int step = int(-dy * 48);
        if (popup_ >= 0) popupScroll_ = std::clamp(popupScroll_ + step, 0, popupMax_);
        else if (kSideR.contains(mx_, my_)) side_ = std::clamp(side_ + step, 0, sideMax_);
        else scroll_ = std::clamp(scroll_ + step, 0, scrollMax_);
    }

private:
    HelpFile& hf_;
    Fonts fonts_;
    std::unique_ptr<Painter> painter_;
    bool done_ = false;
    int topic_ = 0, scroll_ = 0, scrollMax_ = 0;
    Layout layout_;
    std::vector<std::pair<int, int>> history_;   // topic, scroll
    int popup_ = -1, popupScroll_ = 0, popupMax_ = 0;
    Layout popupLayout_;
    Rect popupR_{};
    std::string search_;
    std::vector<int> list_;
    int side_ = 0, sideMax_ = 0;
    int mx_ = -1, my_ = -1;
    bool pressed_ = false, dragging_ = false;
    int pressY_ = 0, pressIn_ = 0, pressScroll_ = 0;
    struct Hit { Rect r; int id; };   // id: topic for links (+popup flag), or a button
    std::vector<Hit> hits_;
    enum { kBack = -10, kContents = -11, kClose = -12, kSearch = -13, kPopupClose = -14 };
    static constexpr int kPopupBit = 1 << 20, kListBit = 1 << 21;

    int width() const { return kViewR.w() - 2 * kPad - 10; }

    void go(int topic, bool remember) {
        if (topic < 0 || topic >= int(hf_.topics.size())) return;
        if (remember) history_.push_back({topic_, scroll_});
        topic_ = topic;
        scroll_ = 0;
        popup_ = -1;
        layout_ = layout(hf_, hf_.topics[size_t(topic)], width(), fonts_);
        scrollMax_ = std::max(0, layout_.height + 2 * kPad - kViewR.h());
    }
    void back() {
        if (history_.empty()) return;
        auto [t, s] = history_.back();
        history_.pop_back();
        go(t, false);
        scroll_ = std::min(s, scrollMax_);
    }
    void open_popup(int topic) {
        popup_ = topic;
        popupScroll_ = 0;
        popupLayout_ = layout(hf_, hf_.topics[size_t(topic)], 340, fonts_);
        int h = std::min(popupLayout_.height + 2 * kPad + 22, 340);
        int cx = (kViewR.left + kViewR.right) / 2;
        popupR_ = Rect{cx - 185, 240 - h / 2, cx + 185, 240 - h / 2 + h};
        popupMax_ = std::max(0, popupLayout_.height + 2 * kPad + 22 - h);
    }

    // the topic list: contents and glossary first, then the others; the search
    // keeps topics whose title or text contains it
    void filter() {
        list_.clear();
        std::string q;
        for (char ch : search_) {
            unsigned char c = static_cast<unsigned char>(ch);
            q += (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? char(c - 0x20) : ch;
        }
        auto matches = [&](int i) {
            const Topic& t = hf_.topics[size_t(i)];
            if (t.title.empty()) return false;
            if (q.empty()) return true;
            std::string title;
            for (char ch : t.title) {
                unsigned char c = static_cast<unsigned char>(ch);
                title += (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? char(c - 0x20) : ch;
            }
            return title.find(q) != std::string::npos || t.text.find(q) != std::string::npos;
        };
        int glossary = -1;
        for (int i = 0; i < int(hf_.topics.size()); ++i)
            if (hf_.topics[size_t(i)].title == "Ordlista") glossary = i;
        for (int first : {hf_.contents, glossary})
            if (first >= 0 && matches(first)) list_.push_back(first);
        for (int i = 0; i < int(hf_.topics.size()); ++i)
            if (i != hf_.contents && i != glossary && matches(i)) list_.push_back(i);
        side_ = 0;
    }

    void click(int x, int y) {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (!it->r.contains(x, y)) continue;
            int id = it->id;
            if (id == kBack) back();
            else if (id == kContents) go(hf_.contents, true);
            else if (id == kClose) done_ = true;
            else if (id == kPopupClose) popup_ = -1;
            else if (id == kSearch) host_text_input(true);   // phones: open the keyboard
            else if (id & kListBit) go(id & ~kListBit, true);
            else if (id & kPopupBit) open_popup(id & ~kPopupBit);
            else go(id, true);
            return;
        }
        if (popup_ >= 0 && !popupR_.contains(x, y)) popup_ = -1;   // outside the card
    }

    // ---- drawing ----
    void text(int x, int y, const std::string& t, uint16_t font, Color c) {
        DC* dc = dc_get(painter_->hdc);
        if (!dc) return;
        dc->font = font;
        dc->textColor = c;
        dc->bkMode = 1;
        draw_text(*dc, x, y, t, nullptr, 0);
    }
    void blit(int x0, int y0, const Surface& img, Rect clip) {
        Surface& s = painter_->s;
        for (int y = std::max(0, clip.top - y0); y < img.h && y0 + y < clip.bottom; ++y)
            for (int x = std::max(0, clip.left - x0); x < img.w && x0 + x < clip.right; ++x) s.set(x0 + x, y0 + y, img.get(x, y));
    }
    void button(Rect r, const std::string& label, int id, bool enabled = true) {
        Painter& p = *painter_;
        p.fill(r, enabled && r.contains(mx_, my_) ? kCardHover : kCard);
        p.frame(r, kBorder);
        text(r.left + (r.w() - text_width(fonts_.bold, label)) / 2, r.top + (r.h() - text_height(fonts_.bold)) / 2, label,
             fonts_.bold, enabled ? kText : kDim);
        if (enabled) hits_.push_back({r, id});
    }
    void draw_layout(const Layout& L, int ox, int oy, Rect clip) {
        Painter& p = *painter_;
        for (const Op& op : L.ops) {
            int x = ox + op.x, y = oy + op.y;
            if (y + op.h < clip.top || y > clip.bottom) continue;
            if (op.kind == Op::Picture) {
                blit(x, y, hf_.pictures[size_t(op.picture)], clip);
                continue;
            }
            bool hover = op.link >= 0 && Rect{x, y, x + op.w, y + op.h}.contains(mx_, my_);
            text(x, y, op.text, op.font, hover ? kText : op.color);
            if (op.link >= 0) {
                int tw = text_width(op.font, op.text.substr(0, op.text.find_last_not_of(' ') + 1));
                int uy = y + op.h - 2;
                if (uy >= clip.top && uy < clip.bottom) {
                    if (op.popup) {   // definition: dotted underline
                        for (int k = 0; k < tw; k += 3) p.fill(Rect{x + k, uy, x + k + 1, uy + 1}, op.color);
                    } else {
                        p.fill(Rect{x, uy, x + tw, uy + 1}, op.color);
                    }
                }
                Rect hr{x, std::max(y, clip.top), x + op.w, std::min(y + op.h, clip.bottom)};
                if (!hr.empty()) hits_.push_back({hr, op.popup ? (op.link | kPopupBit) : op.link});
            }
        }
    }
    void scrollbar(Rect track, int scroll, int max, int viewH) {
        if (max <= 0) return;
        Painter& p = *painter_;
        p.fill(track, 0x2A3440);
        int total = viewH + max;
        int h = std::max(24, track.h() * viewH / total);
        int y = track.top + (track.h() - h) * scroll / max;
        p.fill(Rect{track.left, y, track.right, y + h}, kBorder);
    }

    void draw() {
        Painter& p = *painter_;
        hits_.clear();
        p.fill(Rect{0, 0, 640, 480}, 0x0C1016);
        p.fill(kPanelR, kPanel);
        p.frame(kPanelR, kBorder);

        // the topic (drawn first; the bars are drawn over its overflow)
        p.fill(kViewR, kPaper);
        draw_layout(layout_, kViewR.left + kPad, kViewR.top + kPad - scroll_, kViewR);
        scrollbar(Rect{kViewR.right - 6, kViewR.top + 2, kViewR.right - 2, kViewR.bottom - 2}, scroll_, scrollMax_, kViewR.h());

        // the topic list
        p.fill(kSideR, kSide);
        Rect field{kSideR.left, kSideR.top, kSideR.right, kSideR.top + 26};
        p.fill(field, 0x101820);
        p.frame(field, search_.empty() ? kBorder : kAccent);
        if (search_.empty()) text(field.left + 8, field.top + 5, "S\xF6k...", fonts_.small, kDim);
        else {
            text(field.left + 8, field.top + 5, search_, fonts_.small, kText);
            int cx = field.left + 8 + text_width(fonts_.small, search_);
            if ((SDL_GetTicks() / 500) % 2) p.fill(Rect{cx + 1, field.top + 5, cx + 2, field.bottom - 5}, kText);
        }
        hits_.push_back({field, kSearch});
        Rect listR{kSideR.left, field.bottom + 4, kSideR.right, kSideR.bottom};
        const int rowH = 19;
        sideMax_ = std::max(0, int(list_.size()) * rowH - listR.h());
        side_ = std::min(side_, sideMax_);
        if (list_.empty()) text(listR.left + 8, listR.top + 6, "Inget hittades", fonts_.small, kDim);
        for (size_t i = 0; i < list_.size(); ++i) {
            int y = listR.top + int(i) * rowH - side_;
            if (y + rowH <= listR.top || y >= listR.bottom) continue;
            Rect row{listR.left, std::max(y, listR.top), listR.right - 6, std::min(y + rowH, listR.bottom)};
            int t = list_[i];
            bool sel = t == topic_;
            if (sel) p.fill(row, kSel);
            else if (row.contains(mx_, my_)) p.fill(row, 0x283442);
            if (y >= listR.top && y + rowH <= listR.bottom) {
                std::string name = hf_.topics[size_t(t)].title;
                while (!name.empty() && text_width(fonts_.small, name) > row.w() - 14) name.pop_back();
                bool pinned = search_.empty() && i < 2;   // contents and glossary
                text(row.left + 8, y + 3, name, pinned ? fonts_.bold : fonts_.small, sel ? kText : kDim);
            }
            hits_.push_back({row, t | kListBit});
        }
        scrollbar(Rect{listR.right - 4, listR.top, listR.right, listR.bottom}, side_, sideMax_, listR.h());

        // the top bar (over any overflow)
        p.fill(Rect{kPanelR.left + 1, kPanelR.top + 1, kPanelR.right - 1, kViewR.top}, kPanel);
        p.fill(Rect{kPanelR.left + 1, kViewR.bottom, kPanelR.right - 1, kPanelR.bottom - 1}, kPanel);
        button(Rect{12, 12, 48, 44}, "<", kBack, !history_.empty());
        std::string head = hf_.title.empty() ? "Handbok" : hf_.title;
        text(58, 16, head, fonts_.title, kText);
        button(Rect{kPanelR.right - 194, 12, kPanelR.right - 92, 44}, "Inneh\xE5ll", kContents, topic_ != hf_.contents);
        button(Rect{kPanelR.right - 86, 12, kPanelR.right - 6, 44}, "St\xE4ng", kClose);

        // a definition card over everything
        if (popup_ >= 0) {
            hits_.erase(std::remove_if(hits_.begin(), hits_.end(), [](const Hit& h) { return h.id != kClose; }), hits_.end());
            for (int y = 0; y < 480; ++y)
                for (int x = 0; x < 640; ++x) {
                    Color c = p.s.get(x, y);
                    p.s.set(x, y, (c >> 1) & 0x7F7F7F);
                }
            p.fill(popupR_, kPanel);
            p.frame(popupR_, kAccent);
            Rect inner{popupR_.left + 1, popupR_.top + 1, popupR_.right - 1, popupR_.bottom - 1};
            draw_layout(popupLayout_, popupR_.left + kPad, popupR_.top + kPad - popupScroll_, inner);
            // close mark over the content
            Rect x{popupR_.right - 26, popupR_.top + 4, popupR_.right - 4, popupR_.top + 24};
            p.fill(x, x.contains(mx_, my_) ? kDanger : kCard);
            text(x.left + 7, x.top + 2, "x", fonts_.bold, kText);
            hits_.push_back({x, kPopupClose});
            scrollbar(Rect{popupR_.right - 6, popupR_.top + 28, popupR_.right - 2, popupR_.bottom - 4}, popupScroll_, popupMax_,
                      popupR_.h());
        }
    }
};

}  // namespace

void run_help_screen(const std::string& file, unsigned command, uint32_t data) {
    HelpFile* hf = help_file(file);
    if (!hf) {
        show_note("Handboken (MYULK.HLP) hittades inte");
        return;
    }
    int topic = hf->contents;
    if (command == 1) {   // HELP_CONTEXT
        auto it = hf->contextIds.find(int32_t(data));
        if (it != hf->contextIds.end()) topic = it->second;
    }
    HelpScreen(*hf, topic).run();
}

}  // namespace win16
