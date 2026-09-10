// Worker child processes: stdin/stdout pipes, stderr inherited.
// Windows: the child runs in a Job Object (memory limit, killed together with the job, no error dialogs).
// Linux/macOS: posix_spawn; the supervisor watches the child's resident memory (resident_bytes()) instead.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <condition_variable>
#include <mutex>
#endif

namespace p2i {

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { close(); }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts `exe` with the arguments `args` (argv[1..]). memoryLimitMB > 0 sets the Job Object limit on Windows.
    // Failures are logged.
    bool start(const std::wstring& exe, const std::vector<std::wstring>& args, int memoryLimitMB);
    unsigned long pid() const { return pid_; }

    // Writes all of `data` to the child's stdin; on failure `error` is the OS error code.
    bool write(std::string_view data, unsigned long& error);
    void close_stdin();
    // Blocking read from the child's stdout. False at end of file, on error, or once `stop` is set (checked every
    // 50 ms on Linux/macOS; on Windows a blocked read is cancelled by stop_reader_thread()).
    bool read(char* buf, size_t size, size_t& got, const std::atomic<bool>& stop);

    // Waits up to `ms` milliseconds (0 = just check). True once the process has exited (or was never started).
    bool wait(uint32_t ms);
    // Kills the process (Windows: the whole job). Asynchronous: use wait() to know when it is gone.
    void terminate();
    // How the process ended, for messages: "code 0xC0000005" (Windows), "exit code 3" / "signal 9" (Linux/macOS).
    std::string exit_description();
    // Resident memory in bytes (Linux/macOS; 0 if unknown). Windows returns 0: the Job Object enforces the limit.
    uint64_t resident_bytes() const;
    // Closes pipes and handles. Does not kill the process (on Windows closing the job does, like before).
    void close();

#ifdef _WIN32
    void* native_handle() const { return proc_; }
#endif

private:
    unsigned long pid_ = 0;
#ifdef _WIN32
    void* proc_ = nullptr;
    void* job_ = nullptr;
    void* stdin_ = nullptr;
    void* stdout_ = nullptr;
#else
    int stdin_ = -1;
    int stdout_ = -1;
    bool exited_ = false;
    int status_ = 0;
#endif
};

// Stops a thread that is blocked in ChildProcess::read() and joins it, without ever blocking on a child that is still
// alive or still dying: `stop` ends the read loop, and on Windows CancelSynchronousIo aborts a read already waiting.
void stop_reader_thread(std::thread& reader, std::atomic<bool>& stop, ChildProcess& proc);

// Auto-reset event: set() wakes the waiter; wait() returns after a set() or the timeout.
class WakeEvent {
public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;
    void set();
    void wait(uint32_t ms);

private:
#ifdef _WIN32
    void* h_ = nullptr;
#else
    std::mutex mu_;
    std::condition_variable cv_;
    bool signaled_ = false;
#endif
};

} // namespace p2i
