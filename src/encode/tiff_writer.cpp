#include "encoders.h"

#include <tiffio.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace p2i {

namespace {
// libtiff reports errors through global handlers; capture the last message instead of printing to stderr.
thread_local std::string t_lastTiffError;

void tiff_error_handler(const char* module, const char* fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    t_lastTiffError = std::string(module ? module : "libtiff") + ": " + buf;
}
void tiff_warning_handler(const char*, const char*, va_list) {}

struct HandlerInstaller {
    HandlerInstaller() {
        TIFFSetErrorHandler(tiff_error_handler);
        TIFFSetWarningHandler(tiff_warning_handler);
    }
};
void ensure_handlers() { static HandlerInstaller once; }

uint16_t map_compression(Compression c, int bpp) {
    switch (c) {
    case Compression::None: return COMPRESSION_NONE;
    case Compression::Lzw: return COMPRESSION_LZW;
    case Compression::Jpeg: return COMPRESSION_JPEG;
    case Compression::G3: return COMPRESSION_CCITTFAX3;
    case Compression::G4: case Compression::ClassF: case Compression::ClassF196: return COMPRESSION_CCITTFAX4;
    case Compression::PackBits: case Compression::Default: default: (void)bpp; return COMPRESSION_PACKBITS;
    }
}
} // namespace

TiffWriter::~TiffWriter() {
    if (tif_) { TIFFClose(tif_); tif_ = nullptr; }
}

bool TiffWriter::open(const std::wstring& path, std::string& err) {
    ensure_handlers();
    t_lastTiffError.clear();
#ifdef _WIN32
    tif_ = TIFFOpenW(path.c_str(), "w");
#else
    tif_ = TIFFOpen(narrow(path).c_str(), "w");
#endif
    if (!tif_) {
        err = t_lastTiffError.empty() ? "cannot create TIFF file " + narrow(path) : t_lastTiffError;
        return false;
    }
    return true;
}

bool TiffWriter::add_page(const Image& img, const EncodeParams& p, int pageIndex, int pageCount, std::string& err) {
    if (!tif_) { err = "TIFF file not open"; return false; }
    t_lastTiffError.clear();
    TIFF* t = tif_;
    const int bps = (img.bpp == 24) ? 8 : img.bpp;
    const int spp = (img.bpp == 24) ? 3 : 1;
    uint16_t compression = map_compression(p.compression, img.bpp);
    // Sanity: CCITT needs 1-bit, JPEG needs 8-bit samples; the pipeline guarantees this, but be safe.
    if ((compression == COMPRESSION_CCITTFAX3 || compression == COMPRESSION_CCITTFAX4) && img.bpp != 1) compression = COMPRESSION_PACKBITS;
    if (compression == COMPRESSION_JPEG && bps != 8) compression = COMPRESSION_PACKBITS;

    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(img.width));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(img.height));
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, static_cast<uint16_t>(bps));
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(spp));
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(t, TIFFTAG_XRESOLUTION, static_cast<float>(img.dpiX));
    TIFFSetField(t, TIFFTAG_YRESOLUTION, static_cast<float>(img.dpiY));
    TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    TIFFSetField(t, TIFFTAG_COMPRESSION, compression);

    if (img.bpp == 1) {
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE); // our bits: 1 = black
    } else if (img.bpp == 24) {
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    } else if (img.bpp == 8 && img.gray) {
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    } else { // palette (4 or 8 bit)
        const int n = 1 << bps;
        std::vector<uint16_t> r(n, 0), g(n, 0), b(n, 0);
        for (int i = 0; i < n && i < img.paletteSize; ++i) {
            uint32_t c = img.palette ? img.palette[i] : 0;
            r[i] = static_cast<uint16_t>(((c >> 16) & 255) * 257);
            g[i] = static_cast<uint16_t>(((c >> 8) & 255) * 257);
            b[i] = static_cast<uint16_t>((c & 255) * 257);
        }
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
        TIFFSetField(t, TIFFTAG_COLORMAP, r.data(), g.data(), b.data());
    }

    if (compression == COMPRESSION_LZW && bps == 8 && p.lzwPredictor) TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    if (compression == COMPRESSION_JPEG) {
        TIFFSetField(t, TIFFTAG_JPEGQUALITY, p.jpegQuality);
        TIFFSetField(t, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RAW); // keep Photometric=RGB like the old tool
    }
    if (compression == COMPRESSION_CCITTFAX3) TIFFSetField(t, TIFFTAG_GROUP3OPTIONS, static_cast<uint32_t>(GROUP3OPT_FILLBITS));
    if (compression == COMPRESSION_CCITTFAX3 || compression == COMPRESSION_CCITTFAX4) {
        TIFFSetField(t, TIFFTAG_FILLORDER, p.faxFillOrder ? FILLORDER_LSB2MSB : FILLORDER_MSB2LSB);
    }

    // One strip per image like the old tool, unless the strip would be huge (> 256 MB): then use ~1 MB strips.
    const uint64_t rowBytes = static_cast<uint64_t>(img.stride);
    uint32_t rowsPerStrip = static_cast<uint32_t>(img.height);
    if (rowBytes * static_cast<uint64_t>(img.height) > (256ull << 20)) {
        rowsPerStrip = static_cast<uint32_t>((1ull << 20) / (rowBytes ? rowBytes : 1));
        if (rowsPerStrip < 16) rowsPerStrip = 16;
        rowsPerStrip = (rowsPerStrip + 15) & ~15u; // JPEG needs multiples of 16
    }
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, rowsPerStrip);

    if (pageCount > 1) {
        TIFFSetField(t, TIFFTAG_SUBFILETYPE, static_cast<uint32_t>(FILETYPE_PAGE));
        TIFFSetField(t, TIFFTAG_PAGENUMBER, static_cast<uint16_t>(pageIndex), static_cast<uint16_t>(pageCount));
    }
    TIFFSetField(t, TIFFTAG_SOFTWARE, "pdf2img");

    // Stream the strip through a 1 MB raw buffer instead of a strip-sized one (layout in the file is unchanged).
    TIFFWriteBufferSetup(t, nullptr, static_cast<tmsize_t>(1 << 20));
    // Write scanlines. libtiff wants RGB order; our 24-bit rows are BGR.
    std::vector<uint8_t> rowBuf;
    if (img.bpp == 24) rowBuf.resize(static_cast<size_t>(img.width) * 3);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* src = img.row(y);
        const void* line = src;
        if (img.bpp == 24) {
            uint8_t* d = rowBuf.data();
            for (int x = 0; x < img.width; ++x, src += 3, d += 3) { d[0] = src[2]; d[1] = src[1]; d[2] = src[0]; }
            line = rowBuf.data();
        }
        if (TIFFWriteScanline(t, const_cast<void*>(line), static_cast<uint32_t>(y), 0) < 0) {
            err = t_lastTiffError.empty() ? "TIFF write failed" : t_lastTiffError;
            return false;
        }
    }
    if (pageCount > 1 || pageIndex > 0) {
        if (!TIFFWriteDirectory(t)) {
            err = t_lastTiffError.empty() ? "TIFF directory write failed" : t_lastTiffError;
            return false;
        }
    }
    return true;
}

bool TiffWriter::close(std::string& err) {
    if (!tif_) return true;
    t_lastTiffError.clear();
    // TIFFClose flushes the last directory; it has no return value, so check the error slot afterwards.
    TIFFClose(tif_);
    tif_ = nullptr;
    if (!t_lastTiffError.empty()) { err = t_lastTiffError; return false; }
    return true;
}

} // namespace p2i
