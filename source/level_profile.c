/*
 * Clean-release compatibility translation unit.
 *
 * Level-load profiling is intentionally compiled out in level_profile.h.
 * Keeping this empty file makes the release source compatible with older
 * project Makefiles without restoring profiler code or runtime overhead.
 */
#include "level_profile.h"
