#include "core/form.hpp"
#include "test.hpp"

using myulk::Form;
using myulk::parseColor;
using nlohmann::json;

TEST(color_idents_and_tcolor_numbers) {
    CHECK_EQ(*parseColor(json{{"ident", "clBlack"}}), 0x000000u);
    CHECK_EQ(*parseColor(json{{"ident", "clBtnFace"}}), 0xC0C0C0u);
    CHECK_EQ(*parseColor(json(0x0000FF)), 0xFF0000u);           // TColor is 0x00BBGGRR
    CHECK_EQ(*parseColor(json(static_cast<std::int64_t>(0x8000000F))), 0xC0C0C0u);  // clBtnFace
}

TEST(form_parses_components_and_events) {
    json j = json::parse(R"({
      "class": "TFormX", "name": "FormX",
      "props": {"ClientWidth": 320, "ClientHeight": 200, "Color": {"ident": "clBlack"}},
      "children": [
        {"class": "TPanel", "name": "Panel", "props": {"Left": 10, "Top": 20, "Width": 100,
          "Height": 50, "BevelOuter": {"ident": "bvNone"}},
         "children": [
           {"class": "TImage", "name": "Img", "props": {"Left": 1, "Top": 2, "Width": 3,
             "Height": 4, "Picture.Data": {"file": "TFORMX/Img.bmp", "size": 9, "kind": "bmp"},
             "OnClick": {"ident": "ImgClick"}}, "children": []},
           {"class": "TautoButton", "name": "Btn", "props": {"onClick": {"ident": "BtnClick"},
             "Visible": false}, "children": []}
         ]}
      ]})");
    Form f = Form::fromJson(j);
    CHECK_EQ(f.clientWidth, 320);
    CHECK_EQ(*f.root->color, 0x000000u);
    auto* img = f.root->find("Img");
    CHECK(img != nullptr);
    CHECK_EQ(img->picture, std::string("TFORMX/Img.bmp"));
    CHECK_EQ(img->events.at("OnClick"), std::string("ImgClick"));
    CHECK(img->parent == f.root->find("Panel"));
    CHECK_EQ(f.root->find("Panel")->bevelOuter, 0);
    auto* btn = f.root->find("Btn");
    CHECK(!btn->visible);
    CHECK_EQ(btn->events.at("OnClick"), std::string("BtnClick"));
}
