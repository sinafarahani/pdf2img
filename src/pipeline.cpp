#include "pipeline.h"
#include "image/convert.h"
#include "image/palettes.h"

#include <cmath>
#include <utility>

namespace p2i {

ConversionSpec make_spec(OutFormat fmt, int dpiX, int dpiY, int bitCount, bool grayOpt, Compression comp, int quality,
                         int defaultDpi, int defaultQuality, bool lzwPredictor, bool antialias, uint64_t maxBitmapBytes) {
    ConversionSpec s;
    s.format = fmt;
    s.dpiX = dpiX > 0 ? dpiX : defaultDpi;
    s.dpiY = dpiY > 0 ? dpiY : defaultDpi;
    s.bitCount = bitCount > 0 ? bitCount : 24;
    s.gray = grayOpt && s.bitCount == 8;           // old tool: -g only with -b 8
    s.compression = comp;
    s.jpegQuality = quality > 0 ? quality : defaultQuality;
    s.lzwPredictor = lzwPredictor;
    s.antialias = antialias;
    s.maxBitmapBytes = maxBitmapBytes;
    s.fallbackDpi = defaultDpi;

    if (fmt == OutFormat::Tiff) {
        switch (comp) {
        case Compression::G3: case Compression::G4:
            s.bitCount = 1; s.gray = false; break;
        case Compression::ClassF:
            s.bitCount = 1; s.gray = false; s.classF = true; s.dpiX = 204; s.dpiY = 98; break;
        case Compression::ClassF196:
            s.bitCount = 1; s.gray = false; s.classF = true; s.dpiX = 204; s.dpiY = 196; break;
        case Compression::Jpeg:
            if (s.bitCount == 1 || s.bitCount == 4) { s.bitCount = 8; s.gray = true; }
            else if (s.bitCount == 8) s.gray = true; // palette cannot be JPEG-compressed
            break;
        default: break;
        }
    } else {
        s.compression = Compression::Default; // -c is "for tif only"
        switch (fmt) {
        case OutFormat::Jpeg:
            if (s.bitCount == 1) { s.bitCount = 8; s.gray = true; }
            else if (s.bitCount == 4 || (s.bitCount == 8 && !s.gray)) s.bitCount = 24; // old tool: colour JPEG
            break;
        case OutFormat::Gif:
            if (s.bitCount == 24) { s.bitCount = 8; s.gray = false; }
            break;
        case OutFormat::Tga:
            if (s.bitCount == 1) { s.bitCount = 8; s.gray = true; }
            break;
        default: break; // PNG, BMP, PCX handle 1/4/8/24 (PCX 4-bit is expanded to 8-bit palette below)
        }
    }
    return s;
}

RenderRequest make_render_request(const ConversionSpec& spec) {
    RenderRequest r;
    r.dpiX = spec.dpiX;
    r.dpiY = spec.dpiY;
    r.gray = (spec.bitCount == 1) || (spec.bitCount == 8 && spec.gray);
    r.antialias = spec.antialias;
    r.maxBitmapBytes = spec.maxBitmapBytes;
    r.fallbackDpi = spec.fallbackDpi;
    return r;
}

static Image expand_4bit_to_8bit(const Image& src) {
    Image out;
    out.allocate(src.width, src.height, 8);
    out.gray = false;
    out.palette = src.palette;
    out.paletteSize = src.paletteSize;
    out.dpiX = src.dpiX; out.dpiY = src.dpiY;
    for (int y = 0; y < src.height; ++y) {
        const uint8_t* s = src.row(y);
        uint8_t* d = out.row(y);
        for (int x = 0; x < src.width; ++x) d[x] = (x & 1) ? (s[x >> 1] & 15) : (s[x >> 1] >> 4);
    }
    return out;
}

Image finalize_image(Image&& rendered, const ConversionSpec& spec) {
    Image img = std::move(rendered);
    const double dpiX = img.dpiX, dpiY = img.dpiY;

    if (spec.classF) {
        // Fax Class F: 1728 px wide, aspect preserved, then threshold.
        Image g = (img.bpp == 8 && img.gray) ? std::move(img) : to_gray(img);
        const int newW = 1728;
        const int newH = static_cast<int>(static_cast<double>(g.height) * newW / g.width); // truncation, like the old tool (2156*1728/1734 -> 2148)
        Image rs = resample_gray(g, newW, newH);
        Image b = gray_to_bilevel(rs, 128);
        b.dpiX = spec.dpiX; b.dpiY = spec.dpiY; // old tool wrote 204x98 / 204x196
        return b;
    }

    Image out;
    switch (spec.bitCount) {
    case 1: {
        Image g = (img.bpp == 8 && img.gray) ? std::move(img) : to_gray(img);
        out = gray_to_bilevel(g, 128);
        break;
    }
    case 4:
        out = to_palette(img, kPaletteVga16, 16, 4);
        if (spec.format == OutFormat::Pcx || spec.format == OutFormat::Tga) out = expand_4bit_to_8bit(out);
        break;
    case 8:
        if (spec.gray) out = (img.bpp == 8 && img.gray) ? std::move(img) : to_gray(img);
        else out = to_palette(img, kPaletteHalftone256, 256, 8);
        break;
    default: // 24
        if (img.bpp == 24) out = std::move(img);
        else {
            // gray rendered but colour requested (should not happen): expand to BGR
            Image g = to_gray(img);
            out.allocate(g.width, g.height, 24);
            for (int y = 0; y < g.height; ++y) {
                const uint8_t* s = g.row(y); uint8_t* d = out.row(y);
                for (int x = 0; x < g.width; ++x, d += 3) d[0] = d[1] = d[2] = s[x];
            }
        }
        break;
    }
    out.dpiX = dpiX; out.dpiY = dpiY;
    return out;
}

EncodeParams make_encode_params(const ConversionSpec& spec) {
    EncodeParams p;
    p.compression = spec.compression;
    p.jpegQuality = spec.jpegQuality;
    p.lzwPredictor = spec.lzwPredictor;
    p.faxFillOrder = spec.classF || spec.compression == Compression::G3 || spec.compression == Compression::G4;
    return p;
}

} // namespace p2i
