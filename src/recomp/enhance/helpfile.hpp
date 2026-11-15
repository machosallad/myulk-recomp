// Reader for Windows 3.1 help files (.HLP), enough for the game's handbook
// (MYULK.HLP): topics with paragraphs, fonts, jump and popup links, and the
// embedded pictures. The format is the one documented by helpdeco
// (helpfile.txt); see helpfile.cpp for the parts used.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "recomp/win16/gdi.hpp"

namespace win16::help {

enum Style : uint8_t { kBody, kBold, kHeading, kTitle };

struct Item {
    enum Kind : uint8_t { Text, Picture, LineBreak, Tab } kind = Text;
    std::string text;     // Windows-1252
    Style style = kBody;
    bool accent = false;  // coloured text in the original (headings are red)
    int link = -1;        // topic index, -1: none
    bool popup = false;   // popup (definition) rather than a jump
    int picture = -1;     // index into HelpFile::pictures
    int place = 0;        // pictures: 0 in the text, 1 left, 2 right
};

struct Para {
    int align = 0;   // 0 left, 1 centre, 2 right
    int indent = 0;  // left indent in pixels (from the help file's twips)
    std::vector<Item> items;
};

struct Topic {
    std::string title;
    std::vector<Para> paras;
    std::string text;   // all text, upper case, for searching
};

struct HelpFile {
    std::string title;                 // |SYSTEM title ("Myulk handbok")
    std::vector<Topic> topics;
    std::vector<Surface> pictures;
    int contents = 0;                  // the contents topic
    std::map<int32_t, int> contextIds; // HELP_CONTEXT numbers -> topic

    bool load(const std::vector<uint8_t>& file);   // false if it is no help file
};

}  // namespace win16::help
