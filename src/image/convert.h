// Pixel-format conversions replicating the old tool (threshold, fixed palettes, fax resampling).
#pragma once

#include "image.h"

namespace p2i {

// BGR24 -> 8-bit gray (BT.601 luma, rounded).
Image to_gray(const Image& bgr);

// 8-bit gray -> 1-bit bilevel by threshold (gray < threshold -> black). No dithering (old behaviour).
Image gray_to_bilevel(const Image& gray, int threshold = 128);

// Any of BGR24 / gray8 -> palette image (bpp 4 or 8) with nearest colour in `palette` (no dithering).
Image to_palette(const Image& src, const uint32_t* palette, int paletteSize, int bpp);

// 8-bit gray -> 8-bit palette image with a 256-entry gray ramp (for formats without a native gray mode).
extern const uint32_t kGrayRamp256[256];

// Resample a gray8 image to newW x newH (area-averaging when shrinking, bilinear when enlarging).
Image resample_gray(const Image& gray, int newW, int newH);

// BGR24 -> RGB24 in place (swap B and R).
void swap_bgr_rgb(Image& img);

} // namespace p2i
