// BMP, PCX, TGA writers (hand-written; these formats are trivial), BinaryFile helper, dispatcher.
#include "encoders.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>

namespace p2i {

// ---------------------------------------------------------------- BinaryFile
BinaryFile::~BinaryFile() {
    if (opened()) { std::string e; close(e); }
}

#ifdef _WIN32

bool BinaryFile::opened() const { return h_ != nullptr; }

bool BinaryFile::open(const std::wstring& path, std::string& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create file " + narrow(path); return false; }
    h_ = h;
    buf_.reserve(1 << 16);
    failed_ = false;
    return true;
}

bool BinaryFile::write_raw(const uint8_t* p, size_t n) {
    DWORD written = 0;
    size_t off = 0;
    while (off < n) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(n - off, 1u << 30));
        if (!WriteFile(static_cast<HANDLE>(h_), p + off, chunk, &written, nullptr) || written == 0) return false;
        off += written;
    }
    return true;
}

#else

bool BinaryFile::opened() const { return fd_ >= 0; }

bool BinaryFile::open(const std::wstring& path, std::string& err) {
    const int fd = ::open(narrow(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) { err = "cannot create file " + narrow(path); return false; }
    fd_ = fd;
    buf_.reserve(1 << 16);
    failed_ = false;
    return true;
}

bool BinaryFile::write_raw(const uint8_t* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd_, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

#endif

bool BinaryFile::flush() {
    if (buf_.empty()) return true;
    if (!write_raw(buf_.data(), buf_.size())) { failed_ = true; buf_.clear(); return false; }
    buf_.clear();
    return true;
}

bool BinaryFile::write(const void* data, size_t n) {
    if (failed_ || !opened()) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (n >= (1u << 16)) { if (!flush()) return false; buf_.assign(p, p + n); return flush(); }
    if (buf_.size() + n > (1u << 16)) { if (!flush()) return false; }
    buf_.insert(buf_.end(), p, p + n);
    return true;
}

bool BinaryFile::write_u16le(uint16_t v) { uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)}; return write(b, 2); }
bool BinaryFile::write_u32le(uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    return write(b, 4);
}

bool BinaryFile::close(std::string& err) {
    if (!opened()) return !failed_;
    bool ok = flush();
#ifdef _WIN32
    if (!CloseHandle(static_cast<HANDLE>(h_))) ok = false;
    h_ = nullptr;
#else
    if (::close(fd_) != 0) ok = false;
    fd_ = -1;
#endif
    if (!ok) err = "write error";
    return ok;
}

// ---------------------------------------------------------------- BMP
bool write_bmp(const std::wstring& path, const Image& img, const EncodeParams&, std::string& err) {
    if (!(img.bpp == 1 || img.bpp == 4 || img.bpp == 8 || img.bpp == 24)) { err = "BMP encoder: unsupported pixel layout"; return false; }
    BinaryFile f;
    if (!f.open(path, err)) return false;
    const int bpp = img.bpp;
    const int palCount = bpp == 24 ? 0 : (1 << bpp);
    const uint32_t rowBytes = (static_cast<uint32_t>(img.width) * bpp + 31) / 32 * 4;
    const uint64_t pixelBytes64 = static_cast<uint64_t>(rowBytes) * static_cast<uint64_t>(img.height);
    const uint32_t headerBytes = 14 + 40 + static_cast<uint32_t>(palCount) * 4;
    if (pixelBytes64 + headerBytes > 0xFFFFFFFFull) { std::string e; f.close(e); err = "BMP encoder: image too large for the BMP format (" + std::to_string(pixelBytes64 >> 20) + " MB)"; return false; }
    const uint32_t pixelBytes = static_cast<uint32_t>(pixelBytes64);
    // BITMAPFILEHEADER
    f.write("BM", 2);
    f.write_u32le(headerBytes + pixelBytes);
    f.write_u32le(0);
    f.write_u32le(headerBytes);
    // BITMAPINFOHEADER
    f.write_u32le(40);
    f.write_i32le(img.width);
    f.write_i32le(img.height); // bottom-up
    f.write_u16le(1);
    f.write_u16le(static_cast<uint16_t>(bpp));
    f.write_u32le(0); // BI_RGB
    f.write_u32le(pixelBytes);
    f.write_i32le(static_cast<int32_t>(img.dpiX / 0.0254 + 0.5));
    f.write_i32le(static_cast<int32_t>(img.dpiY / 0.0254 + 0.5));
    f.write_u32le(static_cast<uint32_t>(palCount));
    f.write_u32le(0);
    // palette (B,G,R,0)
    for (int i = 0; i < palCount; ++i) {
        uint32_t c;
        if (bpp == 1) c = (i == 0) ? 0xFFFFFF : 0x000000;                  // our bits: 1 = black
        else if (bpp == 8 && img.gray) c = (static_cast<uint32_t>(i) << 16) | (static_cast<uint32_t>(i) << 8) | static_cast<uint32_t>(i);
        else c = (img.palette && i < img.paletteSize) ? img.palette[i] : 0;
        uint8_t e[4] = {static_cast<uint8_t>(c & 255), static_cast<uint8_t>((c >> 8) & 255), static_cast<uint8_t>((c >> 16) & 255), 0};
        f.write(e, 4);
    }
    std::vector<uint8_t> row(rowBytes, 0);
    for (int y = img.height - 1; y >= 0; --y) {
        std::memcpy(row.data(), img.row(y), std::min<size_t>(img.stride, rowBytes));
        if (!f.write(row.data(), rowBytes)) break;
    }
    return f.close(err);
}

// ---------------------------------------------------------------- PCX
namespace {
void pcx_rle_line(BinaryFile& f, const uint8_t* line, size_t n, std::vector<uint8_t>& enc) {
    enc.clear();
    size_t i = 0;
    while (i < n) {
        uint8_t v = line[i];
        size_t run = 1;
        while (i + run < n && run < 63 && line[i + run] == v) ++run;
        if (run > 1 || v >= 0xC0) enc.push_back(static_cast<uint8_t>(0xC0 | run));
        enc.push_back(v);
        i += run;
    }
    f.write(enc.data(), enc.size());
}
} // namespace

bool write_pcx(const std::wstring& path, const Image& img, const EncodeParams&, std::string& err) {
    if (!(img.bpp == 1 || img.bpp == 8 || img.bpp == 24)) { err = "PCX encoder: unsupported pixel layout"; return false; }
    BinaryFile f;
    if (!f.open(path, err)) return false;
    const int planes = img.bpp == 24 ? 3 : 1;
    const int bitsPerPlane = img.bpp == 1 ? 1 : 8;
    uint32_t bytesPerLine = static_cast<uint32_t>((img.width * bitsPerPlane + 7) / 8);
    if (bytesPerLine & 1) ++bytesPerLine; // must be even
    uint8_t hdr[128] = {0};
    hdr[0] = 10; hdr[1] = 5; hdr[2] = 1; hdr[3] = static_cast<uint8_t>(bitsPerPlane);
    auto put16 = [&](int off, uint32_t v) { hdr[off] = static_cast<uint8_t>(v); hdr[off + 1] = static_cast<uint8_t>(v >> 8); };
    put16(4, 0); put16(6, 0); put16(8, static_cast<uint32_t>(img.width - 1)); put16(10, static_cast<uint32_t>(img.height - 1));
    put16(12, static_cast<uint32_t>(img.dpiX + 0.5)); put16(14, static_cast<uint32_t>(img.dpiY + 0.5));
    // 16-colour header palette: for mono, 0 = black, 1 = white (PCX convention: bit 1 = white)
    if (img.bpp == 1) { hdr[16] = 0; hdr[17] = 0; hdr[18] = 0; hdr[19] = 255; hdr[20] = 255; hdr[21] = 255; }
    hdr[65] = static_cast<uint8_t>(planes);
    put16(66, bytesPerLine);
    put16(68, 1); // palette info: colour
    f.write(hdr, sizeof hdr);

    std::vector<uint8_t> line(bytesPerLine, 0), enc;
    enc.reserve(bytesPerLine * 2);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* s = img.row(y);
        if (img.bpp == 24) {
            for (int c = 2; c >= 0; --c) { // planes R, G, B; our pixels are B,G,R
                std::fill(line.begin(), line.end(), static_cast<uint8_t>(0));
                for (int x = 0; x < img.width; ++x) line[static_cast<size_t>(x)] = s[x * 3 + c];
                pcx_rle_line(f, line.data(), bytesPerLine, enc);
            }
        } else if (img.bpp == 8) {
            std::fill(line.begin(), line.end(), static_cast<uint8_t>(0));
            std::memcpy(line.data(), s, static_cast<size_t>(img.width));
            pcx_rle_line(f, line.data(), bytesPerLine, enc);
        } else { // 1-bit: invert so that 1 = white (PCX convention)
            std::fill(line.begin(), line.end(), static_cast<uint8_t>(0));
            size_t n = (static_cast<size_t>(img.width) + 7) / 8;
            for (size_t i = 0; i < n; ++i) line[i] = static_cast<uint8_t>(~s[i]);
            // mask padding bits of the last byte to white(1)
            pcx_rle_line(f, line.data(), bytesPerLine, enc);
        }
        if (f.failed()) break;
    }
    if (img.bpp == 8) {
        f.write_u8(0x0C);
        for (int i = 0; i < 256; ++i) {
            uint32_t c = img.gray ? ((static_cast<uint32_t>(i) << 16) | (static_cast<uint32_t>(i) << 8) | static_cast<uint32_t>(i))
                                  : ((img.palette && i < img.paletteSize) ? img.palette[i] : 0);
            uint8_t e[3] = {static_cast<uint8_t>((c >> 16) & 255), static_cast<uint8_t>((c >> 8) & 255), static_cast<uint8_t>(c & 255)};
            f.write(e, 3);
        }
    }
    return f.close(err);
}

// ---------------------------------------------------------------- TGA
bool write_tga(const std::wstring& path, const Image& img, const EncodeParams&, std::string& err) {
    if (!(img.bpp == 8 || img.bpp == 24)) { err = "TGA encoder: unsupported pixel layout"; return false; }
    BinaryFile f;
    if (!f.open(path, err)) return false;
    const bool mapped = img.bpp == 8 && !img.gray;
    uint8_t hdr[18] = {0};
    hdr[1] = mapped ? 1 : 0;                         // colour map type
    hdr[2] = static_cast<uint8_t>(img.bpp == 24 ? 2 : (mapped ? 1 : 3)); // 2 = truecolor, 1 = mapped, 3 = gray
    if (mapped) { hdr[3] = 0; hdr[4] = 0; hdr[5] = 0; hdr[6] = 1; hdr[7] = 24; } // 256 entries, 24-bit
    hdr[12] = static_cast<uint8_t>(img.width); hdr[13] = static_cast<uint8_t>(img.width >> 8);
    hdr[14] = static_cast<uint8_t>(img.height); hdr[15] = static_cast<uint8_t>(img.height >> 8);
    hdr[16] = static_cast<uint8_t>(img.bpp);
    hdr[17] = 0x20; // top-left origin
    f.write(hdr, sizeof hdr);
    if (mapped) {
        for (int i = 0; i < 256; ++i) {
            uint32_t c = (img.palette && i < img.paletteSize) ? img.palette[i] : 0;
            uint8_t e[3] = {static_cast<uint8_t>(c & 255), static_cast<uint8_t>((c >> 8) & 255), static_cast<uint8_t>((c >> 16) & 255)}; // B,G,R
            f.write(e, 3);
        }
    }
    for (int y = 0; y < img.height; ++y) {
        if (!f.write(img.row(y), Image::row_bytes(img.width, img.bpp))) break;
    }
    return f.close(err);
}

// ---------------------------------------------------------------- dispatcher
bool write_image(OutFormat fmt, const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err) {
    switch (fmt) {
    case OutFormat::Tiff: {
        TiffWriter w;
        if (!w.open(path, err)) return false;
        if (!w.add_page(img, p, 0, 1, err)) { std::string e2; w.close(e2); return false; }
        return w.close(err);
    }
    case OutFormat::Jpeg: return write_jpeg(path, img, p, err);
    case OutFormat::Png: return write_png(path, img, p, err);
    case OutFormat::Bmp: return write_bmp(path, img, p, err);
    case OutFormat::Gif: return write_gif(path, img, p, err);
    case OutFormat::Pcx: return write_pcx(path, img, p, err);
    case OutFormat::Tga: return write_tga(path, img, p, err);
    default: err = std::string("unsupported output format: ") + format_name(fmt); return false;
    }
}

} // namespace p2i
