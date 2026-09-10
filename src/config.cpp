#include "config.h"
#include "common.h"
#include "fsutil.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cwchar>

namespace p2i {

#ifdef _WIN32

static std::wstring ini_string(const std::wstring& file, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024];
    DWORD n = GetPrivateProfileStringW(L"Options", key, def, buf, static_cast<DWORD>(std::size(buf)), file.c_str());
    return std::wstring(buf, n);
}

#else

static std::wstring trim(const std::wstring& s) {
    const wchar_t* ws = L" \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// The GetPrivateProfileString rules that matter here: [Options] section and keys matched case-insensitively, the value
// trimmed and one pair of surrounding quotes removed, ';' starts a comment line. UTF-8 file (a BOM is allowed).
static std::wstring ini_string(const std::wstring& file, const wchar_t* key, const wchar_t* def) {
    FILE* f = std::fopen(narrow(file).c_str(), "rb");
    if (!f) return def;
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    std::fclose(f);
    if (content.rfind("\xEF\xBB\xBF", 0) == 0) content.erase(0, 3);
    const std::wstring text = widen(content);
    const std::wstring wanted = to_lower_ascii(std::wstring(key));
    bool inSection = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        const std::wstring line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == L';') continue;
        if (line[0] == L'[') {
            const size_t close = line.find(L']');
            inSection = close != std::wstring::npos && to_lower_ascii(trim(line.substr(1, close - 1))) == L"options";
            continue;
        }
        if (!inSection) continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || to_lower_ascii(trim(line.substr(0, eq))) != wanted) continue;
        std::wstring value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        return value;
    }
    return def;
}

#endif

static long ini_int(const std::wstring& file, const wchar_t* key, long def) {
    std::wstring s = ini_string(file, key, L"");
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    return end == s.c_str() ? def : v;
}

static void env_int(const wchar_t* name, int& v) {
    std::wstring s;
    if (get_env(name, s)) { long x = wcstol(s.c_str(), nullptr, 10); if (x > 0 || s == L"0") v = static_cast<int>(x); }
}

std::wstring sanitize_suffix_format(std::wstring fmt) {
    if (fmt.empty()) return L"%04d";
    int conversions = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != L'%') continue;
        if (i + 1 < fmt.size() && fmt[i + 1] == L'%') { ++i; continue; } // literal %%
        // flags / width
        size_t j = i + 1;
        while (j < fmt.size() && (fmt[j] == L'0' || fmt[j] == L'-' || fmt[j] == L'+' || fmt[j] == L' ' || fmt[j] == L'#')) ++j;
        while (j < fmt.size() && fmt[j] >= L'0' && fmt[j] <= L'9') ++j;
        if (j >= fmt.size()) return L"%04d";
        wchar_t conv = fmt[j];
        if (conv != L'd' && conv != L'i' && conv != L'u') return L"%04d";
        if (j - i > 8) return L"%04d"; // absurd width
        ++conversions;
        i = j;
    }
    return conversions == 1 ? fmt : L"%04d";
}

std::wstring format_page_suffix(const std::wstring& fmt, int page) {
    wchar_t buf[256];
#ifdef _WIN32
    int n = _snwprintf_s(buf, _TRUNCATE, fmt.c_str(), page);
    if (n < 0) { _snwprintf_s(buf, _TRUNCATE, L"%04d", page); }
#else
    int n = std::swprintf(buf, std::size(buf), fmt.c_str(), page);
    if (n < 0) std::swprintf(buf, std::size(buf), L"%04d", page);
#endif
    return buf;
}

Config load_config() {
    Config c;
    std::wstring ini = path_join(exe_directory(), L"pdf2img.ini");
    if (path_exists(ini)) {
        c.iniPath = ini;
        c.suffixFormat = sanitize_suffix_format(ini_string(ini, L"AddFileNameSuffix", L"%04d"));
        c.defaultDpi = static_cast<int>(ini_int(ini, L"DefaultDPI", c.defaultDpi));
        c.jpegQuality = static_cast<int>(ini_int(ini, L"JpegQuality", c.jpegQuality));
        c.pageTimeoutSec = static_cast<int>(ini_int(ini, L"PageTimeoutSec", c.pageTimeoutSec));
        c.totalTimeoutSec = static_cast<int>(ini_int(ini, L"TotalTimeoutSec", c.totalTimeoutSec));
        c.workers = static_cast<int>(ini_int(ini, L"Workers", c.workers));
        c.workerMemoryLimitMB = static_cast<int>(ini_int(ini, L"WorkerMemoryLimitMB", c.workerMemoryLimitMB));
        long mb = ini_int(ini, L"MaxBitmapMB", static_cast<long>(c.maxBitmapBytes >> 20));
        if (mb > 0) c.maxBitmapBytes = static_cast<uint64_t>(mb) << 20;
        c.lzwPredictor = ini_int(ini, L"LzwPredictor", 1) != 0;
        c.antialias = ini_int(ini, L"Antialias", 1) != 0;
        c.verbose = ini_int(ini, L"Verbose", 0) != 0;
        c.logFile = ini_string(ini, L"LogFile", L"");
    }
    // Environment overrides.
    std::wstring s;
    if (get_env(L"PDF2IMG_SUFFIX", s)) c.suffixFormat = sanitize_suffix_format(s);
    env_int(L"PDF2IMG_DEFAULT_DPI", c.defaultDpi);
    env_int(L"PDF2IMG_JPEG_QUALITY", c.jpegQuality);
    env_int(L"PDF2IMG_PAGE_TIMEOUT", c.pageTimeoutSec);
    env_int(L"PDF2IMG_TOTAL_TIMEOUT", c.totalTimeoutSec);
    env_int(L"PDF2IMG_WORKERS", c.workers);
    env_int(L"PDF2IMG_WORKER_MEMORY_MB", c.workerMemoryLimitMB);
    if (get_env(L"PDF2IMG_MAX_BITMAP_MB", s)) { long mb = wcstol(s.c_str(), nullptr, 10); if (mb > 0) c.maxBitmapBytes = static_cast<uint64_t>(mb) << 20; }
    if (get_env(L"PDF2IMG_VERBOSE", s)) c.verbose = (s != L"0" && !s.empty());
    if (get_env(L"PDF2IMG_LOG", s)) c.logFile = s;
    if (get_env(L"PDF2IMG_ANTIALIAS", s)) c.antialias = (s != L"0");
    if (get_env(L"PDF2IMG_LZW_PREDICTOR", s)) c.lzwPredictor = (s != L"0");

    if (c.defaultDpi <= 0) c.defaultDpi = 101;
    if (c.jpegQuality <= 0 || c.jpegQuality > 100) c.jpegQuality = 90;
    if (c.pageTimeoutSec < 0) c.pageTimeoutSec = 0;
    if (c.totalTimeoutSec < 0) c.totalTimeoutSec = 0;
    if (c.workers < 0) c.workers = 0;
    if (c.workerMemoryLimitMB < 256) c.workerMemoryLimitMB = 256;
    return c;
}

} // namespace p2i
