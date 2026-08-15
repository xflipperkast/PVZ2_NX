/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

/* Logging is file-backed; NxLink's optional BSD socket cleanup crashes this
 * title during exit, and the game itself uses offline network stubs. */
void userAppInit(void) {}
void userAppExit(void) {}

#endif

#if DEBUG_LOG
/* Log through libnx's low-level fsFileWrite -- NOT newlib open/write/FILE.
 *
 * Why: the engine's worker threads constantly open/close asset files, which
 * mutate newlib's shared stdio/fd tables. Those tables are not under the malloc
 * lock, so a concurrent newlib write() from our logging on the main thread
 * races them and reads a corrupted fd->device slot -> _write_r faults. Routing
 * the log through the fsp-srv service directly (fsFileWrite) touches none of
 * newlib's shared state, so it can't be corrupted by, or corrupt, engine I/O.
 * Everything here is guarded by one libnx mutex; the FsFile handle is opened
 * lazily on first use. */
static FsFile s_log_file;
static s64    s_log_off = 0;     /* bytes actually written to the file */
static s64    s_log_cap = 0;     /* current file size (we over-allocate) */
static int    s_log_ready = 0;   /* 0=unopened, 1=open, -1=failed */
static Mutex  s_log_mutex;

/* Buffer log text in RAM; only touch the SD card when it fills. See
 * log_flush_locked() for why (per-line resize+flush was pegging the CPU). */
#define LOG_BUF_SIZE (128 * 1024)
#define LOG_GROW     (256 * 1024)
static char   s_log_buf[LOG_BUF_SIZE];
static size_t s_log_used = 0;

static void log_open_locked(void) {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (!fs) { s_log_ready = -1; return; }
  /* LOG_NAME is on the SD card; strip the "sdmc:" mount prefix
   * for the raw fs API (path is relative to the sd filesystem root). */
  const char *path = LOG_NAME;
  const char *colon = path;
  while (*colon && *colon != ':') colon++;
  if (*colon == ':') path = colon + 1;   /* -> SD-card-relative log path */
  fsFsCreateFile(fs, path, 0, 0);        /* no-op if it already exists */
  if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &s_log_file))) {
    fsFileSetSize(&s_log_file, 0);       /* truncate for a fresh run */
    s_log_off = 0;
    s_log_cap = 0;
    s_log_ready = 1;
  } else {
    s_log_ready = -1;
  }
}

/* Flush the in-memory buffer to the file. Caller holds s_log_mutex.
 *
 * Per-line fsFileSetSize() and fsFileWrite(FsWriteOption_Flush) force blocking
 * SD-card IPC. At tens of lines a second, that is a large permanent CPU/IO cost.
 *
 * Now: append into a RAM buffer and only touch the filesystem when it fills.
 * The file is grown in big chunks (LOG_GROW) instead of per line, and we do NOT
 * force a flush per write -- fsFileFlush is called on shutdown / on demand. */
static void log_flush_locked(void) {
  if (s_log_ready != 1 || s_log_used == 0)
    return;

  const s64 need = s_log_off + (s64)s_log_used;
  if (need > s_log_cap) {                       /* grow in large steps */
    s64 want = need + LOG_GROW;
    if (R_SUCCEEDED(fsFileSetSize(&s_log_file, want)))
      s_log_cap = want;
    else if (R_SUCCEEDED(fsFileSetSize(&s_log_file, need)))
      s_log_cap = need;                         /* fall back to exact */
    else
      { s_log_used = 0; return; }               /* give up on this chunk */
  }

  if (R_SUCCEEDED(fsFileWrite(&s_log_file, s_log_off, s_log_buf,
                              (u64)s_log_used, FsWriteOption_None)))
    s_log_off += s_log_used;
  s_log_used = 0;
}
#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  char buf[2048];
  va_list list;
  va_start(list, text);
  int n = vsnprintf(buf, sizeof buf - 1, text, list);
  va_end(list);
  if (n <= 0) return 0;
  if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
  buf[n] = 0;
#endif
#if DEBUG_LOG
  /* serialise: the engine calls our shims from multiple threads */
  mutexLock(&s_log_mutex);
  if (s_log_ready == 0) log_open_locked();
  if (s_log_ready == 1) {
    if (s_log_used + (size_t)n > LOG_BUF_SIZE) log_flush_locked();
    if ((size_t)n <= LOG_BUF_SIZE) {
      memcpy(s_log_buf + s_log_used, buf, (size_t)n);
      s_log_used += (size_t)n;
    }
  }
  mutexUnlock(&s_log_mutex);
#endif
  return 0;
}

/* Advisory checkpoint used by historical probes. There are more than a
 * hundred call sites, many on startup and callback paths, so draining on every
 * call would still turn the logger into synchronous SD traffic even without an
 * fsFileFlush. Only emit a sizeable batch; full durability belongs to
 * debugLogSync() on crash/watchdog/clean-exit paths. */
void debugLogFlush(void) {
#if DEBUG_LOG
  mutexLock(&s_log_mutex);
  if (s_log_used >= LOG_BUF_SIZE / 4u)
    log_flush_locked();
  mutexUnlock(&s_log_mutex);
#endif
}

void debugLogSync(void) {
#if DEBUG_LOG
  mutexLock(&s_log_mutex);
  log_flush_locked();
  if (s_log_ready == 1) {
    fsFileSetSize(&s_log_file, s_log_off);
    s_log_cap = s_log_off;
    fsFileFlush(&s_log_file);
  }
  mutexUnlock(&s_log_mutex);
#endif
}


/* The engine is bionic code: it keeps its thread-locals -- including the
 * stack-protector cookie at slot 5 (+0x28) -- off the ARM64 *user* thread
 * pointer TPIDR_EL0, and it reads that register in ~200 places.
 *
 * Two separate registers matter on Switch, and mixing them up is fatal:
 *   TPIDRRO_EL0 (armGetTls)  - KERNEL-owned: IPC command buffer + libnx
 *                              ThreadVars (thread handle, newlib reent).
 *                              Never write here; corrupting it breaks every
 *                              service call (file I/O, GPU) instantly.
 *   TPIDR_EL0   (armSetTlsRw) - user-settable and unused by libnx: this is the
 *                              one bionic expects, and the one we own.
 *
 * The bug this replaces: a single GLOBAL block was installed into TPIDR_EL0 on
 * every thread, so all of the engine's worker threads shared one TLS area and
 * scribbled over each other -- the source of the random, wandering faults.
 * Each thread now gets its own private block. */
extern uintptr_t __stack_chk_guard;

/* One block PER THREAD (not shared). Installed unconditionally: libnx already
 * points TPIDR_EL0 at the ELF TLS base on every thread, so we must NOT skip on
 * "already set" -- the engine needs its OWN bionic-layout block here or its
 * first TLS access (stack cookie at slot 5) faults. Each thread that runs
 * engine code (main + every worker via the trampoline) calls this exactly once,
 * so there's no repeated-call leak concern. */
/* Per-thread bionic TLS block.
 *
 * CRITICAL LAYOUT DETAIL (matches the working colorsheep/cr3 Switch ports):
 * bionic's thread pointer is the MIDDLE of a TCB -- there are valid thread-local
 * slots at NEGATIVE offsets from tpidr_el0 (the ELF TLS block sits below the TCB
 * in the AArch64 variant-I layout). If we point tpidr_el0 at the START of our
 * allocation, any negative-offset TLS access writes BEFORE the block, corrupting
 * whatever the allocator placed there (newlib's FILE pool / fd table -> the
 * fopen/_open_r/_write_r null-derefs we kept hitting). So the thread pointer
 * must sit with headroom BELOW it: allocate a block and point tp into its middle.
 * Size 0x400, tp at +0x200 -> 0x200 of negative headroom and 0x200 above. */
#define BIONIC_TLS_SIZE      0x400
#define BIONIC_TLS_TP_OFFSET 0x200

/* Pin the calling thread to a single core (see SINGLE_CORE in config.h).
 * svcSetThreadCoreMask(handle, preferred_core, affinity_mask). Homebrew in
 * title-takeover may use cores 0..2; core 3 is reserved for the system. */
void pin_current_thread(void) {
#if SINGLE_CORE
  Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, SINGLE_CORE_ID,
                                   (u32)(1u << SINGLE_CORE_ID));
  if (R_FAILED(rc))
    debugPrintf("[core] pin to core %d failed rc=0x%x\n", SINGLE_CORE_ID, rc);
#endif
}

void tls_setup_guard(void) {
  uint8_t *block = calloc(1, BIONIC_TLS_SIZE);   /* per-thread; never freed (tp points into it) */
  if (!block)
    return;

  uint8_t *tp = block + BIONIC_TLS_TP_OFFSET;
  *(uint64_t *)(tp + 0x28) = (uint64_t)__stack_chk_guard;  /* bionic stack-guard slot 5 */
  armSetTlsRw(tp);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }

void dump_memory_hex(const char *tag, const void *ptr, size_t len) {
  if (!ptr || len == 0) return;
  debugPrintf("HEX DUMP [%s] at %p (%zu bytes):\n", tag, ptr, len);
  const unsigned char *b = (const unsigned char *)ptr;
  for (size_t i = 0; i < len; i += 16) {
    debugPrintf("  %04zx: ", i);
    for (size_t j = 0; j < 16; j++) {
      if (i + j < len)
        debugPrintf("%02x ", b[i + j]);
      else
        debugPrintf("   ");
    }
    debugPrintf(" | ");
    for (size_t j = 0; j < 16; j++) {
      if (i + j < len) {
        unsigned char c = b[i + j];
        debugPrintf("%c", (c >= 32 && c <= 126) ? c : '.');
      }
    }
    debugPrintf("\n");
  }
}
