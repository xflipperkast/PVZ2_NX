/* Release build: frame counter ABI without the diagnostic watchdog/logger. */

#include <stdint.h>

volatile unsigned long long g_frame_count = 0;

void watchdog_set_suspended(int suspended) { (void)suspended; }
void watchdog_register_thread(void) {}
void watchdog_start(void *mod) { (void)mod; }
