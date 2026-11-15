// The settings menu (F2, or the gear button on the web page): display, sound,
// convenience options and quick-save, usable with mouse, touch or keyboard.
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

struct Option {
    const char* label;
    const char* key;
    int def;
    std::vector<int> values;
    std::vector<const char*> names;
};

const std::vector<Option>& options() {
    static const std::vector<Option> o = {
        {"Bildskalning", "scaling", settings::kSharpSmooth, {0, 1, 2, 3},
         {"skarpa pixlar", "mjuk skalning", "utj\xE4mnad", "heltalsskalning"}},
        {"Avdithring (mjukare f\xE4rgytor)", "dedither", 0, {0, 1}, {"av", "p\xE5"}},
        {"Text", "crisp_text", 0, {0, 1}, {"mjuk", "skarp (som originalet)"}},
        {"Volym", "volume", 100, {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}, {}},
        {"Helsk\xE4rm", "", 0, {0, 1}, {"av", "p\xE5"}},
        {"Klick hoppar \xF6ver filmer och tal", "click_skips", 1, {0, 1}, {"av", "p\xE5"}},
        {"G\xE5rdens hastighet", "farm_speed", 1, {1, 2, 4, 8}, {"1\xD7", "2\xD7", "4\xD7", "8\xD7"}},
        {"Autospara", "autosave", 1, {0, 1}, {"av", "p\xE5"}},
    };
    return o;
}

enum { kDone = 1, kQuickSave, kQuickLoad, kLess = 100, kMore = 200 };

class SettingsScreen : public OverlayScreen {
    int row_ = 0;   // keyboard focus

    int value(const Option& o) const {
        if (!*o.key) return host_fullscreen() ? 1 : 0;
        return settings::get_int(o.key, o.def);
    }
    void set(const Option& o, int v) {
        if (!*o.key) { host_set_fullscreen(v != 0); return; }
        settings::set_int(o.key, v);
        if (std::string(o.key) == "crisp_text") screen_invalidate();
        if (std::string(o.key) == "volume") mm_set_volume(float(v) / 100.0f);
    }
    void step(int i, int dir) {
        const Option& o = options()[size_t(i)];
        auto it = std::find(o.values.begin(), o.values.end(), value(o));
        int idx = it == o.values.end() ? 0 : int(it - o.values.begin());
        idx = std::clamp(idx + dir, 0, int(o.values.size()) - 1);
        set(o, o.values[size_t(idx)]);
    }
    std::string shown(const Option& o) const {
        int v = value(o);
        if (o.names.empty()) return std::to_string(v) + " %";
        auto it = std::find(o.values.begin(), o.values.end(), v);
        return it == o.values.end() ? "?" : o.names[size_t(it - o.values.begin())];
    }

protected:
    void draw(Painter& p) override {
        Rect pr{60, 30, 580, 450};
        panel(p, pr, "Inst\xE4llningar");
        const auto& opts = options();
        for (size_t i = 0; i < opts.size(); ++i) {
            int y = 84 + int(i) * 34;
            Rect rowR{pr.left + 12, y - 4, pr.right - 12, y + 30};
            if (int(i) == row_) p.fill(rowR, 0x26303C);
            p.text(pr.left + 24, y + 6, opts[i].label, int(i) == row_ ? kText : kDim);
            Rect less{336, y, 368, y + 26}, more{528, y, 560, y + 26};
            button(p, less, "<", kLess + int(i));
            button(p, more, ">", kMore + int(i));
            std::string v = p.fit(shown(opts[i]), 150);
            p.text(372 + (152 - p.width(v)) / 2, y + 5, v, kAccent);
        }
        int by = 84 + int(opts.size()) * 34 + 10;
        button(p, Rect{pr.left + 24, by, pr.left + 184, by + 32}, "Snabbspara (F5)", kQuickSave);
        button(p, Rect{pr.left + 196, by, pr.left + 356, by + 32}, "Snabbladda (F9)", kQuickLoad);
        button(p, Rect{pr.right - 124, by, pr.right - 24, by + 32}, "Klar", kDone, kAccent);
        p.text(pr.left + 24, pr.bottom - 28,
               p.fit("F2 inst\xE4llningar \xB7 F5/F9 snabbspara/ladda \xB7 P paus \xB7 F7/F8 bild/text \xB7 F11 helsk\xE4rm",
                     pr.w() - 48),
               kDim);
    }
    void activate(int id) override {
        if (id == kDone) { close(); return; }
        if (id == kQuickSave) { quick::request_save(); close(); return; }
        if (id == kQuickLoad) { quick::request_load(); close(); return; }
        if (id >= kMore) { row_ = id - kMore; step(row_, +1); return; }
        if (id >= kLess) { row_ = id - kLess; step(row_, -1); return; }
    }
    void on_key(int vk) override {
        int n = int(options().size());
        if (vk == 0x26) row_ = (row_ + n - 1) % n;          // up
        else if (vk == 0x28) row_ = (row_ + 1) % n;         // down
        else if (vk == 0x25) step(row_, -1);                 // left
        else if (vk == 0x27) step(row_, +1);                 // right
        else if (vk == 0x0D || vk == 0x71) close();          // Enter, F2
    }
};

}  // namespace

void run_settings_screen() { SettingsScreen().run(); }

}  // namespace win16
