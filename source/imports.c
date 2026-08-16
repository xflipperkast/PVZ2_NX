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
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
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
#include "pvz2.h"

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

/* Remaining unbridged raw-network imports stay mapped individually to the
 * fail-closed generic stubs below. The compatibility layer restores only the POSIX socket/DNS
 * surface required by the stock embedded Nimble curl. */

/* bionic FIONBIO has the Linux request number 0x5421. Stock OpenSSL uses this
 * through BIO_socket_nbio(); translate only that proven request and keep all
 * unknown Android ioctls fail-closed. */
static int ioctl_bionic_fake(int fd, unsigned long request, void *argument) {
  enum { BIONIC_FIONBIO = 0x5421u };
  if (request != BIONIC_FIONBIO || !argument) {
    errno = BIONIC_ENOSYS;
    return -1;
  }

  const int enabled = *(const int *)argument != 0;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (enabled) flags |= O_NONBLOCK;
  else flags &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}

static const char *gai_strerror_bionic_fake(int error) {
  switch (error) {
    case 0: return "Success";
    case 1: return "Address family for hostname not supported";
    case 2: return "Temporary failure in name resolution";
    case 3: return "Invalid value for ai_flags";
    case 4: return "Non-recoverable failure in name resolution";
    case 5: return "ai_family not supported";
    case 6: return "Memory allocation failure";
    case 7: return "No address associated with hostname";
    case 8: return "hostname nor servname provided, or not known";
    case 9: return "Servname not supported for ai_socktype";
    case 10: return "ai_socktype not supported";
    case 11: return "System error returned in errno";
    case 12: return "Invalid value for hints";
    case 13: return "Resolved protocol is unknown";
    case 14: return "Argument buffer overflow";
    default: return "Unknown error";
  }
}

/* The compatibility layer transport shims are implemented in pvz2.c, which already owns the
 * Switch socket/DNS compatibility layer. Keep only external declarations here
 * so this bionic import table does not pull host socket ABI headers into the
 * import-compat translation unit. */
extern int bionic_bind_fake();
extern int bionic_connect_fake();
extern int bionic_gethostname_fake();
extern int bionic_getpeername_fake();
extern int bionic_getsockname_fake();
extern int bionic_getsockopt_fake();
extern int bionic_pipe_fake();
extern int bionic_poll_fake();
extern long bionic_recv_fake();
extern long bionic_recvfrom_fake();
extern long bionic_send_fake();
extern int bionic_setsockopt_fake();
extern int bionic_socket_fake();
extern int bionic_accept_fake();
extern int bionic_getaddrinfo_fake();
extern void bionic_freeaddrinfo_fake();
extern int bionic_listen_fake();
extern int bionic_select_fake();
extern long bionic_sendto_fake();
extern int bionic_shutdown_fake();
extern int bionic_socketpair_fake();
extern unsigned long bionic_inet_addr_fake();
extern const char *bionic_inet_ntop_fake();
extern int bionic_inet_pton_fake();

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
  char buf[1024];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (!android_log_enabled(prio)) return 0;
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
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  if (!android_log_enabled(prio)) return 0;
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
  if (!rw) return EINVAL;
  /* POSIX makes destroying a locked/in-use rwlock undefined. Atomically detach
   * the lazy object so a concurrent first user can never observe freed memory. */
  void *lock = __atomic_exchange_n(rw, NULL, __ATOMIC_ACQ_REL);
  free(lock);
  return 0;
}

int pthread_condattr_setclock_fake(void *attr, int clock_id) { (void)attr; (void)clock_id; return 0; }

int pthread_detach_fake(pthread_t t) {
  /*  preserve the proven compatibility no-op. The r2 hardware
   * crash showed PVZ2 workers still sleeping while the pvz2_nx mapping had
   * disappeared; do not change libnx thread ownership/reaping semantics. */
  (void)t;
  return 0;
}
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
  static unsigned trace_count;
  if (length > 256) { errno = EIO; return -1; }
  const ssize_t got = getrandom_fake(buffer, length, 0);
  if (trace_count < 16) {
    ++trace_count;
    (void)0;
  }
  return got == (ssize_t)length ? 0 : -1;
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

/* Android/bionic and Switch newlib do not expose the same struct tm ABI.
 * The compatibility layer proved it when stock Nimble read bionic tm_zone at +0x30 from a
 * newlib localtime() result and faulted. The full audit applies that same ABI
 * bridge consistently to localtime[_r], gmtime[_r], mktime, timegm and
 * strftime[_l]. Compiler TLS is intentionally unavailable because TPIDR_EL0
 * belongs to the emulated bionic TCB, so non-reentrant results use a small
 * lock-free ring. */
typedef struct BionicTmCompat {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
  long gmtoff;
  const char *zone;
} BionicTmCompat;

_Static_assert(offsetof(BionicTmCompat, gmtoff) == 0x28,
               "bionic tm gmtoff offset");
_Static_assert(offsetof(BionicTmCompat, zone) == 0x30,
               "bionic tm zone offset");
_Static_assert(sizeof(BionicTmCompat) == 0x38,
               "bionic tm LP64 size");

#define BIONIC_TM_SLOTS 16u
static BionicTmCompat s_bionic_local_tm[BIONIC_TM_SLOTS];
static BionicTmCompat s_bionic_utc_tm[BIONIC_TM_SLOTS];
static char s_bionic_local_zone[BIONIC_TM_SLOTS][9];
static unsigned s_bionic_local_next;
static unsigned s_bionic_utc_next;

static void native_tm_to_bionic(BionicTmCompat *out, const struct tm *native,
                                long offset, const char *zone) {
  if (!out || !native) return;
  out->tm_sec = native->tm_sec;
  out->tm_min = native->tm_min;
  out->tm_hour = native->tm_hour;
  out->tm_mday = native->tm_mday;
  out->tm_mon = native->tm_mon;
  out->tm_year = native->tm_year;
  out->tm_wday = native->tm_wday;
  out->tm_yday = native->tm_yday;
  out->tm_isdst = native->tm_isdst;
  out->gmtoff = offset;
  out->zone = zone;
}

static void bionic_tm_to_native(struct tm *out, const BionicTmCompat *input) {
  memset(out, 0, sizeof(*out));
  if (!input) return;
  out->tm_sec = input->tm_sec;
  out->tm_min = input->tm_min;
  out->tm_hour = input->tm_hour;
  out->tm_mday = input->tm_mday;
  out->tm_mon = input->tm_mon;
  out->tm_year = input->tm_year;
  out->tm_wday = input->tm_wday;
  out->tm_yday = input->tm_yday;
  out->tm_isdst = input->tm_isdst;
}

static int64_t floor_div_i64(int64_t value, int64_t divisor) {
  int64_t quotient = value / divisor;
  const int64_t remainder = value % divisor;
  if (remainder < 0) --quotient;
  return quotient;
}

/* Howard Hinnant's proleptic-Gregorian civil-date transform, expressed with
 * signed 64-bit intermediates. The result is days relative to 1970-01-01. */
static int64_t days_from_civil_i64(int64_t year, unsigned month,
                                   unsigned day) {
  year -= month <= 2u;
  const int64_t era = floor_div_i64(year, 400);
  const unsigned year_of_era = (unsigned)(year - era * 400);
  const unsigned shifted_month = month > 2u ? month - 3u : month + 9u;
  const unsigned day_of_year =
      (153u * shifted_month + 2u) / 5u + day - 1u;
  const unsigned day_of_era = year_of_era * 365u + year_of_era / 4u -
      year_of_era / 100u + day_of_year;
  return era * 146097 + (int64_t)day_of_era - 719468;
}

static BionicTmCompat *gmtime_r_bionic_fake(const time_t *timer,
                                             BionicTmCompat *out) {
  if (!timer || !out) return NULL;
  struct tm native;
  if (!gmtime_r(timer, &native)) return NULL;
  native_tm_to_bionic(out, &native, 0, "UTC");
  out->tm_isdst = 0;
  return out;
}

static BionicTmCompat *gmtime_bionic_fake(const time_t *timer) {
  if (!timer) return NULL;
  const unsigned slot = __atomic_fetch_add(&s_bionic_utc_next, 1u,
                                            __ATOMIC_RELAXED) % BIONIC_TM_SLOTS;
  return gmtime_r_bionic_fake(timer, &s_bionic_utc_tm[slot]);
}

/* Android/bionic timegm() interprets the standard tm fields as UTC. Mapping
 * it to newlib mktime() made the result depend on the console's local time
 * zone and DST rule. Keep the bionic layout and normalize the public fields
 * exactly as a successful time conversion does. */
static time_t timegm_bionic_fake(BionicTmCompat *input) {
  if (!input) {
    errno = EINVAL;
    return (time_t)-1;
  }

  int64_t year = (int64_t)input->tm_year + 1900;
  int64_t month = input->tm_mon;
  const int64_t month_era = floor_div_i64(month, 12);
  year += month_era;
  month -= month_era * 12;

  const int64_t days = days_from_civil_i64(
      year, (unsigned)month + 1u, 1u) + (int64_t)input->tm_mday - 1;
  const __int128 total = (__int128)days * 86400 +
      (__int128)input->tm_hour * 3600 +
      (__int128)input->tm_min * 60 + input->tm_sec;
  const time_t result = (time_t)total;
  if ((__int128)result != total) {
    errno = EOVERFLOW;
    return (time_t)-1;
  }

  (void)gmtime_r_bionic_fake(&result, input);
  return result;
}

static BionicTmCompat *localtime_r_bionic_slot(const time_t *timer,
                                                  BionicTmCompat *out,
                                                  char *zone) {
  if (!timer || !out || !zone) return NULL;
  memset(out, 0, sizeof(*out));
  zone[0] = '\0';

  TimeCalendarTime cal;
  TimeCalendarAdditionalInfo info;
  memset(&cal, 0, sizeof(cal));
  memset(&info, 0, sizeof(info));
  Result rc = timeToCalendarTimeWithMyRule((u64)*timer, &cal, &info);
  if (R_SUCCEEDED(rc)) {
    out->tm_sec = cal.second;
    out->tm_min = cal.minute;
    out->tm_hour = cal.hour;
    out->tm_mday = cal.day;
    out->tm_mon = (int)cal.month - 1;
    out->tm_year = (int)cal.year - 1900;
    out->tm_wday = (int)info.wday;
    out->tm_yday = (int)info.yday;
    out->tm_isdst = info.DST ? 1 : 0;
    out->gmtoff = (long)info.offset;
    memcpy(zone, info.timezoneName, sizeof(info.timezoneName));
    zone[sizeof(info.timezoneName)] = '\0';
  } else {
    /* libnx initializes newlib's TZ from the same Horizon rule at startup.
     * If direct time-service conversion ever fails, use only the standard
     * newlib tm fields and render the zone into our own bionic-safe storage. */
    struct tm native_tm;
    if (!localtime_r(timer, &native_tm)) return NULL;
    out->tm_sec = native_tm.tm_sec;
    out->tm_min = native_tm.tm_min;
    out->tm_hour = native_tm.tm_hour;
    out->tm_mday = native_tm.tm_mday;
    out->tm_mon = native_tm.tm_mon;
    out->tm_year = native_tm.tm_year;
    out->tm_wday = native_tm.tm_wday;
    out->tm_yday = native_tm.tm_yday;
    out->tm_isdst = native_tm.tm_isdst;
    (void)strftime(zone, 9, "%Z", &native_tm);
    out->gmtoff = 0;
  }
  out->zone = zone;

  (void)rc;
  return out;
}

static BionicTmCompat *localtime_bionic_fake(const time_t *timer) {
  if (!timer) return NULL;
  const unsigned slot = __atomic_fetch_add(&s_bionic_local_next, 1u,
                                            __ATOMIC_RELAXED) % BIONIC_TM_SLOTS;
  return localtime_r_bionic_slot(timer, &s_bionic_local_tm[slot],
                                 s_bionic_local_zone[slot]);
}

static BionicTmCompat *localtime_r_bionic_fake(const time_t *timer,
                                               BionicTmCompat *out) {
  if (!timer || !out) return NULL;
  const unsigned slot = __atomic_fetch_add(&s_bionic_local_next, 1u,
                                            __ATOMIC_RELAXED) % BIONIC_TM_SLOTS;
  return localtime_r_bionic_slot(timer, out, s_bionic_local_zone[slot]);
}

static time_t mktime_bionic_fake(BionicTmCompat *input) {
  if (!input) { errno = EINVAL; return (time_t)-1; }
  struct tm native;
  bionic_tm_to_native(&native, input);
  const time_t result = mktime(&native);
  if (result == (time_t)-1) return result;
  const unsigned slot = __atomic_fetch_add(&s_bionic_local_next, 1u,
                                            __ATOMIC_RELAXED) % BIONIC_TM_SLOTS;
  BionicTmCompat normalized;
  if (localtime_r_bionic_slot(&result, &normalized,
                              s_bionic_local_zone[slot]))
    *input = normalized;
  return result;
}

static size_t strftime_bionic_fake(char *destination, size_t capacity,
                                   const char *format,
                                   const BionicTmCompat *input) {
  if (!destination || !capacity || !format || !input) return 0;
  struct tm native;
  bionic_tm_to_native(&native, input);

  /* Reconstruct a native tm through the matching timezone path so %Z/%z do
   * not depend on newlib exposing bionic's tm_gmtoff/tm_zone fields. */
  BionicTmCompat scratch = *input;
  time_t epoch;
  if (input->zone && !strcmp(input->zone, "UTC")) {
    epoch = timegm_bionic_fake(&scratch);
    if (epoch != (time_t)-1) (void)gmtime_r(&epoch, &native);
  } else {
    epoch = mktime(&native);
    if (epoch != (time_t)-1) (void)localtime_r(&epoch, &native);
  }
  return strftime(destination, capacity, format, &native);
}

static size_t strftime_l_bionic_fake(char *destination, size_t capacity,
                                     const char *format,
                                     const BionicTmCompat *input, void *locale) {
  (void)locale;
  return strftime_bionic_fake(destination, capacity, format, input);
}

/* Android bionic's LP64 basename/dirname take const char* and explicitly do
 * NOT modify the input.  The old shim used glibc-style in-place truncation.
 * Stock PVZ2's TAR writer calls dirname(target) and immediately reuses target
 * to remove/open the file, so mutating target destroys the extraction path.
 *
 * Bionic uses per-thread buffers.  Compiler TLS is unsafe in this port because
 * TPIDR_EL0 is temporarily owned by the emulated bionic TLS block, so use a
 * small lock-free result ring instead.  Callers consume libgen results
 * immediately; 16 independent 4K slots keep concurrent workers separated. */
#define BIONIC_LIBGEN_SLOTS 16u
#define BIONIC_LIBGEN_BUFSZ 4096u
static char s_basename_buf[BIONIC_LIBGEN_SLOTS][BIONIC_LIBGEN_BUFSZ];
static char s_dirname_buf[BIONIC_LIBGEN_SLOTS][BIONIC_LIBGEN_BUFSZ];
static unsigned s_basename_next;
static unsigned s_dirname_next;

static char *libgen_slot(char slots[BIONIC_LIBGEN_SLOTS][BIONIC_LIBGEN_BUFSZ],
                         unsigned *counter) {
  const unsigned n = __atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
  return slots[n % BIONIC_LIBGEN_SLOTS];
}

static char *basename_w(const char *path) {
  char *out = libgen_slot(s_basename_buf, &s_basename_next);
  const char *start = path;
  const char *end = NULL;
  size_t len = 0;
  if (!path || !*path) { start = "."; len = 1; goto copy; }
  end = path + strlen(path) - 1;
  while (end > path && *end == '/') --end;
  if (end == path && *end == '/') { start = "/"; len = 1; goto copy; }
  start = end;
  while (start > path && start[-1] != '/') --start;
  len = (size_t)(end - start + 1);
copy:
  if (len >= BIONIC_LIBGEN_BUFSZ) { errno = ENAMETOOLONG; return NULL; }
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
}

static char *dirname_w(const char *path) {
  char *out = libgen_slot(s_dirname_buf, &s_dirname_next);
  const char *start = path;
  const char *end = NULL;
  size_t len = 0;
  if (!path || !*path) { start = "."; len = 1; goto copy; }
  end = path + strlen(path) - 1;
  while (end > path && *end == '/') --end;
  while (end > path && *end != '/') --end;
  if (end == path) {
    start = (*end == '/') ? "/" : ".";
    len = 1;
    goto copy;
  }
  do { --end; } while (end > path && *end == '/');
  start = path;
  len = (size_t)(end - path + 1);
copy:
  if (len >= BIONIC_LIBGEN_BUFSZ) { errno = ENAMETOOLONG; return NULL; }
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
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
#if PVZ2_ENABLE_STARTUP_PROFILER
static u64 g_startup_gl_source_ticks;
static u64 g_startup_gl_compile_ticks;
static u64 g_startup_gl_link_ticks;
static unsigned g_startup_gl_source_count;
static unsigned g_startup_gl_compile_count;
static unsigned g_startup_gl_link_count;

static unsigned long long imports_profile_ticks_ms(u64 ticks) {
  const u64 hz = armGetSystemTickFreq();
  return hz ? (unsigned long long)((ticks * 1000ULL) / hz) : 0;
}

#endif

void imports_startup_profile_snapshot(const char *tag) {
#if PVZ2_ENABLE_STARTUP_PROFILER
  if (!pvz2_startup_profile_active()) return;
  debugPrintf("STARTUPPROF +%llums GL-SUMMARY tag=%s source=%u/%llums compile=%u/%llums link=%u/%llums\n",
              pvz2_startup_profile_elapsed_ms(), tag ? tag : "?",
              g_startup_gl_source_count, imports_profile_ticks_ms(g_startup_gl_source_ticks),
              g_startup_gl_compile_count, imports_profile_ticks_ms(g_startup_gl_compile_ticks),
              g_startup_gl_link_count, imports_profile_ticks_ms(g_startup_gl_link_ticks));
#else
  (void)tag;
#endif
}
GLuint glCreateShader_log(GLenum type) {
  GLuint s = glCreateShader(type);
  (void)type;
  return s;
}
void glShaderSource_log(GLuint s, GLsizei c, const GLchar *const *str, const GLint *len) {
  #if PVZ2_ENABLE_STARTUP_PROFILER
  const u64 profile_begin = armGetSystemTick();
  #endif
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

  if (patched)
    debugPrintf("gl: shaderSource(%u) stripped incompatible ES3 #version\n", s);

  if (patched) {
    const char *one = src; GLint onelen = p;
    glShaderSource(s, 1, &one, &onelen);
  } else {
    glShaderSource(s, c, str, len);
  }
#if PVZ2_ENABLE_STARTUP_PROFILER
  if (pvz2_startup_profile_active()) {
    g_startup_gl_source_count++;
    g_startup_gl_source_ticks += armGetSystemTick() - profile_begin;
  }
#endif
}
void glCompileShader_log(GLuint s) {
  #if PVZ2_ENABLE_STARTUP_PROFILER
  const u64 profile_begin = armGetSystemTick();
  #endif
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, 0x8B81 /*GL_COMPILE_STATUS*/, &ok);
  if (!ok) {
    char lg[600]; GLsizei n = 0; glGetShaderInfoLog(s, sizeof lg - 1, &n, lg);
    lg[(n < (GLsizei)sizeof lg) ? n : (GLsizei)sizeof lg - 1] = 0;
    debugPrintf("gl: SHADER ERROR %u: %s\n", s, lg);
  }
#if PVZ2_ENABLE_STARTUP_PROFILER
  if (pvz2_startup_profile_active()) {
    g_startup_gl_compile_count++;
    g_startup_gl_compile_ticks += armGetSystemTick() - profile_begin;
  }
#endif
}
void glLinkProgram_log(GLuint p) {
  #if PVZ2_ENABLE_STARTUP_PROFILER
  const u64 profile_begin = armGetSystemTick();
  #endif
  glLinkProgram(p);
  GLint ok = 0; glGetProgramiv(p, 0x8B82 /*GL_LINK_STATUS*/, &ok);
  if (!ok) {
    char lg[600]; GLsizei n = 0; glGetProgramInfoLog(p, sizeof lg - 1, &n, lg);
    lg[(n < (GLsizei)sizeof lg) ? n : (GLsizei)sizeof lg - 1] = 0;
    debugPrintf("gl: LINK ERROR %u: %s\n", p, lg);
  }
#if PVZ2_ENABLE_STARTUP_PROFILER
  if (pvz2_startup_profile_active()) {
    g_startup_gl_link_count++;
    g_startup_gl_link_ticks += armGetSystemTick() - profile_begin;
  }
#endif
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
  { "bind", (uintptr_t)&bionic_bind_fake },
  { "bsearch", (uintptr_t)&bsearch },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&pvz2_calloc_traced },
  { "chmod", (uintptr_t)&chmod_fake },
  { "clock", (uintptr_t)&clock },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "close", (uintptr_t)&close_fake },
  { "closedir", (uintptr_t)&closedir_fake },
  { "connect", (uintptr_t)&bionic_connect_fake },
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
  { "gethostname", (uintptr_t)&bionic_gethostname_fake },
  { "getpeername", (uintptr_t)&bionic_getpeername_fake },
  { "getpid", (uintptr_t)&getpid },
  { "getpwuid", (uintptr_t)&getpwuid_fake },
  { "getsockname", (uintptr_t)&bionic_getsockname_fake },
  { "getsockopt", (uintptr_t)&bionic_getsockopt_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday_fake },
  { "getwc", (uintptr_t)&getwc },
  { "glActiveTexture", (uintptr_t)&glActiveTexture_fake },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer_fake },
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
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray_fake },
  { "glDrawArrays", (uintptr_t)&glDrawArrays_fake },
  { "glDrawElements", (uintptr_t)&glDrawElements_fake },
  { "glEnable", (uintptr_t)&glEnable_fake },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray_fake },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFlush", (uintptr_t)&glFlush },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures_fake },
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
  { "gmtime", (uintptr_t)&gmtime_bionic_fake },
  { "gmtime_r", (uintptr_t)&gmtime_r_bionic_fake },
  { "inet_addr", (uintptr_t)&bionic_inet_addr_fake },
  { "inet_ntop", (uintptr_t)&bionic_inet_ntop_fake },
  { "inet_pton", (uintptr_t)&bionic_inet_pton_fake },
  { "ioctl", (uintptr_t)&ioctl_bionic_fake },
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
  { "localtime", (uintptr_t)&localtime_bionic_fake },
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
  { "mktime", (uintptr_t)&mktime_bionic_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "munmap", (uintptr_t)&munmap_fake },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "open", (uintptr_t)&open_fake },
  { "opendir", (uintptr_t)&opendir_fake },
  { "pathconf", (uintptr_t)&pathconf },
  { "pipe", (uintptr_t)&bionic_pipe_fake },
  { "poll", (uintptr_t)&bionic_poll_fake },
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
  { "recv", (uintptr_t)&bionic_recv_fake },
  { "recvfrom", (uintptr_t)&bionic_recvfrom_fake },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "rmdir", (uintptr_t)&rmdir },
  { "sched_yield", (uintptr_t)&sched_yield },
  { "send", (uintptr_t)&bionic_send_fake },
  { "setjmp", (uintptr_t)&setjmp },
  { "setlocale", (uintptr_t)&setlocale_fake },
  { "setsockopt", (uintptr_t)&bionic_setsockopt_fake },
  { "setvbuf", (uintptr_t)&setvbuf_fake },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sinh", (uintptr_t)&sinh },
  { "snprintf", (uintptr_t)&snprintf },
  { "socket", (uintptr_t)&bionic_socket_fake },
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
  { "strftime", (uintptr_t)&strftime_bionic_fake },
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
  { "accept", (uintptr_t)&bionic_accept_fake },
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
  { "freeaddrinfo", (uintptr_t)&bionic_freeaddrinfo_fake },
  { "freelocale", (uintptr_t)&imp_retvoid },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "funlockfile", (uintptr_t)&funlockfile },
  { "getaddrinfo", (uintptr_t)&bionic_getaddrinfo_fake },
  { "getc_unlocked", (uintptr_t)&getc_unlocked },
  { "getegid", (uintptr_t)&imp_ret0 },
  { "getentropy", (uintptr_t)&getentropy_stub },
  { "getgid", (uintptr_t)&imp_ret0 },
  { "getrandom", (uintptr_t)&getrandom_fake },
  { "gethostbyname", (uintptr_t)&imp_retnull },
  { "getpwuid_r", (uintptr_t)&imp_retm1 },
  { "getservbyname", (uintptr_t)&imp_retnull },
  { "getuid", (uintptr_t)&imp_ret0 },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glColorMask", (uintptr_t)&glColorMask_fake },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glIsTexture", (uintptr_t)&glIsTexture_fake },
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
  { "listen", (uintptr_t)&bionic_listen_fake },
  { "localeconv", (uintptr_t)&localeconv },
  { "localtime_r", (uintptr_t)&localtime_r_bionic_fake },
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
  { "select", (uintptr_t)&bionic_select_fake },
  { "sendmsg", (uintptr_t)&imp_retm1 },
  { "sendto", (uintptr_t)&bionic_sendto_fake },
  { "shutdown", (uintptr_t)&bionic_shutdown_fake },
  { "signal", (uintptr_t)&signal },
  { "sincos", (uintptr_t)&sincos_w },
  { "sincosf", (uintptr_t)&sincosf_w },
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "socketpair", (uintptr_t)&bionic_socketpair_fake },
  { "srandom", (uintptr_t)&srandom },
  { "strcoll_l", (uintptr_t)&strcoll_lw },
  { "strftime_l", (uintptr_t)&strftime_l_bionic_fake },
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
  { "gai_strerror", (uintptr_t)&gai_strerror_bionic_fake },
  { "getgrgid", (uintptr_t)&imp_retnull },
  { "getgrnam", (uintptr_t)&imp_retnull },
  { "getnameinfo", (uintptr_t)&imp_retm1 },
  { "getppid", (uintptr_t)&imp_ret0 },
  { "getpwnam", (uintptr_t)&imp_retnull },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glDepthRangef", (uintptr_t)&glDepthRangef },
  { "glIsProgram", (uintptr_t)&glIsProgram },
  { "glIsShader", (uintptr_t)&glIsShader_fake },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "gzclose", (uintptr_t)&gzclose_fake },
  { "gzopen", (uintptr_t)&gzopen_fake },
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
  { "timegm", (uintptr_t)&timegm_bionic_fake },
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
