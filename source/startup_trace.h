#ifndef PVZ2_STARTUP_TRACE_H
#define PVZ2_STARTUP_TRACE_H

#include "config.h"

#if PVZ2_ENABLE_STARTUP_TO_LAWN_TRACE
#error "Startup tracing was removed from the clean release source. Use a diagnostic source build instead."
#endif

/* Clean release: all trace calls are compile-time no-ops and their arguments
 * are never evaluated. */
#define startup_trace_begin() ((void)0)
#define startup_trace_active() 0
#define startup_trace_hotpath_active() 0
#define startup_trace_is_main_thread() 0
#define startup_trace_main_thread_id() 0ULL
#define startup_trace_mark(...) ((void)0)
#define startup_trace_markf(...) ((void)0)
#define startup_trace_title_state(...) ((void)0)
#define startup_trace_nav_context(...) ((void)0)
#define startup_trace_input(...) ((void)0)
#define startup_trace_lawn_ready_input(...) ((void)0)
#define startup_trace_note_frame(...) ((void)0)
#define startup_trace_note_obb(...) ((void)0)
#define startup_trace_note_obb_miss(...) ((void)0)
#define startup_trace_note_fs(...) ((void)0)
#define startup_trace_note_asset_open(...) ((void)0)
#define startup_trace_note_gl_upload(...) ((void)0)
#define startup_trace_note_gl_stage(...) ((void)0)
#define startup_trace_note_mmap(...) ((void)0)
#define startup_trace_checkpoint_now(...) ((void)0)
#define startup_trace_pump() ((void)0)
#define startup_trace_shutdown() ((void)0)

#endif
