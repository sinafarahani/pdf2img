#include "encoders.h"

#include <gif_lib.h>

#include <vector>

namespace p2i {

namespace {
int gif_write_cb(GifFileType* gif, const GifByteType* data, int n) {
    BinaryFile* f = static_cast<BinaryFile*>(gif->UserData);
    return f->write(data, static_cast<size_t>(n)) ? n : 0;
}
} // namespace

bool write_gif(const std::wstring& path, const Image& img, const EncodeParams&, std::string& err) {
    if (!(img.bpp == 1 || img.bpp == 4 || img.bpp == 8)) { err = "GIF encoder: unsupported pixel layout"; return false; }
    BinaryFile f;
    if (!f.open(path, err)) return false;

    // Palette: 2, 16 or 256 entries.
    const int count = 1 << img.bpp;
    std::vector<GifColorType> colors(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        uint32_t c;
        if (img.bpp == 1) c = (i == 0) ? 0xFFFFFF : 0x000000; // our bits: 1 = black
        else if (img.bpp == 8 && img.gray) c = (static_cast<uint32_t>(i) << 16) | (static_cast<uint32_t>(i) << 8) | static_cast<uint32_t>(i);
        else c = (img.palette && i < img.paletteSize) ? img.palette[i] : 0;
        colors[static_cast<size_t>(i)].Red = static_cast<GifByteType>((c >> 16) & 255);
        colors[static_cast<size_t>(i)].Green = static_cast<GifByteType>((c >> 8) & 255);
        colors[static_cast<size_t>(i)].Blue = static_cast<GifByteType>(c & 255);
    }
    int gerr = 0;
    GifFileType* gif = EGifOpen(&f, gif_write_cb, &gerr);
    if (!gif) { err = std::string("GIF encoder: ") + (GifErrorString(gerr) ? GifErrorString(gerr) : "open failed"); return false; }
    ColorMapObject* cmap = GifMakeMapObject(count, colors.data());
    if (!cmap) { EGifCloseFile(gif, &gerr); err = "GIF encoder: cannot create colour map"; return false; }
    EGifSetGifVersion(gif, false); // GIF87a (no extensions needed)
    bool ok = EGifPutScreenDesc(gif, img.width, img.height, 8, 0, cmap) == GIF_OK &&
              EGifPutImageDesc(gif, 0, 0, img.width, img.height, false, nullptr) == GIF_OK;
    std::vector<GifPixelType> line(static_cast<size_t>(img.width));
    for (int y = 0; ok && y < img.height; ++y) {
        const uint8_t* s = img.row(y);
        if (img.bpp == 8) {
            for (int x = 0; x < img.width; ++x) line[static_cast<size_t>(x)] = s[x];
        } else if (img.bpp == 4) {
            for (int x = 0; x < img.width; ++x) line[static_cast<size_t>(x)] = (x & 1) ? (s[x >> 1] & 15) : (s[x >> 1] >> 4);
        } else {
            for (int x = 0; x < img.width; ++x) line[static_cast<size_t>(x)] = (s[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
        }
        ok = EGifPutLine(gif, line.data(), img.width) == GIF_OK;
    }
    if (!ok) err = std::string("GIF encoder: ") + (GifErrorString(gif->Error) ? GifErrorString(gif->Error) : "write failed");
    if (EGifCloseFile(gif, &gerr) != GIF_OK && ok) { ok = false; err = "GIF encoder: close failed"; }
    GifFreeMapObject(cmap);
    std::string cerr;
    if (!f.close(cerr) && ok) { ok = false; err = cerr; }
    return ok;
}

} // namespace p2i
