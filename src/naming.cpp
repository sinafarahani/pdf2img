#include "naming.h"
#include "config.h"
#include "fsutil.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>

namespace p2i {

// Order in which wildcard matches are converted.
static bool input_order(const std::wstring& a, const std::wstring& b) {
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
#else
    // Case-insensitive (ASCII) like Windows, then byte order so that the result is deterministic.
    const std::wstring la = to_lower_ascii(a), lb = to_lower_ascii(b);
    return la != lb ? la < lb : a < b;
#endif
}

static std::vector<std::wstring> expand_input(const std::wstring& pattern, std::vector<JobProblem>& problems) {
    std::vector<std::wstring> out;
    std::wstring abs = make_absolute(pattern);
    if (!has_wildcard(abs)) {
        if (!file_exists(abs)) {
            problems.push_back({pattern, EXIT_INPUT, "input file not found: " + narrow(abs)});
            return out;
        }
        out.push_back(abs);
        return out;
    }
    out = find_files(path_dir(abs), path_name(abs));
    std::sort(out.begin(), out.end(), input_order);
    if (out.empty()) problems.push_back({pattern, EXIT_INPUT, "no input files match: " + narrow(abs)});
    return out;
}

std::vector<InputJob> resolve_jobs(const Options& opts, std::vector<JobProblem>& problems) {
    std::vector<InputJob> jobs;
    std::vector<std::wstring> inputs = expand_input(opts.input, problems);
    if (inputs.empty()) return jobs;

    const bool multi = inputs.size() > 1 || has_wildcard(opts.input);
    // "-o <existing directory>" behaves like the hidden -d switch (the old tool would have produced 0-byte files).
    bool outputIsDir = opts.outputIsDir;
    if (!outputIsDir && !opts.output.empty() && !has_wildcard(opts.output) && directory_exists(make_absolute(opts.output)))
        outputIsDir = true;
    for (const std::wstring& in : inputs) {
        InputJob j;
        j.inputPath = in;
        OutputSpec& o = j.out;
        if (opts.output.empty()) {
            // Old tool: <input dir>\<input stem><suffix>.tif
            o.dir = path_dir(in);
            o.stem = path_stem(in);
            o.ext = L".tif";
        } else if (opts.outputIsDir) {
            // Hidden -d (observed behaviour): <-o directory>\<input stem>\<input stem><suffix>.<ext from -o if any, else .tif>
            std::wstring outAbs = make_absolute(opts.output);
            std::wstring ext = path_ext(outAbs);
            std::wstring base;
            if (!ext.empty() && format_from_extension(ext) != OutFormat::Unknown) { base = path_dir(outAbs); o.ext = ext; }
            else { base = outAbs; o.ext = L".tif"; }
            o.stem = path_stem(in);
            o.dir = extend_long_path(path_join(base, o.stem));
        } else if (outputIsDir) {
            // -o names an existing directory (no -d): <dir>\<input stem><suffix>.tif directly inside it
            o.dir = make_absolute(opts.output);
            o.stem = path_stem(in);
            o.ext = L".tif";
        } else if (multi || has_wildcard(opts.output)) {
            // Wildcard batch: output dir + extension from -o, stem from the input.
            std::wstring outAbs = make_absolute(opts.output);
            o.dir = path_dir(outAbs);
            o.stem = path_stem(in);
            o.ext = path_ext(outAbs);
        } else {
            std::wstring outAbs = make_absolute(opts.output);
            o.dir = path_dir(outAbs);
            o.stem = path_stem(outAbs);
            o.ext = path_ext(outAbs);
        }
        o.format = format_from_extension(o.ext);
        if (o.format == OutFormat::Unknown) {
            problems.push_back({in, EXIT_UNSUPPORTED_FORMAT,
                                o.ext.empty() ? "output file has no extension (cannot determine image format): " + narrow(plain_output_path(o))
                                              : "unsupported output format '" + narrow(o.ext) + "': " + narrow(plain_output_path(o))});
        } else if (o.format == OutFormat::Wmf || o.format == OutFormat::Emf) {
            problems.push_back({in, EXIT_UNSUPPORTED_FORMAT, "WMF/EMF output is not supported by this build: " + narrow(plain_output_path(o))});
        }
        jobs.push_back(std::move(j));
    }
    return jobs;
}

void resolve_page_range(int firstOpt, int lastOpt, int pageCount, int& first, int& last) {
    if (pageCount < 1) { first = 1; last = 0; return; }
    first = firstOpt > 0 ? firstOpt : 1;
    last = lastOpt > 0 ? lastOpt : pageCount;
    if (first > pageCount) { first = 1; last = pageCount; return; } // old tool: invalid range -> whole document
    if (last > pageCount) last = pageCount;
    if (last < first) last = first;                                 // old tool: only the first page
}

std::wstring page_output_path(const OutputSpec& o, int pageNumber, const std::wstring& suffixFmt) {
    return extend_long_path(path_join(o.dir, o.stem + format_page_suffix(suffixFmt, pageNumber) + o.ext));
}

std::wstring plain_output_path(const OutputSpec& o) {
    return extend_long_path(path_join(o.dir, o.stem + o.ext));
}

bool uses_page_suffix(OutFormat fmt, bool multipage, int pagesProduced) {
    if (fmt == OutFormat::Tiff) return !multipage;
    return pagesProduced > 1;
}

} // namespace p2i
