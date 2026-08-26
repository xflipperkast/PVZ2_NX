/* jni_fake.h -- fake JNI environment used by the game runtime
 *
 * The Java layer is represented by thin "Wrapper" classes called through JNI.
 * We recreate just enough JNI for the native runtime to (a) run JNI_OnLoad,
 * (b) FindClass / GetMethodID the wrappers, and (c) call them -- routing the
 * calls that matter into the platform compatibility layer and logging the rest.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stddef.h>
#include <stdint.h>
#include "jni.h"

extern JavaVM fake_vm;   // JavaVM* handed to JNI_OnLoad
extern JNIEnv fake_env;  // JNIEnv* handed to every entry point

// Set when the engine (via TextInput) asks for the soft keyboard; main.c
// services it with the Switch software keyboard.
extern volatile int g_kbd_requested;
extern char g_kbd_initial[64];   // current field text, if the engine provided it

// Set if the engine ever asks the activity to finish.
extern volatile int jni_quit_requested;

void jni_init(void);

// the fake activity instance / class handed to the JNI entry points
void *jni_make_thiz(void);

// fake Java object constructors used by main.c when driving the lifecycle
jstring jni_make_string(const char *utf);
const char *jni_cstr(jobject s);   /* C string from a fake jstring, or NULL */
jobjectArray jni_make_string_array(const char *const *values, int count);
jobjectArray jni_make_object_array(const jobject *values, int count);
int jni_object_array_length(jobject array);
jobject jni_object_array_get(jobject array, int index);
jobject jni_make_object(void);
jobject jni_make_object_class(const char *class_name);
/* As above, with a small Java-side value (used by enum-like platform objects). */
jobject jni_make_object_class_value(const char *class_name, jlong value);
/* Generic Java wrapper carrying a string property such as a component id. */
jobject jni_make_object_class_string(const char *class_name, const char *value);
/* Exact com.ea.nimble.Error wrapper used when a platform service is unavailable. */
jobject jni_make_nimble_error(int code, const char *message);
/* Small string->string HashMap constructor used by Nimble identity bridges. */
jobject jni_make_string_map(const char *const *keys, const char *const *values, int count);
const char *jni_object_string(jobject object);
const char *jni_object_class_name(jobject object);
jobject jni_make_activity(void);
jobject jni_make_surface(void);

/* A small Java-side representation of a Synergy/MTX catalog item.  The
 * native game only sees NimbleCatalogItem virtuals through JNI, so keeping
 * this description here lets the real HTTP response cross that boundary
 * without inventing a purchase or catalog result. */
typedef struct {
  const char *sku;
  const char *sell_id;
  const char *title;
  const char *description;
  const char *formatted_price;
  const char *metadata_url;
  float price;
  int item_type; /* NimbleCatalogItem.ItemType ordinal. */
  int is_free;
} JniCatalogItem;

jobject jni_make_catalog_list(const JniCatalogItem *items, int count);
int     jni_list_length(jobject list);

// Resolve a native callback by the name the engine passed to RegisterNatives
// (populated during JNI_OnLoad). Handles obfuscated/static symbols uniformly.
void *jni_registered(const char *name);
jclass  jni_find_class_c(const char *name);   // same as env FindClass, for C use

// --- audio: wrap a raw PCM buffer as a jbyteArray for nativeMixData ---
// The engine fills it via GetByteArrayElements/ReleaseByteArrayElements.
jbyteArray jni_wrap_bytearray(void *data, int len_bytes);
jbyteArray jni_make_bytearray_copy(const void *data, int len_bytes);
void       jni_free_wrapper(jobject o);
int       *jni_intarray_data(jobject o, int *len);
void      *jni_buffer_data(jobject o, int *len);
jlong      jni_object_long(jobject o);
void       jni_object_set_long(jobject o, jlong value);
const char *jni_http_url(jobject o);
const char *jni_http_method(jobject o);
const char *jni_http_headers(jobject o);
const void *jni_http_body(jobject o, int *len);
void        jni_http_set_header(jobject o, const char *name, const char *value);
void        jni_http_set_body(jobject o, jobject body);
void        jni_http_set_timeout(jobject o, int timeout_ms);
int         jni_http_timeout(jobject o);
void        jni_http_set_status(jobject o, int status);
int         jni_http_status(jobject o);
/* Returns true once when the Java transaction is released.  This mirrors the
 * Android bridge's final native cleanup notification without duplicating it
 * if the engine calls both Release and Cleanup. */
int         jni_http_take_cleanup(jobject o);
void        jni_http_set_response_headers(jobject o, const void *data, size_t len);
const char *jni_http_get_response_header(jobject o, const char *name);

#endif
