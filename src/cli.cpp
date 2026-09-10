#include "cli.h"
#include "version.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace p2i {

#ifdef _WIN32
#define P2I_IN "C:\\in\\"
#define P2I_OUT "C:\\out\\"
#else
#define P2I_IN "/in/"
#define P2I_OUT "/out/"
#endif

// Usage text. The switches are those of the VeryPDF PDF2Image v2.1 command line, which this program replaces.
static const char kUsage[] =
"pdf2img " PDF2IMG_VERSION " - convert PDF pages to TIFF, JPEG, PNG, BMP, GIF, PCX or TGA images\n"
"Command-line compatible with VeryPDF PDF To Image Converter 2.1 (not affiliated with VeryPDF).\n"
"Usage: pdf2img [options] -i <PDF file> [-o <output file>]\n"
"-i <file>        : Input PDF file; a wildcard converts several files (-i \"" P2I_IN "*.pdf\")\n"
"-o <file>        : Output file; its extension selects the format (default: <input>.tif)\n"
"-g               : 8-bit grayscale (only together with -b 8)\n"
"-m               : All pages in one multi-page TIFF (default: one file per page)\n"
"-r <dpi>         : Resolution, e.g. -r 300 or -r 200x300 (default 101)\n"
"-f <page>        : First page to convert\n"
"-l <page>        : Last page to convert\n"
"-c <method>      : TIFF compression: none, lzw, jpeg, packbits (default), g3, g4,\n"
"                   ClassF (fax 204x98), ClassF196 (fax 204x196)\n"
"-q <quality>     : JPEG quality 1-100 (default 90)\n"
"-b <bits>        : Bits per pixel: 1, 4, 8 or 24 (default 24)\n"
"-?               : This help (-h also lists the additional options)\n"
"Examples:\n"
"    pdf2img -i " P2I_IN "input.pdf -o " P2I_OUT "page.tif\n"
"    pdf2img -c lzw -r 300 -i " P2I_IN "input.pdf -o " P2I_OUT "page.tif\n"
"    pdf2img -m -i " P2I_IN "input.pdf -o " P2I_OUT "all-pages.tif\n"
"    pdf2img -q 80 -f 1 -l 9 -i " P2I_IN "input.pdf -o " P2I_OUT "page.jpg\n"
"    pdf2img -b 1 -c ClassF -m -i " P2I_IN "input.pdf -o " P2I_OUT "fax.tif\n"
"    pdf2img -i \"" P2I_IN "*.pdf\" -o \"" P2I_OUT "*.png\"\n";

#undef P2I_IN
#undef P2I_OUT

static const char kUsageExtra[] =
"Additional options (this build):\n"
"-upw [password]      : User password for encrypted PDF files\n"
"-opw [password]      : Owner password for encrypted PDF files\n"
"-timeout [seconds]   : Time budget per page (default 120)\n"
"-totaltimeout [sec]  : Time budget for the whole run (default 1800)\n"
"-workers [n]         : Number of parallel render processes (default: CPU count, max 8)\n"
"-v                   : Also report every page and step (stderr and log file)\n"
"-log [file]          : Append errors and warnings to a log file (everything with -v)\n"
"-version             : Print version and exit\n"
"Exit codes: 0 ok, 1 usage, 2 input, 3 render, 4 output, 5 timeout, 6 format, 7 password, 9 internal\n";

const char* usage_text() { return kUsage; }
const char* usage_extra_text() { return kUsageExtra; }

static bool password_switch_prefix(std::wstring_view a, size_t& len) {
    const std::wstring l = to_lower_ascii(std::wstring(a));
    for (const wchar_t* s : {L"--upw", L"--opw", L"-upw", L"-opw"}) {
        const size_t n = wcslen(s);
        if (l.compare(0, n, s) == 0) { len = n; return true; }
    }
    return false;
}

// Position of a password switch that follows whitespace inside one argument (e.g. produced by a broken quote such as
// -o "C:\dir\" -upw "x", where \" is an escaped quote), or npos.
static size_t embedded_password_switch(std::wstring_view a) {
    for (size_t p = 0; p + 1 < a.size(); ++p) {
        if (a[p] != L' ' && a[p] != L'\t') continue;
        size_t len = 0;
        if (password_switch_prefix(a.substr(p + 1), len)) return p + 1;
    }
    return std::wstring_view::npos;
}

bool has_password_switch(int argc, wchar_t** argv) {
    size_t len = 0;
    for (int i = 1; i < argc; ++i)
        if (argv[i] && (password_switch_prefix(argv[i], len) || embedded_password_switch(argv[i]) != std::wstring_view::npos)) return true;
    return false;
}

std::string loggable_command_line(int argc, wchar_t** argv, bool parsedOk, const std::vector<int>* passwordArgs) {
    std::wstring out;
    bool redactNext = false, redactRest = false;
    const bool exact = parsedOk && passwordArgs != nullptr;
    for (int i = 0; i < argc; ++i) {
        std::wstring a = argv[i] ? argv[i] : L"";
        size_t len = 0;
        if (i == 0) {
            // program name: never masked
        } else if (exact) {
            // The parser reported exactly which arguments are password values.
            if (std::find(passwordArgs->begin(), passwordArgs->end(), i) != passwordArgs->end()) a = L"***";
        } else if (redactRest || redactNext) {
            a = L"***"; redactNext = false;
        } else if (password_switch_prefix(a, len)) {
            if (a.size() > len) a = a.substr(0, len) + L"***"; // attached value: -upw=secret, -upwsecret
            else redactNext = true;
            if (!parsedOk) redactRest = true;                  // argument boundaries are unreliable: mask everything after
        } else if (const size_t p = embedded_password_switch(a); p != std::wstring_view::npos) {
            a = a.substr(0, p) + L"***";                        // switch folded into this argument by a broken quote
            redactRest = true;
        }
        if (i > 0) out += L' ';
        if (a.empty() || a.find_first_of(L" \t\"") != std::wstring::npos) {
            out += L'"';
            for (wchar_t c : a) { if (c == L'"') out += L'\\'; out += c; }
            out += L'"';
        } else {
            out += a;
        }
    }
    return narrow(out);
}

// atoi()-like: optional whitespace/sign, digits, stops at first non-digit; no digits -> 0.
static long atoi_like(std::wstring_view s) {
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == L'+' || s[i] == L'-')) { neg = s[i] == L'-'; ++i; }
    long v = 0; bool any = false;
    while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
        any = true;
        if (v < 100000000) v = v * 10 + (s[i] - L'0');
        ++i;
    }
    if (!any) return 0;
    return neg ? -v : v;
}

bool parse_resolution(std::wstring_view s, int& x, int& y) {
    size_t sep = s.find_first_of(L"xX");
    long a = atoi_like(s.substr(0, sep));
    long b = (sep == std::wstring_view::npos) ? a : atoi_like(s.substr(sep + 1));
    if (a <= 0 || b <= 0 || a > 10000 || b > 10000) return false;
    x = static_cast<int>(a);
    y = static_cast<int>(b);
    return true;
}

bool parse_compression(std::wstring_view sv, Compression& c) {
    std::wstring s = to_lower_ascii(std::wstring(sv));
    if (s == L"none" || s == L"1") c = Compression::None;
    else if (s == L"lzw" || s == L"5") c = Compression::Lzw;
    else if (s == L"jpeg" || s == L"jpg" || s == L"7") c = Compression::Jpeg;
    else if (s == L"packbits" || s == L"32773") c = Compression::PackBits;
    else if (s == L"g3" || s == L"3") c = Compression::G3;
    else if (s == L"g4" || s == L"4") c = Compression::G4;
    else if (s == L"classf") c = Compression::ClassF;
    else if (s == L"classf196") c = Compression::ClassF196;
    else return false;
    return true;
}

static bool takes_value(wchar_t c) {
    return c == L'i' || c == L'o' || c == L'r' || c == L'f' || c == L'l' || c == L'c' || c == L'q' || c == L'b';
}
static bool is_flag(wchar_t c) {
    return c == L'$' || c == L'm' || c == L'g' || c == L'd' || c == L'?';
}

static bool apply_value_option(wchar_t letter, std::wstring_view value, Options& o, std::wstring& message) {
    switch (letter) {
    case L'i': o.input = std::wstring(value); return true;
    case L'o': o.output = std::wstring(value); return true;
    case L'r':
        if (!parse_resolution(value, o.dpiX, o.dpiY)) { message = L"invalid resolution: " + std::wstring(value); return false; }
        return true;
    case L'f': { long v = atoi_like(value); o.firstPage = v > 0 ? static_cast<int>(v) : 0; return true; }
    case L'l': { long v = atoi_like(value); o.lastPage = v > 0 ? static_cast<int>(v) : 0; return true; }
    case L'c':
        if (!parse_compression(value, o.compression)) { message = L"unknown compression: " + std::wstring(value); return false; }
        return true;
    case L'q': {
        long v = atoi_like(value);
        if (v <= 0) o.quality = 0; else o.quality = static_cast<int>(v > 100 ? 100 : v);
        return true;
    }
    case L'b': {
        long v = atoi_like(value);
        if (v == 1 || v == 4 || v == 8 || v == 24) o.bitCount = static_cast<int>(v);
        else if (v == 16 || v == 32) o.bitCount = 24;
        else { message = L"invalid bit count: " + std::wstring(value); return false; }
        return true;
    }
    default: return false;
    }
}

ParseStatus parse_command_line(int argc, wchar_t** argv, Options& o, std::wstring& message) {
    message.clear();
    if (argc <= 1) return ParseStatus::Error; // old tool: usage, exit 1

    auto need_value = [&](int& i, std::wstring_view name, std::wstring& out) -> bool {
        if (i + 1 >= argc) { message = L"option " + std::wstring(name) + L" requires an argument"; return false; }
        out = argv[++i];
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        std::wstring_view a = argv[i];
        const std::wstring lower(a); // additional switches are matched case-sensitively (old getopt letters are too)

        // Internal / additional long switches first (the old getopt would reject these letters).
        if (lower == L"--worker") { o.workerMode = true; continue; }
        if (lower == L"-upw" || lower == L"--upw") { std::wstring v; if (!need_value(i, a, v)) return ParseStatus::Error; o.userPassword = narrow(v); o.passwordArgIndices.push_back(i); continue; }
        if (lower == L"-opw" || lower == L"--opw") { std::wstring v; if (!need_value(i, a, v)) return ParseStatus::Error; o.ownerPassword = narrow(v); o.passwordArgIndices.push_back(i); continue; }
        if (lower == L"-timeout" || lower == L"--timeout") { std::wstring v; if (!need_value(i, a, v)) return ParseStatus::Error; o.pageTimeoutSec = static_cast<int>(atoi_like(v)); continue; }
        if (lower == L"-totaltimeout" || lower == L"--totaltimeout") { std::wstring v; if (!need_value(i, a, v)) return ParseStatus::Error; o.totalTimeoutSec = static_cast<int>(atoi_like(v)); continue; }
        if (lower == L"-workers" || lower == L"--workers") { std::wstring v; if (!need_value(i, a, v)) return ParseStatus::Error; o.workers = static_cast<int>(atoi_like(v)); continue; }
        if (lower == L"-log" || lower == L"--log") { std::wstring v; if (!need_value(i, a, v)) return ParseStatus::Error; o.logFile = v; continue; }
        if (lower == L"-v" || lower == L"--verbose") { o.verbose = true; continue; }
        if (lower == L"-version" || lower == L"--version") { o.showVersion = true; continue; }
        if (lower == L"-h" || lower == L"-help" || lower == L"--help") { o.help = true; o.helpExtras = true; continue; }

        if (a.size() < 2 || a[0] != L'-') {
            // Positional argument: the old tool printed usage and exited 1.
            message = L"unexpected argument: " + std::wstring(a);
            return ParseStatus::Error;
        }

        // getopt-style: "-x", "-xVALUE", clustered flags "-mg", "-mr300".
        for (size_t k = 1; k < a.size(); ++k) {
            wchar_t c = a[k];
            if (takes_value(c)) {
                std::wstring value;
                if (k + 1 < a.size()) value = std::wstring(a.substr(k + 1));
                else if (!need_value(i, a, value)) return ParseStatus::Error;
                if (!apply_value_option(c, value, o, message)) return ParseStatus::Error;
                break;
            } else if (is_flag(c)) {
                switch (c) {
                case L'$': break; // hidden switch of the old tool: accepted for compatibility, no effect
                case L'm': o.multipage = true; break;
                case L'g': o.grayscale = true; break;
                case L'd': o.outputIsDir = true; break;
                case L'?': o.help = true; break;
                }
            } else {
                message = L"unknown option: -" + std::wstring(1, c);
                return ParseStatus::Error;
            }
        }
    }

    // A password switch inside another argument means the caller's quoting is broken (e.g. a trailing backslash before a
    // closing quote): reject it rather than use the mangled value, which would also carry the password into messages.
    // Checked after the loop so that -log is already known and the (masked) error reaches the log file.
    for (int i = 1; i < argc; ++i)
        if (argv[i] && embedded_password_switch(argv[i]) != std::wstring_view::npos) {
            message = L"an argument contains a password switch; check the quoting of the arguments";
            return ParseStatus::Error;
        }
    if (o.help) return ParseStatus::Help;
    if (o.showVersion || o.workerMode) return ParseStatus::Ok;
    if (o.input.empty()) { message = L"missing -i"; return ParseStatus::Error; }
    return ParseStatus::Ok;
}

} // namespace p2i
