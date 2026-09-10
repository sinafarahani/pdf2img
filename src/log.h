// Minimal logging: errors/warnings to stderr, optional log file, verbose diagnostics.
#pragma once

#include <string>
#include <string_view>

namespace p2i {
namespace log {

void init(bool verbose, const std::wstring& logFile, const char* prefix /* e.g. "pdf2img" or "worker 2" */);
// Command line recorded once, before a process's first log-file record. Passwords must already be redacted.
void set_command_line(std::string commandLine);
bool verbose();

void error(std::string_view msg);   // always shown
void warn(std::string_view msg);    // always shown
void info(std::string_view msg);    // only when verbose
void raw_stdout(std::string_view msg); // normal program output (e.g. "Converting ...")

// printf-style helpers (UTF-8 messages).
void errorf(const char* fmt, ...);
void warnf(const char* fmt, ...);
void infof(const char* fmt, ...);

} // namespace log
} // namespace p2i
