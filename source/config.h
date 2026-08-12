/* PVZ2 Switch port build-time constants and platform configuration. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#include "game_version.h"

// The single native module from the APK's lib/arm64-v8a/.
#define CXX_SO_NAME    "libc++_shared.so"
#define NIMBLE_SO_NAME "libNimble.so"
#define SO_NAME        "libPVZ2.so"

#define DATA_DIR "sdmc:/switch/pvz2_nx"
#define LOG_NAME DATA_DIR "/pvz2_nx.log"
#define OBB_NAME "main.1051.com.ea.game.pvz2_row.obb"

// Address-space split (see __libnx_initheap in main.c). libnative.so is ~12 MB
// of code+data; reserve a fixed zone for it + relocation scratch and give the
// rest to newlib's heap (textures, audio, the parsed JSON/jet game state).
/* PVZ2 + libc++ + Nimble map to just under 48 MiB. Keep room for alignment
 * without exhausting Homebrew Menu's applet heap. Raise only if a newer game
 * module exceeds this zone. */
#define SO_ZONE_MB 64

// Keep normal play builds free of log formatting and SD-card log flushes.
// Set to 1 temporarily when investigating a specific issue.
#define DEBUG_LOG 0

/* Keep optional graphics and input diagnostics off in normal builds. */
#define PVZ2_ENABLE_TOUCH_TRACE 0
#define PVZ2_ENABLE_GL_TRACE 0
/* Keep request and loader diagnostics out of normal release builds. */
#define PVZ2_ENABLE_READINESS_TRACE 0
#define PVZ2_ENABLE_BLOCKER_TRACE 0

/* Keep normal logging limited to load/save, watchdog, and error markers. */
#define PVZ2_ENABLE_VERBOSE_RUNTIME_LOG 0

/* Reopened UI assets are decompressed once and retained under a strict cap.
 * This targets map/almanac/store/pinata tab churn without turning the complete
 * 1 GiB OBB into a permanent heap allocation. */
#define OBB_HOT_CACHE_SLOTS 96u
#define OBB_HOT_CACHE_MAX_BYTES (16u * 1024u * 1024u)
#define OBB_HOT_CACHE_MAX_ASSET (1024u * 1024u)

/* Pin EVERY thread (main + all engine workers + audio) to one CPU core.
 *
 * This does not remove races outright -- Horizon still preempts -- but it
 * removes true parallelism, so two threads can no longer be executing inside the
 * same critical section on two CPUs simultaneously. That closes most race
 * windows. Diagnostic value:
 *   - if the crashes/hangs STOP with this on  -> the bug is a data race
 *   - if they persist identically             -> it is NOT a race; look elsewhere
 * Costs performance (all engine work on one core), so turn off once stable. */
/* Upper bound on a blocking condvar/futex wait, in ms.  0 = NO CAP.
 *
 *   0   -> real, indefinite blocking waits. Idle engine threads sleep until they
 *          are actually signalled and use ZERO cpu. This is how a normal pthread
 *          implementation behaves, and it is what we want.
 *   >0  -> every wait wakes at least this often, whether signalled or not. This
 *          was a defence against a suspected lost wakeup -- but that theory was
 *          wrong. The real bug was returning newlib's ETIMEDOUT (116) where the
 *          engine's boost expected bionic's (110), which made every timed wait
 *          throw an uncaught exception. With that fixed, the cap does nothing
 *          except wake every idle thread 62x/second (at 16ms) and burn cpu.
 *
 * Set to 0 while normal waits are reliable. If a lost wakeup is diagnosed, use
 * a small polling interval here (100 ms is reasonable). */
#define COND_WAIT_CAP_MS 0

#define SINGLE_CORE 0
#define SINGLE_CORE_ID 0

// Asset lookup roots, tried in order under the game dir. On Android,
// AAssetManager_open("Assets/x") means assets/Assets/x, so "assets" comes first.
// The game may request paths with or without the Assets/ prefix.
#define ASSET_ROOTS { "assets", "No_Backup/CDN.13.3", ".", "romfs:" }

// Packed archive the engine memory-maps first; loose files override.
#define JET_ARCHIVE ""

extern int screen_width;
extern int screen_height;

#define CONFIG_NAME DATA_DIR "/config.txt"

typedef struct {
  int screen_width;    // 0 = automatic (per dock state)
  int screen_height;   // 0 = automatic
  int docked_width;    // default 1920
  int docked_height;   // default 1080
  char language[8];    // "auto" or 2-letter code
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
