/* imports.c -- import resolution for the native game library
 *
 * Pthread wrappers, lazy-init synchronization handling, and the import table
 * bridge Android/Bionic symbols to their Switch-compatible implementations.
 * The Java-facing side is implemented by jni_fake.c.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <locale.h>
#include <malloc.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <zlib.h>
#include <switch.h>

#include "config.h"
#include "imports.h"
#include "so_util.h"
#include "util.h"
#include "platform.h"
#include "libc_shim.h"
#include "os_shims.h"
#include "opensles.h"

/* newlib provides these symbols but no prototype (BSD non-sigmask jmp). */
extern int  _setjmp(void *env);
extern void _longjmp(void *env, int val);

/* newlib provides these as globals; the engine imports them by name. */
extern uintptr_t __cxa_atexit;
// Stack Smashing Protection symbols the engine imports; provided by the
// toolchain's SSP runtime. We hand the library the host's own guard + handler.
extern uintptr_t __stack_chk_fail;
extern uintptr_t __stack_chk_guard;

/* ------------------------------------------------------------------------- */
/* stubs for imports we deliberately neutralise                              */
/* ------------------------------------------------------------------------- */

/* networking: no sockets in a homebrew port. Fail so the engine's optional
 * ad/telemetry/cloud paths detect "offline" and skip themselves. errno=ENOSYS
 * keeps any error-string logging sane. */
static long net_fail_stub(void) { errno = BIONIC_ENOSYS; return -1; }

/* Google/bionic scheduler hints used to bracket blocking regions for the ART
 * runtime. There is no ART here; they are pure no-ops. */
static void noop_stub(void) {}

/* Android logging -> our debug log. Normal compatibility builds retain
 * warnings/errors/fatals but suppress verbose/debug/info chatter. Menu-heavy
 * screens can emit thousands of low-priority lines through this global mutex. */
static int android_log_enabled(int prio) {
  return PVZ2_ENABLE_VERBOSE_RUNTIME_LOG || prio >= 5;
}

static int __android_log_print_stub(int prio, const char *tag, const char *fmt, ...) {
  if (!android_log_enabled(prio)) return 0;
  char buf[1024];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return debugPrintf("[AB/%s] %s\n", tag ? tag : "?", buf);
}
static void __android_log_assert_stub(const char *cond, const char *tag, const char *fmt, ...) {
  char buf[1024] = {0};
  if (fmt) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
  }
  debugPrintf("[AB/%s] ASSERT(%s): %s\n", tag ? tag : "?", cond ? cond : "", buf);
}
static int __android_log_write_stub(int prio, const char *tag, const char *text) {
  if (!android_log_enabled(prio)) return 0;
  return debugPrintf("[PVZ2/%s] %s\n", tag ? tag : "?", text ? text : "");
}
static int __android_log_vprint_stub(int prio, const char *tag, const char *fmt, va_list ap) {
  if (!android_log_enabled(prio)) return 0;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  return debugPrintf("[PVZ2/%s] %s\n", tag ? tag : "?", buf);
}
static void __assert2_stub(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assert: %s:%d %s: %s\n", file, line, func, expr);
  abort();
}


// ---------------------------------------------------------------------------
// pthread wrappers: bionic allocates the opaque sync types inline and zero-
// inits them, so we lazily back them with heap-allocated newlib objects
// stashed through the caller's pointer slot.
// ---------------------------------------------------------------------------

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

/* forward decls: the lock/unlock/trylock wrappers below use these, but the
 * definitions live further down (after pthread_cond_init_fake they depend on). */
static int ensure_mutex(pthread_mutex_t **uid);
static int ensure_cond(pthread_cond_t **cnd);

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}

/* Lazy-init of engine mutexes/condvars (created from bionic's static
 * PTHREAD_*_INITIALIZER sentinels) must be ATOMIC: two threads hitting the same
 * still-uninitialized object would otherwise each create a separate real
 * object, and a signal on one would never wake a waiter on the other -> a
 * permanent condvarWaitTimeout hang. Serialize the check-and-create with one
 * global lock (statically initialized, so no chicken-and-egg). */
static pthread_mutex_t g_lazy_lock = PTHREAD_MUTEX_INITIALIZER;

static int ensure_mutex(pthread_mutex_t **uid) {
  int ret = 0;
  pthread_mutex_lock(&g_lazy_lock);
  uintptr_t cur = (uintptr_t)*uid;
  if (cur == 0)          { int a = 0; ret = pthread_mutex_init_fake(uid, &a); }
  else if (cur == 0x4000){ int a = 1; ret = pthread_mutex_init_fake(uid, &a); } // recursive sentinel
  pthread_mutex_unlock(&g_lazy_lock);
  return ret;
}

static int ensure_cond(pthread_cond_t **cnd) {
  int ret = 0;
  pthread_mutex_lock(&g_lazy_lock);
  if (!*cnd) ret = pthread_cond_init_fake(cnd, NULL);
  pthread_mutex_unlock(&g_lazy_lock);
  return ret;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (ensure_cond(cnd) < 0) return -1;
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (ensure_cond(cnd) < 0) return -1;
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

/* COND_WAIT_CAP_MS comes from config.h */

/* Cap the UNTIMED wait, not just the timed one.
 *
 * The engine blocks its main thread in a plain pthread_cond_wait (render sync /
 * asset preload handoff). If a signal is raced/lost -- or a bionic static-cond
 * object is mismatched across signal/wait -- that wait NEVER returns and the
 * whole engine hangs forever with the main thread parked in condvarWaitTimeout.
 * That is precisely our "reaches STAGE 14, first nativeTick never returns" hang.
 *
 * Waking every ~16ms and reporting it as a SPURIOUS WAKEUP lets the caller
 * re-check its predicate. This is POSIX-legal (a correct waiter always loops on
 * its predicate), so it cannot break correct code -- it only breaks the stall.
 */
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (ensure_cond(cnd) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;

#if COND_WAIT_CAP_MS <= 0
  /* NO CAP: a real, indefinite blocking wait. The thread sleeps until it is
   * actually signalled and burns ZERO cpu while idle.
   *
   * The cap was a defence against a lost wakeup, from back when that was the
   * suspected cause of the hangs. It wasn't -- the real bug was returning
   * newlib's ETIMEDOUT (116) where the engine's boost expected bionic's (110),
   * so every timed wait threw. With that fixed, the cap only forced every idle
   * engine thread to wake ~62x/second for nothing (the 99% cpu).
   *
   * boost::condition_variable::wait() throws on ANY non-zero return, so a clean
   * 0 from a genuine signal is exactly what it wants. */
  return pthread_cond_wait(*cnd, *mtx);
#else
  struct timespec cap;
  clock_gettime(CLOCK_REALTIME, &cap);          /* newlib's condvar clock */
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  += (cap.tv_nsec + add) / 1000000000L;
  cap.tv_nsec  = (cap.tv_nsec + add) % 1000000000L;

  int r = pthread_cond_timedwait(*cnd, *mtx, &cap);
  return (r == ETIMEDOUT) ? 0 : r;              /* timeout -> spurious wakeup */
#endif
}

/* ---------------------------------------------------------------------------
 * BIONIC errno values.
 *
 * libnative.so was compiled against bionic; we run on newlib. For errno codes
 * below ~34 the two agree, but above that they DIVERGE:
 *
 *     ETIMEDOUT   newlib 116   bionic 110
 *     ENOSYS      newlib  88   bionic  38
 *     EDEADLK     newlib  45   bionic  35
 *     ENOTSUP     newlib 134   bionic  95
 *
 * pthread functions return these codes BY VALUE, so a shim returning newlib's
 * number hands the engine a code it cannot recognise. This was fatal:
 *
 *     boost::condition_variable::do_wait_until():
 *         res = pthread_cond_timedwait(...);
 *         if (res == ETIMEDOUT) return false;      // bionic's 110
 *         if (res) throw condition_error(res);     // anything else -> FATAL
 *
 * We returned 116, boost compared against 110, so every timeout threw an
 * uncaught boost::condition_error -> std::terminate -> abort. That is the
 * "terminating with uncaught exception ... do_wait_until failed in
 * pthread_cond_timedwait: Connection timed out" crash.
 * ------------------------------------------------------------------------- */
/* BIONIC_* errno values live in libc_shim.h (shared with libc_shim.c) */

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (ensure_cond(cnd) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;
  if (!t) return pthread_cond_wait_fake(cnd, mtx);

  // Some engine threads set their condvar clock to CLOCK_MONOTONIC and compute
  // the deadline from clock_gettime(CLOCK_MONOTONIC); newlib's condvar waits on
  // CLOCK_REALTIME. Reduce the caller's absolute deadline to a relative delay
  // against whichever clock it matches, then rebuild it as a REALTIME deadline
  // newlib understands.
  struct timespec mono, real;
  clock_gettime(CLOCK_MONOTONIC, &mono);
  clock_gettime(CLOCK_REALTIME, &real);
  long long t_ns    = (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
  long long mono_ns = (long long)mono.tv_sec * 1000000000LL + mono.tv_nsec;
  long long real_ns = (long long)real.tv_sec * 1000000000LL + real.tv_nsec;
  long long rel_m = t_ns - mono_ns;
  long long rel_r = t_ns - real_ns;
  long long rel = (llabs(rel_m) <= llabs(rel_r)) ? rel_m : rel_r;
  if (rel < 0) rel = 0;

  /* Honour the caller's deadline exactly: boost's wait_for()/wait_until() treat
   * an early return as a REAL timeout, so shortening it makes callers take their
   * timeout path far too soon. A timed wait cannot hang by construction (it has
   * a deadline), so no clamp is needed once the clock basis is right. */
#if COND_WAIT_CAP_MS > 0
  const long long MAX_WAIT_NS = 1000000000LL;      /* 1s safety clamp */
  if (rel > MAX_WAIT_NS) rel = MAX_WAIT_NS;
#endif

  long long deadline = real_ns + rel;
  struct timespec abs;
  abs.tv_sec  = (time_t)(deadline / 1000000000LL);
  abs.tv_nsec = (long)(deadline % 1000000000LL);

  int r = pthread_cond_timedwait(*cnd, *mtx, &abs);

  /* Translate newlib's return code into what bionic code expects. Returning
   * newlib's ETIMEDOUT (116) here made boost throw a fatal condition_error. */
  /* libnx returns Result 0xEA01 directly from condvarWaitTimeout. */
  if (r == ETIMEDOUT || (unsigned)r == 0xEA01u) return BIONIC_ETIMEDOUT;
  return r;                                        /* 0, or a code that matches */
}

/* Exactly one thread runs init_routine; all other callers wait for it to
 * complete. States: 0 = not started, 1 = in progress, 2 = done. */
int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  for (;;) {
    int prev = __sync_val_compare_and_swap(once_control, 0, 1);
    if (prev == 0) {                 // we won: run init, then publish "done"
      (*init_routine)();
      __sync_lock_test_and_set(once_control, 2);
      return 0;
    }
    if (prev == 2)                   // already fully initialized
      return 0;
    svcSleepThread(50000);           // in progress on another thread: wait 50us
  }
}

// Engine worker threads must get tpidr_el0 pointed at a stack-guard block before
// they run any guarded code (see tls_setup_guard in util.c)
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  pin_current_thread();         /* SINGLE_CORE: serialize all engine threads */
  return ts.entry(ts.arg);
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;

  /* The engine was built against bionic, whose DEFAULT thread stack is 1 MB.
   * devkitPro's pthread default is far smaller, so the engine's worker threads
   * (asset decode, XML parse, audio mixing) overflow their stack and fault at
   * random addresses. Give them a bionic-sized stack. We ignore the caller's
   * attr (bionic's pthread_attr_t layout differs) and just size up. */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
  int r = pthread_create(thread, &attr, thread_trampoline, ts);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    debugPrintf("pthread_create(%p) failed: %d\n", entry, r);
    free(ts);
  }
  return r;
}

// bionic pthread_mutexattr_t is an int; we store the type plainly and read it
// back in pthread_mutex_init_fake (PTHREAD_MUTEX_RECURSIVE == 1 in bionic).
int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// rwlock slots are also lazy void* (the rdlock/wrlock/unlock fakes in
// libc_shim.c create the libnx primitive on first use). init just clears the
// slot; destroy frees the FakeRwLock if one was created.
int pthread_rwlock_init_fake(void **rw, const void *attr) {
  (void)attr;
  if (rw) *rw = NULL;
  return 0;
}
int pthread_rwlock_destroy_fake(void **rw) {
  if (rw && *rw) { free(*rw); *rw = NULL; }
  return 0;
}

int pthread_condattr_setclock_fake(void *attr, int clock_id) { (void)attr; (void)clock_id; return 0; }

int pthread_detach_fake(pthread_t t) { (void)t; return 0; } // libnx reaps on exit
int pthread_equal_fake(unsigned long a, unsigned long b) { return a == b; }
// a stable, unique per-thread token (the thread's TLS base); good enough for
// the engine's "am I on the main thread" comparisons.
unsigned long pthread_self_fake(void) { return (unsigned long)(uintptr_t)armGetTls(); }


/* ------------------------------------------------------------------------- */
/* import table for the native game library */
/* ------------------------------------------------------------------------- */


/* ===========================================================================
 * Supplementary imports required by the PVZ2 engine: extra GLES2/EGL entry
 * points and libc, plus networking /
 * process / locale / OpenSL ES symbols. Real functions map straight through
 * (see the table); the definitions below cover the ones that need a wrapper
 * (bionic->newlib ABI, _l locale variants, _chk fortify, sincos) or a safe
 * offline stub (sockets, process control, dlopen -- all unused on Switch).
 * ========================================================================= */

/* generic stubs (offline-safe) */
static int   imp_retm1(void)   { return -1; }
static int   imp_ret0(void)    { return 0; }
static void *imp_retnull(void) { return NULL; }
static void  imp_retvoid(void) { }
static float imp_retf0(void) { return 0.0f; }

/* Anzu is the APK's optional ad/video SDK. Returning unavailable keeps its
 * integration dormant without affecting the game renderer or gameplay. */
static int getentropy_stub(void *buffer, size_t length) {
  if (length > 256) { errno = EIO; return -1; }
  return R_SUCCEEDED(csrngGetRandomBytes(buffer, length)) ? 0 : -1;
}

/* Android libc++ random_device opens /dev/urandom, which Horizon does not
 * expose. Route its tiny interface directly to Switch's cryptographic RNG. */
static void random_device_ctor_stub(void *self, const void *token) {
  (void)self; (void)token;
}
static void random_device_dtor_stub(void *self) { (void)self; }
static unsigned random_device_call_stub(void *self) {
  unsigned value;
  (void)self;
  return R_SUCCEEDED(csrngGetRandomBytes(&value, sizeof(value))) ? value : 0x6d6f6465u;
}
static double random_device_entropy_stub(void *self) { (void)self; return 0.0; }

/* Has an OUTPUT parameter: must initialize *state (0 == PTHREAD_CREATE_JOINABLE
 * in bionic) rather than leave it as uninitialized caller memory. */
static int pthread_attr_getdetachstate_stub(void *attr, int *state) {
  (void)attr;
  if (state) *state = 0;
  return 0;
}

static int pthread_attr_init_stub(void *attr) { if (attr) memset(attr, 0, sizeof(int)); return 0; }
static int pthread_attr_noop_stub(void *attr) { (void)attr; return 0; }
static int pthread_attr_setint_stub(void *attr, int value) { (void)attr; (void)value; return 0; }

static ssize_t read_chk_w(int fd, void *buf, size_t count, size_t buflen) {
  if (count > buflen) abort();
  return read_fake(fd, buf, count);
}
static ssize_t write_chk_w(int fd, const void *buf, size_t count, size_t buflen) {
  if (count > buflen) abort();
  return write(fd, buf, count);
}
static size_t strlcpy_chk_w(char *dst, const char *src, size_t size, size_t dstlen) {
  if (size > dstlen) abort();
  const size_t len = strlen(src);
  if (size) {
    const size_t copy = len < size - 1 ? len : size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
  }
  return len;
}
static int FD_ISSET_chk_w(int fd, const void *set, size_t setlen) {
  if (!set || fd < 0 || (size_t)fd >= setlen * 8) return 0;
  return (((const unsigned long *)set)[fd / (8 * sizeof(long))] >>
           (fd % (8 * sizeof(long)))) & 1;
}

static FILE *stdin_compat = (FILE *)&fake_sF[0];
static FILE *stdout_compat = (FILE *)&fake_sF[1];
static FILE *stderr_compat = (FILE *)&fake_sF[2];
static long timezone_compat;

static char *basename_w(char *path) {
  static char dot[] = ".";
  if (!path || !*path) return dot;
  char *end = path + strlen(path) - 1;
  while (end > path && *end == '/') *end-- = '\0';
  if (path[0] == '/' && !path[1]) return path;
  char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char *dirname_w(char *path) {
  static char dot[] = ".";
  if (!path || !*path) return dot;
  char *end = path + strlen(path) - 1;
  while (end > path && *end == '/') *end-- = '\0';
  char *slash = strrchr(path, '/');
  if (!slash) return dot;
  while (slash > path && slash[-1] == '/') slash--;
  if (slash == path) { path[1] = '\0'; return path; }
  *slash = '\0';
  return path;
}

/* newlib has memalign but not posix_memalign; wrap it. */
int posix_memalign_w(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

/* bionic's _ctype_ is a `const char*` into a 257-byte classic-BSD table with
 * bionic's flag bits (U=1 L=2 N=4 S=8 P=16 C=32 X=64 B=128). newlib's _ctype_
 * is a different array, so classifying with it corrupts the engine's parsers.
 * Provide a bionic-layout table; the "_ctype_" import points at the pointer
 * variable below so the engine's inlined `(_ctype_+1)[c]` works. */
static const unsigned char _bionic_ctype_tab[257] = {
  0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xa8, 0x28, 0x28, 0x28, 0x28, 0x20,
  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
  0x20, 0x88, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
  0x10, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x10, 0x10, 0x10, 0x10, 0x10,
  0x10, 0x10, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x10, 0x10, 0x10,
  0x10, 0x10, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
  0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x10, 0x10, 0x10, 0x10,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,


  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00,
};
const char *_ctype_compat_ptr = (const char *)_bionic_ctype_tab;

/* --- GL shader compatibility wrappers. Keep the ES-version repair but
 * no longer serializes every successful shader/source/link to the SD log.  On
 * this title those diagnostics run during Native_onSurfaceCreated and visibly
 * extend the pre-PopCap startup.  Failures still print the driver error log. */
GLuint glCreateShader_log(GLenum type) {
  GLuint s = glCreateShader(type);
#if PVZ2_ENABLE_GL_TRACE
  debugPrintf("gl: createShader(%s) -> %u\n",
              type == 0x8B31 ? "VERT" : type == 0x8B30 ? "FRAG" : "?", s);
#else
  (void)type;
#endif
  return s;
}
void glShaderSource_log(GLuint s, GLsizei c, const GLchar *const *str, const GLint *len) {
  static char src[8192];
  int p = 0;
  for (GLsizei i = 0; i < c && p < (int)sizeof src - 1; i++) {
    const char *chunk = str[i]; if (!chunk) continue;
    int cl = (len && len[i] > 0) ? len[i] : (int)strlen(chunk);
    for (int k = 0; k < cl && p < (int)sizeof src - 1; k++) src[p++] = chunk[k];
  }
  src[p] = 0;

  int es1_body = (strstr(src, "attribute ") || strstr(src, "varying ") ||
                  strstr(src, "texture2D") || strstr(src, "gl_FragColor"));
  char *ver = strstr(src, "#version");
  int patched = 0;
  if (ver && es1_body) {
    int vn = 0; const char *q = ver + 8; while (*q == ' ') q++;
    while (*q >= '0' && *q <= '9') { vn = vn * 10 + (*q - '0'); q++; }
    if (vn >= 300) {
      char *nl = strchr(ver, '\n');
      if (nl) { for (char *w = ver; w <= nl; w++) *w = ' '; patched = 1; }
    }
  }

#if PVZ2_ENABLE_GL_TRACE
  debugPrintf("gl: shaderSource(%u, %d)%s\n", s, c,
              patched ? " [stripped bad #version]" : "");
  debugPrintf("gl: --- src ---\n%s\n----------\n", src);
#else
  if (patched)
    debugPrintf("gl: shaderSource(%u) stripped incompatible ES3 #version\n", s);
#endif

  if (patched) {
    const char *one = src; GLint onelen = p;
    glShaderSource(s, 1, &one, &onelen);
  } else {
    glShaderSource(s, c, str, len);
  }
}
void glCompileShader_log(GLuint s) {
#if PVZ2_ENABLE_GL_TRACE
  debugPrintf("gl: compileShader(%u) start\n", s);
#endif
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, 0x8B81 /*GL_COMPILE_STATUS*/, &ok);
#if PVZ2_ENABLE_GL_TRACE
  debugPrintf("gl: compileShader(%u) -> %s\n", s, ok ? "OK" : "FAIL");
#endif
  if (!ok) {
    char lg[600]; GLsizei n = 0; glGetShaderInfoLog(s, sizeof lg - 1, &n, lg);
    lg[(n < (GLsizei)sizeof lg) ? n : (GLsizei)sizeof lg - 1] = 0;
    debugPrintf("gl: SHADER ERROR %u: %s\n", s, lg);
  }
}
void glLinkProgram_log(GLuint p) {
#if PVZ2_ENABLE_GL_TRACE
  debugPrintf("gl: linkProgram(%u) start\n", p);
#endif
  glLinkProgram(p);
  GLint ok = 0; glGetProgramiv(p, 0x8B82 /*GL_LINK_STATUS*/, &ok);
#if PVZ2_ENABLE_GL_TRACE
  debugPrintf("gl: linkProgram(%u) -> %s\n", p, ok ? "OK" : "FAIL");
#endif
  if (!ok) {
    char lg[600]; GLsizei n = 0; glGetProgramInfoLog(p, sizeof lg - 1, &n, lg);
    lg[(n < (GLsizei)sizeof lg) ? n : (GLsizei)sizeof lg - 1] = 0;
    debugPrintf("gl: LINK ERROR %u: %s\n", p, lg);
  }
}

/* pthread attr helper */
int pthread_mutexattr_destroy_fake(int *a)         { (void)a;         return 0; }

/* math */
void sincos_w(double x, double *s, double *c)  { *s = sin(x);  *c = cos(x);  }
void sincosf_w(float x, float *s, float *c)    { *s = sinf(x); *c = cosf(x); }

/* fortify (_chk) -> plain calls; we don't enforce the object-size guard */
int  strlen_chk_w(const char *s, size_t n)                       { (void)n; return strlen(s); }
void *memmove_chk_w(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memmove(d, s, n); }
int  vsnprintf_chk_w(char *s, size_t maxlen, int flag, size_t slen,
                     const char *fmt, va_list ap) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, ap); }
void FD_SET_chk_w(int fd, void *set, size_t sz)  { (void)fd; (void)set; (void)sz; }

/* locale: the engine never really needs per-locale behaviour offline, so the
 * _l variants defer to the current (C) locale and newlocale/uselocale hand back
 * a harmless non-NULL token. locale_t is taken as void* to avoid header churn. */
void *newlocale_w(int cat, const char *loc, void *base) { (void)cat;(void)loc;(void)base; return (void*)1; }
void *uselocale_w(void *loc)                            { (void)loc; return (void*)1; }

int iswlower_lw(wint_t c, void *l){(void)l;return iswlower(c);}  int iswspace_lw(wint_t c, void *l){(void)l;return iswspace(c);}
int iswprint_lw(wint_t c, void *l){(void)l;return iswprint(c);}  int iswblank_lw(wint_t c, void *l){(void)l;return iswblank(c);}
int iswcntrl_lw(wint_t c, void *l){(void)l;return iswcntrl(c);}  int iswupper_lw(wint_t c, void *l){(void)l;return iswupper(c);}
int iswalpha_lw(wint_t c, void *l){(void)l;return iswalpha(c);}  int iswdigit_lw(wint_t c, void *l){(void)l;return iswdigit(c);}
int iswpunct_lw(wint_t c, void *l){(void)l;return iswpunct(c);}  int iswxdigit_lw(wint_t c, void *l){(void)l;return iswxdigit(c);}
int isxdigit_lw(int c, void *l){(void)l;return isxdigit(c);}     int isdigit_lw(int c, void *l){(void)l;return isdigit(c);}
int islower_lw(int c, void *l){(void)l;return islower(c);}       int isupper_lw(int c, void *l){(void)l;return isupper(c);}
int toupper_lw(int c, void *l){(void)l;return toupper(c);}       int tolower_lw(int c, void *l){(void)l;return tolower(c);}
wint_t towupper_lw(wint_t c, void *l){(void)l;return towupper(c);} wint_t towlower_lw(wint_t c, void *l){(void)l;return towlower(c);}

size_t strftime_lw(char *s, size_t m, const char *f, const struct tm *t, void *l){(void)l;return strftime(s,m,f,t);}
int    strcoll_lw(const char *a, const char *b, void *l){(void)l;return strcoll(a,b);}
size_t strxfrm_lw(char *d, const char *s, size_t n, void *l){(void)l;return strxfrm(d,s,n);}
int    wcscoll_lw(const wchar_t *a, const wchar_t *b, void *l){(void)l;return wcscoll(a,b);}
size_t wcsxfrm_lw(wchar_t *d, const wchar_t *s, size_t n, void *l){(void)l;return wcsxfrm(d,s,n);}
long long strtoll_lw(const char *n, char **e, int base, void *l){(void)l;return strtoll(n,e,base);}
unsigned long long strtoull_lw(const char *n, char **e, int base, void *l){(void)l;return strtoull(n,e,base);}
long double strtold_lw(const char *n, char **e, void *l){(void)l;return strtold(n,e);}


/* ------------------------------------------------------------------------- */
/* Allocation provenance for the pending request                              */
/* ------------------------------------------------------------------------- */

#define PVZ2_ALLOC_TRACE_RING 2048u

typedef struct Pvz2AllocTraceEvent {
  uintptr_t base;
  size_t size;
  uintptr_t caller;
  uintptr_t parent1;
  uintptr_t parent2;
  uintptr_t parent3;
  unsigned sequence;
  unsigned kind;
} Pvz2AllocTraceEvent;

static Pvz2AllocTraceEvent g_alloc_trace[PVZ2_ALLOC_TRACE_RING];
static unsigned g_alloc_trace_sequence;

static uintptr_t alloc_trace_parent_lr(uintptr_t fp, uintptr_t *next_fp) {
  if (next_fp) *next_fp = 0;
  if (!fp || (fp & 0xf)) return 0;
  MemoryInfo mi;
  u32 page_info = 0;
  if (R_FAILED(svcQueryMemory(&mi, &page_info, fp)) ||
      !(mi.perm & Perm_R) || fp < mi.addr || fp + 16 > mi.addr + mi.size)
    return 0;
  const uintptr_t next = *(const uintptr_t *)fp;
  const uintptr_t lr = *(const uintptr_t *)(fp + 8);
  if (next_fp && next > fp && !(next & 0xf)) *next_fp = next;
  return lr;
}

static void alloc_trace_record(void *ptr, size_t size, unsigned kind,
                               uintptr_t caller, uintptr_t fp) {
  if (!ptr || !size) return;
  /* Capture a few parent return addresses only for object-sized allocations.
   * This keeps malloc/calloc overhead tiny while still resolving C++ operator
   * new -> libPVZ2 constructor call chains for the ~0x80-byte blocker object. */
  uintptr_t parent1 = 0, parent2 = 0, parent3 = 0;
  if (size >= 0x40 && size <= 0x1000) {
    uintptr_t next = 0;
    (void)alloc_trace_parent_lr(fp, &next); /* wrapper frame */
    if (next) {
      uintptr_t next2 = 0;
      parent1 = alloc_trace_parent_lr(next, &next2);
      if (next2) {
        uintptr_t next3 = 0;
        parent2 = alloc_trace_parent_lr(next2, &next3);
        if (next3) parent3 = alloc_trace_parent_lr(next3, NULL);
      }
    }
  }
  const unsigned seq = __atomic_add_fetch(&g_alloc_trace_sequence, 1,
                                           __ATOMIC_RELAXED);
  Pvz2AllocTraceEvent *event = &g_alloc_trace[(seq - 1) % PVZ2_ALLOC_TRACE_RING];
  *event = (Pvz2AllocTraceEvent){
      .base = (uintptr_t)ptr, .size = size, .caller = caller,
      .parent1 = parent1, .parent2 = parent2, .parent3 = parent3,
      .sequence = seq, .kind = kind};
}

static void *pvz2_malloc_traced(size_t size) {
  uintptr_t caller = 0, fp = 0;
  __asm__ volatile("mov %0, x30\nmov %1, x29" : "=r"(caller), "=r"(fp));
  void *ptr = malloc(size);
  alloc_trace_record(ptr, size, 1, caller, fp);
  return ptr;
}

static void *pvz2_calloc_traced(size_t count, size_t size) {
  uintptr_t caller = 0, fp = 0;
  __asm__ volatile("mov %0, x30\nmov %1, x29" : "=r"(caller), "=r"(fp));
  void *ptr = calloc(count, size);
  size_t total = 0;
  if (!__builtin_mul_overflow(count, size, &total))
    alloc_trace_record(ptr, total, 2, caller, fp);
  return ptr;
}

static void *pvz2_realloc_traced(void *old_ptr, size_t size) {
  uintptr_t caller = 0, fp = 0;
  __asm__ volatile("mov %0, x30\nmov %1, x29" : "=r"(caller), "=r"(fp));
  void *ptr = realloc(old_ptr, size);
  if (ptr && size) alloc_trace_record(ptr, size, 3, caller, fp);
  return ptr;
}

int pvz2_alloc_trace_lookup(const void *address, Pvz2AllocTraceInfo *out) {
  if (!address || !out) return 0;
  const uintptr_t target = (uintptr_t)address;
  unsigned best = 0;
  Pvz2AllocTraceEvent found = {0};
  for (unsigned i = 0; i < PVZ2_ALLOC_TRACE_RING; ++i) {
    const Pvz2AllocTraceEvent *event = &g_alloc_trace[i];
    if (!event->sequence || !event->base || !event->size) continue;
    if (target < event->base) continue;
    const uintptr_t delta = target - event->base;
    if (delta >= event->size) continue;
    if (event->sequence > best) {
      best = event->sequence;
      found = *event;
    }
  }
  if (!best) return 0;
  *out = (Pvz2AllocTraceInfo){
      .base = found.base, .size = found.size, .caller = found.caller,
      .parent1 = found.parent1, .parent2 = found.parent2,
      .parent3 = found.parent3, .sequence = found.sequence,
      .kind = found.kind};
  return 1;
}

DynLibFunction dynlib_functions[] = {
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "Anzu_ApplicationActive", (uintptr_t)&imp_retvoid },
  { "Anzu_GetVersionFloat", (uintptr_t)&imp_retf0 },
  { "Anzu_Initialize", (uintptr_t)&imp_ret0 },
  { "Anzu_RegisterLogCallback", (uintptr_t)&imp_retvoid },
  { "Anzu_RegisterMessageCallback", (uintptr_t)&imp_retvoid },
  { "Anzu_RegisterTextureInitCallback", (uintptr_t)&imp_retvoid },
  { "Anzu_RegisterTextureUpdateCallback", (uintptr_t)&imp_retvoid },
  { "Anzu_RegisterUriSchemaHook", (uintptr_t)&imp_retvoid },
  { "Anzu_SetCoppaRegulated", (uintptr_t)&imp_retvoid },
  { "Anzu_SetGDPRConsent", (uintptr_t)&imp_retvoid },
  { "Anzu_Uninitialize", (uintptr_t)&imp_retvoid },
  { "Anzu__Texture_CreateInstance", (uintptr_t)&imp_retnull },
  { "Anzu__Texture_NativeRenderer_AssignCustomHandler", (uintptr_t)&imp_retvoid },
  { "Anzu__Texture_NativeRenderer_GetRenderCallback", (uintptr_t)&imp_retnull },
  { "Anzu__Texture_NativeRenderer_GetRenderID", (uintptr_t)&imp_ret0 },
  { "Anzu__Texture_NativeRenderer_SetExpectedFormat", (uintptr_t)&imp_retvoid },
  { "Anzu__Texture_PausePlayback", (uintptr_t)&imp_retvoid },
  { "Anzu__Texture_RemoveInstance", (uintptr_t)&imp_retvoid },
  { "Anzu__Texture_ResumePlayback", (uintptr_t)&imp_retvoid },
  { "Anzu__Texture_SetVisibility", (uintptr_t)&imp_retvoid },
  { "Anzu__Texture_SetVisibilityScore", (uintptr_t)&imp_retvoid },
  { "__android_log_assert", (uintptr_t)&__android_log_assert_stub },
  { "__android_log_print", (uintptr_t)&__android_log_print_stub },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&noop_stub },
  { "__google_potentially_blocking_region_end", (uintptr_t)&noop_stub },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
  { "_ctype_", (uintptr_t)&_ctype_compat_ptr },
  { "abort", (uintptr_t)&abort },
  { "access", (uintptr_t)&access_fake },
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin },
  { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atof", (uintptr_t)&atof },
  { "basename", (uintptr_t)&basename_w },
  { "bind", (uintptr_t)&net_fail_stub },
  { "bsearch", (uintptr_t)&bsearch },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&pvz2_calloc_traced },
  { "chmod", (uintptr_t)&chmod },
  { "clock", (uintptr_t)&clock },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "close", (uintptr_t)&close_fake },
  { "closedir", (uintptr_t)&closedir_fake },
  { "connect", (uintptr_t)&net_fail_stub },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "cosh", (uintptr_t)&cosh },
  { "difftime", (uintptr_t)&difftime },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dup", (uintptr_t)&dup },
  { "exit", (uintptr_t)&exit },
  { "exp", (uintptr_t)&exp },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "fdopen", (uintptr_t)&fdopen_fake },
  { "feof", (uintptr_t)&feof_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "fnmatch", (uintptr_t)&fnmatch_fake },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "free", (uintptr_t)&free },
  { "freopen", (uintptr_t)&freopen_fake },
  { "frexp", (uintptr_t)&frexp },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "fsync", (uintptr_t)&fsync },
  { "ftell", (uintptr_t)&ftell_fake },
  { "ftello", (uintptr_t)&ftello_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "getenv", (uintptr_t)&getenv_fake },
  { "geteuid", (uintptr_t)&geteuid_fake },
  { "gethostname", (uintptr_t)&net_fail_stub },
  { "getpeername", (uintptr_t)&net_fail_stub },
  { "getpid", (uintptr_t)&getpid },
  { "getpwuid", (uintptr_t)&getpwuid_fake },
  { "getsockname", (uintptr_t)&net_fail_stub },
  { "getsockopt", (uintptr_t)&net_fail_stub },
  { "gettimeofday", (uintptr_t)&gettimeofday_fake },
  { "getwc", (uintptr_t)&getwc },
  { "glActiveTexture", (uintptr_t)&glActiveTexture_fake },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer_fake },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture_fake },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendFunc", (uintptr_t)&glBlendFunc_fake },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glClear", (uintptr_t)&glClear_fake },
  { "glClearColor", (uintptr_t)&glClearColor_fake },
  { "glCompileShader", (uintptr_t)&glCompileShader_log },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D_fake },
  { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader_log },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures_fake },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDetachShader", (uintptr_t)&glDetachShader },
  { "glDisable", (uintptr_t)&glDisable_fake },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays_fake },
  { "glDrawElements", (uintptr_t)&glDrawElements_fake },
  { "glEnable", (uintptr_t)&glEnable_fake },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFlush", (uintptr_t)&glFlush },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation_fake },
  { "glGetUniformfv", (uintptr_t)&glGetUniformfv },
  { "glLinkProgram", (uintptr_t)&glLinkProgram_log },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor_fake },
  { "glShaderSource", (uintptr_t)&glShaderSource_log },
  { "glTexImage2D", (uintptr_t)&glTexImage2D_fake },
  { "glTexParameteri", (uintptr_t)&glTexParameteri_fake },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D_fake },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1i", (uintptr_t)&glUniform1i_fake },
  { "glUniform4f", (uintptr_t)&glUniform4f_fake },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv_fake },
  { "glUseProgram", (uintptr_t)&glUseProgram_fake },
  { "glValidateProgram", (uintptr_t)&glValidateProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer_fake },
  { "glViewport", (uintptr_t)&glViewport_fake },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "inet_addr", (uintptr_t)&net_fail_stub },
  { "inet_ntop", (uintptr_t)&net_fail_stub },
  { "inet_pton", (uintptr_t)&net_fail_stub },
  { "ioctl", (uintptr_t)&net_fail_stub },
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "islower", (uintptr_t)&islower },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "iswctype", (uintptr_t)&iswctype },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "ldexp", (uintptr_t)&ldexp },
  { "localtime", (uintptr_t)&localtime },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "longjmp", (uintptr_t)&longjmp },
  { "lrint", (uintptr_t)&lrint },
  { "lrintf", (uintptr_t)&lrintf },
  { "lseek", (uintptr_t)&lseek_fake },
  { "malloc", (uintptr_t)&pvz2_malloc_traced },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memrchr", (uintptr_t)&memrchr },
  { "memset", (uintptr_t)&memset },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "mktime", (uintptr_t)&mktime },
  { "mmap", (uintptr_t)&mmap_fake },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "munmap", (uintptr_t)&munmap_fake },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "open", (uintptr_t)&open_fake },
  { "opendir", (uintptr_t)&opendir_fake },
  { "pathconf", (uintptr_t)&pathconf },
  { "pipe", (uintptr_t)&net_fail_stub },
  { "poll", (uintptr_t)&net_fail_stub },
  { "pow", (uintptr_t)&pow },
  { "printf", (uintptr_t)&printf_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake },
  { "pthread_equal", (uintptr_t)&pthread_equal_fake },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_fake },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_self", (uintptr_t)&pthread_self_fake },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific_fake },
  { "putc", (uintptr_t)&putc },
  { "puts", (uintptr_t)&puts_fake },
  { "putwc", (uintptr_t)&putwc },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "read", (uintptr_t)&read_fake },
  { "readdir_r", (uintptr_t)&imp_retm1 },   /* not imported by the engine; raw version would deref our FakeDir* */
  { "realloc", (uintptr_t)&pvz2_realloc_traced },
  { "recv", (uintptr_t)&net_fail_stub },
  { "recvfrom", (uintptr_t)&net_fail_stub },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "rmdir", (uintptr_t)&rmdir },
  { "sched_yield", (uintptr_t)&sched_yield },
  { "send", (uintptr_t)&net_fail_stub },
  { "setjmp", (uintptr_t)&setjmp },
  { "setlocale", (uintptr_t)&setlocale_fake },
  { "setsockopt", (uintptr_t)&net_fail_stub },
  { "setvbuf", (uintptr_t)&setvbuf_fake },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sinh", (uintptr_t)&sinh },
  { "snprintf", (uintptr_t)&snprintf },
  { "socket", (uintptr_t)&net_fail_stub },
  { "sprintf", (uintptr_t)&sprintf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "srand", (uintptr_t)&srand },
  { "sscanf", (uintptr_t)&sscanf },
  { "stat", (uintptr_t)&stat_fake },
  { "statfs", (uintptr_t)&statfs_fake },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh },
  { "time", (uintptr_t)&time },
  { "tmpfile", (uintptr_t)&tmpfile_fake },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "unlink", (uintptr_t)&unlink_fake },
  { "utime", (uintptr_t)&utime_fake },
  { "vprintf", (uintptr_t)&vprintf_fake },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wctob", (uintptr_t)&wctob },
  { "wctype", (uintptr_t)&wctype },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "write", (uintptr_t)&write },
  { "writev", (uintptr_t)&writev_fake },
  /* ---- supplementary imports required by PVZ2 ---- */
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
  { "__FD_SET_chk", (uintptr_t)&FD_SET_chk_w },
  { "__memmove_chk", (uintptr_t)&memmove_chk_w },
  { "__strlen_chk", (uintptr_t)&strlen_chk_w },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__vsnprintf_chk", (uintptr_t)&vsnprintf_chk_w },
  { "_longjmp", (uintptr_t)&_longjmp },
  { "_setjmp", (uintptr_t)&_setjmp },
  { "accept", (uintptr_t)&imp_retm1 },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "atoi", (uintptr_t)&atoi },
  { "chdir", (uintptr_t)&imp_ret0 },
  { "clearerr", (uintptr_t)&clearerr_fake },
  { "closelog", (uintptr_t)&imp_retvoid },
  { "dlclose", (uintptr_t)&imp_ret0 },
  { "dlerror", (uintptr_t)&imp_retnull },
  { "dup2", (uintptr_t)&imp_retm1 },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig_fake },
  { "eglCreateContext", (uintptr_t)&eglCreateContext_fake },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface_fake },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay_fake },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglInitialize", (uintptr_t)&eglInitialize_fake },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent_fake },
  { "eglQuerySurface", (uintptr_t)&eglQuerySurface_fake },
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers_fake },
  { "epoll_create", (uintptr_t)&imp_retm1 },
  { "epoll_create1", (uintptr_t)&imp_retm1 },
  { "epoll_ctl", (uintptr_t)&imp_retm1 },
  { "epoll_wait", (uintptr_t)&imp_retm1 },
  { "eventfd", (uintptr_t)&imp_retm1 },
  { "execl", (uintptr_t)&imp_retm1 },
  { "flockfile", (uintptr_t)&flockfile },
  { "fork", (uintptr_t)&imp_retm1 },
  { "freeaddrinfo", (uintptr_t)&imp_retvoid },
  { "freelocale", (uintptr_t)&imp_retvoid },
  { "ftruncate", (uintptr_t)&imp_retm1 },
  { "funlockfile", (uintptr_t)&funlockfile },
  { "getaddrinfo", (uintptr_t)&imp_retm1 },
  { "getc_unlocked", (uintptr_t)&getc_unlocked },
  { "getegid", (uintptr_t)&imp_ret0 },
  { "getentropy", (uintptr_t)&getentropy_stub },
  { "getgid", (uintptr_t)&imp_ret0 },
  { "gethostbyname", (uintptr_t)&imp_retnull },
  { "getpwuid_r", (uintptr_t)&imp_retm1 },
  { "getservbyname", (uintptr_t)&imp_retnull },
  { "getuid", (uintptr_t)&imp_ret0 },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glColorMask", (uintptr_t)&glColorMask_fake },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glIsTexture", (uintptr_t)&glIsTexture },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
  { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilMask", (uintptr_t)&glStencilMask },
  { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1iv", (uintptr_t)&glUniform1iv },
  { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform2iv", (uintptr_t)&glUniform2iv },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform3iv", (uintptr_t)&glUniform3iv },
  { "glUniform4fv", (uintptr_t)&glUniform4fv_fake },
  { "glUniform4iv", (uintptr_t)&glUniform4iv },
  { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
  { "if_indextoname", (uintptr_t)&imp_retnull },
  { "if_nametoindex", (uintptr_t)&imp_retm1 },
  { "isdigit_l", (uintptr_t)&isdigit_lw },
  { "isgraph", (uintptr_t)&isgraph },
  { "islower_l", (uintptr_t)&islower_lw },
  { "isupper_l", (uintptr_t)&isupper_lw },
  { "iswalpha_l", (uintptr_t)&iswalpha_lw },
  { "iswblank_l", (uintptr_t)&iswblank_lw },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_lw },
  { "iswdigit_l", (uintptr_t)&iswdigit_lw },
  { "iswlower_l", (uintptr_t)&iswlower_lw },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswprint_l", (uintptr_t)&iswprint_lw },
  { "iswpunct_l", (uintptr_t)&iswpunct_lw },
  { "iswspace_l", (uintptr_t)&iswspace_lw },
  { "iswupper_l", (uintptr_t)&iswupper_lw },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_lw },
  { "isxdigit_l", (uintptr_t)&isxdigit_lw },
  { "kill", (uintptr_t)&imp_retm1 },
  { "link", (uintptr_t)&imp_retm1 },
  { "listen", (uintptr_t)&imp_retm1 },
  { "localeconv", (uintptr_t)&localeconv },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "lstat", (uintptr_t)&lstat_fake },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mkstemp", (uintptr_t)&mkstemp_fake },
  { "newlocale", (uintptr_t)&newlocale_w },
  { "openlog", (uintptr_t)&imp_retvoid },
  { "pclose", (uintptr_t)&imp_retm1 },
  { "popen", (uintptr_t)&imp_retnull },
  { "pthread_attr_getdetachstate", (uintptr_t)&pthread_attr_getdetachstate_stub },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "random", (uintptr_t)&random },
  { "_ZNKSt6__ndk113random_device7entropyEv", (uintptr_t)&random_device_entropy_stub },
  { "_ZNSt6__ndk113random_deviceC1ERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", (uintptr_t)&random_device_ctor_stub },
  { "_ZNSt6__ndk113random_deviceC2ERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", (uintptr_t)&random_device_ctor_stub },
  { "_ZNSt6__ndk113random_deviceD1Ev", (uintptr_t)&random_device_dtor_stub },
  { "_ZNSt6__ndk113random_deviceD2Ev", (uintptr_t)&random_device_dtor_stub },
  { "_ZNSt6__ndk113random_deviceclEv", (uintptr_t)&random_device_call_stub },
  /* Preserve native component registrations for later C++ lookups. */
  { "_ZN2EA6Nimble12BaseInternal25NimbleCppComponentManager17registerComponentERKNSt6__ndk112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS3_10shared_ptrINS1_18NimbleCppComponentEEE", (uintptr_t)&nimble_cpp_component_register },
  { "_ZN2EA6Nimble12BaseInternal25NimbleCppComponentManager12getComponentERKNSt6__ndk112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE", (uintptr_t)&nimble_cpp_component_get },
  { "readdir", (uintptr_t)&readdir_fake },
  { "readlink", (uintptr_t)&imp_retm1 },
  { "recvmsg", (uintptr_t)&imp_retm1 },
  { "rewind", (uintptr_t)&rewind_fake },
  { "select", (uintptr_t)&imp_retm1 },
  { "sendmsg", (uintptr_t)&imp_retm1 },
  { "sendto", (uintptr_t)&imp_retm1 },
  { "shutdown", (uintptr_t)&imp_retm1 },
  { "signal", (uintptr_t)&signal },
  { "sincos", (uintptr_t)&sincos_w },
  { "sincosf", (uintptr_t)&sincosf_w },
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "socketpair", (uintptr_t)&imp_retm1 },
  { "srandom", (uintptr_t)&srandom },
  { "strcoll_l", (uintptr_t)&strcoll_lw },
  { "strftime_l", (uintptr_t)&strftime_lw },
  { "strspn", (uintptr_t)&strspn },
  { "strtold_l", (uintptr_t)&strtold_lw },
  { "strtoll_l", (uintptr_t)&strtoll_lw },
  { "strtoull_l", (uintptr_t)&strtoull_lw },
  { "strxfrm_l", (uintptr_t)&strxfrm_lw },
  { "swprintf", (uintptr_t)&swprintf },
  { "symlink", (uintptr_t)&imp_retm1 },
  { "syslog", (uintptr_t)&imp_retvoid },
  { "tolower_l", (uintptr_t)&tolower_lw },
  { "toupper_l", (uintptr_t)&toupper_lw },
  { "towlower_l", (uintptr_t)&towlower_lw },
  { "towupper_l", (uintptr_t)&towupper_lw },
  { "truncate", (uintptr_t)&imp_retm1 },
  { "uselocale", (uintptr_t)&uselocale_w },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "waitpid", (uintptr_t)&imp_retm1 },
  { "wcscoll_l", (uintptr_t)&wcscoll_lw },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_lw },
  { "posix_memalign", (uintptr_t)&posix_memalign_w },
  /* ---- PVZ2 v13.3.1 supplementary imports ---- */
  { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION },
  { "__FD_ISSET_chk", (uintptr_t)&FD_ISSET_chk_w },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint_stub },
  { "__android_log_write", (uintptr_t)&__android_log_write_stub },
  { "__assert2", (uintptr_t)&__assert2_stub },
  { "__cmsg_nxthdr", (uintptr_t)&imp_retnull },
  { "__fwrite_chk", (uintptr_t)&fwrite_fake },
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memset_chk", (uintptr_t)&memset },
  { "__open_2", (uintptr_t)&open_fake },
  { "__read_chk", (uintptr_t)&read_chk_w },
  { "__readlink_chk", (uintptr_t)&readlink_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlcpy_chk", (uintptr_t)&strlcpy_chk_w },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__write_chk", (uintptr_t)&write_chk_w },
  { "_exit", (uintptr_t)&exit },
  { "adler32", (uintptr_t)&adler32 },
  { "atanf", (uintptr_t)&atanf },
  { "crc32", (uintptr_t)&crc32 },
  { "ctime", (uintptr_t)&ctime },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "dirname", (uintptr_t)&dirname_w },
  { "dladdr", (uintptr_t)&dladdr_fake },
  { "execv", (uintptr_t)&imp_retm1 },
  { "exp2f", (uintptr_t)&exp2f },
  { "expf", (uintptr_t)&expf },
  { "fchmod", (uintptr_t)&imp_ret0 },
  { "fchmodat", (uintptr_t)&imp_ret0 },
  { "fchown", (uintptr_t)&imp_ret0 },
  { "fdopendir", (uintptr_t)&imp_retnull },
  { "fgetc", (uintptr_t)&fgetc_fake },
  { "fscanf", (uintptr_t)&fscanf_fake },
  { "fsetpos", (uintptr_t)&fsetpos },
  { "fwide", (uintptr_t)&fwide },
  { "gai_strerror", (uintptr_t)&imp_retnull },
  { "getgrgid", (uintptr_t)&imp_retnull },
  { "getgrnam", (uintptr_t)&imp_retnull },
  { "getnameinfo", (uintptr_t)&imp_retm1 },
  { "getppid", (uintptr_t)&imp_ret0 },
  { "getpwnam", (uintptr_t)&imp_retnull },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glDepthRangef", (uintptr_t)&glDepthRangef },
  { "glIsProgram", (uintptr_t)&glIsProgram },
  { "glIsShader", (uintptr_t)&glIsShader },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "gzclose", (uintptr_t)&gzclose },
  { "gzopen", (uintptr_t)&gzopen },
  { "gzread", (uintptr_t)&gzread },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "iswalnum", (uintptr_t)&iswalnum },
  { "lchown", (uintptr_t)&imp_ret0 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "lseek64", (uintptr_t)&lseek_fake },
  { "madvise", (uintptr_t)&imp_ret0 },
  { "memalign", (uintptr_t)&memalign },
  { "mkfifo", (uintptr_t)&imp_retm1 },
  { "mknod", (uintptr_t)&imp_retm1 },
  { "mlock", (uintptr_t)&mlock_fake },
  { "mprotect", (uintptr_t)&imp_ret0 },
  { "mremap", (uintptr_t)&imp_retnull },
  { "openat", (uintptr_t)&openat_fake },
  { "powf", (uintptr_t)&powf },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_noop_stub },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_stub },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setint_stub },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setint_stub },
  { "pthread_condattr_destroy", (uintptr_t)&pthread_attr_noop_stub },
  { "pthread_condattr_init", (uintptr_t)&pthread_attr_init_stub },
  { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_mutexattr_setpshared", (uintptr_t)&pthread_attr_setint_stub },
  { "ptrace", (uintptr_t)&imp_retm1 },
  { "putchar", (uintptr_t)&putchar },
  { "raise", (uintptr_t)&raise },
  { "realpath", (uintptr_t)&realpath_fake },
  { "recvmmsg", (uintptr_t)&imp_retm1 },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sendfile", (uintptr_t)&imp_retm1 },
  { "sendmmsg", (uintptr_t)&imp_retm1 },
  { "setenv", (uintptr_t)&setenv },
  { "sleep", (uintptr_t)&sleep },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "stderr", (uintptr_t)&stderr_compat },
  { "stdin", (uintptr_t)&stdin_compat },
  { "stdout", (uintptr_t)&stdout_compat },
  { "strsep", (uintptr_t)&strsep },
  { "strtok", (uintptr_t)&strtok },
  { "swscanf", (uintptr_t)&swscanf },
  { "system", (uintptr_t)&system_fake },
  { "timegm", (uintptr_t)&mktime },
  { "timezone", (uintptr_t)&timezone_compat },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "usleep", (uintptr_t)&usleep },
  { "utimensat", (uintptr_t)&imp_ret0 },
  { "utimes", (uintptr_t)&imp_ret0 },
  { "vswprintf", (uintptr_t)&vswprintf },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcsncpy", (uintptr_t)&wcsncpy },
  { "zlibVersion", (uintptr_t)&zlibVersion },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  /* no runtime hook swaps needed for the GLES2 path */
}
