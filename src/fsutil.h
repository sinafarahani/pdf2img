// Small file-system helpers (wide paths; on Linux/macOS they are converted to UTF-8 file names).
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace p2i {

bool path_exists(const std::wstring& path);      // file, directory or anything else
bool file_exists(const std::wstring& path);      // exists and is not a directory
bool directory_exists(const std::wstring& path);
// Creates the directory and all missing parents. Returns true if it exists afterwards.
bool ensure_directory(const std::wstring& path, std::string& err);
// Replaces `target` with `temp` atomically (MoveFileEx REPLACE_EXISTING / rename). Deletes temp on failure.
bool replace_file(const std::wstring& temp, const std::wstring& target, std::string& err);
void delete_file_quiet(const std::wstring& path);
void remove_directory_quiet(const std::wstring& path); // empty directories only
long long file_size_of(const std::wstring& path);      // -1 if it cannot be read
FILE* open_file_for_writing(const std::wstring& path); // fopen(path, "wb")
bool create_new_file(const std::wstring& path);        // atomically creates an empty file; false if it exists
std::wstring temp_directory();                         // %TEMP% (Windows), $TMPDIR or /tmp; "" if unavailable
// Regular files in `dir` whose name matches `pattern` (* and ?), as full paths, unsorted.
// Windows: FindFirstFileEx semantics (case-insensitive); elsewhere fnmatch (case-sensitive).
std::vector<std::wstring> find_files(const std::wstring& dir, const std::wstring& pattern);
// Temporary name for writing `target` (same directory): "<target>.<pid>.tmp", or a short "~p2i_<pid>_<hash>.tmp" when the
// name would exceed the 255-character component limit. The supervisor uses the pid overload for cleanup.
std::wstring temp_name_for(const std::wstring& target);
std::wstring temp_name_for(const std::wstring& target, unsigned long pid);
// Hardening for unattended use. Windows: no error dialogs, CRT asserts/aborts go to stderr.
// Linux/macOS: SIGPIPE is ignored (a vanished worker or reader must not kill the process).
void disable_error_dialogs();
unsigned hardware_threads();

} // namespace p2i
