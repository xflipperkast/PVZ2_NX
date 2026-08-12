#ifndef PVZ2_JAVA_SHELL_H
#define PVZ2_JAVA_SHELL_H

#include <stdarg.h>
#include "jni.h"

jvalue pvz2_upcall(const char *cls, const char *name, const char *sig,
                   jobject self, va_list ap);
/* Array-form JNI calls already arrive decoded as jvalue entries. Route them
 * through the same platform compatibility layer instead of silently returning
 * zero from Call*MethodA. */
jvalue pvz2_upcall_args(const char *cls, const char *name, const char *sig,
                        jobject self, const jvalue *argv);
const char *pvz2_device_uuid(void);
/* Android's connectivity receiver reports 1 for Wi-Fi, 9 for Ethernet,
 * and -1 when there is no connection. */
int pvz2_current_network_status(void);
/* Advance asynchronous curl work once per Switch frame. The Android input
 * callback is not guaranteed to run after modal system UI closes. */
void pvz2_http_pump(void);

/* Run the native lifecycle setup for one Java-side Nimble C++ component
 * wrapper. The Android Base setup normally drives this registrar. */
void nimble_setup_cpp_component(jobject component);

void pvz2_catalog_refresh_requested(void);
int pvz2_take_catalog_refresh_request(void);
int pvz2_catalog_refresh_ready(void);
int pvz2_catalog_refresh_failed(void);
int pvz2_catalog_item_count(void);
/* State 13 is reached only after the title has consumed startup services. One
 * Android Handler timeout remains active on Switch; release that scheduled
 * timeout instead of forcing a loader
 * state. */
void pvz2_release_startup_wait_timer(void);


/* Ensure the persistent Java property store has been loaded before file-shim
 * startup recovery runs. */
void pvz2_java_store_ensure_loaded(void);

#endif
