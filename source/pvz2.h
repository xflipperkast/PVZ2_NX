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
/* Android Context.sendBroadcast() is asynchronous. Deliver at most one
 * stock Utility.sendBroadcastSerializable event on a later Switch frame. */
void pvz2_nimble_broadcast_pump(void);
/* Mirror a real Switch connectivity transition into Nimble's registered
 * networkStatusChange receiver. The stock callback re-queries INetwork, so
 * no status field or synthetic payload data is supplied. */
void pvz2_nimble_network_status_changed(int status);
/* Read-only gate for replaying real Switch lifecycle edges only after the
 * stock Base.setupNimble component setup has completed. */
int pvz2_nimble_setup_ready(void);
/* Track the real platform foreground edge for DEX ApplicationEnvironment
 * isMainApplicationActive() and delayed Synergy startup notification logic. */
void pvz2_nimble_set_main_application_active(int active);

/*  read-only hardware probe. JNI wrappers pass the native return
 * address for selected anonymous-auth environment/conductor calls so main.c
 * can resolve it against the relocated libPVZ2 image and dump the exact
 * surrounding stock AArch64. No callback or component method is invoked. */

/* Run the native lifecycle setup for one Java-side Nimble C++ component
 * wrapper. The Android Base setup normally drives this registrar. */
void nimble_setup_cpp_component(jobject component);
void nimble_restore_cpp_component(jobject component);

/*  exact stock BaseNativeCallback.nativeCallback export recovered from
 * libPVZ2. The Java-shell layer uses it only for the real Nimble startup
 * notification after Base.setupNimble has completed. */
void pvz2_set_basenativecallback_native(void *fn);

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

/*  summarize the exact stock Persistence calls observed during
 * Base.setupNimble and allow the active Nexus restore only when that bridge
 * completed without unsupported types, capacity failures, or commit errors. */
int pvz2_nimble_persistence_preflight_ok(void);
void pvz2_nimble_persistence_log_summary(void);

#endif
