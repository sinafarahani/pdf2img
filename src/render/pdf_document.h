// PDFium wrapper: open documents (with qpdf repair fallback), render pages to BGR24/Gray8 with a deadline.
#pragma once

#include "../common.h"
#include "../image/image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p2i {

// Process-wide PDFium initialisation (call once per process, before any PdfDocument).
void pdf_library_init();
void pdf_library_shutdown();

struct OpenError {
    int exitCode = EXIT_INPUT;     // EXIT_INPUT / EXIT_PASSWORD
    std::string message;
};

struct RenderRequest {
    int dpiX = 101;
    int dpiY = 101;
    bool gray = false;             // render directly to 8-bit gray
    bool antialias = true;
    uint64_t maxBitmapBytes = 1536ull << 20; // fall back to fallbackDpi when the page bitmap would exceed this
    int fallbackDpi = 101;
};

enum class RenderStatus { Ok, Timeout, Failed };

struct RenderResult {
    Image image;                   // bpp 24 (BGR) or 8 (gray)
    bool usedFallbackDpi = false;
    int dpiX = 0, dpiY = 0;        // effective DPI used
    double pageWidthPt = 0, pageHeightPt = 0;
};

// Read-only memory-mapped file (kept open while PDFium uses the buffer).
class FileMapping {
public:
    FileMapping() = default;
    ~FileMapping() { close(); }
    FileMapping(const FileMapping&) = delete;
    FileMapping& operator=(const FileMapping&) = delete;
    bool open(const std::wstring& path, std::string& err);
    void close();
    const void* data() const { return view_; }
    size_t size() const { return size_; }
private:
#ifdef _WIN32
    void* file_ = nullptr;
    void* mapping_ = nullptr;
#endif
    void* view_ = nullptr;
    size_t size_ = 0;
};

class PdfDocument {
public:
    PdfDocument() = default;
    ~PdfDocument();
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    // Opens `path` (UTF-16). Tries PDFium; on a format error runs a qpdf repair into a temp file and retries.
    // On success, repaired_path() is non-empty when a repaired copy is in use (caller owns/deletes it).
    bool open(const std::wstring& path, const std::string& password, OpenError& err);
    void close();

    bool is_open() const { return doc_ != nullptr; }
    int page_count() const { return pageCount_; }
    bool xref_was_rebuilt() const { return xrefRebuilt_; }
    bool was_repaired() const { return !repairedPath_.empty(); }
    const std::wstring& repaired_path() const { return repairedPath_; }

    // Renders 0-based page `index`. `deadlineTicks` = tick_ms() value after which rendering is aborted (0 = none).
    RenderStatus render_page(int index, const RenderRequest& req, uint64_t deadlineTicks, RenderResult& out, std::string& err);

private:
    bool load_from_memory(const std::string& password, unsigned long& pdfiumError);
    void* doc_ = nullptr;      // FPDF_DOCUMENT
    void* form_ = nullptr;     // FPDF_FORMHANDLE
    void* formInfo_ = nullptr; // FPDF_FORMFILLINFO*
    FileMapping map_;
    std::wstring repairedPath_;
    int pageCount_ = 0;
    bool xrefRebuilt_ = false;
};

// qpdf-based repair: rewrites `in` into `outTemp` (recovering xref, decrypting with `password`).
bool qpdf_repair(const std::wstring& in, const std::string& password, const std::wstring& outTemp, std::string& err, bool* passwordError = nullptr);

// Creates a unique temp file path (not created) in the temp directory: pdf2img_<pid>_<unique><ext>.
std::wstring make_temp_path(const wchar_t* ext);

} // namespace p2i
