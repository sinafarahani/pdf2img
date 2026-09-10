// Line-oriented supervisor <-> worker protocol helpers.
// A message is one line: fields separated by TAB, terminated by LF. Fields are UTF-8 with
// %XX escaping for TAB, LF, CR and '%'.
//
// supervisor -> worker:
//   SPEC <key> <value>                       (repeated)   render/encode settings
//   SPECEND
//   OPEN  <fileId> <path>                                   open and report the page count
//   PAGE  <fileId> <path> <page> <outPath>                  render one page (1-based) into outPath
//   MULTI <fileId> <path> <first> <last> <outPath>          render a page range into one multi-page TIFF
//   QUIT
// worker -> supervisor:
//   READY
//   FATAL    <message>                                      (worker cannot start; sent instead of READY, then exits)
//   BEGIN    <fileId>                                       (acknowledges OPEN/PAGE/MULTI before starting the work)
//   OPENED   <fileId> <pageCount> <repairedPath|""> <xrefRebuilt 0|1>
//   OPENFAIL <fileId> <exitCode> <message>
//   PAGEOK   <fileId> <page> <width>x<height> <ms> <outPath> <fallbackDpi 0|1>
//   PAGEFAIL <fileId> <page> <exitCode> <message>
//   PROGRESS <fileId> <page> <fallbackDpi 0|1>              (during MULTI, after each page)
//   MULTIOK  <fileId> <pagesWritten> <pagesFailed> <ms> <outPath>
//   MULTIFAIL <fileId> <exitCode> <message>
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace p2i {
namespace ipc {

std::string escape(std::string_view field);
std::string unescape(std::string_view field);
std::string join(const std::vector<std::string>& fields); // adds the trailing LF
std::vector<std::string> split(std::string_view line);   // unescapes each field; strips trailing CR/LF

} // namespace ipc
} // namespace p2i
