#include "core/form.hpp"

#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace myulk {
namespace {

using nlohmann::json;

// Windows 3.1 default system colours, indexed by COLOR_* constant.
constexpr Rgb kSystemColors[] = {
    0xC0C0C0,  // 0  COLOR_SCROLLBAR
    0x008080,  // 1  COLOR_BACKGROUND
    0x000080,  // 2  COLOR_ACTIVECAPTION
    0xFFFFFF,  // 3  COLOR_INACTIVECAPTION
    0xC0C0C0,  // 4  COLOR_MENU
    0xFFFFFF,  // 5  COLOR_WINDOW
    0x000000,  // 6  COLOR_WINDOWFRAME
    0x000000,  // 7  COLOR_MENUTEXT
    0x000000,  // 8  COLOR_WINDOWTEXT
    0xFFFFFF,  // 9  COLOR_CAPTIONTEXT
    0xC0C0C0,  // 10 COLOR_ACTIVEBORDER
    0xC0C0C0,  // 11 COLOR_INACTIVEBORDER
    0xC0C0C0,  // 12 COLOR_APPWORKSPACE
    0x000080,  // 13 COLOR_HIGHLIGHT
    0xFFFFFF,  // 14 COLOR_HIGHLIGHTTEXT
    0xC0C0C0,  // 15 COLOR_BTNFACE
    0x808080,  // 16 COLOR_BTNSHADOW
    0x808080,  // 17 COLOR_GRAYTEXT
    0x000000,  // 18 COLOR_BTNTEXT
    0x000000,  // 19 COLOR_INACTIVECAPTIONTEXT
    0xFFFFFF,  // 20 COLOR_BTNHIGHLIGHT
};

const std::unordered_map<std::string, Rgb>& namedColors() {
    static const std::unordered_map<std::string, Rgb> m = {
        {"clBlack", 0x000000},   {"clMaroon", 0x800000}, {"clGreen", 0x008000},
        {"clOlive", 0x808000},   {"clNavy", 0x000080},   {"clPurple", 0x800080},
        {"clTeal", 0x008080},    {"clGray", 0x808080},   {"clSilver", 0xC0C0C0},
        {"clRed", 0xFF0000},     {"clLime", 0x00FF00},   {"clYellow", 0xFFFF00},
        {"clBlue", 0x0000FF},    {"clFuchsia", 0xFF00FF}, {"clAqua", 0x00FFFF},
        {"clWhite", 0xFFFFFF},   {"clLtGray", 0xC0C0C0}, {"clDkGray", 0x808080},
        {"clScrollBar", kSystemColors[0]},   {"clBackground", kSystemColors[1]},
        {"clActiveCaption", kSystemColors[2]}, {"clInactiveCaption", kSystemColors[3]},
        {"clMenu", kSystemColors[4]},        {"clWindow", kSystemColors[5]},
        {"clWindowFrame", kSystemColors[6]}, {"clMenuText", kSystemColors[7]},
        {"clWindowText", kSystemColors[8]},  {"clCaptionText", kSystemColors[9]},
        {"clActiveBorder", kSystemColors[10]}, {"clInactiveBorder", kSystemColors[11]},
        {"clAppWorkSpace", kSystemColors[12]}, {"clHighlight", kSystemColors[13]},
        {"clHighlightText", kSystemColors[14]}, {"clBtnFace", kSystemColors[15]},
        {"clBtnShadow", kSystemColors[16]},  {"clGrayText", kSystemColors[17]},
        {"clBtnText", kSystemColors[18]},    {"clInactiveCaptionText", kSystemColors[19]},
        {"clBtnHighlight", kSystemColors[20]},
    };
    return m;
}

int intProp(const json& props, const char* key, int def) {
    auto it = props.find(key);
    return (it != props.end() && it->is_number_integer()) ? it->get<int>() : def;
}

bool boolProp(const json& props, const char* key, bool def) {
    auto it = props.find(key);
    return (it != props.end() && it->is_boolean()) ? it->get<bool>() : def;
}

std::string strProp(const json& props, const char* key) {
    auto it = props.find(key);
    return (it != props.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

std::string identProp(const json& props, const char* key) {
    auto it = props.find(key);
    if (it != props.end() && it->is_object() && it->contains("ident"))
        return (*it)["ident"].get<std::string>();
    return {};
}

std::string fileProp(const json& props, const char* key) {
    auto it = props.find(key);
    if (it != props.end() && it->is_object() && it->contains("file"))
        return (*it)["file"].get<std::string>();
    return {};
}

std::optional<Font> parseFont(const json& props, const Font* base) {
    bool any = props.contains("Font.Color") || props.contains("Font.Height") ||
               props.contains("Font.Name") || props.contains("Font.Style");
    if (!any) return std::nullopt;
    Font f = base ? *base : Font{};
    if (auto c = props.find("Font.Color"); c != props.end())
        if (auto rgb = parseColor(*c)) f.color = *rgb;
    if (int h = intProp(props, "Font.Height", 0); h != 0) f.height = h < 0 ? -h : h;
    if (auto n = strProp(props, "Font.Name"); !n.empty()) f.name = n;
    if (auto s = props.find("Font.Style"); s != props.end() && s->is_object() && s->contains("set")) {
        for (const auto& st : (*s)["set"]) {
            if (st == "fsBold") f.bold = true;
            if (st == "fsItalic") f.italic = true;
        }
    }
    return f;
}

std::unique_ptr<Component> parseComponent(const json& j, Component* parent) {
    auto c = std::make_unique<Component>();
    c->cls = j.value("class", "");
    c->name = j.value("name", "");
    c->parent = parent;
    const json& p = j.contains("props") ? j["props"] : json::object();
    c->raw = p;

    c->left = intProp(p, "Left", 0);
    c->top = intProp(p, "Top", 0);
    c->width = intProp(p, "Width", intProp(p, "ClientWidth", 0));
    c->height = intProp(p, "Height", intProp(p, "ClientHeight", 0));
    c->visible = boolProp(p, "Visible", true);
    if (auto it = p.find("Color"); it != p.end()) c->color = parseColor(*it);
    c->parentColor = boolProp(p, "ParentColor", c->cls == "TLabel" && !c->color);
    c->caption = strProp(p, "Caption");
    c->transparent = boolProp(p, "Transparent", false);
    c->autoSize = boolProp(p, "AutoSize", c->cls == "TLabel");
    c->stretch = boolProp(p, "Stretch", false);
    auto al = identProp(p, "Alignment");
    c->alignment = al == "taCenter" ? Align::Center : al == "taRightJustify" ? Align::Right : Align::Left;
    auto bo = identProp(p, "BevelOuter");
    c->bevelOuter = bo == "bvNone" ? 0 : bo == "bvLowered" ? 2 : 1;
    c->font = parseFont(p, nullptr);

    c->picture = fileProp(p, "Picture.Data");
    c->bitmapUp = fileProp(p, "BitMapUp.Data");
    c->bitmapDown = fileProp(p, "BitMapDown.Data");

    for (auto it = p.begin(); it != p.end(); ++it) {
        const std::string& k = it.key();
        if (k.size() > 2 && (k[0] == 'O' || k[0] == 'o') && k[1] == 'n' && it->is_object() &&
            it->contains("ident")) {
            std::string ev = "On" + k.substr(2);   // TautoButton uses "onClick"
            c->events[ev] = (*it)["ident"].get<std::string>();
        }
    }

    if (j.contains("children"))
        for (const auto& ch : j["children"]) c->children.push_back(parseComponent(ch, c.get()));
    return c;
}

}  // namespace

std::optional<Rgb> parseColor(const json& v) {
    if (v.is_object() && v.contains("ident")) {
        auto& m = namedColors();
        auto it = m.find(v["ident"].get<std::string>());
        if (it != m.end()) return it->second;
        return std::nullopt;
    }
    if (v.is_number_integer()) {
        auto n = static_cast<std::uint32_t>(v.get<std::int64_t>());
        if (n & 0x80000000u) {
            auto idx = n & 0xFFu;
            if (idx < std::size(kSystemColors)) return kSystemColors[idx];
            return std::nullopt;
        }
        Rgb r = n & 0xFF, g = (n >> 8) & 0xFF, b = (n >> 16) & 0xFF;
        return (r << 16) | (g << 8) | b;
    }
    return std::nullopt;
}

bool Component::isWindowed() const {
    return parent == nullptr || cls == "TPanel" || cls == "TNotebook" || cls == "TPage" ||
           cls == "TGroupBox" || cls == "TScrollBox";
}

const Component* Component::find(const std::string& n) const {
    if (name == n) return this;
    for (const auto& ch : children)
        if (auto* f = ch->find(n)) return f;
    return nullptr;
}

Component* Component::find(const std::string& n) {
    return const_cast<Component*>(static_cast<const Component*>(this)->find(n));
}

Form Form::fromJson(const json& j) {
    Form f;
    f.root = parseComponent(j, nullptr);
    const json& p = f.root->raw;
    f.clientWidth = intProp(p, "ClientWidth", f.root->width ? f.root->width : 640);
    f.clientHeight = intProp(p, "ClientHeight", f.root->height ? f.root->height : 480);
    if (auto font = parseFont(p, nullptr)) f.font = *font;
    if (!f.root->color) f.root->color = kSystemColors[15];  // clBtnFace
    return f;
}

Form Form::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open form: " + path);
    return fromJson(json::parse(in));
}

}  // namespace myulk
