/*
 * Clean-release compatibility translation unit.
 *
 * Startup tracing is intentionally compiled out in startup_trace.h.  This
 * empty file is kept so older/stale project Makefiles that still list
 * startup_trace.c continue to build without adding any runtime code.
 */
#include "startup_trace.h"
