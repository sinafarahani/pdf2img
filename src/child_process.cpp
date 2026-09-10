#include "child_process.h"
#include "common.h"
#include "log.h"

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
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
extern char** environ;
#endif

#include <cstdio>

namespace p2i {

#ifdef _WIN32

namespace {
struct InheritableHandle {
    HANDLE h = nullptr;
    ~InheritableHandle() { if (h) CloseHandle(h); }
};
} // namespace

bool ChildProcess::start(const std::wstring& exe, const std::vector<std::wstring>& args, int memoryLimitMB) {
    close();
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0)) { log::error("cannot create pipe"); return false; }
    if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); log::error("cannot create pipe"); return false; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    // stderr for the child: an inheritable duplicate of ours (or NUL).
    InheritableHandle err;
    HANDLE myErr = GetStdHandle(STD_ERROR_HANDLE);
    if (myErr && myErr != INVALID_HANDLE_VALUE) {
        DuplicateHandle(GetCurrentProcess(), myErr, GetCurrentProcess(), &err.h, 0, TRUE, DUPLICATE_SAME_ACCESS);
    }
    if (!err.h) err.h = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);

    HANDLE handles[3] = {inR, outW, err.h};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool attrOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = err.h;
    si.lpAttributeList = attrOk ? attrs : nullptr;

    std::wstring cmd = L"\"" + exe + L"\"";
    for (const auto& a : args) { cmd += L' '; cmd += a; } // internal switches only (no quoting needed)
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD cpErr = ok ? 0 : GetLastError();
    if (attrOk) DeleteProcThreadAttributeList(attrs);
    CloseHandle(inR); CloseHandle(outW);
    if (!ok) {
        CloseHandle(inW); CloseHandle(outR);
        log::errorf("cannot start worker process (error %lu)", cpErr);
        return false;
    }

    // Job object: memory limit, kill on close, no WER dialogs.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (memoryLimitMB > 0) {
            lim.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            lim.ProcessMemoryLimit = static_cast<SIZE_T>(memoryLimitMB) << 20;
        }
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            log::infof("AssignProcessToJobObject failed (error %lu); worker runs without a job object", GetLastError());
            CloseHandle(job); job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    proc_ = pi.hProcess; job_ = job; stdin_ = inW; stdout_ = outR; pid_ = pi.dwProcessId;
    return true;
}

bool ChildProcess::write(std::string_view data, unsigned long& error) {
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(stdin_), data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
        error = GetLastError();
        return false;
    }
    error = 0;
    return true;
}

void ChildProcess::close_stdin() {
    if (stdin_) { CloseHandle(static_cast<HANDLE>(stdin_)); stdin_ = nullptr; }
}

bool ChildProcess::read(char* buf, size_t size, size_t& got, const std::atomic<bool>&) {
    DWORD n = 0;
    got = 0;
    if (!ReadFile(static_cast<HANDLE>(stdout_), buf, static_cast<DWORD>(size), &n, nullptr) || n == 0) return false;
    got = n;
    return true;
}

bool ChildProcess::wait(uint32_t ms) {
    if (!proc_) return true;
    return WaitForSingleObject(static_cast<HANDLE>(proc_), ms) == WAIT_OBJECT_0;
}

void ChildProcess::terminate() {
    if (job_) TerminateJobObject(static_cast<HANDLE>(job_), 1);
    if (proc_) TerminateProcess(static_cast<HANDLE>(proc_), 1);
}

std::string ChildProcess::exit_description() {
    DWORD code = 0;
    if (proc_) GetExitCodeProcess(static_cast<HANDLE>(proc_), &code);
    char buf[32];
    std::snprintf(buf, sizeof buf, "code 0x%lX", static_cast<unsigned long>(code));
    return buf;
}

uint64_t ChildProcess::resident_bytes() const { return 0; }

void ChildProcess::close() {
    if (stdin_) { CloseHandle(static_cast<HANDLE>(stdin_)); stdin_ = nullptr; }
    if (stdout_) { CloseHandle(static_cast<HANDLE>(stdout_)); stdout_ = nullptr; }
    if (job_) { CloseHandle(static_cast<HANDLE>(job_)); job_ = nullptr; }
    if (proc_) { CloseHandle(static_cast<HANDLE>(proc_)); proc_ = nullptr; }
}

void stop_reader_thread(std::thread& reader, std::atomic<bool>& stop, ChildProcess& proc) {
    if (!reader.joinable()) return;
    stop = true;
    HANDLE th = static_cast<HANDLE>(reader.native_handle());
    while (WaitForSingleObject(th, 0) == WAIT_TIMEOUT) {
        // Once the process has exited the pipe closes and the reader ends by itself; give it a moment first.
        HANDLE ph = static_cast<HANDLE>(proc.native_handle());
        if (ph && WaitForSingleObject(ph, 0) == WAIT_OBJECT_0 && WaitForSingleObject(th, 50) == WAIT_OBJECT_0) break;
        CancelSynchronousIo(th);
        WaitForSingleObject(th, 10);
    }
    reader.join();
}

WakeEvent::WakeEvent() : h_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
WakeEvent::~WakeEvent() { if (h_) CloseHandle(static_cast<HANDLE>(h_)); }
void WakeEvent::set() { SetEvent(static_cast<HANDLE>(h_)); }
void WakeEvent::wait(uint32_t ms) { WaitForSingleObject(static_cast<HANDLE>(h_), ms); }

#else // ---------------------------------------------------------------- Linux / macOS

namespace {
// A pipe whose descriptors are close-on-exec and above 2 (so that dup2 onto 0/1 in the child always clears the flag).
bool make_pipe(int fds[2]) {
    if (::pipe(fds) != 0) return false;
    for (int k = 0; k < 2; ++k) {
        if (fds[k] <= 2) {
            const int moved = ::fcntl(fds[k], F_DUPFD_CLOEXEC, 3);
            ::close(fds[k]);
            if (moved < 0) { ::close(fds[1 - k]); return false; }
            fds[k] = moved;
        } else {
            ::fcntl(fds[k], F_SETFD, FD_CLOEXEC);
        }
    }
    return true;
}
} // namespace

bool ChildProcess::start(const std::wstring& exe, const std::vector<std::wstring>& args, int /*memoryLimitMB*/) {
    close();
    int inPipe[2], outPipe[2];
    if (!make_pipe(inPipe)) { log::error("cannot create pipe"); return false; }
    if (!make_pipe(outPipe)) { ::close(inPipe[0]); ::close(inPipe[1]); log::error("cannot create pipe"); return false; }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    const std::string exe8 = narrow(exe);
    std::vector<std::string> argv8{exe8};
    for (const auto& a : args) argv8.push_back(narrow(a));
    std::vector<char*> argv;
    for (auto& a : argv8) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe8.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    if (rc != 0) {
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        log::errorf("cannot start worker process (%s)", std::strerror(rc));
        return false;
    }
    pid_ = static_cast<unsigned long>(pid);
    stdin_ = inPipe[1];
    stdout_ = outPipe[0];
    exited_ = false;
    status_ = 0;
    return true;
}

bool ChildProcess::write(std::string_view data, unsigned long& error) {
    error = 0;
    if (stdin_ < 0) { error = EBADF; return false; }
    const char* p = data.data();
    size_t n = data.size();
    while (n > 0) {
        const ssize_t w = ::write(stdin_, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            error = static_cast<unsigned long>(errno);
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

void ChildProcess::close_stdin() {
    if (stdin_ >= 0) { ::close(stdin_); stdin_ = -1; }
}

bool ChildProcess::read(char* buf, size_t size, size_t& got, const std::atomic<bool>& stop) {
    got = 0;
    while (!stop) {
        pollfd pfd{};
        pfd.fd = stdout_;
        pfd.events = POLLIN;
        const int r = ::poll(&pfd, 1, 50);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) continue;
        const ssize_t n = ::read(stdout_, buf, size);
        if (n > 0) { got = static_cast<size_t>(n); return true; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false; // end of file or error
    }
    return false;
}

bool ChildProcess::wait(uint32_t ms) {
    if (pid_ == 0 || exited_) return true;
    const uint64_t deadline = tick_ms() + ms;
    for (;;) {
        int st = 0;
        const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &st, WNOHANG);
        if (r == static_cast<pid_t>(pid_)) { exited_ = true; status_ = st; return true; }
        if (r < 0 && errno != EINTR) { exited_ = true; return true; } // ECHILD: nothing left to wait for
        if (tick_ms() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ChildProcess::terminate() {
    if (pid_ != 0 && !exited_) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

std::string ChildProcess::exit_description() {
    if (!exited_) wait(0);
    if (!exited_) return "still running";
    if (WIFEXITED(status_)) return "exit code " + std::to_string(WEXITSTATUS(status_));
    if (WIFSIGNALED(status_)) return "signal " + std::to_string(WTERMSIG(status_));
    return "status " + std::to_string(status_);
}

uint64_t ChildProcess::resident_bytes() const {
    if (pid_ == 0 || exited_) return 0;
    // Private memory only, like the Windows job limit: file-backed pages (the memory-mapped input PDF, the PDFium
    // library) are not counted.
#if defined(__APPLE__)
    // Physical footprint (what Activity Monitor calls "Memory"); the resident size only as a fallback.
    rusage_info_v2 ri{};
    if (proc_pid_rusage(static_cast<int>(pid_), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) == 0 && ri.ri_phys_footprint > 0)
        return ri.ri_phys_footprint;
    proc_taskinfo ti{};
    if (proc_pidinfo(static_cast<int>(pid_), PROC_PIDTASKINFO, 0, &ti, sizeof ti) <= 0) return 0;
    return ti.pti_resident_size;
#else
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%lu/statm", pid_);
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0, shared = 0; // pages; "shared" = resident file-backed pages
    const int n = std::fscanf(f, "%llu %llu %llu", &size, &resident, &shared);
    std::fclose(f);
    if (n != 3) return 0;
    const unsigned long long privatePages = resident > shared ? resident - shared : 1;
    return static_cast<uint64_t>(privatePages) * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

void ChildProcess::close() {
    if (stdin_ >= 0) { ::close(stdin_); stdin_ = -1; }
    if (stdout_ >= 0) { ::close(stdout_); stdout_ = -1; }
    if (pid_ != 0 && !exited_) wait(0); // reap it if it has already exited
}

void stop_reader_thread(std::thread& reader, std::atomic<bool>& stop, ChildProcess&) {
    if (!reader.joinable()) return;
    stop = true; // ChildProcess::read() polls the flag every 50 ms
    reader.join();
}

WakeEvent::WakeEvent() = default;
WakeEvent::~WakeEvent() = default;

void WakeEvent::set() {
    { std::lock_guard<std::mutex> lock(mu_); signaled_ = true; }
    cv_.notify_one();
}

void WakeEvent::wait(uint32_t ms) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return signaled_; });
    signaled_ = false;
}

#endif

} // namespace p2i
