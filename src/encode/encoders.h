// Image encoders. Each encoder documents the Image layouts it accepts; the pipeline (worker) converts
// the rendered page into an accepted layout first (see pipeline.h).
#pragma once

#include "../common.h"
#include "../image/image.h"

#include <string>

struct tiff; // libtiff handle (TIFF)

namespace p2i {

struct EncodeParams {
    Compression compression = Compression::Default; // TIFF only
    int jpegQuality = 90;                           // JPEG and TIFF/JPEG
    bool lzwPredictor = true;                       // TIFF/LZW horizontal differencing for 8/24-bit
    bool faxFillOrder = false;                      // TIFF: FillOrder=2 (LSB first) for fax-style files
};

// TIFF (single or multi-page). Accepts bpp 1 (1=black -> MinIsWhite), 4/8 palette, 8 gray, 24 BGR.
class TiffWriter {
public:
    TiffWriter() = default;
    ~TiffWriter();
    TiffWriter(const TiffWriter&) = delete;
    TiffWriter& operator=(const TiffWriter&) = delete;

    bool open(const std::wstring& path, std::string& err);
    // pageIndex is 0-based; pageCount is the number of pages the file will contain (1 for single-page files).
    bool add_page(const Image& img, const EncodeParams& p, int pageIndex, int pageCount, std::string& err);
    bool close(std::string& err);
    bool is_open() const { return tif_ != nullptr; }

private:
    tiff* tif_ = nullptr;
};

// JPEG: accepts 24 BGR and 8 gray.
bool write_jpeg(const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err);
// PNG: accepts 1, 4/8 palette, 8 gray, 24 BGR.
bool write_png(const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err);
// BMP: accepts 1, 4/8 palette, 8 gray, 24 BGR.
bool write_bmp(const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err);
// GIF: accepts 1, 4/8 palette, 8 gray.
bool write_gif(const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err);
// PCX: accepts 1, 8 palette, 8 gray, 24 BGR.
bool write_pcx(const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err);
// TGA: accepts 8 palette, 8 gray, 24 BGR.
bool write_tga(const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err);

// Single-image convenience dispatcher (TIFF included, as a one-page file).
bool write_image(OutFormat fmt, const std::wstring& path, const Image& img, const EncodeParams& p, std::string& err);

// Small helper shared by the hand-written encoders: a buffered binary file writer (wide path).
class BinaryFile {
public:
    BinaryFile() = default;
    ~BinaryFile();
    BinaryFile(const BinaryFile&) = delete;
    BinaryFile& operator=(const BinaryFile&) = delete;
    bool open(const std::wstring& path, std::string& err);
    bool write(const void* data, size_t n);
    bool write_u8(uint8_t v) { return write(&v, 1); }
    bool write_u16le(uint16_t v);
    bool write_u32le(uint32_t v);
    bool write_i32le(int32_t v) { return write_u32le(static_cast<uint32_t>(v)); }
    bool close(std::string& err);
    bool failed() const { return failed_; }
private:
#ifdef _WIN32
    void* h_ = nullptr; // HANDLE
#else
    int fd_ = -1;
#endif
    std::vector<uint8_t> buf_;
    bool failed_ = false;
    bool opened() const;
    bool write_raw(const uint8_t* p, size_t n);
    bool flush();
};

} // namespace p2i
