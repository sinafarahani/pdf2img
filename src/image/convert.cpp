#include "convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace p2i {

static constexpr uint32_t make_gray_ramp_entry(int i) { return (static_cast<uint32_t>(i) << 16) | (static_cast<uint32_t>(i) << 8) | static_cast<uint32_t>(i); }

const uint32_t kGrayRamp256[256] = {
#define G4(i) make_gray_ramp_entry(i), make_gray_ramp_entry(i + 1), make_gray_ramp_entry(i + 2), make_gray_ramp_entry(i + 3)
#define G16(i) G4(i), G4(i + 4), G4(i + 8), G4(i + 12)
#define G64(i) G16(i), G16(i + 16), G16(i + 32), G16(i + 48)
    G64(0), G64(64), G64(128), G64(192)
#undef G64
#undef G16
#undef G4
};

Image to_gray(const Image& src) {
    if (src.bpp == 8 && src.gray) return src;
    Image g;
    g.allocate(src.width, src.height, 8);
    g.gray = true;
    g.dpiX = src.dpiX; g.dpiY = src.dpiY;
    if (src.bpp == 24) {
        for (int y = 0; y < src.height; ++y) {
            const uint8_t* s = src.row(y);
            uint8_t* d = g.row(y);
            for (int x = 0; x < src.width; ++x, s += 3) {
                d[x] = static_cast<uint8_t>((s[2] * 299 + s[1] * 587 + s[0] * 114 + 500) / 1000);
            }
        }
    } else if (src.bpp == 8) { // palette
        for (int y = 0; y < src.height; ++y) {
            const uint8_t* s = src.row(y);
            uint8_t* d = g.row(y);
            for (int x = 0; x < src.width; ++x) {
                uint32_t c = (src.palette && s[x] < src.paletteSize) ? src.palette[s[x]] : 0;
                d[x] = static_cast<uint8_t>((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114 + 500) / 1000);
            }
        }
    } else if (src.bpp == 4) {
        for (int y = 0; y < src.height; ++y) {
            const uint8_t* s = src.row(y);
            uint8_t* d = g.row(y);
            for (int x = 0; x < src.width; ++x) {
                int idx = (x & 1) ? (s[x >> 1] & 15) : (s[x >> 1] >> 4);
                uint32_t c = (src.palette && idx < src.paletteSize) ? src.palette[idx] : 0;
                d[x] = static_cast<uint8_t>((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114 + 500) / 1000);
            }
        }
    } else { // 1-bit: 1 = black
        for (int y = 0; y < src.height; ++y) {
            const uint8_t* s = src.row(y);
            uint8_t* d = g.row(y);
            for (int x = 0; x < src.width; ++x) d[x] = (s[x >> 3] & (0x80 >> (x & 7))) ? 0 : 255;
        }
    }
    return g;
}

Image gray_to_bilevel(const Image& gray, int threshold) {
    Image b;
    b.allocate(gray.width, gray.height, 1);
    b.dpiX = gray.dpiX; b.dpiY = gray.dpiY;
    for (int y = 0; y < gray.height; ++y) {
        const uint8_t* s = gray.row(y);
        uint8_t* d = b.row(y);
        for (int x = 0; x < gray.width; ++x) {
            if (s[x] < threshold) d[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
        }
    }
    return b;
}

namespace {
// Exact nearest-colour lookup with a lazily filled 24-bit table (16 MB + 2 MB of valid bits per conversion).
// Pages contain few distinct colours, so almost every pixel is a table hit.
class NearestColor {
public:
    NearestColor(const uint32_t* pal, int n) : pal_(pal), n_(n), table_(1u << 24, 0), valid_((1u << 24) / 64, 0) {}
    uint8_t lookup(int r, int g, int b) {
        const uint32_t key = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        uint64_t& bits = valid_[key >> 6];
        const uint64_t mask = 1ull << (key & 63);
        if (bits & mask) return table_[key];
        uint8_t best = exact(r, g, b);
        table_[key] = best;
        bits |= mask;
        return best;
    }
    // Exact nearest (no cache) — used for the 16-colour palette where cells are coarse anyway.
    uint8_t exact(int r, int g, int b) const {
        int best = 0; long bestD = 0x7FFFFFFF;
        for (int i = 0; i < n_; ++i) {
            int pr = (pal_[i] >> 16) & 255, pg = (pal_[i] >> 8) & 255, pb = pal_[i] & 255;
            long d = static_cast<long>(pr - r) * (pr - r) + static_cast<long>(pg - g) * (pg - g) + static_cast<long>(pb - b) * (pb - b);
            if (d < bestD) { bestD = d; best = i; if (d == 0) break; }
        }
        return static_cast<uint8_t>(best);
    }
private:
    const uint32_t* pal_;
    int n_;
    std::vector<uint8_t> table_;
    std::vector<uint64_t> valid_;
};
} // namespace

Image to_palette(const Image& src, const uint32_t* palette, int paletteSize, int bpp) {
    Image out;
    out.allocate(src.width, src.height, bpp);
    out.gray = false;
    out.palette = palette;
    out.paletteSize = paletteSize;
    out.dpiX = src.dpiX; out.dpiY = src.dpiY;
    NearestColor nc(palette, paletteSize);
    Image grayCopy;
    if (src.bpp != 24 && !(src.bpp == 8 && src.gray)) grayCopy = to_gray(src);
    auto put = [&](uint8_t* drow, int x, uint8_t idx) {
        if (bpp == 8) drow[x] = idx;
        else if (x & 1) drow[x >> 1] |= static_cast<uint8_t>(idx & 15);
        else drow[x >> 1] = static_cast<uint8_t>(idx << 4);
    };
    for (int y = 0; y < src.height; ++y) {
        const uint8_t* s = src.row(y);
        uint8_t* d = out.row(y);
        if (src.bpp == 24) {
            for (int x = 0; x < src.width; ++x, s += 3) {
                uint8_t idx = nc.lookup(s[2], s[1], s[0]);
                put(d, x, idx);
            }
        } else { // gray8 (other layouts were converted to gray above)
            const uint8_t* gs = (src.bpp == 8 && src.gray) ? s : grayCopy.row(y);
            for (int x = 0; x < src.width; ++x) {
                uint8_t v = gs[x];
                uint8_t idx = nc.lookup(v, v, v);
                put(d, x, idx);
            }
        }
    }
    return out;
}

namespace {
struct Weights { std::vector<int> start; std::vector<int> count; std::vector<float> w; int maxCount = 0; };

// Box filter for downscaling, bilinear (triangle) for upscaling, along one axis.
Weights compute_weights(int srcLen, int dstLen) {
    Weights ws;
    ws.start.resize(static_cast<size_t>(dstLen));
    ws.count.resize(static_cast<size_t>(dstLen));
    double scale = static_cast<double>(srcLen) / dstLen; // source pixels per destination pixel
    if (scale >= 1.0) {
        ws.maxCount = static_cast<int>(std::ceil(scale)) + 2;
        ws.w.resize(static_cast<size_t>(dstLen) * ws.maxCount, 0.f);
        for (int d = 0; d < dstLen; ++d) {
            double s0 = d * scale, s1 = (d + 1) * scale;
            int i0 = static_cast<int>(std::floor(s0));
            int i1 = std::min(static_cast<int>(std::ceil(s1)), srcLen);
            ws.start[d] = i0;
            int n = 0; float sum = 0.f;
            for (int i = i0; i < i1 && n < ws.maxCount; ++i, ++n) {
                double lo = std::max(s0, static_cast<double>(i)), hi = std::min(s1, static_cast<double>(i + 1));
                float wv = static_cast<float>(std::max(0.0, hi - lo));
                ws.w[static_cast<size_t>(d) * ws.maxCount + n] = wv; sum += wv;
            }
            ws.count[d] = n;
            if (sum > 0) for (int k = 0; k < n; ++k) ws.w[static_cast<size_t>(d) * ws.maxCount + k] /= sum;
        }
    } else {
        ws.maxCount = 2;
        ws.w.resize(static_cast<size_t>(dstLen) * 2, 0.f);
        for (int d = 0; d < dstLen; ++d) {
            double c = (d + 0.5) * scale - 0.5;
            int i0 = static_cast<int>(std::floor(c));
            float t = static_cast<float>(c - i0);
            if (i0 < 0) { i0 = 0; t = 0.f; }
            if (i0 >= srcLen - 1) { i0 = srcLen - 1; t = 0.f; }
            ws.start[d] = i0;
            ws.count[d] = (i0 + 1 < srcLen) ? 2 : 1;
            ws.w[static_cast<size_t>(d) * 2] = 1.f - t;
            ws.w[static_cast<size_t>(d) * 2 + 1] = t;
        }
    }
    return ws;
}
} // namespace

Image resample_gray(const Image& gray, int newW, int newH) {
    Image out;
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;
    if (gray.width == newW && gray.height == newH) return gray;
    Weights wx = compute_weights(gray.width, newW);
    Weights wy = compute_weights(gray.height, newH);
    // horizontal pass -> float rows
    std::vector<float> tmp(static_cast<size_t>(newW) * gray.height);
    for (int y = 0; y < gray.height; ++y) {
        const uint8_t* s = gray.row(y);
        float* t = tmp.data() + static_cast<size_t>(y) * newW;
        for (int x = 0; x < newW; ++x) {
            float acc = 0.f;
            const float* w = wx.w.data() + static_cast<size_t>(x) * wx.maxCount;
            int st = wx.start[x];
            for (int k = 0; k < wx.count[x]; ++k) acc += w[k] * s[st + k];
            t[x] = acc;
        }
    }
    out.allocate(newW, newH, 8);
    out.gray = true;
    out.dpiX = gray.dpiX * newW / gray.width;
    out.dpiY = gray.dpiY * newH / gray.height;
    for (int y = 0; y < newH; ++y) {
        uint8_t* d = out.row(y);
        const float* w = wy.w.data() + static_cast<size_t>(y) * wy.maxCount;
        int st = wy.start[y];
        for (int x = 0; x < newW; ++x) {
            float acc = 0.f;
            for (int k = 0; k < wy.count[y]; ++k) acc += w[k] * tmp[static_cast<size_t>(st + k) * newW + x];
            int v = static_cast<int>(acc + 0.5f);
            d[x] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    return out;
}

void swap_bgr_rgb(Image& img) {
    if (img.bpp != 24) return;
    for (int y = 0; y < img.height; ++y) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) std::swap(p[0], p[2]);
    }
}

} // namespace p2i
