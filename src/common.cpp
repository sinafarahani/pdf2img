#include "common.h"

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
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include <algorithm>

namespace p2i {

#ifdef _WIN32

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

#else

std::string narrow(std::wstring_view w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        const uint32_t c = static_cast<uint32_t>(wc);
        if (c >= 0xDC80 && c <= 0xDCFF) { s += static_cast<char>(c - 0xDC00); continue; } // escaped raw byte
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else if (c < 0x800) {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += static_cast<char>(0xE0 | (c >> 12));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x110000) {
            s += static_cast<char>(0xF0 | (c >> 18));
            s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += '?';
        }
    }
    return s;
}

std::wstring widen(std::string_view s) {
    std::wstring w;
    w.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) { w += static_cast<wchar_t>(b0); ++i; continue; }
        size_t len = 0;
        uint32_t c = 0, min = 0;
        if ((b0 & 0xE0) == 0xC0) { len = 2; c = b0 & 0x1Fu; min = 0x80; }
        else if ((b0 & 0xF0) == 0xE0) { len = 3; c = b0 & 0x0Fu; min = 0x800; }
        else if ((b0 & 0xF8) == 0xF0) { len = 4; c = b0 & 0x07u; min = 0x10000; }
        bool ok = len > 0 && i + len <= s.size();
        for (size_t k = 1; ok && k < len; ++k) {
            const unsigned char b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) ok = false;
            else c = (c << 6) | (b & 0x3Fu);
        }
        if (ok && (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))) ok = false;
        if (!ok) { w += static_cast<wchar_t>(0xDC00 + b0); ++i; continue; } // not UTF-8: keep the raw byte
        w += static_cast<wchar_t>(c);
        i += len;
    }
    return w;
}

#endif

std::string to_lower_ascii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::wstring to_lower_ascii(std::wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

bool iequals_ascii(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return true;
}

bool is_path_sep(wchar_t c) {
#ifdef _WIN32
    return c == L'\\' || c == L'/';
#else
    return c == L'/';
#endif
}

static size_t last_sep(std::wstring_view p) {
#ifdef _WIN32
    return p.find_last_of(L"\\/");
#else
    return p.find_last_of(L'/');
#endif
}

std::wstring path_dir(std::wstring_view p) {
    size_t i = last_sep(p);
    if (i == std::wstring_view::npos) return {};
    if (i == 0) return std::wstring(p.substr(0, 1));                 // "\x" -> "\"
#ifdef _WIN32
    if (i == 2 && p[1] == L':') return std::wstring(p.substr(0, 3)); // "C:\x" -> "C:\"
#endif
    return std::wstring(p.substr(0, i));
}

std::wstring path_name(std::wstring_view p) {
    size_t i = last_sep(p);
    return std::wstring(i == std::wstring_view::npos ? p : p.substr(i + 1));
}

std::wstring path_ext(std::wstring_view p) {
    std::wstring name = path_name(p);
    size_t d = name.find_last_of(L'.');
    if (d == std::wstring::npos || d == 0) return {};
    return name.substr(d);
}

std::wstring path_stem(std::wstring_view p) {
    std::wstring name = path_name(p);
    size_t d = name.find_last_of(L'.');
    if (d == std::wstring::npos || d == 0) return name;
    return name.substr(0, d);
}

std::wstring path_join(std::wstring_view dir, std::wstring_view name) {
    if (dir.empty()) return std::wstring(name);
    std::wstring r(dir);
    if (!is_path_sep(r.back())) r += kPathSep;
    r += name;
    return r;
}

bool same_path(std::wstring_view a, std::wstring_view b) {
#ifdef _WIN32
    return iequals_ascii(a, b);
#else
    return a == b;
#endif
}

bool has_wildcard(std::wstring_view p) {
    return p.find_first_of(L"*?") != std::wstring_view::npos;
}

std::wstring exe_path() {
#if defined(_WIN32)
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    char real[PATH_MAX];
    if (realpath(buf.c_str(), real)) return widen(real);
    return widen(buf);
#else
    std::string buf(4096, '\0');
    for (;;) {
        const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return {};
        if (static_cast<size_t>(n) < buf.size()) { buf.resize(static_cast<size_t>(n)); return widen(buf); }
        buf.resize(buf.size() * 2);
    }
#endif
}

std::wstring exe_directory() {
    const std::wstring p = exe_path();
    if (p.empty()) return L".";
    return path_dir(p);
}

#ifdef _WIN32

std::wstring make_absolute(std::wstring_view p) {
    std::wstring in(p);
    if (in.rfind(L"\\\\?\\", 0) == 0) return in; // already an extended-length path
    DWORD n = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    if (n == 0) return in;
    std::wstring out(n, L'\0');
    DWORD m = GetFullPathNameW(in.c_str(), n, out.data(), nullptr);
    if (m == 0 || m >= n) return in;
    out.resize(m);
    return extend_long_path(std::move(out));
}

std::wstring extend_long_path(std::wstring p) {
    // Long paths: switch to the extended-length form (independent of the system LongPathsEnabled policy;
    // the manifest also declares longPathAware). Short paths stay as they are so messages look normal.
    if (p.size() < 240 || p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return L"\\\\?\\" + p;
    return p; // relative paths cannot be extended
}

uint64_t tick_ms() { return GetTickCount64(); }

unsigned long current_pid() { return GetCurrentProcessId(); }

bool get_env(const wchar_t* name, std::wstring& value) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return false;
    std::wstring buf(n, L'\0');
    DWORD m = GetEnvironmentVariableW(name, buf.data(), n);
    if (m == 0 || m >= n) return false;
    buf.resize(m);
    value = std::move(buf);
    return true;
}

#else

// Lexical normalisation like GetFullPathNameW: drops "." and empty components, resolves ".." (symlinks are not
// followed), keeps a trailing separator.
static std::wstring normalize_absolute(const std::wstring& abs) {
    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i <= abs.size()) {
        size_t j = abs.find(L'/', i);
        if (j == std::wstring::npos) j = abs.size();
        std::wstring part = abs.substr(i, j - i);
        if (part.empty() || part == L".") {
        } else if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(std::move(part));
        }
        i = j + 1;
    }
    std::wstring out;
    for (const auto& part : parts) { out += L'/'; out += part; }
    if (out.empty()) return L"/";
    if (abs.back() == L'/') out += L'/';
    return out;
}

std::wstring make_absolute(std::wstring_view p) {
    std::wstring in(p);
    if (in.empty()) return in;
    if (in[0] != L'/') {
        std::string cwd(4096, '\0');
        while (!getcwd(cwd.data(), cwd.size())) {
            if (errno != ERANGE) return in;
            cwd.resize(cwd.size() * 2);
        }
        cwd.resize(std::strlen(cwd.c_str()));
        in = widen(cwd) + L"/" + in;
    }
    return normalize_absolute(in);
}

std::wstring extend_long_path(std::wstring p) { return p; }

uint64_t tick_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

unsigned long current_pid() { return static_cast<unsigned long>(getpid()); }

bool get_env(const wchar_t* name, std::wstring& value) {
    const char* v = std::getenv(narrow(name).c_str());
    if (!v || !*v) return false;
    value = widen(v);
    return true;
}

#endif

OutFormat format_from_extension(std::wstring_view ext) {
    std::wstring e = to_lower_ascii(std::wstring(ext));
    if (!e.empty() && e[0] == L'.') e.erase(0, 1);
    if (e == L"tif" || e == L"tiff") return OutFormat::Tiff;
    if (e == L"jpg" || e == L"jpeg") return OutFormat::Jpeg;
    if (e == L"png") return OutFormat::Png;
    if (e == L"bmp") return OutFormat::Bmp;
    if (e == L"gif") return OutFormat::Gif;
    if (e == L"pcx") return OutFormat::Pcx;
    if (e == L"tga") return OutFormat::Tga;
    if (e == L"wmf") return OutFormat::Wmf;
    if (e == L"emf") return OutFormat::Emf;
    return OutFormat::Unknown;
}

const char* format_name(OutFormat f) {
    switch (f) {
    case OutFormat::Tiff: return "tiff";
    case OutFormat::Jpeg: return "jpeg";
    case OutFormat::Png: return "png";
    case OutFormat::Bmp: return "bmp";
    case OutFormat::Gif: return "gif";
    case OutFormat::Pcx: return "pcx";
    case OutFormat::Tga: return "tga";
    case OutFormat::Wmf: return "wmf";
    case OutFormat::Emf: return "emf";
    default: return "unknown";
    }
}

const char* compression_name(Compression c) {
    switch (c) {
    case Compression::None: return "none";
    case Compression::Lzw: return "lzw";
    case Compression::Jpeg: return "jpeg";
    case Compression::PackBits: return "packbits";
    case Compression::G3: return "g3";
    case Compression::G4: return "g4";
    case Compression::ClassF: return "ClassF";
    case Compression::ClassF196: return "ClassF196";
    default: return "default";
    }
}

} // namespace p2i
