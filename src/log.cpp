#include "log.h"
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
#else
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace p2i {
namespace log {

// stderr and the log file (-log / LogFile=) receive the same selection: errors and warnings always,
// informational lines only with -v / Verbose=1. Without Verbose the log file therefore records only
// problems and can stay enabled permanently.
// The file is opened for each record and each record is appended with a single write, so lines from
// concurrent pdf2img runs sharing one log never interleave. A log that another program holds open is retried
// briefly (Windows); if it stays unwritable it is skipped for a while (no more waiting) and records go to
// pdf2img-errors.log next to the executable, or in the temp directory if that directory is not writable either.
// The first record a process writes to each file is preceded by its command line (passwords redacted by the caller).

static std::mutex g_mutex;
static bool g_verbose = false;
static std::wstring g_logPath;
static std::string g_commandLine;
static bool g_cmdWrittenPrimary = false;
static bool g_cmdWrittenFallback = false;
static bool g_fallbackNoticeWritten = false;
static uint64_t g_primaryBrokenUntil = 0;
static unsigned long g_primaryError = 0;
static std::string g_prefix = "pdf2img";

constexpr int kPrimaryAttempts = 10;       // x 50 ms: how long one record waits for a log locked by another program
constexpr int kFallbackAttempts = 2;
constexpr uint64_t kBrokenCooldownMs = 30000; // after that, records skip the configured log for this long

void init(bool verbose, const std::wstring& logFile, const char* prefix) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_verbose = verbose;
    if (prefix) g_prefix = prefix;
    g_logPath = logFile;
}

void set_command_line(std::string commandLine) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_commandLine = std::move(commandLine);
}

bool verbose() { return g_verbose; }

#ifdef _WIN32

constexpr DWORD kRetryMs = 50;

static void write_stream(bool toStdout, std::string_view s) {
    HANDLE h = GetStdHandle(toStdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    DWORD written = 0;
    WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
}

// Appends `data` to `path` with one WriteFile. Returns false (with *error set) if the file cannot be written.
static bool append_to(const std::wstring& path, std::string_view data, int attempts, unsigned long* error) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
            *error = ok ? 0 : GetLastError();
            CloseHandle(h);
            return ok;
        }
        *error = GetLastError();
        if (*error != ERROR_SHARING_VIOLATION && *error != ERROR_LOCK_VIOLATION) return false;
        if (attempt + 1 < attempts) Sleep(kRetryMs);
    }
    return false;
}

static std::string file_stamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    snprintf(ts, sizeof ts, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, static_cast<unsigned long>(GetCurrentProcessId()));
    return ts;
}

#else

static bool write_fd(int fd, std::string_view s) {
    const char* p = s.data();
    size_t n = s.size();
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

static void write_stream(bool toStdout, std::string_view s) { write_fd(toStdout ? 1 : 2, s); }

// Appends `data` to `path` with one O_APPEND write. No sharing violations exist here, so there is nothing to retry.
static bool append_to(const std::wstring& path, std::string_view data, int /*attempts*/, unsigned long* error) {
    const int fd = ::open(narrow(path).c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) { *error = static_cast<unsigned long>(errno); return false; }
    const bool ok = write_fd(fd, data);
    *error = ok ? 0 : static_cast<unsigned long>(errno);
    ::close(fd);
    return ok;
}

static std::string file_stamp() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    const time_t secs = ts.tv_sec;
    tm lt{};
    localtime_r(&secs, &lt);
    char buf[64];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%lu] ", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec, static_cast<long>(ts.tv_nsec / 1000000L), current_pid());
    return buf;
}

#endif

// Must be called with g_mutex held.
static void write_record(const std::string& stamp, const std::string& line) {
    const std::string cmdRecord = g_commandLine.empty() ? std::string() : stamp + g_prefix + ": command: " + g_commandLine + "\n";
    if (tick_ms() >= g_primaryBrokenUntil) {
        const std::string record = (g_cmdWrittenPrimary ? std::string() : cmdRecord) + stamp + line;
        unsigned long err = 0;
        if (append_to(g_logPath, record, kPrimaryAttempts, &err)) {
            g_cmdWrittenPrimary = true;
            g_primaryBrokenUntil = 0;
            return;
        }
        g_primaryError = err;
        g_primaryBrokenUntil = tick_ms() + kBrokenCooldownMs; // stop waiting on it for every record
    }
    // Fallbacks: next to the executable, then the temp directory.
    std::string record;
    if (!g_fallbackNoticeWritten)
        record += stamp + g_prefix + ": warning: cannot write to log file " + narrow(g_logPath) + " (error " +
                  std::to_string(g_primaryError) + "); writing here instead\n";
    if (!g_cmdWrittenFallback) record += cmdRecord;
    record += stamp + line;
    const std::wstring tempDir = temp_directory();
    const std::wstring candidates[] = {path_join(exe_directory(), L"pdf2img-errors.log"),
                                       tempDir.empty() ? std::wstring() : path_join(tempDir, L"pdf2img-errors.log")};
    for (const std::wstring& path : candidates) {
        unsigned long err = 0;
        if (!path.empty() && append_to(path, record, kFallbackAttempts, &err)) {
            g_fallbackNoticeWritten = true;
            g_cmdWrittenFallback = true;
            return;
        }
    }
}

static void emit(const char* level, std::string_view msg) {
    std::string line;
    line.reserve(msg.size() + 32);
    line += g_prefix; line += ": ";
    if (level && *level) { line += level; line += ": "; }
    line += msg;
    if (line.empty() || line.back() != '\n') line += '\n';
    std::lock_guard<std::mutex> lock(g_mutex);
    write_stream(false, line);
    if (!g_logPath.empty()) write_record(file_stamp(), line);
}

void error(std::string_view msg) { emit("error", msg); }
void warn(std::string_view msg) { emit("warning", msg); }
void info(std::string_view msg) { if (g_verbose) emit("", msg); }

void raw_stdout(std::string_view msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    write_stream(true, msg);
}

static std::string vformat(const char* fmt, va_list ap) {
    char buf[2048];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) { va_end(ap2); return fmt; }
    if (static_cast<size_t>(n) < sizeof buf) { va_end(ap2); return std::string(buf, static_cast<size_t>(n)); }
    std::string big(static_cast<size_t>(n) + 1, '\0');
    vsnprintf(big.data(), big.size(), fmt, ap2);
    va_end(ap2);
    big.resize(static_cast<size_t>(n));
    return big;
}

void errorf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); error(vformat(fmt, ap)); va_end(ap); }
void warnf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); warn(vformat(fmt, ap)); va_end(ap); }
void infof(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt); info(vformat(fmt, ap)); va_end(ap);
}

} // namespace log
} // namespace p2i
