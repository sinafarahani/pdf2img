// In-memory raster image used between the renderer and the encoders.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace p2i {

struct Image {
    int width = 0;
    int height = 0;
    int bpp = 24;               // 1, 4, 8 or 24
    bool gray = false;          // bpp==8: true = grayscale samples, false = palette indices
    const uint32_t* palette = nullptr; // bpp 4/8 palette images: 0xRRGGBB entries
    int paletteSize = 0;
    size_t stride = 0;          // bytes per row (rows are packed, no padding unless stated)
    std::vector<uint8_t> data;
    // Pixel layouts:
    //   bpp 24: B,G,R per pixel (PDFium native order)
    //   bpp 8 : one byte per pixel (gray value or palette index)
    //   bpp 4 : two pixels per byte, high nibble first
    //   bpp 1 : eight pixels per byte, MSB first, 1 = black (ink), 0 = white
    double dpiX = 101.0;
    double dpiY = 101.0;

    static size_t row_bytes(int width, int bpp) {
        switch (bpp) {
        case 1: return (static_cast<size_t>(width) + 7) / 8;
        case 4: return (static_cast<size_t>(width) + 1) / 2;
        case 8: return static_cast<size_t>(width);
        default: return static_cast<size_t>(width) * 3;
        }
    }
    void allocate(int w, int h, int bitsPerPixel) {
        width = w; height = h; bpp = bitsPerPixel;
        stride = row_bytes(w, bitsPerPixel);
        data.assign(stride * static_cast<size_t>(h), 0);
    }
    const uint8_t* row(int y) const { return data.data() + stride * static_cast<size_t>(y); }
    uint8_t* row(int y) { return data.data() + stride * static_cast<size_t>(y); }
    bool empty() const { return width <= 0 || height <= 0 || data.empty(); }
};

} // namespace p2i
