// Bilateral de-dither filter, 5x5, integer weights (see dedither.hpp).
// Each output pixel is a weighted average of its neighbours, where a
// neighbour counts less the farther away it is (spatial sigma 1.3 px) and the
// more its colour differs (range sigma 30 in RGB distance). Dither dots are
// small differences and get averaged away; real edges are big differences
// and are left alone. Tuned on screenshots of the adventure scenes and farm.
#include "recomp/enhance/dedither.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace win16 {
namespace {

constexpr int kRadius = 2;
constexpr double kSigmaS = 1.3, kSigmaR = 30.0;
constexpr int kRangeShift = 4;                     // squared distance >> 4 indexes the table
constexpr int kRangeSize = (3 * 255 * 255 >> kRangeShift) + 1;  // every distance: no branch

struct Tables {
    std::array<uint32_t, (2 * kRadius + 1) * (2 * kRadius + 1)> spatial{};
    std::array<uint32_t, kRangeSize> range{};
    Tables() {
        for (int dy = -kRadius; dy <= kRadius; ++dy)
            for (int dx = -kRadius; dx <= kRadius; ++dx)
                spatial[size_t((dy + kRadius) * (2 * kRadius + 1) + dx + kRadius)] =
                    uint32_t(std::lround(256.0 * std::exp(-(dx * dx + dy * dy) / (2 * kSigmaS * kSigmaS))));
        for (int i = 0; i < kRangeSize; ++i)
            range[size_t(i)] = uint32_t(
                std::lround(256.0 * std::exp(-double(i << kRangeShift) / (2 * kSigmaR * kSigmaR))));
    }
};

Surface g_prev, g_out;
std::vector<int16_t> g_r, g_g, g_b;   // padded colour planes of the area being filtered

void filter_rect(const Surface& s, int x0, int y0, int x1, int y1) {
    static const Tables t;
    // copy the area plus a border (edge pixels repeated) into separate planes,
    // so the inner loop needs no clamping or unpacking
    const int pw = x1 - x0 + 2 * kRadius, ph = y1 - y0 + 2 * kRadius;
    g_r.resize(size_t(pw) * ph); g_g.resize(g_r.size()); g_b.resize(g_r.size());
    for (int y = 0; y < ph; ++y) {
        const Color* row = &s.px[size_t(std::clamp(y0 + y - kRadius, 0, s.h - 1)) * s.w];
        for (int x = 0; x < pw; ++x) {
            Color c = row[std::clamp(x0 + x - kRadius, 0, s.w - 1)];
            size_t i = size_t(y) * pw + x;
            g_r[i] = int16_t(c >> 16 & 0xFF); g_g[i] = int16_t(c >> 8 & 0xFF); g_b[i] = int16_t(c & 0xFF);
        }
    }
    constexpr int kTaps = (2 * kRadius + 1) * (2 * kRadius + 1);
    int offs[kTaps];
    for (int dy = 0, k = 0; dy <= 2 * kRadius; ++dy)
        for (int dx = 0; dx <= 2 * kRadius; ++dx) offs[k++] = dy * pw + dx;
    for (int y = 0; y < y1 - y0; ++y) {
        Color* out = &g_out.px[size_t(y0 + y) * s.w + x0];
        for (int x = 0; x < x1 - x0; ++x) {
            size_t base = size_t(y) * pw + x, ci = base + size_t(kRadius) * pw + kRadius;
            const int16_t *pr = &g_r[base], *pg = &g_g[base], *pb = &g_b[base];
            int cr = g_r[ci], cg = g_g[ci], cb = g_b[ci];
            uint32_t sr = 0, sg = 0, sb = 0, sw = 0;
            for (int k = 0; k < kTaps; ++k) {
                int nr = pr[offs[k]], ng = pg[offs[k]], nb = pb[offs[k]];
                int d2 = (nr - cr) * (nr - cr) + (ng - cg) * (ng - cg) + (nb - cb) * (nb - cb);
                uint32_t w = t.spatial[size_t(k)] * t.range[size_t(d2 >> kRangeShift)];
                sr += w * uint32_t(nr); sg += w * uint32_t(ng); sb += w * uint32_t(nb); sw += w;
            }
            // the centre always has weight 256*256, so sw > 0
            float inv = 1.0f / float(sw);
            out[x] = uint32_t(float(sr) * inv + 0.5f) << 16 | uint32_t(float(sg) * inv + 0.5f) << 8 |
                     uint32_t(float(sb) * inv + 0.5f);
        }
    }
}

}  // namespace

const Surface& dedither(const Surface& s) {
    bool fresh = g_prev.w != s.w || g_prev.h != s.h;
    int x0 = 0, y0 = 0, x1 = s.w, y1 = s.h;
    if (fresh) {
        g_prev = s;
        g_out = Surface(s.w, s.h, s.bpp);
    } else {
        // bounding box of what changed since the last call
        y0 = s.h; y1 = -1; x0 = s.w; x1 = -1;
        for (int y = 0; y < s.h; ++y) {
            const Color* a = &s.px[size_t(y) * s.w];
            const Color* b = &g_prev.px[size_t(y) * s.w];
            if (std::memcmp(a, b, size_t(s.w) * sizeof(Color)) == 0) continue;
            y0 = std::min(y0, y); y1 = y;
            int l = 0, r = s.w - 1;
            while (a[l] == b[l]) ++l;
            while (a[r] == b[r]) --r;
            x0 = std::min(x0, l); x1 = std::max(x1, r);
        }
        if (y1 < 0) return g_out;
        std::memcpy(&g_prev.px[size_t(y0) * s.w], &s.px[size_t(y0) * s.w],
                    size_t(y1 - y0 + 1) * s.w * sizeof(Color));
        // pixels within the radius of a change see it too
        x0 = std::max(0, x0 - kRadius); y0 = std::max(0, y0 - kRadius);
        x1 = std::min(s.w, x1 + 1 + kRadius); y1 = std::min(s.h, y1 + 1 + kRadius);
    }
    filter_rect(s, x0, y0, x1, y1);
    return g_out;
}

}  // namespace win16
