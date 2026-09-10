// Maps the user's options onto an effective render/encode specification and converts a rendered page
// into the layout a given output format accepts, replicating the old tool's semantics.
#pragma once

#include "common.h"
#include "image/image.h"
#include "encode/encoders.h"
#include "render/pdf_document.h"

#include <string>

namespace p2i {

struct ConversionSpec {
    OutFormat format = OutFormat::Tiff;
    int dpiX = 101, dpiY = 101;       // effective DPI (ClassF overrides)
    int bitCount = 24;                // effective 1/4/8/24
    bool gray = false;                // 8-bit grayscale requested (-g with -b 8)
    Compression compression = Compression::Default;
    int jpegQuality = 90;
    bool lzwPredictor = true;
    bool antialias = true;
    bool classF = false;              // ClassF/ClassF196: fax width 1728 px
    uint64_t maxBitmapBytes = 1536ull << 20;
    int fallbackDpi = 101;
};

// Derives the effective spec from CLI values (0 = unset) and config defaults.
ConversionSpec make_spec(OutFormat fmt, int dpiX, int dpiY, int bitCount, bool grayOpt, Compression comp, int quality,
                         int defaultDpi, int defaultQuality, bool lzwPredictor, bool antialias, uint64_t maxBitmapBytes);

// What the renderer must produce for this spec (gray vs colour).
RenderRequest make_render_request(const ConversionSpec& spec);

// Converts the rendered page into the final layout for spec.format / spec.bitCount (threshold, palettes,
// ClassF resampling). `rendered` is consumed (moved from) when no conversion is needed.
Image finalize_image(Image&& rendered, const ConversionSpec& spec);

EncodeParams make_encode_params(const ConversionSpec& spec);

} // namespace p2i
