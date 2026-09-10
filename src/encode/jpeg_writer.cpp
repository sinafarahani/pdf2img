#include "encoders.h"
#include "../fsutil.h"
#ifdef _MSC_VER
#pragma warning(disable : 4611) // setjmp/longjmp is the libjpeg/libpng error model; no C++ objects with destructors are skipped
#endif

#include <cstdio>
#include <csetjmp>
#include <vector>

extern "C" {
#include <jpeglib.h>
#include <jerror.h>
}

namespace p2i {

namespace {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // jmp_buf alignment padding is expected
#endif
struct ErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void on_error(j_common_ptr cinfo) {
    ErrorMgr* e = reinterpret_cast<ErrorMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, e->message);
    longjmp(e->jump, 1);
}
void on_message(j_common_ptr) {}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
} // namespace

bool write_jpeg(const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err) {
    if (!(img.bpp == 24 || (img.bpp == 8 && img.gray))) { err = "JPEG encoder: unsupported pixel layout"; return false; }
    FILE* f = open_file_for_writing(path);
    if (!f) { err = "cannot create file " + narrow(path); return false; }

    jpeg_compress_struct cinfo{};
    ErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = on_error;
    jerr.pub.output_message = on_message;
    bool ok = false;
    if (setjmp(jerr.jump)) {
        err = std::string("JPEG encoder: ") + jerr.message;
        jpeg_destroy_compress(&cinfo);
        fclose(f);
        return false;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = static_cast<JDIMENSION>(img.width);
    cinfo.image_height = static_cast<JDIMENSION>(img.height);
    // Always 3 components (BGR): the old tool wrote 3-component 4:2:0 JPEGs even for grayscale pixels.
    cinfo.input_components = 3; cinfo.in_color_space = JCS_EXT_BGR;
    jpeg_set_defaults(&cinfo);
    int q = p.jpegQuality <= 0 ? 90 : (p.jpegQuality > 100 ? 100 : p.jpegQuality);
    jpeg_set_quality(&cinfo, q, TRUE);
    cinfo.density_unit = 1; // dots per inch
    cinfo.X_density = static_cast<UINT16>(img.dpiX + 0.5);
    cinfo.Y_density = static_cast<UINT16>(img.dpiY + 0.5);
    cinfo.optimize_coding = FALSE;
    cinfo.dct_method = JDCT_ISLOW;
    jpeg_start_compress(&cinfo, TRUE);
    std::vector<uint8_t> grayRow;
    if (img.bpp == 8) grayRow.resize(static_cast<size_t>(img.width) * 3);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* src = img.row(static_cast<int>(cinfo.next_scanline));
        JSAMPROW row;
        if (img.bpp == 8) {
            uint8_t* d = grayRow.data();
            for (int x = 0; x < img.width; ++x, d += 3) d[0] = d[1] = d[2] = src[x];
            row = grayRow.data();
        } else {
            row = const_cast<JSAMPROW>(src);
        }
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) err = "write error on " + narrow(path);
    return ok;
}

} // namespace p2i
