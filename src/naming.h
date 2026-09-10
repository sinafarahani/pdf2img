// Output naming rules of the old tool (see docs/OLD_TOOL_BEHAVIOR.md) and input resolution (wildcards, -d).
#pragma once

#include "common.h"
#include "cli.h"

#include <string>
#include <vector>

namespace p2i {

struct OutputSpec {
    std::wstring dir;    // output directory ("" = current directory)
    std::wstring stem;   // base file name without extension
    std::wstring ext;    // extension including the dot, e.g. ".tif"  (may be empty -> unsupported)
    OutFormat format = OutFormat::Unknown;
};

struct InputJob {
    std::wstring inputPath; // absolute path of one PDF
    OutputSpec out;
};

struct JobProblem {
    std::wstring inputPattern;
    int exitCode;           // EXIT_INPUT / EXIT_UNSUPPORTED_FORMAT
    std::string message;
};

// Expands -i (wildcards), pairs each input with its output spec (from -o / -d / default .tif).
// Inputs that do not exist are reported in `problems`. Output format problems are reported too,
// but the job is still returned with format Unknown so the caller can decide.
std::vector<InputJob> resolve_jobs(const Options& opts, std::vector<JobProblem>& problems);

// Page range rules of the old tool. pageCount >= 1.
//   -f > pageCount  -> whole document;  -l > pageCount -> clamp;  -l < -f -> only page -f; 0 = unset.
void resolve_page_range(int firstOpt, int lastOpt, int pageCount, int& first, int& last);

// File name for a single page: <stem><suffix><ext> (suffix = printf(fmt, pageNumber)).
std::wstring page_output_path(const OutputSpec& o, int pageNumber, const std::wstring& suffixFmt);
// File name without page suffix: <stem><ext> (multi-page TIFF, or single-page non-TIFF output).
std::wstring plain_output_path(const OutputSpec& o);

// Whether the page-number suffix is used: TIFF (not -m) always; -m TIFF never; other formats only
// when more than one page is produced.
bool uses_page_suffix(OutFormat fmt, bool multipage, int pagesProduced);

// Whether -m applies (TIFF only).
inline bool multipage_applies(OutFormat fmt, bool multipageOpt) { return multipageOpt && fmt == OutFormat::Tiff; }

} // namespace p2i
