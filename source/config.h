/* PVZ2 Switch port build-time constants and platform configuration. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#include "game_version.h"

// The single native module from the APK's lib/arm64-v8a/.
#define CXX_SO_NAME    "libc++_shared.so"
#define NIMBLE_SO_NAME "libNimble.so"
#define SO_NAME        "libPVZ2.so"

#define DATA_DIR "sdmc:/switch/pvz2_nx"
#define OBB_NAME "main.1051.com.ea.game.pvz2_row.obb"

// Address-space split (see __libnx_initheap in main.c). libnative.so is ~12 MB
// of code+data; reserve a fixed zone for it + relocation scratch and give the
// rest to newlib's heap (textures, audio, the parsed JSON/jet game state).
/* PVZ2 + libc++ + Nimble map to just under 48 MiB. Keep room for alignment
 * without exhausting Homebrew Menu's applet heap. Raise only if a newer game
 * module exceeds this zone. */
#define SO_ZONE_MB 64

// Release build: no runtime log files or debug trace output.
#define DEBUG_LOG 0

/* Optional diagnostics are disabled in release builds. */
#define PVZ2_ENABLE_TOUCH_TRACE 0
#define PVZ2_ENABLE_GL_TRACE 0
#define PVZ2_ENABLE_AUDIO_DIAGNOSTICS 0
#define PVZ2_ENABLE_READINESS_TRACE 0
#define PVZ2_ENABLE_BLOCKER_TRACE 0
#define PVZ2_ENABLE_VERBOSE_RUNTIME_LOG 0
#define PVZ2_ENABLE_ARCHIVE_DIAGNOSTICS 0
#define PVZ2_ENABLE_IO_TIMING 0
#define PVZ2_ENABLE_STARTUP_PROFILER 0
#define PVZ2_ENABLE_IDENTITY_PROVENANCE_TRACE 0
#define PVZ2_ENABLE_FILESYSTEM_TRACE 0
#define PVZ2_ENABLE_ASSET_TRACE 0
#define PVZ2_ENABLE_HTTP_DETAIL_TRACE 0
#define PVZ2_ENABLE_HTTP_PAYLOAD_DUMPS 0
#define PVZ2_ENABLE_COMPONENT_LIFECYCLE_TRACE 0
#define PVZ2_ENABLE_NIMBLE_BROADCAST_TRACE 0

/* Match the production Android Nimble log floor. */
#define PVZ2_NIMBLE_LOG_MIN_LEVEL 500

/* Cache physical network state instead of querying NIFM every frame. */
#define PVZ2_NETWORK_STATUS_CACHE_MS 500u

#define PVZ2_CDN_HTTP_INSTALL_FALLBACK 0

/* Reopened UI assets are decompressed once and retained under a strict cap.
 * This targets map/almanac/store/pinata tab churn without turning the complete
 * 1 GiB OBB into a permanent heap allocation. */
#define OBB_HOT_CACHE_SLOTS 96u
#define OBB_HOT_CACHE_MAX_BYTES (16u * 1024u * 1024u)
#define OBB_HOT_CACHE_MAX_ASSET (1024u * 1024u)

/* Optional single-core mode for platform debugging. Keep disabled in release. */
/* Optional cap for compatibility debugging; zero preserves normal blocking waits. */
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
