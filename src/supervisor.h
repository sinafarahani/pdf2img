// Supervisor: resolves jobs, runs a pool of worker processes (memory and time limits: Job Objects on Windows, resident
// memory watch on Linux/macOS), dispatches pages, enforces timeouts, aggregates the exit code.
#pragma once

#include "cli.h"
#include "config.h"

namespace p2i {
int run_supervisor(const Options& opts, const Config& cfg);
}
