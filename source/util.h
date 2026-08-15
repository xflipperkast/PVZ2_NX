/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(char *text, ...);
/* Advisory buffered checkpoint; does not force physical SD durability. */
void debugLogFlush(void);
/* Crash/watchdog/clean-exit durability point. */
void debugLogSync(void);
void dump_memory_hex(const char *tag, const void *ptr, size_t len);

void cpu_boost(int on);

// The native library was built with -mstack-protector-guard=tls: every guarded
// function reads the canary from tpidr_el0 + 0x28. The Switch leaves tpidr_el0 free
// (libnx keeps the thread TLS in tpidrro_el0), so we point it at a block with
// a guard. Must run on every thread that executes engine code.
void tls_setup_guard(void);
void pin_current_thread(void);

int ret0(void);
int retm1(void);

static inline void* armGetTlsRw(void) {
  void* ret;
  __asm__ ("mrs %x[data], s3_3_c13_c0_2" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#endif

