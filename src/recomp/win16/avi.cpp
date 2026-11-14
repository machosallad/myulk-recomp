#include "recomp/win16/avi.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace win16 {
namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (p[1] << 8) | p[2]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
bool tag(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

uint32_t rgb(int y, int u, int v) {
    // Cinepak's colour space
    int r = y + 2 * v, g = y - u / 2 - v, b = y + 2 * u;
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

}  // namespace

bool Avi::open(const std::string& hostPath) {
    std::ifstream f(hostPath, std::ios::binary);
    if (!f) return false;
    file_.assign(std::istreambuf_iterator<char>(f), {});
    const uint8_t* d = file_.data();
    size_t n = file_.size();
    if (n < 12 || !tag(d, "RIFF") || !tag(d + 8, "AVI ")) return false;
    double usPerFrame = 0;
    bool inVideo = false, inAudio = false;
    double vRate = 0;
    // walk all chunks; LISTs are entered
    std::vector<std::pair<size_t, size_t>> stack{{12, n}};
    while (!stack.empty()) {
        auto [pos, end] = stack.back();
        stack.pop_back();
        while (pos + 8 <= end && pos + 8 <= n) {
            const uint8_t* c = d + pos;
            uint32_t sz = le32(c + 4);
            size_t body = pos + 8;
            size_t next = body + sz + (sz & 1);
            if (body + sz > n) break;
            if (tag(c, "LIST") || tag(c, "RIFF")) {
                stack.push_back({next, end});
                pos = body + 4;
                end = body + sz;
                continue;
            }
            const uint8_t* b = d + body;
            if (tag(c, "avih") && sz >= 40) {
                usPerFrame = le32(b);
                width = int(le32(b + 32));
                height = int(le32(b + 36));
            } else if (tag(c, "strh") && sz >= 28) {
                inVideo = tag(b, "vids");
                inAudio = tag(b, "auds");
                if (inVideo) {
                    uint32_t scale = le32(b + 20), rate = le32(b + 24);
                    if (scale) vRate = double(rate) / scale;
                }
            } else if (tag(c, "strf")) {
                if (inVideo && sz >= 12) {
                    width = int(le32(b + 4));
                    height = std::abs(int(int32_t(le32(b + 8))));
                } else if (inAudio && sz >= 16) {
                    audioChannels = le16(b + 2);
                    audioRate = int(le32(b + 4));
                    audioBits = le16(b + 14);
                }
            } else if (c[2] == 'd' && (c[3] == 'c' || c[3] == 'b') && c[0] == '0' && c[1] == '0') {
                frames_.push_back({body, sz});
            } else if (c[2] == 'w' && c[3] == 'b') {
                audio.insert(audio.end(), b, b + sz);
            }
            pos = next;
        }
    }
    if (vRate > 0) fps = vRate;
    else if (usPerFrame > 0) fps = 1e6 / usPerFrame;
    if (width <= 0 || height <= 0) return false;
    pic_.assign(size_t(width) * height, 0);
    decoded_ = -1;
    return true;
}

void Avi::decode_to(int index) {
    if (index >= int(frames_.size())) index = int(frames_.size()) - 1;
    if (index < decoded_) {
        decoded_ = -1;
        std::fill(pic_.begin(), pic_.end(), 0);
    }
    while (decoded_ < index) {
        ++decoded_;
        const Chunk& ch = frames_[size_t(decoded_)];
        if (ch.size >= 10) decode_frame(file_.data() + ch.offset, ch.size);
    }
}

void Avi::decode_frame(const uint8_t* d, size_t n) {
    const uint8_t* end = d + n;
    uint8_t flags = d[0];
    int numStrips = be16(d + 8);
    const uint8_t* p = d + 10;
    if (int(strips_.size()) < numStrips) strips_.resize(size_t(numStrips));
    int y0 = 0;
    for (int i = 0; i < numStrips && p + 12 <= end; ++i) {
        Strip& s = strips_[size_t(i)];
        uint32_t stripSize = be24(p + 1);
        int y1 = be16(p + 4), x1 = be16(p + 6), y2 = be16(p + 8), x2 = be16(p + 10);
        if (y1 == 0) { y1 = y0; y2 = y0 + be16(p + 8); }   // relative to the previous strip
        if (i > 0 && !(flags & 0x01)) {                     // codebooks carried over
            std::memcpy(s.v4, strips_[size_t(i - 1)].v4, sizeof s.v4);
            std::memcpy(s.v1, strips_[size_t(i - 1)].v1, sizeof s.v1);
        }
        const uint8_t* sEnd = std::min(end, p + stripSize);
        const uint8_t* q = p + 12;
        while (q + 4 <= sEnd) {
            int id = q[0];
            uint32_t csize = be24(q + 1);
            if (csize < 4) break;
            const uint8_t* cEnd = std::min(sEnd, q + csize);
            switch (id) {
                case 0x20: case 0x21: case 0x24: case 0x25: decode_codebook(s.v4, id, q + 4, cEnd); break;
                case 0x22: case 0x23: case 0x26: case 0x27: decode_codebook(s.v1, id, q + 4, cEnd); break;
                case 0x30: case 0x31: case 0x32: decode_vectors(s, id, x1, y1, x2, y2, q + 4, cEnd); break;
                default: break;
            }
            q += csize;
        }
        y0 = y2;
        p += stripSize;
    }
}

void Avi::decode_codebook(Entry* cb, int id, const uint8_t* d, const uint8_t* end) {
    int n = (id & 0x04) ? 4 : 6;
    uint32_t flag = 0, mask = 0;
    for (int i = 0; i < 256; ++i) {
        if ((id & 0x01) && !(mask >>= 1)) {
            if (d + 4 > end) return;
            flag = (uint32_t(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
            d += 4;
            mask = 0x80000000u;
        }
        if (!(id & 0x01) || (flag & mask)) {
            if (d + n > end) return;
            Entry& e = cb[i];
            std::memcpy(e.y, d, 4);
            e.u = n == 6 ? int8_t(d[4]) : 0;
            e.v = n == 6 ? int8_t(d[5]) : 0;
            d += n;
        }
    }
}

void Avi::put_block(int x, int y, const Entry* e[4], bool v1) {
    for (int j = 0; j < 4; ++j) {
        int py = y + j;
        if (py < 0 || py >= height) continue;
        for (int i = 0; i < 4; ++i) {
            int px = x + i;
            if (px < 0 || px >= width) continue;
            const Entry* c;
            int yi;
            if (v1) {
                c = e[0];
                yi = (j >> 1) * 2 + (i >> 1);
            } else {
                c = e[(j >> 1) * 2 + (i >> 1)];
                yi = (j & 1) * 2 + (i & 1);
            }
            pic_[size_t(py) * width + px] = rgb(c->y[yi], c->u, c->v);
        }
    }
}

void Avi::decode_vectors(Strip& s, int id, int x1, int y1, int x2, int y2, const uint8_t* d, const uint8_t* end) {
    uint32_t flag = 0, mask = 0;
    for (int y = y1; y < y2; y += 4) {
        for (int x = x1; x < x2; x += 4) {
            if ((id & 0x01) && !(mask >>= 1)) {
                if (d + 4 > end) return;
                flag = (uint32_t(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
                d += 4;
                mask = 0x80000000u;
            }
            if (!(id & 0x01) || (flag & mask)) {
                if (!(id & 0x02) && !(mask >>= 1)) {
                    if (d + 4 > end) return;
                    flag = (uint32_t(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
                    d += 4;
                    mask = 0x80000000u;
                }
                if ((id & 0x02) || (~flag & mask)) {
                    if (d >= end) return;
                    const Entry* e[4] = {&s.v1[*d++], nullptr, nullptr, nullptr};
                    put_block(x, y, e, true);
                } else {
                    if (d + 4 > end) return;
                    const Entry* e[4] = {&s.v4[d[0]], &s.v4[d[1]], &s.v4[d[2]], &s.v4[d[3]]};
                    d += 4;
                    put_block(x, y, e, false);
                }
            }
        }
    }
}

}  // namespace win16
