// pdf2img — PDF to image converter (PDFium), command-line compatible with VeryPDF PDF2Image v2.1.
#include "cli.h"
#include "common.h"
#include "config.h"
#include "fsutil.h"
#include "log.h"
#include "supervisor.h"
#include "version.h"
#include "worker.h"

#include <string>
#include <vector>

using namespace p2i;

static void print_usage(bool withExtras) {
    log::raw_stdout(usage_text());
    if (withExtras) log::raw_stdout(usage_extra_text());
}

static int run(int argc, wchar_t** argv) {
    Options opts;
    std::wstring message;
    ParseStatus st = parse_command_line(argc, argv, opts, message);

    if (opts.workerMode) {
        log::init(false, L"", "pdf2img-worker");
        return run_worker();
    }

    Config cfg = load_config();
    log::set_command_line(loggable_command_line(argc, argv, st != ParseStatus::Error, &opts.passwordArgIndices)); // -upw/-opw values redacted
    log::init(opts.verbose || cfg.verbose, opts.logFile.empty() ? cfg.logFile : opts.logFile, "pdf2img");
    disable_error_dialogs();

    if (opts.showVersion) {
        log::raw_stdout(std::string("pdf2img ") + PDF2IMG_VERSION + " (" + PDF2IMG_DESCRIPTION + ")\n");
        return EXIT_OK;
    }
    if (st == ParseStatus::Help) {
        print_usage(opts.helpExtras);
        return EXIT_USAGE; // the old tool exited 1 after showing help
    }
    if (st == ParseStatus::Error) {
        if (!message.empty())
            log::error(has_password_switch(argc, argv) ? std::string("invalid command line (details not shown because it contains a password)")
                                                       : narrow(message));
        print_usage(false);
        return EXIT_USAGE;
    }
    if (!cfg.iniPath.empty()) log::infof("using %s", narrow(cfg.iniPath).c_str());
    return run_supervisor(opts, cfg);
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run(argc, argv); }
#else
int main(int argc, char** argv) {
    // Arguments arrive as bytes (normally UTF-8); widen() keeps any other bytes intact (see common.h).
    std::vector<std::wstring> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(widen(argv[i] ? argv[i] : ""));
    std::vector<wchar_t*> ptrs;
    for (auto& a : args) ptrs.push_back(a.data());
    ptrs.push_back(nullptr);
    return run(argc, ptrs.data());
}
#endif
