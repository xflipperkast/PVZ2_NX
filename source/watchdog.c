/* watchdog.c -- stall detector for the PVZ2 frame loop.
 *
 * After "STAGE 15 running" the loop is silent, so a hang there is invisible.
 * This watchdog runs on its own thread, watches g_frame_count (bumped once per
 * nativeTick), and if it stops advancing for STALL_SECS it snapshots every
 * REGISTERED engine thread -- PC/LR/SP + a frame-pointer backtrace, symbolized
 * against libnative.so.
 *
 * A process cannot svcDebugActiveProcess() itself, so (like colorsheep's diag.c)
 * each engine thread registers its own handle via watchdog_register_thread()
 * from our pthread trampoline, and the watchdog reads a snapshot with
 * svcSetThreadActivity(Paused) -> svcGetThreadContext3 -> Runnable.
 */

#include <switch.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "util.h"
#include "so_util.h"

volatile unsigned long long g_frame_count = 0;   /* bumped by the main loop */

#define STALL_SECS   5ull
#define POLL_SECS    1ull
#define MAX_THREADS  64
#define BT_DEPTH     24

typedef struct { int in_use; Handle handle; u64 tid; } WdThread;
static WdThread   s_threads[MAX_THREADS];
static Mutex      s_reg_lock;
static Thread     s_wd_thread;
static so_module *s_mod;
static int        s_started = 0;
static volatile int s_suspended = 0;

void watchdog_set_suspended(int suspended) { s_suspended = suspended ? 1 : 0; }

void watchdog_register_thread(void) {
  mutexLock(&s_reg_lock);
  for (int i = 0; i < MAX_THREADS; i++) {
    if (!s_threads[i].in_use) {
      s_threads[i].in_use = 1;
      s_threads[i].handle = threadGetCurHandle();
      u64 tid = 0;
      if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) s_threads[i].tid = tid;
      break;
    }
  }
  mutexUnlock(&s_reg_lock);
}

static void resolve_addr(char *out, size_t n, uint64_t addr) {
  if (s_mod && s_mod->load_virtbase) {
    uint64_t b = (uint64_t)(uintptr_t)s_mod->load_virtbase;
    if (addr >= b && addr < b + s_mod->load_size) {
      snprintf(out, n, "libnative.so+0x%llx", (unsigned long long)(addr - b));
      return;
    }
  }
  snprintf(out, n, "0x%llx", (unsigned long long)addr);
}

static uint64_t safe_rd64(uint64_t va, int *ok) {
  MemoryInfo mi; u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, va)) || mi.size == 0 ||
      (mi.perm & Perm_R) == 0 || va < mi.addr || va + 8 > mi.addr + mi.size) {
    *ok = 0; return 0;
  }
  *ok = 1;
  return *(const uint64_t *)(uintptr_t)va;
}

static void snapshot_thread(WdThread *t) {
  if (!t->handle || t->handle == threadGetCurHandle()) return;

  ThreadContext ctx;
  Result pr = svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  Result gr = R_SUCCEEDED(pr) ? svcGetThreadContext3(&ctx, t->handle) : pr;
  if (R_SUCCEEDED(pr)) svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  if (R_FAILED(gr)) {
    debugPrintf("[wd]   tid %llu snapshot failed rc=0x%x\n",
                (unsigned long long)t->tid, gr);
    return;
  }

  char pc[48], lr[48];
  resolve_addr(pc, sizeof pc, ctx.pc.x);
  resolve_addr(lr, sizeof lr, ctx.lr);
  debugPrintf("[wd] tid %llu  PC=%s  LR=%s\n", (unsigned long long)t->tid, pc, lr);
  debugPrintf("[wd]   SP=0x%llx FP=0x%llx X0=0x%llx X1=0x%llx\n",
              (unsigned long long)ctx.sp, (unsigned long long)ctx.fp,
              (unsigned long long)ctx.cpu_gprs[0].x,
              (unsigned long long)ctx.cpu_gprs[1].x);

  uint64_t fp = ctx.fp;
  for (int d = 0; d < BT_DEPTH && fp && (fp & 7) == 0; d++) {
    int ok1, ok2;
    uint64_t nextfp = safe_rd64(fp, &ok1);
    uint64_t retlr  = safe_rd64(fp + 8, &ok2);
    if (!ok1 || !ok2 || !retlr) break;
    char s[48]; resolve_addr(s, sizeof s, retlr);
    debugPrintf("[wd]     #%02d %s\n", d, s);
    if (nextfp <= fp) break;
    fp = nextfp;
  }
}

static void dump_all(void) {
  debugPrintf("[wd] ==== STALL: snapshotting registered threads ====\n");
  mutexLock(&s_reg_lock);
  for (int i = 0; i < MAX_THREADS; i++)
    if (s_threads[i].in_use) snapshot_thread(&s_threads[i]);
  mutexUnlock(&s_reg_lock);
  debugPrintf("[wd] ==== end dump ====\n");
  debugLogSync();
}

static void watchdog_main(void *arg) {
  (void)arg;
  debugPrintf("[wd] watchdog thread running\n");
  debugLogFlush();
  unsigned long long last = g_frame_count;   /* usually 0: loop just started */
  u64 last_change = armGetSystemTick();
  int dumped = 0;
  const u64 hz = armGetSystemTickFreq();

  for (;;) {
    svcSleepThread((s64)POLL_SECS * 1000000000LL);
    unsigned long long cur = g_frame_count;
    if (cur != last) { last = cur; last_change = armGetSystemTick(); dumped = 0; continue; }
    if (s_suspended) { last_change = armGetSystemTick(); dumped = 0; continue; }

    /* NOTE: do NOT skip when cur==0. The engine hangs *inside the first
     * nativeTick*, so the counter never leaves 0 -- that is exactly the case we
     * most need to dump. The timer starts when the loop is entered. */
    u64 stalled = (armGetSystemTick() - last_change) / hz;
    if (stalled >= STALL_SECS && !dumped) {
      debugPrintf("[wd] frame counter stuck at %llu for %llus -- STALL%s\n",
                  (unsigned long long)cur, (unsigned long long)stalled,
                  cur == 0 ? " (hung inside the FIRST nativeTick)" : "");
      dump_all();
      dumped = 1;
    }
  }
}

void watchdog_start(so_module *mod) {
  if (s_started) return;
  s_mod = mod;
  watchdog_register_thread();
  /* Run above the 0x2C main thread so a busy loop cannot starve diagnostics. */
  Result rc = threadCreate(&s_wd_thread, watchdog_main, NULL, NULL, 0x4000, 0x2B, -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&s_wd_thread);
  if (R_SUCCEEDED(rc)) {
    s_started = 1;
    debugPrintf("[wd] watchdog armed (stall=%llus)\n", (unsigned long long)STALL_SECS);
  } else {
    debugPrintf("[wd] failed to start watchdog thread rc=0x%x\n", rc);
  }
}
