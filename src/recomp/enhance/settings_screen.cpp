// The settings menu (F2, or the gear button on the web page), usable with
// mouse, touch or keyboard. Two pages: "Bild och ljud" (picture and sound) and
// "Spelhjalp" (gameplay helpers, each on or off, with the play style on top:
// Original = the 1996 game, Modern = every helper). Also the first-start
// question that sets the play style.
#include <algorithm>
#include <string>
#include <vector>

#include "recomp/enhance/quick.hpp"
#include "recomp/enhance/settings.hpp"
#include "recomp/enhance/ui.hpp"
#include "recomp/win16/user.hpp"

namespace win16 {
namespace {

using namespace ui;

constexpr const char* kStyleKey = "*style";   // the derived play-style row
constexpr const char* kFullscreenKey = "*fullscreen";

struct Option {
    const char* label;
    const char* key;
    int def;
    std::vector<int> values;
    std::vector<const char*> names;   // empty: "N %"
    const char* desc;
};

const std::vector<int> kPercent = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
const std::vector<int> kOnOff = {0, 1};
const std::vector<const char*> kOnOffNames = {"av", "p\xE5"};

const std::vector<Option>& picture_page() {
    static const std::vector<Option> o = {
        {"Bildskalning", "scaling", settings::kSharpSmooth, {0, 1, 2, 3},
         {"skarpa pixlar", "mjuk skalning", "utj\xE4mnad", "heltalsskalning"},
         "Hur bilden f\xF6rstoras till f\xF6nstret (F7)."},
        {"Avdithring", "dedither", 0, kOnOff, kOnOffNames,
         "Sl\xE4tar ut prickarna i himmel och ytor, kanter f\xF6rblir skarpa."},
        {"Text", "crisp_text", 0, kOnOff, {"mjuk", "skarp (som originalet)"}, "Utj\xE4mnad eller skarp text (F8)."},
        {"Volym (allt)", "volume", 100, kPercent, {}, "All ljudvolym."},
        {"Musik", "music_volume", 100, kPercent, {}, "Musikslingorna."},
        {"Tal och ljud", "speech_volume", 100, kPercent, {}, "Tal, ljudeffekter och filmernas ljud."},
        {"Helsk\xE4rm", kFullscreenKey, 0, kOnOff, kOnOffNames, "Helsk\xE4rm (F11)."},
    };
    return o;
}

const std::vector<Option>& helper_page() {
    static const std::vector<Option> o = {
        {"Spelstil", kStyleKey, 1, {0, 1}, {"Original", "Modern", "Anpassad"},
         "Original: spelet som 1996. Modern: all hj\xE4lp p\xE5. \xC4ndra raderna f\xF6r att v\xE4lja sj\xE4lv."},
        {"Klick hoppar \xF6ver filmer och tal", "click_skips", 1, kOnOff, kOnOffNames,
         "Ett klick hoppar \xF6ver filmen eller repliken som spelas."},
        {"Autospara", "autosave", 1, kOnOff, kOnOffNames, "Sparar sj\xE4lv var tionde minut och vid nytt kapitel."},
        {"Snabbspara (F5/F9)", "quick_save", 1, kOnOff, kOnOffNames, "F5 sparar, F9 laddar, utan fr\xE5gor."},
        {"Ny spara/ladda-sk\xE4rm", "save_screen", 1, kOnOff, kOnOffNames,
         "Kort med bild och datum i st\xE4llet f\xF6r Windows-dialogerna."},
        {"Forts\xE4tt-kort vid start", "start_card", 1, kOnOff, kOnOffNames,
         "Erbjuder senaste sparningen n\xE4r spelet startar."},
        {"G\xE5 med tangenter och mus", "nav_keys", 1, kOnOff, kOnOffNames,
         "Piltangenter/WASD, Backsteg, h\xF6gerklick och mushjul f\xF6r att g\xE5."},
        {"Visa klickbara st\xE4llen (H)", "hints", 1, kOnOff, kOnOffNames,
         "H\xE5ll H f\xF6r att se allt som g\xE5r att klicka p\xE5."},
        {"G\xE5rdshj\xE4lp", "farm_helper", 1, kOnOff, kOnOffNames,
         "P pausar, klick p\xE5 dygnsklockan (eller N) ger n\xE4sta dag, notiser om pengar."},
        {"G\xE5rdens hastighet", "farm_speed", 1, {1, 2, 4, 8}, {"1\xD7", "2\xD7", "4\xD7", "8\xD7"},
         "Hur fort dagarna g\xE5r p\xE5 g\xE5rden (1\xD7 som originalet)."},
    };
    return o;
}

enum { kDone = 1, kQuickSave, kQuickLoad, kPage0, kPage1, kHelp, kLess = 100, kMore = 200, kRow = 300 };

class SettingsScreen : public OverlayScreen {
    int page_ = 0;
    int row_ = 0;   // keyboard focus

    const std::vector<Option>& opts() const { return page_ == 0 ? picture_page() : helper_page(); }

    int value(const Option& o) const {
        std::string k = o.key;
        if (k == kFullscreenKey) return host_fullscreen() ? 1 : 0;
        if (k == kStyleKey) return settings::style();
        return settings::get_int(o.key, o.def);
    }
    void set(const Option& o, int v) {
        std::string k = o.key;
        if (k == kFullscreenKey) { host_set_fullscreen(v != 0); return; }
        if (k == kStyleKey) {
            settings::set_style(v);
            if (v == settings::kOriginal) set_farm_paused(false);
            return;
        }
        settings::set_int(o.key, v);
        if (k == "crisp_text") screen_invalidate();
        if (k == "volume") mm_set_volume(float(v) / 100.0f);
        if (k == "music_volume" || k == "speech_volume") mm_update_gains();
        if (k == "farm_helper" && !v) set_farm_paused(false);
    }
    void step(int i, int dir) {
        const Option& o = opts()[size_t(i)];
        int cur = value(o);
        if (std::string(o.key) == kStyleKey) {
            // Original <-> Modern; from "Anpassad", "<" gives Original and ">" Modern
            if (cur == settings::kCustom) set(o, dir < 0 ? settings::kOriginal : settings::kModern);
            else set(o, dir < 0 ? settings::kOriginal : settings::kModern);
            return;
        }
        auto it = std::find(o.values.begin(), o.values.end(), cur);
        int idx = it == o.values.end() ? 0 : int(it - o.values.begin());
        idx = std::clamp(idx + dir, 0, int(o.values.size()) - 1);
        set(o, o.values[size_t(idx)]);
    }
    std::string shown(const Option& o) const {
        int v = value(o);
        if (o.names.empty()) return std::to_string(v) + " %";
        if (std::string(o.key) == kStyleKey) return o.names[size_t(std::clamp(v, 0, 2))];
        auto it = std::find(o.values.begin(), o.values.end(), v);
        return it == o.values.end() ? "?" : o.names[size_t(it - o.values.begin())];
    }

protected:
    void draw(Painter& p) override {
        Rect pr{60, 16, 580, 464};
        panel(p, pr, "Inst\xE4llningar");
        button(p, Rect{pr.right - 268, pr.top + 14, pr.right - 144, pr.top + 42}, "Bild och ljud", kPage0,
               page_ == 0 ? kAccent : kBorder);
        button(p, Rect{pr.right - 136, pr.top + 14, pr.right - 16, pr.top + 42}, "Spelhj\xE4lp", kPage1,
               page_ == 1 ? kAccent : kBorder);
        const auto& o = opts();
        const char* desc = nullptr;
        for (size_t i = 0; i < o.size(); ++i) {
            int y = 70 + int(i) * 31;
            Rect rowR{pr.left + 12, y - 2, pr.right - 12, y + 28};
            bool focus = int(i) == row_;
            if (hover(rowR)) desc = o[i].desc;
            if (focus) p.fill(rowR, 0x26303C);
            bool style = std::string(o[i].key) == kStyleKey;
            p.text(pr.left + 24, y + 6, p.fit(o[i].label, 250), focus || style ? kText : kDim);
            Rect less{336, y, 368, y + 26}, more{528, y, 560, y + 26};
            button(p, less, "<", kLess + int(i));
            button(p, more, ">", kMore + int(i));
            std::string v = p.fit(shown(o[i]), 150);
            p.text(372 + (152 - p.width(v)) / 2, y + 5, v, kAccent);
            add(Rect{rowR.left, rowR.top, 330, rowR.bottom}, kRow + int(i));
            if (style) p.fill(Rect{pr.left + 24, y + 29, pr.right - 24, y + 30}, kBorder);   // rule under it
        }
        if (!desc && row_ < int(o.size())) desc = o[size_t(row_)].desc;
        int by = 70 + 10 * 31 + 8;
        if (settings::helper("quick_save")) {
            button(p, Rect{pr.left + 24, by, pr.left + 156, by + 32}, "Snabbspara (F5)", kQuickSave);
            button(p, Rect{pr.left + 164, by, pr.left + 296, by + 32}, "Snabbladda (F9)", kQuickLoad);
        }
        button(p, Rect{pr.left + 304, by, pr.left + 416, by + 32}, "Handbok (F1)", kHelp);
        button(p, Rect{pr.right - 96, by, pr.right - 24, by + 32}, "Klar", kDone, kAccent);
        p.text(pr.left + 24, pr.bottom - 26, p.fit(desc ? desc : "", pr.w() - 48), kDim);
    }
    void activate(int id) override {
        if (id == kDone) { close(); return; }
        if (id == kQuickSave) { quick::request_save(); close(); return; }
        if (id == kQuickLoad) { quick::request_load(); close(); return; }
        if (id == kHelp) { request_help(); close(); return; }
        if (id == kPage0 || id == kPage1) { page_ = id - kPage0; row_ = 0; return; }
        if (id >= kRow) { row_ = id - kRow; step(row_, +1); return; }   // a click on the label also steps
        if (id >= kMore) { row_ = id - kMore; step(row_, +1); return; }
        if (id >= kLess) { row_ = id - kLess; step(row_, -1); return; }
    }
    void on_key(int vk) override {
        int n = int(opts().size());
        if (vk == 0x26) row_ = (row_ + n - 1) % n;          // up
        else if (vk == 0x28) row_ = (row_ + 1) % n;         // down
        else if (vk == 0x25) step(row_, -1);                 // left
        else if (vk == 0x27) step(row_, +1);                 // right
        else if (vk == 0x09) { page_ ^= 1; row_ = 0; }       // Tab: other page
        else if (vk == 0x0D || vk == 0x71) close();          // Enter, F2
    }
};

// First start: how to play. Two big choices; the settings can change it later.
class StyleCard : public OverlayScreen {
protected:
    enum { kModernId = 1, kOriginalId };
    void draw(Painter& p) override {
        Rect d{70, 70, 570, 410};
        p.fill(d, kPanel);
        p.frame(d, kBorder);
        p.text(d.left + 24, d.top + 18, "Hur vill du spela Myulk?", kText, true);
        choice(p, Rect{d.left + 24, d.top + 70, d.right - 24, d.top + 160}, kModernId, "Med f\xF6rb\xE4ttringar",
               "Hoppa \xF6ver filmer, snabbspara, autospara, g\xE5 med tangenter,",
               "visa klickbara st\xE4llen, g\xE5rdshj\xE4lp och mer.");
        choice(p, Rect{d.left + 24, d.top + 172, d.right - 24, d.top + 262}, kOriginalId, "Som originalet (1996)",
               "Spelet som det var, utan till\xE4gg.",
               "Bara bilden och texten \xE4r anpassade till dagens sk\xE4rmar.");
        p.text(d.left + 24, d.bottom - 50, "Du kan \xE4ndra detta n\xE4r som helst under Inst\xE4llningar (F2),", kDim);
        p.text(d.left + 24, d.bottom - 30, "en sak i taget eller allt p\xE5 en g\xE5ng.", kDim);
    }
    void activate(int id) override {
        settings::set_style(id == kModernId ? settings::kModern : settings::kOriginal);
        close();
    }
    void on_key(int vk) override {
        if (vk == 0x0D) activate(kModernId);
    }

private:
    void choice(Painter& p, Rect r, int id, const char* title, const char* line1, const char* line2) {
        p.fill(r, hover(r) ? kCardHover : kCard);
        p.frame(r, id == kModernId ? kAccent : kBorder);
        p.text(r.left + 16, r.top + 12, title, kText, true);
        p.text(r.left + 16, r.top + 48, p.fit(line1, r.w() - 32), kDim);
        p.text(r.left + 16, r.top + 66, p.fit(line2, r.w() - 32), kDim);
        add(r, id);
    }
};

}  // namespace

void run_settings_screen() { SettingsScreen().run(); }

namespace ui {
void run_style_card() {
    StyleCard().run();
    if (!settings::style_chosen()) settings::set_style(settings::kModern);   // Esc: keep everything on
}
}  // namespace ui

}  // namespace win16
