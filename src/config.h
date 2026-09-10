// pdf2img.ini (next to the executable) + environment overrides.
// Old keys: [Options] AddFileNameSuffix=%04d, IsPreProcessPDFFile=0 (accepted, ignored).
#pragma once

#include <cstdint>
#include <string>

namespace p2i {

struct Config {
    std::wstring suffixFormat = L"%04d";     // AddFileNameSuffix (printf format applied to the page number)
    int defaultDpi = 101;                    // DefaultDPI (old tool default)
    int jpegQuality = 90;                    // JpegQuality
    int pageTimeoutSec = 120;                // PageTimeoutSec
    int totalTimeoutSec = 1800;              // TotalTimeoutSec
    int workers = 0;                         // Workers (0 = auto: CPU count, max 8)
    int workerMemoryLimitMB = 2048;          // WorkerMemoryLimitMB (Job Object limit per worker)
    uint64_t maxBitmapBytes = 1536ull << 20; // MaxBitmapMB (above this, fall back to default DPI for the page)
    bool lzwPredictor = true;                // LzwPredictor (horizontal differencing for LZW 8/24-bit)
    bool antialias = true;                   // Antialias (text+graphics)
    bool verbose = false;                    // Verbose / env PDF2IMG_VERBOSE
    std::wstring logFile;                    // LogFile / env PDF2IMG_LOG
    std::wstring iniPath;                    // where it was read from (empty if none)
};

// Loads <exe dir>\pdf2img.ini if present, then applies PDF2IMG_* environment variables.
Config load_config();

// Validates a suffix format: exactly one integer conversion (%d/%i/%u with optional flags/width),
// no other conversions. Returns "%04d" if invalid.
std::wstring sanitize_suffix_format(std::wstring fmt);
std::wstring format_page_suffix(const std::wstring& fmt, int page);

} // namespace p2i
