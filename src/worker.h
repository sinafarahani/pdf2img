// Render worker process entry point (invoked as "<exe> --worker"; talks to the supervisor over stdin/stdout).
#pragma once

namespace p2i {
int run_worker();
}
