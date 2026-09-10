// Command-line parsing compatible with the old VeryPDF pdf2img (getopt optstring "i:o:$mgr:f:l:c:q:b:d?")
// plus a few additional optional switches (see usage_extra_text()).
#pragma once

#include "common.h"

#include <string>

namespace p2i {

struct Options {
    // --- old signature ---
    std::wstring input;          // -i  (may contain wildcards)
    std::wstring output;         // -o  (optional; may contain wildcards when input does)
    bool multipage = false;      // -m
    bool grayscale = false;      // -g
    int dpiX = 0, dpiY = 0;      // -r  (0 = default 101)
    int firstPage = 0;           // -f  (0 = unset)
    int lastPage = 0;            // -l  (0 = unset)
    Compression compression = Compression::Default; // -c
    int quality = 0;             // -q  (0 = default 90)
    int bitCount = 0;            // -b  (0 = default 24)
    bool outputIsDir = false;    // -d  (hidden in old tool: -o is a directory)
    bool help = false;           // -?
    bool helpExtras = false;     // -h / -help / --help: also print the additional options
    // --- additions (all optional) ---
    std::string userPassword;    // -upw <pw>   (UTF-8)
    std::string ownerPassword;   // -opw <pw>
    std::vector<int> passwordArgIndices; // argv positions of the -upw/-opw values (masked in the log)
    int pageTimeoutSec = -1;     // -timeout <sec>       (-1 = from ini/default)
    int totalTimeoutSec = -1;    // -totaltimeout <sec>
    int workers = -1;            // -workers <n>         (0 = auto)
    bool verbose = false;        // -v
    std::wstring logFile;        // -log <file>
    bool showVersion = false;    // -version
    bool workerMode = false;     // --worker (internal: run as render worker)
};

enum class ParseStatus { Ok, Help, Error };

// Parses argv (argv[0] is the program). On Error, `message` holds a short reason (may be empty for
// "no arguments" which the old tool handled by printing usage).
ParseStatus parse_command_line(int argc, wchar_t** argv, Options& opts, std::wstring& message);

// The command line as it should appear in the log file: arguments re-quoted, the values of -upw/-opw replaced by ***.
// Password switches are matched case-insensitively, attached values (-upw=x, -upwx) are masked too, and when the
// command line could not be parsed (parsedOk=false) every argument after a password switch is masked.
// When parsing succeeded and `passwordArgs` (from Options::passwordArgIndices) is given, exactly those positions are masked.
std::string loggable_command_line(int argc, wchar_t** argv, bool parsedOk = true, const std::vector<int>* passwordArgs = nullptr);
// True if any argument looks like a password switch (-upw/-opw/--upw/--opw, any case, with or without a value).
bool has_password_switch(int argc, wchar_t** argv);

// Usage text (the switches of the old tool; printed for -?, no arguments and argument errors).
const char* usage_text();
// Short description of the additional switches (printed after the old usage text).
const char* usage_extra_text();

// Parses "-r" values: "300" or "200x300" (also 'X'). Returns false if invalid.
bool parse_resolution(std::wstring_view s, int& x, int& y);
// Parses "-c" values (case-insensitive). Returns false if unknown.
bool parse_compression(std::wstring_view s, Compression& c);

} // namespace p2i
