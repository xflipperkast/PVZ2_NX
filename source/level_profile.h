#ifndef PVZ2_LEVEL_PROFILE_H
#define PVZ2_LEVEL_PROFILE_H

#include "config.h"

#if PVZ2_ENABLE_LEVEL_LOAD_PROFILER
#error "Level-load profiling was removed from the clean release source. Use a diagnostic source build instead."
#endif

/* Clean release: profiler calls are compile-time no-ops and arguments are not
 * evaluated. */
#define level_profile_write_waiting(...) ((void)0)
#define level_profile_arm(...) ((void)0)
#define level_profile_active() 0
#define level_profile_pump() ((void)0)
#define level_profile_cancel_if_timed_out() ((void)0)
#define level_profile_finish(...) ((void)0)
#define level_profile_finish_if_quiet(...) ((void)0)
#define level_profile_note_frame(...) ((void)0)
#define level_profile_note_obb(...) ((void)0)
#define level_profile_note_obb_miss() ((void)0)
#define level_profile_note_fs(...) ((void)0)
#define level_profile_note_mod_query(...) ((void)0)
#define level_profile_note_asset_open(...) ((void)0)

#endif
