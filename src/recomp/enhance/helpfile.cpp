// Windows 3.1 help file reader (see helpfile.hpp).
//
// The file is a small file system: a B+ tree directory names internal files
// ("|SYSTEM", "|TOPIC", ...). |TOPIC is a chain of records in 4 KB blocks
// (LZ77-compressed to 16 KB when |SYSTEM says so); positions are
// block << 14 | offset. A type 2 record starts a topic (title in its second
// data area); a type 0x20 record is a paragraph: its first data area holds the
// paragraph attributes and a stream of formatting commands, its second the
// text pieces that go between the commands (phrase-compressed in some files).
// Links carry a hash of the target's context name; |CONTEXT maps the hash to a
// topic offset, and |TTLBTREE lists the topics in the order of those offsets.
#include "recomp/enhance/helpfile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace win16::help {
namespace {

using Bytes = std::vector<uint8_t>;

struct Reader {
    const uint8_t* p;
    size_t n;
    bool ok(size_t o, size_t len) const { return o + len <= n; }
    uint8_t u8(size_t o) const { return ok(o, 1) ? p[o] : 0; }
    uint16_t u16(size_t o) const { return ok(o, 2) ? uint16_t(p[o] | p[o + 1] << 8) : 0; }
    uint32_t u32(size_t o) const { return ok(o, 4) ? uint32_t(p[o] | p[o + 1] << 8 | p[o + 2] << 16 | uint32_t(p[o + 3]) << 24) : 0; }
    int32_t i32(size_t o) const { return int32_t(u32(o)); }
    // the help compiler's variable-length numbers: even = short form
    uint32_t cu16(size_t& o) const { return u8(o) & 1 ? (o += 2, u16(o - 2) >> 1) : (o += 1, u8(o - 1) >> 1u); }
    int32_t cs16(size_t& o) const { return u8(o) & 1 ? (o += 2, int32_t(u16(o - 2) >> 1) - 0x4000) : (o += 1, int32_t(u8(o - 1) >> 1) - 0x40); }
    uint32_t cu32(size_t& o) const { return u8(o) & 1 ? (o += 4, u32(o - 4) >> 1) : (o += 2, uint32_t(u16(o - 2) >> 1)); }
    int32_t cs32(size_t& o) const { return u8(o) & 1 ? (o += 4, int32_t(u32(o - 4) >> 1) - 0x40000000) : (o += 2, int32_t(u16(o - 2) >> 1) - 0x4000); }
};

Bytes lz77(const uint8_t* src, size_t n, size_t limit) {
    Bytes out;
    size_t i = 0;
    while (i < n && out.size() < limit) {
        uint8_t flags = src[i++];
        for (int bit = 0; bit < 8 && i < n && out.size() < limit; ++bit) {
            if (flags & (1 << bit)) {
                if (i + 2 > n) break;
                unsigned w = src[i] | src[i + 1] << 8;
                i += 2;
                size_t len = (w >> 12) + 3, dist = (w & 0xFFF) + 1;
                if (dist > out.size()) return out;
                for (size_t k = 0; k < len; ++k) out.push_back(out[out.size() - dist]);
            } else {
                out.push_back(src[i++]);
            }
        }
    }
    return out;
}

// An internal file: 9-byte header (reserved, used size, flags), then the data.
Bytes internal_file(const Reader& f, int32_t off) {
    if (off < 0 || !f.ok(size_t(off), 9)) return {};
    uint32_t used = f.u32(size_t(off) + 4);
    size_t start = size_t(off) + 9;
    if (!f.ok(start, used)) return {};
    return Bytes(f.p + start, f.p + start + used);
}

// Calls `entry(page reader, offset)` for every leaf entry; it returns the
// offset after the entry.
void btree_leaves(const Bytes& data, const std::function<size_t(const Reader&, size_t)>& entry) {
    Reader r{data.data(), data.size()};
    if (r.u16(0) != 0x293B) return;
    size_t pageSize = r.u16(4);
    unsigned page = r.u16(26), levels = r.u16(32);
    const size_t pagesAt = 38;
    for (unsigned l = 1; l < levels; ++l) {   // index pages: the first child is "previous page"
        size_t at = pagesAt + page * pageSize;
        if (!r.ok(at, 6)) return;
        page = r.u16(at + 4);
    }
    for (int guard = 0; page != 0xFFFF && guard < 65536; ++guard) {
        size_t at = pagesAt + page * pageSize;
        if (!r.ok(at, 8)) return;
        Reader pr{data.data() + at, std::min(pageSize, data.size() - at)};
        unsigned count = pr.u16(2);
        page = pr.u16(6);
        size_t o = 8;
        for (unsigned k = 0; k < count && o < pr.n; ++k) o = entry(pr, o);
    }
}

std::string cstr(const Reader& r, size_t& o) {
    std::string s;
    while (o < r.n && r.p[o]) s += char(r.p[o++]);
    ++o;
    return s;
}

// Pictures: an SHG/MRB container ("lp"/"lP", count, offsets); the first
// picture, a device-independent (6) or -dependent (5) bitmap, packed raw,
// run-length, LZ77 or both.
Surface decode_picture(const uint8_t* data, size_t n) {
    Reader c{data, n};
    if (c.u16(2) < 1) return {};
    size_t po = c.u32(4);
    if (po >= n) return {};
    Reader p{data + po, n - po};
    uint8_t type = p.u8(0), pack = p.u8(1);
    if (type != 5 && type != 6) return {};
    size_t q = 2;
    p.cu32(q); p.cu32(q);   // dpi
    p.cu16(q);              // planes
    unsigned bits = p.cu16(q);
    int w = int(p.cu32(q)), h = int(p.cu32(q));
    unsigned used = p.cu32(q);
    p.cu32(q);              // colours important
    size_t csize = p.cu32(q);
    p.cu32(q);              // hotspot size
    size_t coff = p.u32(q), hotoff = p.u32(q + 4);
    (void)hotoff;
    q += 8;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !(bits == 1 || bits == 4 || bits == 8 || bits == 24)) return {};
    std::vector<Color> pal;
    unsigned ncol = bits <= 8 ? (used ? used : 1u << bits) : 0;
    if (type == 6)
        for (unsigned k = 0; k < ncol; ++k)
            pal.push_back(Color(p.u8(q + 4 * k + 2)) << 16 | Color(p.u8(q + 4 * k + 1)) << 8 | p.u8(q + 4 * k));
    else
        for (unsigned k = 0; k < ncol; ++k) pal.push_back(k ? 0xFFFFFF : 0);   // monochrome DDB
    size_t stride = type == 6 ? (size_t(w) * bits + 31) / 32 * 4 : (size_t(w) * bits + 15) / 16 * 2;
    size_t need = stride * size_t(h);
    if (!p.ok(coff, csize)) return {};
    Bytes raw(p.p + coff, p.p + coff + csize);
    auto unrle = [&](const Bytes& src) {
        Bytes out;
        for (size_t i = 0; i < src.size() && out.size() < need;) {
            uint8_t k = src[i++];
            if (k & 0x80) {
                size_t m = k & 0x7F;
                for (size_t j = 0; j < m && i < src.size(); ++j) out.push_back(src[i++]);
            } else if (i < src.size()) {
                out.insert(out.end(), k, src[i++]);
            }
        }
        return out;
    };
    if (pack == 1) raw = unrle(raw);
    else if (pack == 2) raw = lz77(raw.data(), raw.size(), need);
    else if (pack == 3) raw = unrle(lz77(raw.data(), raw.size(), SIZE_MAX));
    raw.resize(need, 0);
    Surface s(w, h, 32);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = raw.data() + (type == 6 ? size_t(h - 1 - y) : size_t(y)) * stride;   // DIBs are bottom-up
        for (int x = 0; x < w; ++x) {
            Color c;
            if (bits == 24) c = Color(row[x * 3 + 2]) << 16 | Color(row[x * 3 + 1]) << 8 | row[x * 3];
            else {
                unsigned idx = bits == 8 ? row[x] : bits == 4 ? (row[x / 2] >> (x % 2 ? 0 : 4)) & 15 : (row[x / 8] >> (7 - x % 8)) & 1;
                c = idx < pal.size() ? pal[idx] : 0;
            }
            s.set(x, y, c);
        }
    }
    return s;
}

std::string upper(const std::string& s) {
    std::string u = s;
    for (char& ch : u) {
        unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) ch = char(c - 0x20);
    }
    return u;
}

}  // namespace

bool HelpFile::load(const std::vector<uint8_t>& file) {
    Reader f{file.data(), file.size()};
    if (f.u32(0) != 0x00035F3F) return false;
    std::map<std::string, int32_t> files;
    btree_leaves(internal_file(f, f.i32(4)), [&](const Reader& r, size_t o) {
        std::string name = cstr(r, o);
        files[name] = r.i32(o);
        return o + 4;
    });
    auto get = [&](const char* name) { auto it = files.find(name); return it == files.end() ? Bytes() : internal_file(f, it->second); };

    // |SYSTEM: version, compression, title, contents topic
    Bytes sys = get("|SYSTEM");
    Reader s{sys.data(), sys.size()};
    if (s.u16(0) != 0x036C) return false;
    unsigned minor = s.u16(2), flags = s.u16(10);
    bool compressed = minor > 16 && (flags == 4 || flags == 8);
    size_t blockSize = minor > 16 && flags == 8 ? 2048 : 4096;
    uint32_t contentsOffset = 0;
    if (minor <= 16) {
        size_t o = 12;
        title = cstr(s, o);
    } else {
        for (size_t o = 12; s.ok(o, 4);) {
            unsigned type = s.u16(o), len = s.u16(o + 2);
            if (type == 1) { size_t t = o + 4; title = cstr(s, t); }
            if (type == 3 && len >= 4) contentsOffset = s.u32(o + 4);
            o += 4 + len;
        }
    }

    // |Phrases (Windows 3.1 phrase compression), when present
    std::vector<std::string> phrases;
    Bytes ph = get("|Phrases");
    if (ph.size() > 4) {
        Reader pr{ph.data(), ph.size()};
        unsigned count = pr.u16(0);
        size_t o = minor > 16 ? 8 : 4;
        std::vector<unsigned> offs;
        for (unsigned k = 0; k <= count; ++k) offs.push_back(pr.u16(o + 2 * k));
        size_t body = o + 2 * (count + 1);
        Bytes text = minor > 16 ? lz77(ph.data() + body, ph.size() - body, size_t(pr.i32(4)))
                                : Bytes(ph.begin() + long(body), ph.end());
        for (unsigned k = 0; k < count; ++k) {
            size_t a = offs[k] - offs[0], b = offs[k + 1] - offs[0];
            phrases.push_back(a <= b && b <= text.size() ? std::string(text.begin() + long(a), text.begin() + long(b)) : "");
        }
    }

    // |TOPIC: the blocks without their 12-byte headers, decompressed
    Bytes top = get("|TOPIC");
    const size_t decBlock = compressed ? 16384 : blockSize - 12;
    Bytes dec;
    for (size_t b = 0; b * blockSize < top.size(); ++b) {
        size_t start = b * blockSize + 12, end = std::min(top.size(), (b + 1) * blockSize);
        Bytes part = start < end ? (compressed ? lz77(top.data() + start, end - start, decBlock)
                                               : Bytes(top.begin() + long(start), top.begin() + long(end)))
                                 : Bytes();
        part.resize(decBlock, 0);
        dec.insert(dec.end(), part.begin(), part.end());
    }
    Reader d{dec.data(), dec.size()};
    Reader t{top.data(), top.size()};

    // |FONT: per descriptor its style
    struct FontInfo { Style style; bool accent; };
    std::vector<FontInfo> fonts;
    Bytes fo = get("|FONT");
    if (fo.size() >= 8) {
        Reader fr{fo.data(), fo.size()};
        unsigned count = fr.u16(2), at = fr.u16(6);
        for (unsigned k = 0; k < count; ++k) {
            size_t o = at + 11 * k;
            uint8_t attr = fr.u8(o), half = fr.u8(o + 1);
            uint32_t fg = uint32_t(fr.u8(o + 5)) << 16 | fr.u8(o + 6) << 8 | fr.u8(o + 7);
            Style st = half >= 34 ? kTitle : half >= 28 ? kHeading : (attr & 1) ? kBold : kBody;
            bool red = (fg >> 16) > 0x80 && ((fg >> 8) & 0xFF) < 0x60;
            fonts.push_back({st, red});
        }
    }

    // links are collected by hash and resolved at the end
    struct PendingLink { int topic, para, item; uint32_t hash; };
    std::vector<PendingLink> pending;

    auto expand = [&](const uint8_t* src, size_t n, size_t outSize) {
        std::string out;
        for (size_t i = 0; i < n && out.size() < outSize;) {
            uint8_t c = src[i];
            if (phrases.empty() || c == 0 || c > 15) { out += char(c); ++i; continue; }
            if (i + 1 >= n) break;
            unsigned x = 256u * (c - 1) + src[i + 1];
            i += 2;
            if ((x >> 1) < phrases.size()) out += phrases[x >> 1];
            if (x & 1) out += ' ';
        }
        return out;
    };

    int32_t pos = t.i32(4);   // first record, from the first block header
    for (int guard = 0; pos != -1 && guard < 200000; ++guard) {
        size_t o = size_t(pos >> 14) * decBlock + size_t(pos & 0x3FFF) - 12;
        if (!d.ok(o, 21)) break;
        int32_t size = d.i32(o), len2 = d.i32(o + 4), next = d.i32(o + 12), len1 = d.i32(o + 16);
        uint8_t type = d.u8(o + 20);
        if (size < 21 || len1 < 21 || len1 > size || !d.ok(o, size_t(size))) break;
        Reader l1{dec.data() + o + 21, size_t(len1 - 21)};
        size_t raw2 = size_t(size - len1);
        std::string text2 = size_t(len2) > raw2 ? expand(dec.data() + o + size_t(len1), raw2, size_t(len2))
                                                : std::string(reinterpret_cast<const char*>(dec.data() + o + size_t(len1)), std::min(raw2, size_t(std::max(len2, 0))));
        if (type == 2) {
            Topic tp;
            tp.title = text2.substr(0, text2.find('\0'));
            topics.push_back(std::move(tp));
        } else if ((type == 0x20 || type == 0x23) && !topics.empty()) {
            Topic& tp = topics.back();
            std::vector<std::string> strs;
            for (size_t a = 0;;) {
                size_t z = text2.find('\0', a);
                strs.push_back(text2.substr(a, z == std::string::npos ? std::string::npos : z - a));
                if (z == std::string::npos) break;
                a = z + 1;
            }
            size_t si = 0, q = 0;
            l1.cs32(q);   // topic size
            l1.cu16(q);   // topic length
            if (type == 0x23) {
                unsigned cols = l1.u8(q), ttype = l1.u8(q + 1);
                q += 2 + (ttype == 0 || ttype == 2 ? 2 : 0) + 4 * cols;
            }
            FontInfo font{kBody, false};
            int link = -1;   // current hotspot: index into `pending` or -1
            bool popup = false;
            while (q < l1.n) {
                if (type == 0x23) {
                    if (int16_t(l1.u16(q)) == -1) break;
                    q += 2;
                }
                q += 4;
                unsigned bits = l1.u16(q);
                q += 2;
                Para para;
                if (bits & 0x1) l1.cu32(q);
                for (unsigned b : {0x2u, 0x4u, 0x8u}) if (bits & b) l1.cs16(q);   // spacing
                if (bits & 0x10) para.indent = std::max(0, int(l1.cs16(q)) * 96 / 1440 * 1);
                if (bits & 0x20) l1.cs16(q);
                if (bits & 0x40) l1.cs16(q);
                if (bits & 0x100) { ++q; l1.cs16(q); }
                if (bits & 0x200) {
                    int tabs = l1.cs16(q);
                    for (int k = 0; k < tabs; ++k) if (l1.cu16(q) & 0x4000) l1.cu16(q);
                }
                para.align = bits & 0x400 ? 2 : bits & 0x800 ? 1 : 0;
                tp.paras.push_back(std::move(para));
                auto add = [&](Item it) {
                    Para& pp = tp.paras.back();
                    if (link >= 0) {
                        it.popup = popup;
                        pending.push_back({int(topics.size() - 1), int(tp.paras.size() - 1), int(pp.items.size()), pending[size_t(link)].hash});
                    }
                    pp.items.push_back(std::move(it));
                };
                bool end = false;
                while (q < l1.n && !end) {
                    if (si < strs.size() && !strs[si].empty()) {
                        Item it;
                        it.text = strs[si];
                        it.style = font.style;
                        it.accent = font.accent;
                        add(std::move(it));
                        tp.text += upper(strs[si]) + " ";
                    }
                    ++si;
                    uint8_t cmd = l1.u8(q++);
                    switch (cmd) {
                        case 0xFF: end = true; break;
                        case 0x80: {
                            unsigned k = l1.u16(q);
                            q += 2;
                            if (k < fonts.size()) font = fonts[k];
                            break;
                        }
                        case 0x81: { Item it; it.kind = Item::LineBreak; tp.paras.back().items.push_back(it); break; }
                        case 0x82: { Para np; np.align = tp.paras.back().align; np.indent = tp.paras.back().indent; tp.paras.push_back(np); break; }
                        case 0x83: { Item it; it.kind = Item::Tab; tp.paras.back().items.push_back(it); break; }
                        case 0x86: case 0x87: case 0x88: {
                            uint8_t ptype = l1.u8(q++);
                            size_t psize = size_t(std::max(0, l1.cs32(q)));   // a signed number
                            if (ptype == 0x22) l1.cu16(q);   // hotspots
                            size_t start = q;
                            if ((ptype == 3 || ptype == 0x22) && l1.u16(q) == 1 && l1.ok(q + 2, psize > 2 ? psize - 2 : 0)) {
                                Surface pic = decode_picture(l1.p + q + 2, psize - 2);
                                if (pic.w > 0) {
                                    Item it;
                                    it.kind = Item::Picture;
                                    it.picture = int(pictures.size());
                                    it.place = cmd == 0x86 ? 0 : cmd == 0x87 ? 1 : 2;
                                    pictures.push_back(std::move(pic));
                                    tp.paras.back().items.push_back(it);
                                }
                            }
                            q = start + psize;
                            break;
                        }
                        case 0x89: link = -1; break;
                        case 0xE2: case 0xE3: case 0xE6: case 0xE7:
                            pending.push_back({-1, -1, -1, l1.u32(q)});
                            link = int(pending.size() - 1);
                            popup = cmd == 0xE2 || cmd == 0xE6;
                            q += 4;
                            break;
                        case 0xC8: case 0xCC: case 0xEA: case 0xEB: case 0xEE: case 0xEF:
                            q += 2 + l1.u16(q);   // macros and links to other files: not followed
                            link = -1;
                            break;
                        default: end = true; q = l1.n; break;   // unknown: stop this record
                    }
                }
            }
        }
        pos = next;
    }

    // resolve links: hash -> topic offset (|CONTEXT) -> topic (|TTLBTREE order)
    std::vector<int32_t> offsets;
    btree_leaves(get("|TTLBTREE"), [&](const Reader& r, size_t o) {
        offsets.push_back(r.i32(o));
        o += 4;
        cstr(r, o);
        return o;
    });
    auto topic_at = [&](int32_t off) {
        auto it = std::upper_bound(offsets.begin(), offsets.end(), off);
        int k = int(it - offsets.begin()) - 1;
        return k >= 0 && k < int(topics.size()) ? k : -1;
    };
    std::map<uint32_t, int32_t> context;
    btree_leaves(get("|CONTEXT"), [&](const Reader& r, size_t o) {
        context[r.u32(o)] = r.i32(o + 4);
        return o + 8;
    });
    for (const PendingLink& pl : pending) {
        if (pl.topic < 0) continue;
        auto it = context.find(pl.hash);
        if (it == context.end()) continue;
        topics[size_t(pl.topic)].paras[size_t(pl.para)].items[size_t(pl.item)].link = topic_at(it->second);
    }
    if (offsets.size() == topics.size()) {
        int c = topic_at(int32_t(contentsOffset));
        if (c >= 0) contents = c;
        Bytes map = get("|CTXOMAP");
        Reader m{map.data(), map.size()};
        for (unsigned k = 0, n = m.u16(0); k < n; ++k) {
            int tk = topic_at(m.i32(2 + 8 * k + 4));
            if (tk >= 0) contextIds[m.i32(2 + 8 * k)] = tk;
        }
    }
    // drop trailing empty paragraphs
    for (Topic& tp : topics)
        while (!tp.paras.empty() && tp.paras.back().items.empty()) tp.paras.pop_back();
    return !topics.empty();
}

}  // namespace win16::help
