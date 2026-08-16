/* ============================================================================
 * nx_crash_handler.c  --  user-exception crash dumper for PVZ2
 *                         Switch port (so-loader).
 *
 * Adapted from the generic drop-in dumper. Two things are wired up here:
 *
 *   1) MODULE RESOLVER (strong crash_resolve_module) walks so_loaded_list(),
 *      so a faulting PC/LR/return address inside the dynamically-loaded
 *      libnative.so prints as  libnative.so+0xoffset  -- feed that straight
 *      into your disassembler. creport can't name it because the .so isn't a
 *      real OS module.
 *
 *   2) CRASH-SAFE SINK. The port's normal debugPrintf() logs through stdio
 *      (FILE*, buffered, locked). Calling that from inside a CPU fault can
 *      deadlock: if the faulting thread already held newlib's stdio lock
 *      (the engine logs from inside nativeTick), re-entering it hangs the
 *      handler. So by default we DO NOT use debugPrintf here. Instead we
 *      append to a separate crash log in the PVZ2 SD-card directory with a
 *      raw fd (open/write/close -- no stdio lock, no heap) opened inside the
 *      handler, and mirror every line to svcOutputDebugString for a debugger.
 *      Residual caveat: the fs devoptab still takes its own mutex; if the
 *      fault happened mid-SD-write on this same thread the file line may be
 *      lost -- that's why CRASH_REBREAK re-raises so Atmosphere's creport also
 *      captures the fault independently.
 *
 * Build: just drop in source/ (auto-globbed by the Makefile). No init call.
 * Providing __libnx_exception_handler + __nx_exception_stack* is what enables
 * user-mode exception handling in libnx.
 *
 * Optional -D overrides (Makefile):
 *   CRASH_LOG_PRINTF=fn     force a custom logger instead of the fd sink
 *                           (e.g. =debugPrintf -- accepts the deadlock risk)
 *   CRASH_SINK_FILE="path"  crash-log path (default PVZ2 crash-log path)
 *   CRASH_STACK_BYTES=0x200 stack bytes to hex-dump around SP
 *   CRASH_BT_DEPTH=16       max backtrace frames
 *   CRASH_REBREAK=1         re-raise via svcBreak after dumping (default 1)
 * ==========================================================================*/

#include <switch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "so_util.h"   /* so_loaded_list(), so_module */
#include "config.h"

#ifndef CRASH_STACK_BYTES
#define CRASH_STACK_BYTES 0x200
#endif
#ifndef CRASH_BT_DEPTH
#define CRASH_BT_DEPTH 16
#endif
#ifndef CRASH_REBREAK
#define CRASH_REBREAK 1
#endif
#ifndef CRASH_SINK_FILE
#define CRASH_SINK_FILE "sdmc:/switch/pvz2_nx/pvz2_crash.log"
#endif
#define CRASH_FILE_CAPACITY (64 * 1024)

#if !DEBUG_LOG

/* Release build: no crash logger/file sink.  Re-raise the exception so the
 * platform crash reporter owns diagnostics without keeping any logging state
 * or filesystem writes in the title. */
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void crash_log_open(void) {}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    (void)ctx;
#if CRASH_REBREAK
    svcBreak(BreakReason_Panic, 0, 0);
#endif
    for (;;) svcSleepThread(1000000000ULL);
}

#else

/* ---- output shim ---------------------------------------------------------- */
#if DEBUG_LOG
#ifdef CRASH_LOG_PRINTF
extern int CRASH_LOG_PRINTF(const char *fmt, ...);
#define CLOG(...) CRASH_LOG_PRINTF(__VA_ARGS__)
#else
static FsFile g_crash_file;
static int    g_crash_ok  = 0;
static s64    g_crash_off = 0;

void crash_log_open(void) {
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return;
    fsFsCreateFile(fs, "/switch/pvz2_nx/pvz2_crash.log", 0, 0);
    if (R_SUCCEEDED(fsFsOpenFile(fs, "/switch/pvz2_nx/pvz2_crash.log",
                                 FsOpenMode_Write, &g_crash_file))) {
        fsFileSetSize(&g_crash_file, 0);
        g_crash_ok = 1; g_crash_off = 0;
    }
}

static void crash_emit(const char *buf, int len) {
    if (len <= 0) return;
    svcOutputDebugString(buf, (size_t)len);
    if (g_crash_ok && g_crash_off + len <= CRASH_FILE_CAPACITY) {
        if (R_SUCCEEDED(fsFileWrite(&g_crash_file, g_crash_off, buf, (u64)len, FsWriteOption_Flush)))
            g_crash_off += len;
    }
}

static void crash_out(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    crash_emit(buf, n);
}
#define CLOG(...) crash_out(__VA_ARGS__)
#endif
#else
void crash_log_open(void) {}
static void crash_emit(const char *buf, int len) { (void)buf; (void)len; }
#define CLOG(...) ((void)0)
#endif

/* ---- libnx user-exception plumbing --------------------------------------- */
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* ---- module resolver: name addresses inside our loaded .so images -------- *
 * Strong override of the weak default. Walks the so-loader's module list. */
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap,
                         uintptr_t *base_out) {
    for (so_module *m = so_loaded_list(); m; m = m->next) {
        uintptr_t b = (uintptr_t)m->load_virtbase;
        if (b && addr >= b && addr < b + m->load_size) {
            snprintf(name_out, name_cap, "%s", m->name);
            *base_out = b;
            return 1;
        }
    }
    return 0;
}

/* ---- guarded memory probing ---------------------------------------------- */
static int mem_readable(uintptr_t addr, size_t len) {
    if (!addr || addr < 0x1000) return 0;
    uintptr_t a = addr, end = addr + len;
    if (end < a) return 0;
    while (a < end) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
        if (mi.type == MemType_Unmapped) return 0;
        if ((mi.perm & Perm_R) == 0) return 0;
        uintptr_t block_end = (uintptr_t)mi.addr + mi.size;
        if (block_end <= a) return 0;
        a = block_end;
    }
    return 1;
}

#ifndef CRASH_LOG_PRINTF
static void crash_raw_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    crash_emit(s, n);
}

static void crash_raw_hex(uintptr_t v) {
    static const char hexd[] = "0123456789abcdef";
    char out[16];
    for (int i = 0; i < 16; ++i)
        out[i] = hexd[(v >> ((15 - i) * 4)) & 0xf];
    crash_emit(out, 16);
}

static void crash_raw_off(unsigned off) {
    static const char hexd[] = "0123456789abcdef";
    char out[2] = {hexd[(off >> 4) & 0xf], hexd[off & 0xf]};
    crash_emit(out, 2);
}

static void crash_dump_object_raw(const char *label, uintptr_t addr) {
    crash_raw_str("[crash] rawobj ");
    crash_raw_str(label);
    crash_raw_str(" @ ");
    crash_raw_hex(addr);
    crash_raw_str("\n");
    if (!addr || !mem_readable(addr, 0x80)) {
        crash_raw_str("[crash] rawobj unreadable\n");
        return;
    }
    const uintptr_t *q = (const uintptr_t *)addr;
    for (unsigned i = 0; i < 16; ++i) {
        crash_raw_str("[crash] rawobj ");
        crash_raw_str(label);
        crash_raw_str(" +");
        crash_raw_off(i * 8);
        crash_raw_str("=");
        crash_raw_hex(q[i]);
        crash_raw_str("\n");
    }
    const uintptr_t first = q[0];
    if (first && first != addr && mem_readable(first, 0x60)) {
        crash_raw_str("[crash] rawvt ");
        crash_raw_str(label);
        crash_raw_str(" @ ");
        crash_raw_hex(first);
        crash_raw_str("\n");
        const uintptr_t *v = (const uintptr_t *)first;
        for (unsigned i = 0; i < 12; ++i) {
            crash_raw_str("[crash] rawvt ");
            crash_raw_str(label);
            crash_raw_str(" +");
            crash_raw_off(i * 8);
            crash_raw_str("=");
            crash_raw_hex(v[i]);
            crash_raw_str("\n");
        }
    }
}
#endif

static const char *sym(uintptr_t v, char *buf, size_t cap) {
    char mod[64]; uintptr_t base = 0;
    if (crash_resolve_module(v, mod, sizeof mod, &base)) {
        snprintf(buf, cap, "%s+0x%lx", mod, (unsigned long)(v - base));
        return buf;
    }
    snprintf(buf, cap, "%016lx", (unsigned long)v);
    return buf;
}

/* ---- the handler ---------------------------------------------------------- */
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    char b1[96], b2[96];

    /* If we fault while handling (e.g. a wild pointer inside our own dump on a
     * worker thread), don't recurse -- hand straight to creport. */
    static volatile int in_handler = 0;
    if (in_handler) { svcBreak(BreakReason_Panic, 0, 0); for (;;) svcSleepThread(1000000000ULL); }
    in_handler = 1;

#ifndef CRASH_LOG_PRINTF
    if (g_crash_ok && R_FAILED(fsFileSetSize(&g_crash_file, CRASH_FILE_CAPACITY)))
        g_crash_ok = 0;
#endif

    /* Emit the essentials with ZERO newlib calls first (svcOutputDebugString +
     * raw write), so even if the richer dump below faults on a worker thread we
     * still capture pc/lr/far. Manual hex, no vsnprintf. */
    {
        static const char hexd[] = "0123456789abcdef";
        char line[32];
        #define EMIT_RAW(p,n) crash_emit((p),(n))
        #define EMIT_STR(s) do { const char *_s=(s); int _n=0; while(_s[_n])_n++; EMIT_RAW(_s,_n); } while (0)
        #define EMIT_HEX(v) do { unsigned long _v=(unsigned long)(v); \
            for (int _i=0; _i<16; _i++) { \
                line[_i]=hexd[(_v>>((15-_i)*4))&0xf]; \
            } \
            line[16]='\n'; \
            EMIT_RAW(line,17); \
        } while (0)
        EMIT_STR("\n[crash] FAULT pc="); EMIT_HEX(ctx->pc.x);
        EMIT_STR("[crash] lr=");        EMIT_HEX(ctx->lr.x);
        EMIT_STR("[crash] far=");       EMIT_HEX(ctx->far.x);
        EMIT_STR("[crash] esr=");       EMIT_HEX(ctx->esr);
        EMIT_STR("[crash] sp=");        EMIT_HEX(ctx->sp.x);
        /* module bases so raw pc/lr can be symbolized even if the rich dump
         * below faults (m->name is a fixed char[64], safe to read). */
        for (so_module *m = so_loaded_list(); m; m = m->next) {
            EMIT_STR("[crash] mod ");        EMIT_STR(m->name);
            EMIT_STR(" base=");              EMIT_HEX((unsigned long)m->load_virtbase);
        }
        /* first few GPRs (often hold the bad pointer / string) */
        for (int _i = 0; _i < 8; _i++) {
            EMIT_STR("[crash] x");
            { char d[3]; int k=0; if(_i>=10){d[k++]='0'+_i/10;} d[k++]='0'+_i%10; d[k++]='='; EMIT_RAW(d,k); }
            EMIT_HEX(ctx->cpu_gprs[_i].x);
        }
#ifndef CRASH_LOG_PRINTF
        /* Dump the live objects from the same GPR roles seen in the
         * post-callback abort using only svcOutputDebugString +
         * fsFileWrite.  This happens before any vsnprintf/newlib formatting. */
        crash_dump_object_raw("x0",  (uintptr_t)ctx->cpu_gprs[0].x);
        crash_dump_object_raw("x6",  (uintptr_t)ctx->cpu_gprs[6].x);
        crash_dump_object_raw("x10", (uintptr_t)ctx->cpu_gprs[10].x);
        crash_dump_object_raw("x19", (uintptr_t)ctx->cpu_gprs[19].x);
        crash_dump_object_raw("x20", (uintptr_t)ctx->cpu_gprs[20].x);
        crash_dump_object_raw("x21", (uintptr_t)ctx->cpu_gprs[21].x);
        crash_dump_object_raw("x24", (uintptr_t)ctx->cpu_gprs[24].x);
        crash_dump_object_raw("x25", (uintptr_t)ctx->cpu_gprs[25].x);
#endif
    }

    CLOG("\n[crash] ================ USER EXCEPTION ================\n");
    CLOG("[crash] type=0x%x  esr=%08x  far=%016lx\n",
         ctx->error_desc, ctx->esr, (unsigned long)ctx->far.x);
    CLOG("[crash] pc=%s\n", sym(ctx->pc.x, b1, sizeof b1));
    CLOG("[crash] lr=%s\n", sym(ctx->lr.x, b2, sizeof b2));
    CLOG("[crash] sp=%016lx  fp=%016lx\n",
         (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);

    for (int i = 0; i < 28; i += 4)
        CLOG("[crash] x%-2d %016lx  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx\n",
             i,   (unsigned long)ctx->cpu_gprs[i].x,
             i+1, (unsigned long)ctx->cpu_gprs[i+1].x,
             i+2, (unsigned long)ctx->cpu_gprs[i+2].x,
             i+3, (unsigned long)ctx->cpu_gprs[i+3].x);
    CLOG("[crash] x28 %016lx\n", (unsigned long)ctx->cpu_gprs[28].x);

    CLOG("[crash] backtrace:\n");
    uintptr_t fp = (uintptr_t)ctx->fp.x;
    for (int d = 0; d < CRASH_BT_DEPTH && fp; d++) {
        if (!mem_readable(fp, 16)) break;
        uintptr_t next = ((uintptr_t *)fp)[0];
        uintptr_t ret  = ((uintptr_t *)fp)[1];
        if (!ret) break;
        CLOG("[crash]   #%02d %s\n", d, sym(ret, b1, sizeof b1));
        if (next <= fp) break;
        fp = next;
    }

    uintptr_t sp = (uintptr_t)ctx->sp.x;
    if (mem_readable(sp, CRASH_STACK_BYTES)) {
        CLOG("[crash] stack @ %016lx:\n", (unsigned long)sp);
        for (size_t off = 0; off < CRASH_STACK_BYTES; off += 0x20) {
            const u64 *q = (const u64 *)(sp + off);
            CLOG("[crash]   +%03zx: %016lx %016lx %016lx %016lx\n",
                 off, (unsigned long)q[0], (unsigned long)q[1],
                 (unsigned long)q[2], (unsigned long)q[3]);
        }
    } else {
        CLOG("[crash] stack @ %016lx: UNREADABLE\n", (unsigned long)sp);
    }

    CLOG("[crash] ============== END EXCEPTION DUMP ==============\n");

#ifndef CRASH_LOG_PRINTF
    if (g_crash_ok) {
        fsFileSetSize(&g_crash_file, g_crash_off);
        fsFileFlush(&g_crash_file);
    }
#endif

#if CRASH_REBREAK
    svcBreak(BreakReason_Panic, 0, 0);
#endif
    for (;;) svcSleepThread(1000000000ULL);
}

#endif /* DEBUG_LOG */
