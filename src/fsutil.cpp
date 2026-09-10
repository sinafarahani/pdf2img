#include "fsutil.h"
#include "common.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <thread>

namespace p2i {

#ifdef _WIN32

bool path_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool file_exists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool directory_exists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool is_root_like(const std::wstring& p) {
    // "C:", "C:\", "\\?\C:", "\\?\C:\", "\\?\UNC", "\\server", "\\?", "\\" ...
    std::wstring s = p;
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    if (s.rfind(L"UNC\\", 0) == 0) s = s.substr(4);
    if (s.size() <= 3 && (s.size() < 2 || s[1] == L':')) return true;
    if (s == L"UNC" || s.empty()) return true;
    return false;
}

#else

static bool stat_path(const std::wstring& path, struct stat& st) {
    return !path.empty() && ::stat(narrow(path).c_str(), &st) == 0;
}

bool path_exists(const std::wstring& path) {
    struct stat st{};
    return stat_path(path, st);
}

bool file_exists(const std::wstring& path) {
    struct stat st{};
    return stat_path(path, st) && !S_ISDIR(st.st_mode);
}

bool directory_exists(const std::wstring& path) {
    struct stat st{};
    return stat_path(path, st) && S_ISDIR(st.st_mode);
}

static bool is_root_like(const std::wstring& p) { return p.empty() || p == L"/"; }

#endif

bool ensure_directory(const std::wstring& path, std::string& err) {
    if (path.empty() || directory_exists(path)) return true;
    std::wstring parent = path_dir(path);
    if (!parent.empty() && parent != path && !is_root_like(parent) && !directory_exists(parent)) {
        if (!ensure_directory(parent, err)) return false;
    }
#ifdef _WIN32
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return directory_exists(path);
    err = "cannot create directory " + narrow(path) + " (error " + std::to_string(GetLastError()) + ")";
    return false;
#else
    const int rc = ::mkdir(narrow(path).c_str(), 0777);
    const int e = rc == 0 ? 0 : errno;
    if ((rc == 0 || e == EEXIST) && directory_exists(path)) return true;
    err = "cannot create directory " + narrow(path) + " (" + std::strerror(e ? e : ENOTDIR) + ")";
    return false;
#endif
}

bool replace_file(const std::wstring& temp, const std::wstring& target, std::string& err) {
#ifdef _WIN32
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return true;
    DWORD e = GetLastError();
    err = "cannot write output file " + narrow(target) + " (error " + std::to_string(e) + ")";
    DeleteFileW(temp.c_str());
    return false;
#else
    if (std::rename(narrow(temp).c_str(), narrow(target).c_str()) == 0) return true;
    const int e = errno;
    err = "cannot write output file " + narrow(target) + " (" + std::strerror(e) + ")";
    ::unlink(narrow(temp).c_str());
    return false;
#endif
}

void delete_file_quiet(const std::wstring& path) {
    if (path.empty()) return;
#ifdef _WIN32
    DeleteFileW(path.c_str());
#else
    ::unlink(narrow(path).c_str());
#endif
}

void remove_directory_quiet(const std::wstring& path) {
    if (path.empty()) return;
#ifdef _WIN32
    RemoveDirectoryW(path.c_str());
#else
    ::rmdir(narrow(path).c_str());
#endif
}

long long file_size_of(const std::wstring& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return -1;
    return (static_cast<long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
#else
    struct stat st{};
    if (!stat_path(path, st)) return -1;
    return static_cast<long long>(st.st_size);
#endif
}

FILE* open_file_for_writing(const std::wstring& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(narrow(path).c_str(), "wb");
#endif
}

bool create_new_file(const std::wstring& path) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    const int fd = ::open(narrow(path).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    ::close(fd);
    return true;
#endif
}

std::wstring temp_directory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return (n == 0 || n > MAX_PATH) ? std::wstring() : std::wstring(buf, n);
#else
    std::wstring t;
    if (get_env(L"TMPDIR", t)) return t;
    return L"/tmp";
#endif
}

std::vector<std::wstring> find_files(const std::wstring& dir, const std::wstring& pattern) {
    std::vector<std::wstring> out;
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(path_join(dir, pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(path_join(dir, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = ::opendir(dir.empty() ? "." : narrow(dir).c_str());
    if (!d) return out;
    const std::string pat = narrow(pattern);
    while (const dirent* e = ::readdir(d)) {
        if (::fnmatch(pat.c_str(), e->d_name, FNM_PERIOD) != 0) continue;
        std::wstring full = path_join(dir, widen(e->d_name));
        if (!file_exists(full)) continue;
        out.push_back(std::move(full));
    }
    ::closedir(d);
#endif
    return out;
}

std::wstring temp_name_for(const std::wstring& target, unsigned long pid) {
    // "<target>.<pid>.tmp" unless that would exceed the 255-unit name-component limit (UTF-16 units on Windows, bytes
    // elsewhere); then a short name in the same directory. Both the worker (writer) and the supervisor (cleanup) derive
    // the same name.
    std::wstring name = path_name(target);
    std::wstring suffix = L"." + std::to_wstring(pid) + L".tmp";
#ifdef _WIN32
    const size_t nameUnits = name.size();
#else
    const size_t nameUnits = narrow(name).size();
#endif
    if (nameUnits + suffix.size() <= 250) return target + suffix;
    uint32_t h = 2166136261u;
    for (wchar_t c : name) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
    char buf[64];
    std::snprintf(buf, sizeof buf, "~p2i_%lu_%08X.tmp", pid, static_cast<unsigned>(h));
    return path_join(path_dir(target), widen(buf));
}

std::wstring temp_name_for(const std::wstring& target) { return temp_name_for(target, current_pid()); }

void disable_error_dialogs() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

unsigned hardware_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

} // namespace p2i
