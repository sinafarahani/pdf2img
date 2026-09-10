#include "encoders.h"
#include "../fsutil.h"
#ifdef _MSC_VER
#pragma warning(disable : 4611) // setjmp/longjmp is the libjpeg/libpng error model; no C++ objects with destructors are skipped
#endif

#include <cstdio>
#include <csetjmp>
#include <vector>

#include <png.h>

namespace p2i {

namespace {
void png_error_fn(png_structp png, png_const_charp msg) {
    std::string* e = static_cast<std::string*>(png_get_error_ptr(png));
    if (e) *e = msg ? msg : "PNG error";
    longjmp(png_jmpbuf(png), 1);
}
void png_warn_fn(png_structp, png_const_charp) {}
} // namespace

bool write_png(const std::wstring& path, const Image& img, const EncodeParams&, std::string& err) {
    if (!(img.bpp == 1 || img.bpp == 4 || img.bpp == 8 || img.bpp == 24)) { err = "PNG encoder: unsupported pixel layout"; return false; }
    FILE* f = open_file_for_writing(path);
    if (!f) { err = "cannot create file " + narrow(path); return false; }

    std::string perr;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &perr, png_error_fn, png_warn_fn);
    if (!png) { fclose(f); err = "PNG encoder: png_create_write_struct failed"; return false; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); fclose(f); err = "PNG encoder: png_create_info_struct failed"; return false; }

    std::vector<png_bytep> rows;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        err = "PNG encoder: " + perr;
        return false;
    }
    png_init_io(png, f);
    png_set_compression_level(png, 6);

    int colorType = PNG_COLOR_TYPE_RGB, bitDepth = 8;
    png_color pal[256];
    int palCount = 0;
    if (img.bpp == 24) {
        colorType = PNG_COLOR_TYPE_RGB; bitDepth = 8;
    } else if (img.bpp == 8 && img.gray) {
        colorType = PNG_COLOR_TYPE_GRAY; bitDepth = 8;
    } else if (img.bpp == 1) {
        // our bits: 1 = black -> palette index 1 = black, index 0 = white
        colorType = PNG_COLOR_TYPE_PALETTE; bitDepth = 1;
        pal[0] = {255, 255, 255}; pal[1] = {0, 0, 0}; palCount = 2;
    } else {
        colorType = PNG_COLOR_TYPE_PALETTE; bitDepth = img.bpp; // 4 or 8
        palCount = 1 << img.bpp;
        for (int i = 0; i < palCount; ++i) {
            uint32_t c = (img.palette && i < img.paletteSize) ? img.palette[i] : 0;
            pal[i].red = static_cast<png_byte>((c >> 16) & 255);
            pal[i].green = static_cast<png_byte>((c >> 8) & 255);
            pal[i].blue = static_cast<png_byte>(c & 255);
        }
    }
    png_set_IHDR(png, info, static_cast<png_uint_32>(img.width), static_cast<png_uint_32>(img.height), bitDepth, colorType,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (palCount) png_set_PLTE(png, info, pal, palCount);
    png_set_pHYs(png, info, static_cast<png_uint_32>(img.dpiX / 0.0254 + 0.5), static_cast<png_uint_32>(img.dpiY / 0.0254 + 0.5), PNG_RESOLUTION_METER);
    png_write_info(png, info);
    if (img.bpp == 24) png_set_bgr(png); // our rows are B,G,R
    rows.resize(static_cast<size_t>(img.height));
    for (int y = 0; y < img.height; ++y) rows[static_cast<size_t>(y)] = const_cast<png_bytep>(img.row(y));
    png_write_image(png, rows.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    bool ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) err = "write error on " + narrow(path);
    return ok;
}

} // namespace p2i
