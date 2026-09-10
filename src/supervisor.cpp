#include "supervisor.h"
#include "common.h"
#include "fsutil.h"
#include "ipc.h"
#include "log.h"
#include "naming.h"
#include "child_process.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace p2i {

namespace {

// Grace period for idle workers to exit after QUIT at the end of a run; stragglers are then terminated together.
constexpr uint32_t kShutdownGraceMs = 500;
// Bounded wait (shared by all terminated stragglers) until they are really gone after they were killed.
constexpr uint32_t kTerminateWaitMs = 2000;
// Work (a page, a multi-page TIFF, or opening a PDF) lost to a worker that exits unexpectedly is retried once on
// another worker: the worker may have died before it even read the command, or on a transient problem.
constexpr int kMaxAttempts = 2;
// Work lost to a worker that died before it acknowledged it (BEGIN) was never attempted: it is resubmitted without
// using up its retry, at most this many times (bounds the respawning if workers keep dying before starting work).
constexpr int kMaxUnstartedLosses = 5;

struct FileTask {
    int id = 0;
    std::wstring path;          // original input
    std::wstring effectivePath; // path workers should open (repaired copy if any)
    OutputSpec out;
    int pageCount = -1;
    bool openSent = false;
    int openAttempts = 0;       // OPENs lost to a worker that exited unexpectedly
    int openUnstartedLosses = 0; // OPENs sent to a worker that died before starting them (not charged)
    bool convertingPrinted = false; // wildcard mode: "Converting <file>......" already printed
    bool opened = false;
    bool failed = false;
    int first = 0, last = 0;
    bool multi = false;
    std::wstring repairedPath;
    int unitsPending = 0;       // units queued or running
    int pagesOk = 0, pagesFailed = 0;
    bool done = false;
};

struct Unit {
    int fileId = -1;
    bool multi = false;
    int page = 0;               // PAGE
    int first = 0, last = 0;    // MULTI
    std::wstring outPath;
    int attempts = 0;           // times this unit was lost to a worker that exited unexpectedly
    int unstartedLosses = 0;    // times it was sent to a worker that died before starting it (not charged)
};

struct Worker {
    int id = 0;
    ChildProcess proc;
    unsigned long pid = 0;
    std::thread reader;
    std::atomic<bool> stopReader{false}; // set before cancelling the reader's blocking read
    std::mutex mu;
    std::deque<std::string> lines;
    bool eof = false;
    bool ready = false;
    bool busy = false;
    bool dead = false;
    bool killedByUs = false;
    bool fatalReported = false;  // worker sent FATAL (could not start) before exiting
    bool started = false;        // worker acknowledged (BEGIN) the command it is busy with
    struct PendingFailure { int code; std::string msg; }; // inside a MULTI unit: PAGEFAIL (code != 0) or warning (code 0)
    std::vector<PendingFailure> unitFailures;             // committed when the unit ends, dropped if the unit is retried
    bool opening = false;       // current operation is an OPEN (no unit)
    int openingFile = -1;
    bool hasUnit = false;
    Unit unit;
    int openFileId = -1;        // document the worker currently has open (for dispatch affinity)
    uint64_t opStart = 0;
    uint64_t lastProgress = 0;
};

class Supervisor {
public:
    Supervisor(const Options& o, const Config& c) : opts_(o), cfg_(c) {}
    int run();

private:
    // setup
    bool build_spec_lines();
    bool spawn_worker();
    void send_line(Worker& w, const std::vector<std::string>& fields);
    void kill_worker(Worker& w, const char* why);
    void reap_worker(Worker& w);
    void delete_worker_temp_files(unsigned long pid);
    void shutdown_workers();
    // loop
    void dispatch();
    void handle_line(Worker& w, const std::string& line);
    void check_timeouts();
    bool all_done() const;
    std::string describe_work(const Worker& w) const;
    void fail_unit(Worker& w, int code, const std::string& msg);
    void commit_unit_failures(Worker& w);
    void note_failure(int code, const std::string& msg);
    void enqueue_units(FileTask& f);
    void finish_file_if_done(FileTask& f);

    const Options& opts_;
    const Config& cfg_;
    std::string specBlock_;
    std::vector<FileTask> files_;
    std::deque<Unit> queue_;
    std::vector<std::unique_ptr<Worker>> workers_;
    WakeEvent wake_;
    int maxWorkers_ = 1;
    int nextWorkerId_ = 1;
    int firstFailure_ = EXIT_OK;
    uint64_t start_ = 0;
    uint64_t totalDeadline_ = 0;
    uint64_t pageBudgetMs_ = 0;
    bool printConverting_ = false;
    std::wstring exePath_;
    int spawnFailures_ = 0;
};

// ------------------------------------------------------------------ process spawning

bool Supervisor::spawn_worker() {
    auto w = std::make_unique<Worker>();
    if (!w->proc.start(exePath_, {L"--worker"}, cfg_.workerMemoryLimitMB)) return false;
    w->id = nextWorkerId_++;
    w->pid = w->proc.pid();
    Worker* wp = w.get();
    WakeEvent* wake = &wake_;
    w->reader = std::thread([wp, wake]() {
        std::string buf;
        char tmp[4096];
        while (!wp->stopReader) {
            size_t got = 0;
            if (!wp->proc.read(tmp, sizeof tmp, got, wp->stopReader)) break;
            buf.append(tmp, got);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                { std::lock_guard<std::mutex> lock(wp->mu); wp->lines.push_back(std::move(line)); }
                wake->set();
            }
        }
        { std::lock_guard<std::mutex> lock(wp->mu); wp->eof = true; }
        wake->set();
    });
    // Send the spec block right away.
    unsigned long err = 0;
    w->proc.write(specBlock_, err);
    log::infof("started worker %d (pid %lu)", w->id, w->pid);
    workers_.push_back(std::move(w));
    return true;
}

void Supervisor::send_line(Worker& w, const std::vector<std::string>& fields) {
    std::string line = ipc::join(fields);
    unsigned long err = 0;
    if (!w.proc.write(line, err)) log::infof("worker %d: write failed (error %lu)", w.id, err);
}

void Supervisor::reap_worker(Worker& w) {
    // Final teardown of a worker whose process has ended (or been terminated): join the reader, close handles,
    // discard any late messages, remove partial outputs and qpdf temp copies, mark dead.
    if (w.dead) return;
    stop_reader_thread(w.reader, w.stopReader, w.proc);
    { std::lock_guard<std::mutex> lock(w.mu); w.lines.clear(); w.eof = true; }
    w.proc.close();
    if (w.hasUnit) delete_file_quiet(temp_name_for(w.unit.outPath, w.pid));
    delete_worker_temp_files(w.pid);
    w.dead = true;
}

void Supervisor::kill_worker(Worker& w, const char* why) {
    if (w.dead) return;
    log::infof("terminating worker %d (%s)", w.id, why);
    w.killedByUs = true;
    w.proc.terminate();
    w.proc.wait(5000);
    reap_worker(w);
}

// qpdf repair copies are named "<temp>/pdf2img_<pid>_*.pdf" (see make_temp_path); remove those of a finished worker.
void Supervisor::delete_worker_temp_files(unsigned long pid) {
    const std::wstring dir = temp_directory();
    if (dir.empty()) return;
    for (const std::wstring& p : find_files(dir, L"pdf2img_" + std::to_wstring(pid) + L"_*.pdf")) {
        bool inUse = false;
        for (const auto& f : files_) if (!f.done && !f.repairedPath.empty() && same_path(f.repairedPath, p)) { inUse = true; break; }
        if (!inUse) delete_file_quiet(p); // in-use repaired copies are deleted by finish_file_if_done()
    }
}

void Supervisor::shutdown_workers() {
    // All work is finished or abandoned. Every page a worker reported was already written and renamed into place,
    // so idle workers hold nothing that must be flushed: ask all of them to quit at once, allow one short shared
    // grace period, then terminate the stragglers together (e.g. workers still starting up, or slowed down by an
    // antivirus scan). Waiting for each worker in turn (up to 5 s each) could add many seconds to a run.
    for (auto& w : workers_) if (!w->dead) { send_line(*w, {"QUIT"}); w->proc.close_stdin(); }
    const uint64_t deadline = tick_ms() + kShutdownGraceMs;
    std::vector<Worker*> terminated;
    for (auto& w : workers_) {
        if (w->dead) continue;
        const uint64_t now = tick_ms();
        const uint32_t wait = now < deadline ? static_cast<uint32_t>(deadline - now) : 0;
        if (!w->proc.wait(wait)) {
            w->killedByUs = true;
            w->proc.terminate();
            terminated.push_back(w.get());
        }
    }
    // Termination is asynchronous: wait once (shared, bounded) until the terminated workers are really gone, so they
    // no longer map the input PDF or a repair copy when temp files are deleted and when this process returns.
    const uint64_t termDeadline = tick_ms() + kTerminateWaitMs;
    for (Worker* w : terminated) {
        const uint64_t now = tick_ms();
        w->proc.wait(now < termDeadline ? static_cast<uint32_t>(termDeadline - now) : 0);
    }
    for (auto& w : workers_) reap_worker(*w);
}

// ------------------------------------------------------------------ job bookkeeping

// Exit-code aggregation is deterministic regardless of worker timing: the most severe code wins.
static int failure_rank(int code) {
    switch (code) {
    case EXIT_INTERNAL: return 7;
    case EXIT_TIMEOUT: return 6;
    case EXIT_OUTPUT: return 5;
    case EXIT_RENDER: return 4;
    case EXIT_PASSWORD: return 3;
    case EXIT_INPUT: return 2;
    case EXIT_UNSUPPORTED_FORMAT: return 1;
    default: return 0;
    }
}

void Supervisor::note_failure(int code, const std::string& msg) {
    if (code != EXIT_OK && failure_rank(code) > failure_rank(firstFailure_)) firstFailure_ = code;
    if (!msg.empty()) log::error(msg);
}

void Supervisor::enqueue_units(FileTask& f) {
    resolve_page_range(opts_.firstPage, opts_.lastPage, f.pageCount, f.first, f.last);
    const int pages = f.last - f.first + 1;
    f.multi = multipage_applies(f.out.format, opts_.multipage);
    if (f.multi) {
        Unit u; u.fileId = f.id; u.multi = true; u.first = f.first; u.last = f.last; u.outPath = plain_output_path(f.out);
        queue_.push_back(u);
        f.unitsPending = 1;
    } else {
        const bool suffix = uses_page_suffix(f.out.format, false, pages);
        for (int p = f.first; p <= f.last; ++p) {
            Unit u; u.fileId = f.id; u.page = p;
            u.outPath = suffix ? page_output_path(f.out, p, cfg_.suffixFormat) : plain_output_path(f.out);
            queue_.push_back(u);
        }
        f.unitsPending = pages;
    }
    log::infof("%s: %d page(s), converting %d..%d -> %s", narrow(f.path).c_str(), f.pageCount, f.first, f.last, narrow(plain_output_path(f.out)).c_str());
}

void Supervisor::finish_file_if_done(FileTask& f) {
    if (f.done) return;
    if (f.failed || (f.opened && f.unitsPending == 0)) {
        f.done = true;
        if (!f.repairedPath.empty()) { delete_file_quiet(f.repairedPath); f.repairedPath.clear(); }
    }
}

// Applies the page failures / warnings buffered while a multi-page TIFF was being written.
void Supervisor::commit_unit_failures(Worker& w) {
    for (const auto& pf : w.unitFailures) {
        if (pf.code == 0) { log::warn(pf.msg); continue; }
        if (w.unit.fileId >= 0 && w.unit.fileId < static_cast<int>(files_.size())) files_[static_cast<size_t>(w.unit.fileId)].pagesFailed++;
        note_failure(pf.code, pf.msg);
    }
    w.unitFailures.clear();
}

void Supervisor::fail_unit(Worker& w, int code, const std::string& msg) {
    if (w.opening) {
        FileTask& f = files_[static_cast<size_t>(w.openingFile)];
        f.failed = true;
        note_failure(code, msg);
        finish_file_if_done(f);
        w.opening = false; w.openingFile = -1;
    } else if (w.hasUnit) {
        FileTask& f = files_[static_cast<size_t>(w.unit.fileId)];
        if (!w.unit.multi) f.pagesFailed++; // MULTI page failures are counted from PAGEFAIL lines
        else commit_unit_failures(w);
        f.unitsPending--;
        note_failure(code, msg);
        finish_file_if_done(f);
        w.hasUnit = false;
    }
    w.busy = false;
}

bool Supervisor::all_done() const {
    for (const auto& f : files_) if (!f.done) return false;
    return true;
}

// What a busy worker is doing, for messages.
std::string Supervisor::describe_work(const Worker& w) const {
    if (w.opening) return "opening " + narrow(files_[static_cast<size_t>(w.openingFile)].path);
    if (w.unit.multi) return "multi-page conversion of " + narrow(files_[static_cast<size_t>(w.unit.fileId)].path);
    return "page " + std::to_string(w.unit.page) + " of " + narrow(files_[static_cast<size_t>(w.unit.fileId)].path);
}

// ------------------------------------------------------------------ dispatch

void Supervisor::dispatch() {
    // Assign work to idle workers.
    for (auto& wp : workers_) {
        Worker& w = *wp;
        if (w.dead || !w.ready || w.busy) continue;
        if (!queue_.empty()) {
            // Prefer a unit of the document this worker already has open (avoids re-opening the PDF).
            auto it = queue_.begin();
            if (w.openFileId >= 0) {
                auto pref = std::find_if(queue_.begin(), queue_.end(), [&](const Unit& q) { return q.fileId == w.openFileId; });
                if (pref != queue_.end()) it = pref;
            }
            Unit u = *it; queue_.erase(it);
            w.openFileId = u.fileId;
            FileTask& f = files_[static_cast<size_t>(u.fileId)];
            w.busy = true; w.hasUnit = true; w.unit = u; w.started = false; w.unitFailures.clear(); w.opStart = w.lastProgress = tick_ms();
            if (u.multi) send_line(w, {"MULTI", std::to_string(u.fileId), narrow(f.effectivePath), std::to_string(u.first), std::to_string(u.last), narrow(u.outPath)});
            else send_line(w, {"PAGE", std::to_string(u.fileId), narrow(f.effectivePath), std::to_string(u.page), narrow(u.outPath)});
            continue;
        }
        // Nothing queued: open the next file that has not been opened yet.
        for (auto& f : files_) {
            if (f.openSent || f.done) continue;
            f.openSent = true;
            w.busy = true; w.opening = true; w.openingFile = f.id; w.started = false; w.opStart = w.lastProgress = tick_ms();
            if (printConverting_ && !f.convertingPrinted) { f.convertingPrinted = true; log::raw_stdout("Converting " + narrow(f.path) + "......\n"); }
            send_line(w, {"OPEN", std::to_string(f.id), narrow(f.effectivePath)});
            break;
        }
    }
    // Spawn more workers when there is more queued work than idle capacity.
    int alive = 0, idle = 0;
    for (auto& w : workers_) if (!w->dead) { ++alive; if (!w->busy) ++idle; } // not-yet-ready workers count as capacity
    int pendingOpens = 0;
    for (auto& f : files_) if (!f.openSent && !f.done) ++pendingOpens;
    int wanted = static_cast<int>(queue_.size()) + pendingOpens;
    while (alive < maxWorkers_ && wanted > idle && spawnFailures_ < 3) {
        if (!spawn_worker()) { ++spawnFailures_; break; }
        ++alive; ++idle;
    }
    if (alive == 0 && spawnFailures_ >= 3) {
        // Cannot run any worker: fail everything.
        for (auto& f : files_) if (!f.done) { f.failed = true; finish_file_if_done(f); }
        note_failure(EXIT_INTERNAL, "no worker process could be started");
    }
}

void Supervisor::handle_line(Worker& w, const std::string& line) {
    auto f = ipc::split(line);
    if (f.empty()) return;
    const std::string& cmd = f[0];
    auto fid = [&](size_t i) { return (f.size() > i) ? atoi(f[i].c_str()) : -1; };

    if (cmd == "READY") { w.ready = true; return; }
    if (cmd == "BEGIN") { w.started = true; w.lastProgress = tick_ms(); return; }
    if (cmd == "FATAL" && f.size() >= 2) {
        // A worker that cannot start reports why (e.g. pdfium.dll not loadable, with the Win32 error code).
        log::errorf("worker process %lu: %s", w.pid, f[1].c_str());
        w.fatalReported = true;
        return;
    }

    if (cmd == "OPENED" && f.size() >= 5) {
        int id = fid(1);
        if (id < 0 || id >= static_cast<int>(files_.size())) return;
        FileTask& ft = files_[static_cast<size_t>(id)];
        ft.pageCount = atoi(f[2].c_str());
        if (!f[3].empty()) {
            ft.repairedPath = widen(f[3]); ft.effectivePath = ft.repairedPath;
            // Output comes from a recovered copy of a damaged PDF and may be incomplete: worth a warning (exit code 0).
            log::warnf("%s: the PDF is damaged; it was repaired with qpdf before rendering, check the output pages", narrow(ft.path).c_str());
        }
        if (f[4] == "1") log::infof("%s: cross-reference table was rebuilt", narrow(ft.path).c_str());
        ft.opened = true;
        w.openFileId = id;
        w.busy = false; w.opening = false; w.openingFile = -1;
        if (ft.pageCount < 1) { ft.failed = true; note_failure(EXIT_INPUT, "PDF has no pages: " + narrow(ft.path)); finish_file_if_done(ft); return; }
        enqueue_units(ft);
        return;
    }
    if (cmd == "OPENFAIL" && f.size() >= 4) {
        int id = fid(1);
        if (id < 0 || id >= static_cast<int>(files_.size())) return;
        FileTask& ft = files_[static_cast<size_t>(id)];
        ft.failed = true;
        w.busy = false; w.opening = false; w.openingFile = -1;
        note_failure(atoi(f[2].c_str()), f[3]);
        finish_file_if_done(ft);
        return;
    }
    if (cmd == "PAGEOK" && f.size() >= 7) {
        int id = fid(1);
        if (id >= 0 && id < static_cast<int>(files_.size())) {
            FileTask& ft = files_[static_cast<size_t>(id)];
            ft.pagesOk++; ft.unitsPending--;
            log::infof("page %s of %s -> %s (%s px, %s ms%s)", f[2].c_str(), narrow(ft.path).c_str(), f[5].c_str(), f[3].c_str(), f[4].c_str(), f[6] == "1" ? ", fallback dpi" : "");
            if (f[6] == "1")
                log::warnf("%s: page %s is too large to render at the requested resolution; it was rendered at %d dpi instead (%s px)",
                           narrow(ft.path).c_str(), f[2].c_str(), cfg_.defaultDpi, f[3].c_str());
            finish_file_if_done(ft);
        }
        w.busy = false; w.hasUnit = false;
        return;
    }
    if (cmd == "PAGEFAIL" && f.size() >= 5) {
        int id = fid(1);
        int code = atoi(f[3].c_str());
        std::string msg = (id >= 0 && id < static_cast<int>(files_.size())) ? narrow(files_[static_cast<size_t>(id)].path) + ": page " + f[2] + ": " + f[4] : f[4];
        if (w.hasUnit && w.unit.multi) {
            // a page inside a multi-page TIFF failed; the unit continues
            // Committed when the TIFF finishes or finally fails; dropped if the unit is retried after a worker crash.
            w.unitFailures.push_back({code, msg});
            w.lastProgress = tick_ms();
            return;
        }
        fail_unit(w, code, msg);
        return;
    }
    if (cmd == "PROGRESS") {
        w.lastProgress = tick_ms();
        int id = fid(1);
        if (f.size() >= 4 && f[3] == "1" && id >= 0 && id < static_cast<int>(files_.size()))
            w.unitFailures.push_back({0, narrow(files_[static_cast<size_t>(id)].path) + ": page " + f[2] +
                                         " is too large to render at the requested resolution; it was rendered at " +
                                         std::to_string(cfg_.defaultDpi) + " dpi instead"});
        return;
    }
    if (cmd == "MULTIOK" && f.size() >= 6) {
        int id = fid(1);
        commit_unit_failures(w);
        if (id >= 0 && id < static_cast<int>(files_.size())) {
            FileTask& ft = files_[static_cast<size_t>(id)];
            ft.pagesOk += atoi(f[2].c_str());
            ft.unitsPending--;
            log::infof("%s -> %s (%s pages, %s failed, %s ms)", narrow(ft.path).c_str(), f[5].c_str(), f[2].c_str(), f[3].c_str(), f[4].c_str());
            finish_file_if_done(ft);
        }
        w.busy = false; w.hasUnit = false;
        return;
    }
    if (cmd == "MULTIFAIL" && f.size() >= 4) {
        int id = fid(1);
        std::string msg = (id >= 0 && id < static_cast<int>(files_.size())) ? narrow(files_[static_cast<size_t>(id)].path) + ": " + f[3] : f[3];
        fail_unit(w, atoi(f[2].c_str()), msg);
        return;
    }
}

void Supervisor::check_timeouts() {
    const uint64_t now = tick_ms();
    if (totalDeadline_ && now >= totalDeadline_) {
        note_failure(EXIT_TIMEOUT, "total time limit exceeded; aborting");
        for (auto& w : workers_) if (!w->dead && w->busy) { commit_unit_failures(*w); kill_worker(*w, "total timeout"); }
        for (auto& f : files_) if (!f.done) {
            // Name every input left incomplete, so an errors-only log shows which outputs are missing.
            note_failure(EXIT_TIMEOUT, narrow(f.path) + ": not completed (total time limit exceeded; " + std::to_string(f.pagesOk) + " page(s) written)");
            f.failed = true; finish_file_if_done(f);
        }
        queue_.clear();
        return;
    }
#ifndef _WIN32
    // Linux/macOS have no Job Object memory limit: a busy worker whose resident memory exceeds the limit is terminated.
    if (cfg_.workerMemoryLimitMB > 0) {
        const uint64_t limit = static_cast<uint64_t>(cfg_.workerMemoryLimitMB) << 20;
        for (auto& wp : workers_) {
            Worker& w = *wp;
            if (w.dead || !w.busy || w.killedByUs) continue;
            if (w.proc.resident_bytes() <= limit) continue;
            const std::string what = describe_work(w);
            kill_worker(w, "memory limit exceeded");
            fail_unit(w, EXIT_RENDER, what + ": the worker process exceeded the memory limit (" + std::to_string(cfg_.workerMemoryLimitMB) + " MB); worker terminated");
        }
    }
#endif
    if (pageBudgetMs_ == 0) return;
    for (auto& wp : workers_) {
        Worker& w = *wp;
        if (w.dead || !w.busy || w.killedByUs) continue;
        if (now - w.lastProgress > pageBudgetMs_) {
            const std::string what = describe_work(w);
            kill_worker(w, "page time limit exceeded");
            fail_unit(w, EXIT_TIMEOUT, what + ": time limit exceeded (" + std::to_string(pageBudgetMs_ / 1000) + " s); worker terminated");
        }
    }
}

// ------------------------------------------------------------------ spec

bool Supervisor::build_spec_lines() {
    // Output format is per file, but all files of one run share -o's extension (or .tif); use the first job's.
    OutFormat fmt = files_.empty() ? OutFormat::Tiff : files_[0].out.format;
    std::string s;
    auto add = [&](const char* k, const std::string& v) { s += ipc::join({"SPEC", k, v}); };
    add("format", format_name(fmt));
    add("dpix", std::to_string(opts_.dpiX));
    add("dpiy", std::to_string(opts_.dpiY));
    add("bitcount", std::to_string(opts_.bitCount));
    add("gray", opts_.grayscale ? "1" : "0");
    add("compression", compression_name(opts_.compression));
    add("quality", std::to_string(opts_.quality));
    add("defaultdpi", std::to_string(cfg_.defaultDpi));
    add("defaultquality", std::to_string(cfg_.jpegQuality));
    add("lzwpredictor", cfg_.lzwPredictor ? "1" : "0");
    add("antialias", cfg_.antialias ? "1" : "0");
    add("maxbitmap", std::to_string(cfg_.maxBitmapBytes));
    int pageTimeout = opts_.pageTimeoutSec >= 0 ? opts_.pageTimeoutSec : cfg_.pageTimeoutSec;
    add("pagetimeout", std::to_string(pageTimeout));
    if (!opts_.userPassword.empty()) add("password", opts_.userPassword);
    if (!opts_.ownerPassword.empty()) add("password", opts_.ownerPassword);
    s += ipc::join({"SPECEND"});
    specBlock_ = s;
    // cooperative budget + grace for the hard kill (5..15 s, scaled with the budget)
    pageTimeout = std::clamp(pageTimeout, 0, 86400);
    const uint64_t graceMs = static_cast<uint64_t>(std::clamp(pageTimeout * 250, 5000, 15000));
    pageBudgetMs_ = pageTimeout > 0 ? static_cast<uint64_t>(pageTimeout) * 1000ull + graceMs : 0;
    return true;
}

// ------------------------------------------------------------------ run

int Supervisor::run() {
    start_ = tick_ms();
    int totalTimeout = std::clamp(opts_.totalTimeoutSec >= 0 ? opts_.totalTimeoutSec : cfg_.totalTimeoutSec, 0, 30 * 86400);
    totalDeadline_ = totalTimeout > 0 ? start_ + static_cast<uint64_t>(totalTimeout) * 1000ull : 0;

    // Resolve inputs/outputs.
    std::vector<JobProblem> problems;
    std::vector<InputJob> jobs = resolve_jobs(opts_, problems);
    for (const auto& p : problems) note_failure(p.exitCode, p.message);
    printConverting_ = has_wildcard(opts_.input);
    for (const auto& j : jobs) {
        if (j.out.format == OutFormat::Unknown || j.out.format == OutFormat::Wmf || j.out.format == OutFormat::Emf) continue; // already reported
        FileTask f;
        f.id = static_cast<int>(files_.size());
        f.path = j.inputPath;
        f.effectivePath = j.inputPath;
        f.out = j.out;
        std::string derr;
        if (!ensure_directory(f.out.dir, derr)) { note_failure(EXIT_OUTPUT, derr); continue; }
        files_.push_back(std::move(f));
    }
    if (files_.empty()) return firstFailure_ == EXIT_OK ? EXIT_INPUT : firstFailure_;

    exePath_ = exe_path();
    maxWorkers_ = opts_.workers > 0 ? opts_.workers : (cfg_.workers > 0 ? cfg_.workers : static_cast<int>(std::min<unsigned>(8, hardware_threads())));
    maxWorkers_ = std::max(1, std::min(maxWorkers_, 32));
    build_spec_lines();

    while (true) {
        dispatch();
        if (all_done()) break;
        bool anyAlive = false;
        for (auto& w : workers_) if (!w->dead) anyAlive = true;
        if (!anyAlive && queue_.empty()) {
            bool pending = false;
            for (auto& f : files_) if (!f.done) pending = true;
            if (!pending) break;
        }
        wake_.wait(100);
        // Drain worker messages.
        for (auto& wp : workers_) {
            Worker& w = *wp;
            if (w.dead) continue;
            std::deque<std::string> lines; bool eof = false;
            { std::lock_guard<std::mutex> lock(w.mu); lines.swap(w.lines); eof = w.eof; }
            for (const auto& l : lines) handle_line(w, l);
            if (eof) {
                if (w.busy) {
                    const std::string what = describe_work(w);
                    w.proc.wait(5000);
                    const std::string how = w.proc.exit_description();
                    reap_worker(w); // also removes partial output + qpdf temp copies
                    const std::string why = what + ": worker process exited unexpectedly (" + how + ")";
                    if (w.killedByUs) {
                        w.busy = false; w.hasUnit = false; w.opening = false;
                    } else if (!w.started && (w.opening ? files_[static_cast<size_t>(w.openingFile)].openUnstartedLosses++
                                                        : w.unit.unstartedLosses++) < kMaxUnstartedLosses) {
                        // Died before acknowledging the command (e.g. right after reporting its previous result): the
                        // work was never attempted, so resubmit it without using up its retry.
                        log::warnf("%s before it started this work; resubmitting it", why.c_str());
                        if (w.opening) { files_[static_cast<size_t>(w.openingFile)].openSent = false; w.opening = false; w.openingFile = -1; }
                        else { w.unitFailures.clear(); queue_.push_back(w.unit); w.hasUnit = false; }
                        w.busy = false;
                    } else if (w.opening && ++files_[static_cast<size_t>(w.openingFile)].openAttempts < kMaxAttempts) {
                        log::warnf("%s; retrying on another worker", why.c_str());
                        files_[static_cast<size_t>(w.openingFile)].openSent = false;
                        w.busy = false; w.opening = false; w.openingFile = -1;
                    } else if (!w.opening && w.hasUnit && ++w.unit.attempts < kMaxAttempts) {
                        log::warnf("%s; retrying on another worker", why.c_str());
                        w.unitFailures.clear(); // the retry renders the whole unit again
                        queue_.push_front(w.unit);
                        w.busy = false; w.hasUnit = false;
                    } else {
                        fail_unit(w, EXIT_INTERNAL, why + "; it also failed on a retry");
                    }
                } else {
                    w.proc.wait(5000);
                    const std::string how = w.proc.exit_description();
                    if (!w.killedByUs && w.ready)
                        log::warnf("worker process %lu exited unexpectedly while idle (%s); the pages it reported are complete and the remaining work continues",
                                   w.pid, how.c_str());
                    if (!w.ready) {
                        // Died before becoming ready (e.g. the PDFium library missing/corrupt): treat like a spawn failure so we do not respawn forever.
                        spawnFailures_ = 3; // deterministic environment problem: do not retry
                        if (w.fatalReported)
                            log::errorf("worker process %lu could not start (%s); see the previous error", w.pid, how.c_str());
                        else
                            log::errorf("worker process %lu exited before it became ready (%s); check that %s is present next to the executable",
                                        w.pid, how.c_str(), kPdfiumLibrary);
                    }
                    reap_worker(w);
                }
            }
        }
        check_timeouts();
    }

    shutdown_workers();
    for (auto& f : files_) if (!f.repairedPath.empty()) delete_file_quiet(f.repairedPath);

    int ok = 0, failedPages = 0;
    for (const auto& f : files_) { ok += f.pagesOk; failedPages += f.pagesFailed; }
    log::infof("done: %d page(s) written, %d failed, %llu ms", ok, failedPages, static_cast<unsigned long long>(tick_ms() - start_));
    return firstFailure_;
}

} // namespace

int run_supervisor(const Options& opts, const Config& cfg) {
    Supervisor s(opts, cfg);
    return s.run();
}

} // namespace p2i
