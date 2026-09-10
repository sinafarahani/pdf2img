#include "pdf_document.h"
#include "../fsutil.h"
#include "../log.h"

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
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <fpdfview.h>
#include <fpdf_formfill.h>
#include <fpdf_progressive.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <new>
#include <cstring>

namespace p2i {

void pdf_library_init() {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    cfg.m_pUserFontPaths = nullptr;
    cfg.m_pIsolate = nullptr;
    cfg.m_v8EmbedderSlot = 0;
    FPDF_InitLibraryWithConfig(&cfg);
}

void pdf_library_shutdown() { FPDF_DestroyLibrary(); }

std::wstring make_temp_path(const wchar_t* ext) {
    std::wstring d = temp_directory();
    if (d.empty()) d = L".";
#ifdef _WIN32
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    const unsigned long long unique = static_cast<unsigned long long>(t.QuadPart);
#else
    static std::atomic<unsigned> counter{0};
    const unsigned long long unique = (static_cast<unsigned long long>(tick_ms()) << 16) ^ counter.fetch_add(1);
#endif
    char name[96];
    std::snprintf(name, sizeof name, "pdf2img_%lu_%llx", current_pid(), unique);
    return path_join(d, widen(name) + (ext ? std::wstring(ext) : std::wstring(L".tmp")));
}

#ifdef _WIN32

// Read-only memory mapping of the PDF: shared by the OS page cache across all worker processes and not
// charged to the job object commit limit (unlike a private copy).
bool FileMapping::open(const std::wstring& path, std::string& err) {
    close();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        err = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? "file not found" : ("cannot open file (error " + std::to_string(e) + ")");
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (4ll << 30)) { CloseHandle(h); err = "file too large or size unavailable"; return false; }
    if (sz.QuadPart == 0) { CloseHandle(h); err = "file is empty"; return false; }
    HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) { DWORD e = GetLastError(); CloseHandle(h); err = "cannot map file (error " + std::to_string(e) + ")"; return false; }
    void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!v) { DWORD e = GetLastError(); CloseHandle(m); CloseHandle(h); err = "cannot map file view (error " + std::to_string(e) + ")"; return false; }
    file_ = h; mapping_ = m; view_ = v; size_ = static_cast<size_t>(sz.QuadPart);
    return true;
}

void FileMapping::close() {
    if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
    if (file_) { CloseHandle(file_); file_ = nullptr; }
    size_ = 0;
}

#else

// Read-only memory mapping of the PDF, shared by the page cache across all worker processes.
bool FileMapping::open(const std::wstring& path, std::string& err) {
    close();
    // O_NONBLOCK: opening a named pipe must not wait for a writer (it is rejected below as not a regular file).
    const int fd = ::open(narrow(path).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        const int e = errno;
        err = (e == ENOENT || e == ENOTDIR) ? "file not found" : ("cannot open file (" + std::string(std::strerror(e)) + ")");
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) == 0 && !S_ISREG(st.st_mode)) { ::close(fd); err = "not a regular file"; return false; }
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > (4ll << 30)) {
        ::close(fd);
        err = "file too large or size unavailable";
        return false;
    }
    if (st.st_size == 0) { ::close(fd); err = "file is empty"; return false; }
    void* v = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    const int e = errno;
    ::close(fd); // the mapping stays valid
    if (v == MAP_FAILED) { err = "cannot map file (" + std::string(std::strerror(e)) + ")"; return false; }
    view_ = v;
    size_ = static_cast<size_t>(st.st_size);
    return true;
}

void FileMapping::close() {
    if (view_) { munmap(view_, size_); view_ = nullptr; }
    size_ = 0;
}

#endif

PdfDocument::~PdfDocument() { close(); }

void PdfDocument::close() {
    if (form_) { FPDFDOC_ExitFormFillEnvironment(static_cast<FPDF_FORMHANDLE>(form_)); form_ = nullptr; }
    if (doc_) { FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(doc_)); doc_ = nullptr; }
    delete static_cast<FPDF_FORMFILLINFO*>(formInfo_);
    formInfo_ = nullptr;
    map_.close();
    pageCount_ = 0;
    xrefRebuilt_ = false;
    // repairedPath_ is intentionally kept: the caller deletes the temp file.
}

bool PdfDocument::load_from_memory(const std::string& password, unsigned long& pdfiumError) {
    FPDF_DOCUMENT d = FPDF_LoadMemDocument64(map_.data(), map_.size(), password.empty() ? nullptr : password.c_str());
    if (!d) { pdfiumError = FPDF_GetLastError(); return false; }
    doc_ = d;
    pageCount_ = FPDF_GetPageCount(d);
    xrefRebuilt_ = !FPDF_DocumentHasValidCrossReferenceTable(d);
    // Form-fill environment so that AcroForm field appearances are drawn (FPDF_FFLDraw).
    FPDF_FORMFILLINFO* fi = new FPDF_FORMFILLINFO();
    std::memset(fi, 0, sizeof(*fi));
    fi->version = 1;
    formInfo_ = fi;
    form_ = FPDFDOC_InitFormFillEnvironment(d, fi);
    return true;
}

bool PdfDocument::open(const std::wstring& path, const std::string& password, OpenError& err) {
    close();
    repairedPath_.clear(); // belongs to the previous document (the supervisor owns/deletes that file)
    std::string ioErr;
    if (!map_.open(path, ioErr)) { err = {EXIT_INPUT, ioErr + ": " + narrow(path)}; return false; }

    unsigned long e = 0;
    if (load_from_memory(password, e)) return true;
    if (e == FPDF_ERR_PASSWORD) {
        err = {EXIT_PASSWORD, password.empty() ? "PDF is password protected (use -upw): " + narrow(path) : "wrong PDF password: " + narrow(path)};
        // A qpdf pass cannot help without the right password.
        map_.close();
        return false;
    }
    // Format / security / unknown errors: try a qpdf repair pass once.
    std::wstring tmp = make_temp_path(L".pdf");
    std::string rerr;
    log::infof("PDFium could not open %s (error %lu); trying qpdf repair", narrow(path).c_str(), e);
    bool pwErr = false;
    if (qpdf_repair(path, password, tmp, rerr, &pwErr)) {
        std::string ioErr2;
        map_.close();
        if (map_.open(tmp, ioErr2)) {
            unsigned long e2 = 0;
            if (load_from_memory(password, e2)) { repairedPath_ = tmp; return true; }
            map_.close();
            if (e2 == FPDF_ERR_PASSWORD) { delete_file_quiet(tmp); err = {EXIT_PASSWORD, "PDF is password protected (use -upw): " + narrow(path)}; return false; }
        }
        delete_file_quiet(tmp);
    } else {
        log::infof("qpdf repair failed: %s", rerr.c_str());
        if (pwErr) { map_.close(); err = {EXIT_PASSWORD, password.empty() ? "PDF is password protected (use -upw): " + narrow(path) : "wrong PDF password: " + narrow(path)}; return false; }
    }
    map_.close();
    const char* why = (e == FPDF_ERR_FORMAT) ? "not a valid PDF (format error)" : (e == FPDF_ERR_SECURITY) ? "unsupported PDF security handler" : (e == FPDF_ERR_FILE) ? "cannot read file" : "cannot load PDF";
    err = {EXIT_INPUT, std::string(why) + ": " + narrow(path) + (rerr.empty() ? std::string() : " (qpdf repair also failed: " + rerr + ")")};
    return false;
}

namespace {
struct PauseCtx {
    IFSDK_PAUSE pause;
    uint64_t deadline;
    bool timedOut;
};
FPDF_BOOL need_to_pause(IFSDK_PAUSE* p) {
    PauseCtx* ctx = reinterpret_cast<PauseCtx*>(p);
    if (ctx->deadline == 0) return 0;
    if (tick_ms() >= ctx->deadline) { ctx->timedOut = true; return 1; }
    return 0;
}
} // namespace

RenderStatus PdfDocument::render_page(int index, const RenderRequest& req, uint64_t deadlineTicks, RenderResult& out, std::string& err) {
    if (!doc_) { err = "document not open"; return RenderStatus::Failed; }
    FPDF_DOCUMENT doc = static_cast<FPDF_DOCUMENT>(doc_);
    FPDF_FORMHANDLE form = static_cast<FPDF_FORMHANDLE>(form_);
    FPDF_PAGE page = FPDF_LoadPage(doc, index);
    if (!page) { err = "cannot load page " + std::to_string(index + 1) + " (PDFium error " + std::to_string(FPDF_GetLastError()) + ")"; return RenderStatus::Failed; }
    if (form) FORM_OnAfterLoadPage(page, form);

    const double wpt = FPDF_GetPageWidthF(page);
    const double hpt = FPDF_GetPageHeightF(page);
    out.pageWidthPt = wpt; out.pageHeightPt = hpt;
    int dpiX = req.dpiX > 0 ? req.dpiX : 101, dpiY = req.dpiY > 0 ? req.dpiY : 101;
    int pw = points_to_pixels(wpt, dpiX), ph = points_to_pixels(hpt, dpiY);
    const int bytesPerPixel = req.gray ? 1 : 3;
    auto bitmapBytes = [&](int w, int h) { return static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * static_cast<uint64_t>(bytesPerPixel) + static_cast<uint64_t>(h) * 4; };
    out.usedFallbackDpi = false;
    if (bitmapBytes(pw, ph) > req.maxBitmapBytes || pw > 65000 || ph > 65000) {
        int fb = req.fallbackDpi > 0 ? req.fallbackDpi : 101;
        int pw2 = points_to_pixels(wpt, fb), ph2 = points_to_pixels(hpt, fb);
        if (bitmapBytes(pw2, ph2) > req.maxBitmapBytes || pw2 > 65000 || ph2 > 65000) {
            if (form) FORM_OnBeforeClosePage(page, form);
            FPDF_ClosePage(page);
            err = "page " + std::to_string(index + 1) + " is too large to render (" + std::to_string(pw) + "x" + std::to_string(ph) + " px)";
            return RenderStatus::Failed;
        }
        // Reported to the supervisor (fallback flag in PAGEOK/PROGRESS), which logs the warning where the user sees it.
        log::infof("page %d: %dx%d px at %dx%d dpi exceeds the bitmap limit; rendering at %d dpi instead", index + 1, pw, ph, dpiX, dpiY, fb);
        dpiX = dpiY = fb; pw = pw2; ph = ph2;
        out.usedFallbackDpi = true;
    }
    out.dpiX = dpiX; out.dpiY = dpiY;

    // Render straight into the output image buffer (no second full-page copy).
    Image& img = out.image;
    try { img.allocate(pw, ph, req.gray ? 8 : 24); } catch (const std::bad_alloc&) { img = Image(); }
    img.gray = req.gray;
    img.dpiX = dpiX; img.dpiY = dpiY;
    FPDF_BITMAP bmp = img.empty() ? nullptr : FPDFBitmap_CreateEx(pw, ph, req.gray ? FPDFBitmap_Gray : FPDFBitmap_BGR, img.data.data(), static_cast<int>(img.stride));
    if (!bmp) {
        img = Image();
        if (form) FORM_OnBeforeClosePage(page, form);
        FPDF_ClosePage(page);
        err = "cannot allocate a " + std::to_string(pw) + "x" + std::to_string(ph) + " bitmap for page " + std::to_string(index + 1);
        return RenderStatus::Failed;
    }
    FPDFBitmap_FillRect(bmp, 0, 0, pw, ph, 0xFFFFFFFFu);

    int flags = FPDF_ANNOT;
    if (req.gray) flags |= FPDF_GRAYSCALE;
    if (!req.antialias) flags |= FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE | FPDF_RENDER_NO_SMOOTHPATH;

    PauseCtx ctx{};
    ctx.pause.version = 1;
    ctx.pause.NeedToPauseNow = need_to_pause;
    ctx.pause.user = nullptr;
    ctx.deadline = deadlineTicks;
    ctx.timedOut = false;

    int status = FPDF_RenderPageBitmap_Start(bmp, page, 0, 0, pw, ph, 0, flags, &ctx.pause);
    while (status == FPDF_RENDER_TOBECONTINUED) {
        if (ctx.timedOut || (deadlineTicks && tick_ms() >= deadlineTicks)) break;
        status = FPDF_RenderPage_Continue(page, &ctx.pause);
    }
    RenderStatus result = RenderStatus::Ok;
    if (status == FPDF_RENDER_TOBECONTINUED) { result = RenderStatus::Timeout; err = "page " + std::to_string(index + 1) + ": render timed out"; }
    else if (status == FPDF_RENDER_FAILED) { result = RenderStatus::Failed; err = "page " + std::to_string(index + 1) + ": render failed"; }
    FPDF_RenderPage_Close(page);

    if (result == RenderStatus::Ok) {
        if (form) FPDF_FFLDraw(form, bmp, page, 0, 0, pw, ph, 0, flags);
    } else {
        img = Image();
    }
    FPDFBitmap_Destroy(bmp);
    if (form) FORM_OnBeforeClosePage(page, form);
    FPDF_ClosePage(page);
    return result;
}

} // namespace p2i
