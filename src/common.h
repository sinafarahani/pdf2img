// Shared types and small utilities for pdf2img.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace p2i {

// Process exit codes. 0 and 1 match the old VeryPDF tool; the rest are new (old tool returned 0 for all failures).
enum ExitCode : int {
    EXIT_OK = 0,
    EXIT_USAGE = 1,              // bad/missing arguments (old tool: usage + exit 1)
    EXIT_INPUT = 2,              // input file not found / unreadable / not a PDF
    EXIT_RENDER = 3,             // one or more pages could not be rendered
    EXIT_OUTPUT = 4,             // output file/directory could not be written
    EXIT_TIMEOUT = 5,            // page or total time budget exceeded
    EXIT_UNSUPPORTED_FORMAT = 6, // unknown/missing output extension or unsupported format (wmf/emf)
    EXIT_PASSWORD = 7,           // PDF requires a password that was not given / was wrong
    EXIT_INTERNAL = 9            // unexpected internal failure (worker crash, IPC)
};

enum class OutFormat { Unknown, Tiff, Jpeg, Png, Bmp, Gif, Pcx, Tga, Wmf, Emf };

// -c values. Default (no -c) is PackBits for TIFF, like the old tool.
enum class Compression { Default, None, Lzw, Jpeg, PackBits, G3, G4, ClassF, ClassF196 };

// Separator used when composing paths.
#ifdef _WIN32
inline constexpr wchar_t kPathSep = L'\\';
#else
inline constexpr wchar_t kPathSep = L'/';
#endif

// The PDFium shared library that must sit next to the executable.
#if defined(_WIN32)
inline constexpr const char* kPdfiumLibrary = "pdfium.dll";
#elif defined(__APPLE__)
inline constexpr const char* kPdfiumLibrary = "libpdfium.dylib";
#else
inline constexpr const char* kPdfiumLibrary = "libpdfium.so";
#endif

// UTF-8 <-> wide strings (UTF-16 on Windows, UTF-32 elsewhere). On Linux/macOS, bytes that are not valid UTF-8 (file
// names are arbitrary bytes there) map to U+DC80..U+DCFF and back, so every path survives the round trip unchanged.
std::string narrow(std::wstring_view w);
std::wstring widen(std::string_view s);

std::string to_lower_ascii(std::string s);
std::wstring to_lower_ascii(std::wstring s);
bool iequals_ascii(std::wstring_view a, std::wstring_view b);

// Path helpers. Windows accepts '\' and '/' as separators; Linux/macOS only '/'.
bool is_path_sep(wchar_t c);
std::wstring path_dir(std::wstring_view p);      // "C:\a\b.tif" -> "C:\a"   ("b.tif" -> "")
std::wstring path_name(std::wstring_view p);     // "C:\a\b.tif" -> "b.tif"
std::wstring path_stem(std::wstring_view p);     // "C:\a\b.tif" -> "b"
std::wstring path_ext(std::wstring_view p);      // "C:\a\b.tif" -> ".tif"  ("" if none)
std::wstring path_join(std::wstring_view dir, std::wstring_view name);
bool same_path(std::wstring_view a, std::wstring_view b); // case-insensitive on Windows
bool has_wildcard(std::wstring_view p);
std::wstring exe_path();                         // full path of the running executable
std::wstring exe_directory();                    // directory of the running executable
std::wstring make_absolute(std::wstring_view p); // Windows: GetFullPathNameW; elsewhere: cwd + lexical normalisation
// Windows: returns p in extended-length form (\\?\ prefix) when it is an absolute path of 240+ characters; otherwise
// unchanged. Other systems: unchanged.
std::wstring extend_long_path(std::wstring p);

uint64_t tick_ms();                              // monotonic milliseconds
unsigned long current_pid();
bool get_env(const wchar_t* name, std::wstring& value); // false if unset or empty

OutFormat format_from_extension(std::wstring_view ext); // ".tif" -> Tiff (case-insensitive)
const char* format_name(OutFormat f);
const char* compression_name(Compression c);

// Pixel size from a PDF box size in points. Reproduces the old tool's rounding exactly
// (verified on 11 observed sizes): (int)(pt * (dpi / 72.0) + 0.5).
inline int points_to_pixels(double pt, int dpi) {
    double v = pt * (static_cast<double>(dpi) / 72.0) + 0.5;
    if (v < 1.0) return 1;
    return static_cast<int>(v);
}

} // namespace p2i
