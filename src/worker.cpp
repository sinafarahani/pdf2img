#include "worker.h"
#include "common.h"
#include "cli.h"
#include "fsutil.h"
#include "ipc.h"
#include "log.h"
#include "pipeline.h"
#include "encode/encoders.h"
#include "render/pdf_document.h"

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
#include <csignal>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace p2i {

namespace {

// ------------------------------------------------------------------ stdin/stdout of the worker (the pipes)
#ifdef _WIN32
using StdStream = HANDLE;
StdStream std_input() { return GetStdHandle(STD_INPUT_HANDLE); }
StdStream std_output() { return GetStdHandle(STD_OUTPUT_HANDLE); }
bool read_some(StdStream h, char* buf, size_t n, size_t& got) {
    DWORD g = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(n), &g, nullptr) || g == 0) return false;
    got = g;
    return true;
}
bool write_all(StdStream h, const std::string& s) {
    DWORD written = 0;
    return WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr) && written == s.size();
}
[[noreturn]] void crash_now(unsigned code) {
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(1000);
}
#else
using StdStream = int;
StdStream std_input() { return 0; }
StdStream std_output() { return 1; }
bool read_some(StdStream fd, char* buf, size_t n, size_t& got) {
    for (;;) {
        const ssize_t r = ::read(fd, buf, n);
        if (r > 0) { got = static_cast<size_t>(r); return true; }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
}
bool write_all(StdStream fd, const std::string& s) {
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
[[noreturn]] void crash_now(unsigned) {
    ::raise(SIGKILL);
    for (;;) ::pause();
}
#endif

class LineReader {
public:
    explicit LineReader(StdStream h) : h_(h) {}
    bool read_line(std::string& line) {
        while (true) {
            size_t nl = buf_.find('\n');
            if (nl != std::string::npos) { line = buf_.substr(0, nl + 1); buf_.erase(0, nl + 1); return true; }
            char tmp[4096];
            size_t got = 0;
            if (!read_some(h_, tmp, sizeof tmp, got)) return false;
            buf_.append(tmp, got);
        }
    }
private:
    StdStream h_;
    std::string buf_;
};

bool send(StdStream out, const std::vector<std::string>& fields) {
    return write_all(out, ipc::join(fields));
}

struct WorkerSpec {
    OutFormat format = OutFormat::Tiff;
    int dpiX = 0, dpiY = 0, bitCount = 0, quality = 0;
    bool gray = false;
    Compression compression = Compression::Default;
    int defaultDpi = 101, defaultQuality = 90;
    bool lzwPredictor = true, antialias = true;
    uint64_t maxBitmapBytes = 1536ull << 20;
    int pageTimeoutSec = 120;
    std::vector<std::string> passwords; // tried in order ("" first)
};

void apply_spec(WorkerSpec& s, const std::string& key, const std::string& value) {
    auto toInt = [&]() { return atoi(value.c_str()); };
    if (key == "format") s.format = format_from_extension(widen("." + value));
    else if (key == "dpix") s.dpiX = toInt();
    else if (key == "dpiy") s.dpiY = toInt();
    else if (key == "bitcount") s.bitCount = toInt();
    else if (key == "gray") s.gray = value == "1";
    else if (key == "compression") { Compression c; if (parse_compression(widen(value), c)) s.compression = c; else s.compression = Compression::Default; }
    else if (key == "quality") s.quality = toInt();
    else if (key == "defaultdpi") s.defaultDpi = toInt();
    else if (key == "defaultquality") s.defaultQuality = toInt();
    else if (key == "lzwpredictor") s.lzwPredictor = value == "1";
    else if (key == "antialias") s.antialias = value == "1";
    else if (key == "maxbitmap") s.maxBitmapBytes = static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
    else if (key == "pagetimeout") s.pageTimeoutSec = toInt();
    else if (key == "password") s.passwords.push_back(value);
}

struct OpenDoc {
    int fileId = -1;
    std::wstring path;
    PdfDocument doc;
};

// Opens (or re-uses) the document for fileId/path. Tries each password candidate.
bool ensure_open(OpenDoc& cur, int fileId, const std::wstring& path, const WorkerSpec& spec, OpenError& err) {
    if (cur.doc.is_open() && cur.fileId == fileId && (cur.path == path || (cur.doc.was_repaired() && cur.doc.repaired_path() == path))) return true;
    cur.doc.close();
    cur.fileId = -1;
    std::vector<std::string> candidates;
    candidates.push_back("");
    for (const auto& p : spec.passwords) if (!p.empty()) candidates.push_back(p);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (cur.doc.open(path, candidates[i], err)) { cur.fileId = fileId; cur.path = path; return true; }
        if (err.exitCode != EXIT_PASSWORD) return false; // only retry with another password for password errors
    }
    return false;
}

uint64_t deadline_from(const WorkerSpec& spec) {
    return spec.pageTimeoutSec > 0 ? tick_ms() + static_cast<uint64_t>(spec.pageTimeoutSec) * 1000ull : 0;
}

// Test hooks (robustness tests only): make the worker hang or crash on a given page number.
int test_hook_page(const wchar_t* name) {
    std::wstring v;
    if (!get_env(name, v) || v.size() >= 32) return 0;
    return static_cast<int>(wcstol(v.c_str(), nullptr, 10));
}
// PDF2IMG_TEST_SLOW_EXIT_MS: delay exiting after QUIT, to test that the supervisor does not wait for slow workers.
int test_hook_slow_exit_ms() { return test_hook_page(L"PDF2IMG_TEST_SLOW_EXIT_MS"); }
// PDF2IMG_TEST_CRASH_ONCE_OPEN_MARKER / _PAGE_MARKER = <file>: crash the first time only (the marker file is created
// atomically, so exactly one worker crashes). Tests the supervisor's retry of lost work.
void crash_once(const wchar_t* envName) {
    std::wstring marker;
    if (!get_env(envName, marker)) return;
    if (!create_new_file(marker)) return; // already crashed once
    crash_now(0xC0000005);
}
// PDF2IMG_TEST_FAIL_ONCE_PAGE=<n> + PDF2IMG_TEST_FAIL_ONCE_MARKER=<file>: report page n as failed the first time only.
bool fail_once(int page1) {
    static const int failPage = test_hook_page(L"PDF2IMG_TEST_FAIL_ONCE_PAGE");
    if (failPage == 0 || failPage != page1) return false;
    std::wstring marker;
    if (!get_env(L"PDF2IMG_TEST_FAIL_ONCE_MARKER", marker)) return false;
    return create_new_file(marker);
}
// PDF2IMG_TEST_ALLOC_MB=<n>: allocate and touch n MB while rendering (tests the worker memory limit).
bool test_allocation(std::string& msg) {
    static const int mb = test_hook_page(L"PDF2IMG_TEST_ALLOC_MB");
    if (mb <= 0) return true;
    const size_t bytes = static_cast<size_t>(mb) << 20;
    uint64_t* p = static_cast<uint64_t*>(std::malloc(bytes));
    if (!p) { msg = "test allocation of " + std::to_string(mb) + " MB failed"; return false; }
    uint64_t x = 88172645463325252ull; // pseudo-random fill: the pages stay resident (no zero-page or compression tricks)
    for (size_t i = 0; i < bytes / sizeof(uint64_t); ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; p[i] = x; }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::free(p);
    return true;
}
void apply_test_hooks(int page1) {
    static const int crashOncePage = test_hook_page(L"PDF2IMG_TEST_CRASH_ONCE_PAGE"); // 0 = on any page
    if (crashOncePage == 0 || crashOncePage == page1) crash_once(L"PDF2IMG_TEST_CRASH_ONCE_PAGE_MARKER");
    static const int hangPage = test_hook_page(L"PDF2IMG_TEST_HANG_PAGE");
    static const int crashPage = test_hook_page(L"PDF2IMG_TEST_CRASH_PAGE");
    if (hangPage == page1) { for (;;) std::this_thread::sleep_for(std::chrono::seconds(1)); }
    if (crashPage == page1) crash_now(0xC0000005);
}

// Renders + finalises one page. Returns an exit code (0 ok) and message.
int render_one(OpenDoc& cur, int page1, const ConversionSpec& cs, const WorkerSpec& spec, Image& out, bool& fallback, std::string& msg) {
    apply_test_hooks(page1);
    if (fail_once(page1)) { msg = "page " + std::to_string(page1) + ": test failure (PDF2IMG_TEST_FAIL_ONCE_PAGE)"; return EXIT_RENDER; }
    if (!test_allocation(msg)) return EXIT_RENDER;
    RenderRequest rr = make_render_request(cs);
    RenderResult res;
    std::string err;
    RenderStatus st = cur.doc.render_page(page1 - 1, rr, deadline_from(spec), res, err);
    if (st == RenderStatus::Timeout) { msg = err; return EXIT_TIMEOUT; }
    if (st == RenderStatus::Failed) { msg = err; return EXIT_RENDER; }
    fallback = res.usedFallbackDpi;
    out = finalize_image(std::move(res.image), cs);
    return EXIT_OK;
}

} // namespace

int run_worker() {
    disable_error_dialogs();
    StdStream in = std_input();
    StdStream out = std_output();
#ifdef _WIN32
    // pdfium.dll is delay-loaded so that a missing/corrupt DLL is reported here instead of by a loader dialog.
    {
        std::wstring dll = path_join(exe_directory(), L"pdfium.dll");
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        DWORD loadErr = h ? 0 : GetLastError();
        if (!h) h = LoadLibraryW(L"pdfium.dll");
        if (!h) {
            // Report the cause over the protocol: the supervisor logs it (this process has no log file, and callers often
            // hide stderr). stderr is only the fallback if the pipe is already gone.
            const std::string msg = "cannot load pdfium.dll (error " + std::to_string(loadErr) +
                                    "); it must be in the same directory as the executable: " + narrow(exe_directory());
            if (!send(out, {"FATAL", msg})) log::error(msg);
            return EXIT_INTERNAL;
        }
    }
#else
    // Linux/macOS: libpdfium is linked normally (the system loader reports a missing library before main()).
    // There is no Job Object to take the worker down with the supervisor, so leave as soon as the supervisor is gone,
    // even in the middle of a page.
    {
        const pid_t parent = getppid();
        std::thread([parent]() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (getppid() != parent) _exit(EXIT_INTERNAL);
            }
        }).detach();
    }
#endif
    LineReader reader(in);
    WorkerSpec spec;
    std::string line;

    // --- SPEC phase ---
    while (reader.read_line(line)) {
        auto f = ipc::split(line);
        if (f.empty()) continue;
        if (f[0] == "SPECEND") break;
        if (f[0] == "SPEC" && f.size() >= 3) apply_spec(spec, f[1], f[2]);
    }
    pdf_library_init();
    ConversionSpec cs = make_spec(spec.format, spec.dpiX, spec.dpiY, spec.bitCount, spec.gray, spec.compression, spec.quality,
                                  spec.defaultDpi, spec.defaultQuality, spec.lzwPredictor, spec.antialias, spec.maxBitmapBytes);
    EncodeParams ep = make_encode_params(cs);
    if (!send(out, {"READY"})) return EXIT_INTERNAL;

    OpenDoc cur;

    while (reader.read_line(line)) {
        auto f = ipc::split(line);
        if (f.empty()) continue;
        const std::string& cmd = f[0];
        if (cmd == "QUIT") {
            if (int ms = test_hook_slow_exit_ms(); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            break;
        }

        if (cmd == "OPEN" && f.size() >= 3) {
            int id = atoi(f[1].c_str());
            std::wstring path = widen(f[2]);
            OpenError oe;
            send(out, {"BEGIN", std::to_string(id)});
            crash_once(L"PDF2IMG_TEST_CRASH_ONCE_OPEN_MARKER");
            if (!ensure_open(cur, id, path, spec, oe)) { send(out, {"OPENFAIL", std::to_string(id), std::to_string(oe.exitCode), oe.message}); continue; }
            send(out, {"OPENED", std::to_string(id), std::to_string(cur.doc.page_count()), narrow(cur.doc.repaired_path()), cur.doc.xref_was_rebuilt() ? "1" : "0"});
            continue;
        }

        if (cmd == "PAGE" && f.size() >= 5) {
            int id = atoi(f[1].c_str());
            std::wstring path = widen(f[2]);
            int page = atoi(f[3].c_str());
            std::wstring outPath = widen(f[4]);
            uint64_t t0 = tick_ms();
            send(out, {"BEGIN", std::to_string(id)});
            OpenError oe;
            if (!ensure_open(cur, id, path, spec, oe)) { send(out, {"PAGEFAIL", std::to_string(id), std::to_string(page), std::to_string(oe.exitCode), oe.message}); continue; }
            if (page < 1 || page > cur.doc.page_count()) { send(out, {"PAGEFAIL", std::to_string(id), std::to_string(page), std::to_string(EXIT_RENDER), "page out of range"}); continue; }
            Image img; bool fallback = false; std::string msg;
            int code = render_one(cur, page, cs, spec, img, fallback, msg);
            if (code != EXIT_OK) { send(out, {"PAGEFAIL", std::to_string(id), std::to_string(page), std::to_string(code), msg}); continue; }
            std::string err;
            std::wstring tmp = temp_name_for(outPath);
            ensure_directory(path_dir(outPath), err);
            if (!write_image(cs.format, tmp, img, ep, err)) { delete_file_quiet(tmp); send(out, {"PAGEFAIL", std::to_string(id), std::to_string(page), std::to_string(EXIT_OUTPUT), err}); continue; }
            if (!replace_file(tmp, outPath, err)) { send(out, {"PAGEFAIL", std::to_string(id), std::to_string(page), std::to_string(EXIT_OUTPUT), err}); continue; }
            send(out, {"PAGEOK", std::to_string(id), std::to_string(page), std::to_string(img.width) + "x" + std::to_string(img.height),
                       std::to_string(tick_ms() - t0), narrow(outPath), fallback ? "1" : "0"});
            // Test hook: die right after reporting a page (tests the supervisor's handling of an idle worker crash).
            if (test_hook_page(L"PDF2IMG_TEST_CRASH_AFTER_PAGE") == page || test_hook_page(L"PDF2IMG_TEST_CRASH_AFTER_EVERY_PAGE") != 0)
                crash_now(0xC0000374);
            continue;
        }

        if (cmd == "MULTI" && f.size() >= 6) {
            int id = atoi(f[1].c_str());
            std::wstring path = widen(f[2]);
            int first = atoi(f[3].c_str()), last = atoi(f[4].c_str());
            std::wstring outPath = widen(f[5]);
            uint64_t t0 = tick_ms();
            send(out, {"BEGIN", std::to_string(id)});
            OpenError oe;
            if (!ensure_open(cur, id, path, spec, oe)) { send(out, {"MULTIFAIL", std::to_string(id), std::to_string(oe.exitCode), oe.message}); continue; }
            if (first < 1) first = 1;
            if (last > cur.doc.page_count()) last = cur.doc.page_count();
            if (last < first) { send(out, {"MULTIFAIL", std::to_string(id), std::to_string(EXIT_RENDER), "empty page range"}); continue; }
            std::string err;
            ensure_directory(path_dir(outPath), err);
            std::wstring tmp = temp_name_for(outPath);
            TiffWriter w;
            if (!w.open(tmp, err)) { send(out, {"MULTIFAIL", std::to_string(id), std::to_string(EXIT_OUTPUT), err}); continue; }
            int written = 0, failed = 0;
            const int total = last - first + 1;
            bool fatal = false;
            for (int page = first; page <= last; ++page) {
                Image img; bool fallback = false; std::string msg;
                int code = render_one(cur, page, cs, spec, img, fallback, msg);
                if (code != EXIT_OK) {
                    ++failed;
                    send(out, {"PAGEFAIL", std::to_string(id), std::to_string(page), std::to_string(code), msg});
                    continue;
                }
                if (!w.add_page(img, ep, page - first, total, err)) {
                    fatal = true;
                    send(out, {"PAGEFAIL", std::to_string(id), std::to_string(page), std::to_string(EXIT_OUTPUT), err});
                    break;
                }
                ++written;
                send(out, {"PROGRESS", std::to_string(id), std::to_string(page), fallback ? "1" : "0"});
            }
            std::string cerr;
            bool closed = w.close(cerr);
            if (fatal || !closed || written == 0) {
                delete_file_quiet(tmp);
                send(out, {"MULTIFAIL", std::to_string(id), std::to_string(fatal || !closed ? EXIT_OUTPUT : EXIT_RENDER), fatal ? err : (!closed ? cerr : "no page could be rendered")});
                continue;
            }
            if (!replace_file(tmp, outPath, err)) { send(out, {"MULTIFAIL", std::to_string(id), std::to_string(EXIT_OUTPUT), err}); continue; }
            send(out, {"MULTIOK", std::to_string(id), std::to_string(written), std::to_string(failed), std::to_string(tick_ms() - t0), narrow(outPath)});
            continue;
        }
        // Unknown command: ignore (forward compatibility).
    }
    cur.doc.close();
    pdf_library_shutdown();
    return EXIT_OK;
}

} // namespace p2i
