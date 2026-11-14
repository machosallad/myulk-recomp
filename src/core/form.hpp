// Delphi form model, loaded from the JSON written by tools/extract_resources.py.
//
// Only the properties the renderer and input handling need are decoded into
// typed fields; everything else stays available in `raw`.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace myulk {

// 0x00RRGGBB
using Rgb = std::uint32_t;

// Resolve a Delphi TColor (ident like clBtnFace, or a number 0x00BBGGRR /
// system colour 0x800000nn) to RGB using Windows 3.1 default system colours.
std::optional<Rgb> parseColor(const nlohmann::json& v);

enum class Align : std::uint8_t { Left, Center, Right };

struct Font {
    Rgb color = 0x000000;
    int height = 13;          // pixels (Delphi Font.Height is negative pixels)
    std::string name = "System";
    bool bold = false;
    bool italic = false;
};

struct Component {
    std::string cls;          // Delphi class, e.g. "TImage"
    std::string name;
    int left = 0, top = 0, width = 0, height = 0;
    bool visible = true;

    std::optional<Rgb> color; // explicit Color, if any
    bool parentColor = false;
    std::string caption;
    bool transparent = false;
    bool autoSize = false;
    bool stretch = false;
    Align alignment = Align::Left;
    int bevelOuter = 1;       // 0 none, 1 raised, 2 lowered (TPanel)
    std::optional<Font> font; // only when the component sets its own font

    // Image files, relative to the forms directory (e.g. "TMAIN/x.bmp").
    std::string picture;      // TImage Picture.Data
    std::string bitmapUp;     // TautoButton BitMapUp.Data
    std::string bitmapDown;   // TautoButton BitMapDown.Data

    std::map<std::string, std::string> events;  // "OnClick" -> "TillbakaClick"
    std::vector<std::unique_ptr<Component>> children;
    Component* parent = nullptr;
    nlohmann::json raw;       // all original properties

    bool isWindowed() const;  // TPanel, TForm... clip and offset children
    const Component* find(const std::string& name) const;
    Component* find(const std::string& name);
};

struct Form {
    std::unique_ptr<Component> root;
    int clientWidth = 640;
    int clientHeight = 480;
    Font font;                // form-level default font

    static Form fromJson(const nlohmann::json& j);
    static Form load(const std::string& path);
};

}  // namespace myulk
