/* jni_fake.c -- fake JNI environment used by the game runtime
 *
 * See jni_fake.h. The interface is a flat array of function pointers indexed by
 * JNI-spec slot (jni.h / jni_slots.h). We implement the structural functions
 * the engine relies on (FindClass, Get*MethodID, the Call* families, strings,
 * byte/int arrays, refs, exceptions, GetJavaVM, RegisterNatives) and fill every
 * remaining slot with a logging catch-all so an unexpected call is visible in
 * the log rather than a jump through NULL. Method calls are routed by name into
 * the platform compatibility layer.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <switch.h>

#include "config.h"
#include "jni.h"
#include "jni_fake.h"
#include "util.h"
#include "pvz2.h"

JavaVM fake_vm = NULL;
JNIEnv fake_env = NULL;
volatile int g_kbd_requested = 0;
char g_kbd_initial[64] = {0};
volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

typedef enum {
  T_GENERIC, T_STRING, T_CLASS, T_BYTEARRAY, T_INTARRAY, T_OBJARRAY,
  T_BUFFER, T_MAP, T_LIST, T_SET, T_ITER, T_CATALOG_ITEM
} Tag;

typedef struct FakeObj {
  Tag tag;
  // string
  char *str;
  // arrays
  void *data;       // element storage
  int   len;        // element count
  int   owns_data;  // free data on release?
  int   is_catalog_list;
  jobject *objs;    // for object arrays
  jlong value;      // native handle retained by Java bridge objects
  char *http_method;
  char *http_headers;
  char *http_response_headers;
  char *http_response_header_value;
  unsigned char *http_body;
  int http_body_len;
  int http_timeout_ms;
  int http_status;
  int http_cleanup_taken;
  // NimbleCatalogItem fields exposed through the fake JNI bridge.
  char *catalog_sku;
  char *catalog_sell_id;
  char *catalog_title;
  char *catalog_description;
  char *catalog_formatted_price;
  char *catalog_metadata_url;
  float catalog_price;
  int catalog_item_type;
  int catalog_is_free;
  // class
  const char *cls;  // interned class name (for T_CLASS)
} FakeObj;

static const char *fake_string(FakeObj *o) {
  return o && o->tag == T_STRING && o->str ? o->str : NULL;
}

static FakeObj *obj_new(Tag t) {
  FakeObj *o = calloc(1, sizeof(*o));
  if (o) o->tag = t;
  return o;
}

static jobject obj_for_class(const char *cls) {
  const Tag tag = !strcmp(cls, "java/util/HashMap") || !strcmp(cls, "java/util/Map") ? T_MAP :
                  !strcmp(cls, "java/util/List") || !strcmp(cls, "java/util/ArrayList") ? T_LIST :
                  !strcmp(cls, "java/util/Set") ? T_SET :
                  !strcmp(cls, "java/util/Iterator") ? T_ITER : T_GENERIC;
  FakeObj *o = obj_new(tag);
  if (o) o->cls = cls;
  return o;
}

static char *fake_strdup(const char *value) {
  const char *source = value ? value : "";
  char *copy = strdup(source);
  return copy;
}

static jobject map_put(FakeObj *map, jobject key, jobject value);

static jobject catalog_item_string(FakeObj *item, const char *name) {
  if (!strcmp(name, "getSku"))
    return jni_make_string(item->catalog_sku);
  if (!strcmp(name, "getSellId"))
    return jni_make_string(item->catalog_sell_id);
  if (!strcmp(name, "getTitle"))
    return jni_make_string(item->catalog_title);
  if (!strcmp(name, "getDescription"))
    return jni_make_string(item->catalog_description);
  if (!strcmp(name, "getPriceWithCurrencyAndFormat") ||
      !strcmp(name, "getFormattedPrice"))
    return jni_make_string(item->catalog_formatted_price);
  if (!strcmp(name, "getMetaDataUrl"))
    return jni_make_string(item->catalog_metadata_url);
  return NULL;
}

static jobject catalog_item_info(FakeObj *item) {
  FakeObj *map = obj_for_class("java/util/HashMap");
  if (!map) return NULL;
  map_put(map, jni_make_string("sellId"), jni_make_string(item->catalog_sell_id));
  map_put(map, jni_make_string("metadata"), jni_make_string(item->catalog_metadata_url));
  return map;
}

static int map_index(FakeObj *map, jobject key) {
  if (!map || map->tag != T_MAP) return -1;
  FakeObj *wanted = key;
  for (int i = 0; i < map->len; i++) {
    FakeObj *found = map->objs[i * 2];
    if (found == wanted ||
        (found && wanted && found->tag == T_STRING && wanted->tag == T_STRING &&
         !strcmp(found->str, wanted->str)))
      return i;
  }
  return -1;
}

static jobject map_put(FakeObj *map, jobject key, jobject value) {
  const int index = map_index(map, key);
  if (index >= 0) {
    jobject previous = map->objs[index * 2 + 1];
    map->objs[index * 2 + 1] = value;
    return previous;
  }
  jobject *entries = realloc(map->objs, (size_t)(map->len + 1) * 2 * sizeof(*entries));
  if (!entries) return NULL;
  map->objs = entries;
  map->objs[map->len * 2] = key;
  map->objs[map->len * 2 + 1] = value;
  map->len++;
  return NULL;
}

static jobject map_get(FakeObj *map, jobject key) {
  const int index = map_index(map, key);
  return index >= 0 ? map->objs[index * 2 + 1] : NULL;
}

static jobject map_view(Tag tag, const char *cls, FakeObj *map) {
  FakeObj *view = obj_new(tag);
  if (view) { view->cls = cls; view->data = map; }
  return view;
}

static void map_self_check(void) {
#ifndef NDEBUG
  FakeObj map = {.tag = T_MAP};
  FakeObj key = {.tag = T_STRING, .str = "key"};
  FakeObj same_key = {.tag = T_STRING, .str = "key"};
  FakeObj value = {.tag = T_GENERIC};
  assert(map_put(&map, &key, &value) == NULL);
  assert(map_get(&map, &same_key) == &value);
  free(map.objs);
#endif
}

// --- class registry: intern class names so the same name -> same handle ---
#define MAX_CLASSES 512
static FakeObj *g_classes[MAX_CLASSES];
static int g_nclasses = 0;

// The engine calls FindClass / GetMethodID / NewObject from worker threads, so
// these registries (and their g_n* counters + backing arrays) are shared mutable
// state. Without a lock, two threads interning at once race the counter and
// corrupt the arrays / leak. RMutex is recursive because find_or_make_class is
// reachable both directly and (potentially) from other locked paths.
static RMutex g_jni_reg_mutex;

static jclass find_or_make_class(const char *name) {
  rmutexLock(&g_jni_reg_mutex);
  jclass ret = NULL;
  for (int i = 0; i < g_nclasses; i++)
    if (strcmp(g_classes[i]->cls, name) == 0) { ret = g_classes[i]; break; }
  if (!ret) {
    if (g_nclasses >= MAX_CLASSES) {
      ret = g_classes[0];
    } else {
      FakeObj *c = obj_new(T_CLASS);
      c->cls = strdup(name);
      g_classes[g_nclasses++] = c;
      ret = c;
    }
  }
  rmutexUnlock(&g_jni_reg_mutex);
  return ret;
}

// --- method registry: (class,name,sig) -> stable token, so cached IDs work ---
typedef struct { const char *cls; char *name; char *sig; } FakeMethod;
#define MAX_METHODS 2048
static FakeMethod *g_methods[MAX_METHODS];
static int g_nmethods = 0;
static int g_last_audio_property;

static jmethodID find_or_make_method(const char *cls, const char *name, const char *sig) {
  rmutexLock(&g_jni_reg_mutex);
  jmethodID ret = NULL;
  const char *wanted_sig = sig ? sig : "";
  for (int i = 0; i < g_nmethods; i++)
    if (g_methods[i]->cls == cls &&
        strcmp(g_methods[i]->name, name) == 0 &&
        strcmp(g_methods[i]->sig, wanted_sig) == 0) {
      ret = g_methods[i];
      break;
    }
  if (!ret) {
    if (g_nmethods >= MAX_METHODS) {
      ret = g_methods[0];
    } else {
      FakeMethod *m = calloc(1, sizeof(*m));
      m->cls = cls; m->name = strdup(name); m->sig = strdup(sig ? sig : "");
      g_methods[g_nmethods++] = m;
      ret = m;
    }
  }
  rmutexUnlock(&g_jni_reg_mutex);
  return ret;
}

// ---------------------------------------------------------------------------
// JNI function implementations
// ---------------------------------------------------------------------------

static jint jf_GetVersion(JNIEnv e) { (void)e; return JNI_VERSION_1_6; }

static jclass jf_FindClass(JNIEnv e, const char *name) {
  (void)e;
  return find_or_make_class(name ? name : "?");
}

static jmethodID jf_GetMethodID(JNIEnv e, jclass c, const char *name, const char *sig) {
  (void)e;
  FakeObj *cl = c;
  const char *cn = (cl && cl->tag == T_CLASS) ? cl->cls : "?";
  return find_or_make_method(cn, name ? name : "?", sig);
}
static jmethodID jf_GetStaticMethodID(JNIEnv e, jclass c, const char *name, const char *sig) {
  return jf_GetMethodID(e, c, name, sig);
}
static jfieldID jf_GetFieldID(JNIEnv e, jclass c, const char *n, const char *s) {
  return (jfieldID)jf_GetMethodID(e, c, n, s);
}
static jfieldID jf_GetStaticFieldID(JNIEnv e, jclass c, const char *n, const char *s) {
  return (jfieldID)jf_GetMethodID(e, c, n, s);
}
static jint jf_GetStaticIntField(JNIEnv e, jclass c, jfieldID f) {
  (void)e; (void)c;
  FakeMethod *field = f;
  /* API 25 keeps Wwise on OpenSL ES: new enough to support it, below AAudio. */
  if (field && !strcmp(field->name, "SDK_INT")) return 25;
  return 0;
}
static jobject jf_GetStaticObjectField(JNIEnv e, jclass c, jfieldID f) {
  (void)e; (void)c;
  FakeMethod *field = f;
  if (!field) return NULL;
  if (strstr(field->cls, "media/AudioManager")) {
    if (!strcmp(field->name, "PROPERTY_OUTPUT_SAMPLE_RATE")) {
      g_last_audio_property = 1;
      return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE");
    }
    if (!strcmp(field->name, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) {
      g_last_audio_property = 2;
      return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER");
    }
  }
  if (strstr(field->cls, "content/Context") &&
      !strcmp(field->name, "AUDIO_SERVICE"))
    return jni_make_string("audio");
  if (strstr(field->cls, "os/Build")) {
    if (!strcmp(field->name, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(field->name, "MODEL")) return jni_make_string("Switch");
  }
  return NULL;
}

// --- strings ---
static jstring jf_NewStringUTF(JNIEnv e, const char *bytes) {
  (void)e;
  FakeObj *o = obj_new(T_STRING);
  o->str = strdup(bytes ? bytes : "");
  return o;
}
static const char *jf_GetStringUTFChars(JNIEnv e, jstring s, jboolean *isCopy) {
  (void)e;
  FakeObj *o = s;
  if (isCopy) *isCopy = JNI_FALSE;
  return (o && o->tag == T_STRING) ? o->str : "";
}
static void jf_ReleaseStringUTFChars(JNIEnv e, jstring s, const char *c) {
  (void)e; (void)s; (void)c; // chars point straight into the FakeObj; nothing to free
}
static jsize jf_GetStringUTFLength(JNIEnv e, jstring s) {
  (void)e; FakeObj *o = s;
  return (o && o->str) ? (jsize)strlen(o->str) : 0;
}
static jsize jf_GetStringLength(JNIEnv e, jstring s) {
  return jf_GetStringUTFLength(e, s); // ASCII paths/locales: char count == byte count
}
// UTF-16 variants: the engine occasionally uses these for font/text work. We
// back them with a per-string malloc'd jchar buffer (freed on release).
static const jchar *jf_GetStringChars(JNIEnv e, jstring s, jboolean *isCopy) {
  (void)e; FakeObj *o = s;
  const char *u = (o && o->str) ? o->str : "";
  size_t n = strlen(u);
  jchar *w = malloc((n + 1) * sizeof(jchar));
  for (size_t i = 0; i < n; i++) w[i] = (jchar)(unsigned char)u[i];
  w[n] = 0;
  if (isCopy) *isCopy = JNI_TRUE;
  return w;
}
static void jf_ReleaseStringChars(JNIEnv e, jstring s, const jchar *c) {
  (void)e; (void)s; free((void *)c);
}
static jstring jf_NewString(JNIEnv e, const jchar *u, jsize len) {
  (void)e;
  FakeObj *o = obj_new(T_STRING);
  o->str = malloc(len + 1);
  for (jsize i = 0; i < len; i++) o->str[i] = (char)u[i];
  o->str[len] = 0;
  return o;
}

// --- arrays ---
static jsize jf_GetArrayLength(JNIEnv e, jarray a) {
  (void)e; FakeObj *o = a; return o ? (jsize)o->len : 0;
}
static jbyteArray jf_NewByteArray(JNIEnv e, jsize len) {
  (void)e;
  FakeObj *o = obj_new(T_BYTEARRAY);
  o->len = len; o->data = calloc(len > 0 ? len : 1, 1); o->owns_data = 1;
  return o;
}
static jintArray jf_NewIntArray(JNIEnv e, jsize len) {
  (void)e;
  FakeObj *o = obj_new(T_INTARRAY);
  o->len = len; o->data = calloc(len > 0 ? len : 1, sizeof(jint)); o->owns_data = 1;
  return o;
}
static jbyte *jf_GetByteArrayElements(JNIEnv e, jbyteArray a, jboolean *isCopy) {
  (void)e; FakeObj *o = a;
  if (isCopy) *isCopy = JNI_FALSE;
  return o ? (jbyte *)o->data : NULL;   // direct pointer: the engine fills PCM here
}
static void jf_SetByteArrayRegion(JNIEnv e, jbyteArray a, jsize start, jsize len,
                                  const jbyte *buf) {
  (void)e;
  FakeObj *o = a;
  if (!o || o->tag != T_BYTEARRAY || start < 0 || len < 0 ||
      start > o->len || len > o->len - start || (len && !buf)) return;
  if (len) memcpy((unsigned char *)o->data + start, buf, (size_t)len);
}
static void jf_ReleaseByteArrayElements(JNIEnv e, jbyteArray a, jbyte *p, jint mode) {
  (void)e; (void)a; (void)p; (void)mode; // data is the array's own storage; nothing to copy back
}
static jint *jf_GetIntArrayElements(JNIEnv e, jintArray a, jboolean *isCopy) {
  (void)e; FakeObj *o = a; if (isCopy) *isCopy = JNI_FALSE;
  return o ? (jint *)o->data : NULL;
}
static void jf_ReleaseIntArrayElements(JNIEnv e, jintArray a, jint *p, jint m) {
  (void)e; (void)a; (void)p; (void)m;
}
static void *jf_GetPrimitiveArrayCritical(JNIEnv e, jarray a, jboolean *isCopy) {
  (void)e; FakeObj *o = a; if (isCopy) *isCopy = JNI_FALSE;
  return o ? o->data : NULL;
}
static void jf_ReleasePrimitiveArrayCritical(JNIEnv e, jarray a, void *p, jint m) {
  (void)e; (void)a; (void)p; (void)m;
}
static jobjectArray jf_NewObjectArray(JNIEnv e, jsize len, jclass cls, jobject init) {
  (void)e; (void)cls;
  FakeObj *o = obj_new(T_OBJARRAY);
  o->len = len; o->objs = calloc(len > 0 ? len : 1, sizeof(jobject));
  for (jsize i = 0; i < len; i++) o->objs[i] = init;
  return o;
}
static jobject jf_GetObjectArrayElement(JNIEnv e, jobjectArray a, jsize i) {
  (void)e; FakeObj *o = a;
  return (o && o->objs && i >= 0 && i < o->len) ? o->objs[i] : NULL;
}
static void jf_SetObjectArrayElement(JNIEnv e, jobjectArray a, jsize i, jobject v) {
  (void)e; FakeObj *o = a;
  if (o && o->objs && i >= 0 && i < o->len) o->objs[i] = v;
}

static jobject jf_NewDirectByteBuffer(JNIEnv e, void *address, jlong capacity) {
  (void)e;
  FakeObj *o = obj_new(T_BUFFER);
  o->data = address;
  o->len = (int)capacity;
  return o;
}

static void *jf_GetDirectBufferAddress(JNIEnv e, jobject buffer) {
  (void)e;
  FakeObj *o = buffer;
  return (o && o->tag == T_BUFFER) ? o->data : NULL;
}

static jlong jf_GetDirectBufferCapacity(JNIEnv e, jobject buffer) {
  (void)e;
  FakeObj *o = buffer;
  return (o && o->tag == T_BUFFER) ? o->len : -1;
}

void *jni_buffer_data(jobject buffer, int *len) {
  FakeObj *o = buffer;
  if (!o || o->tag != T_BUFFER) return NULL;
  if (len) *len = o->len;
  return o->data;
}

jlong jni_object_long(jobject object) {
  FakeObj *o = object;
  return o ? o->value : 0;
}

// --- objects / refs / exceptions ---
static jclass jf_GetObjectClass(JNIEnv e, jobject o) {
  (void)e; FakeObj *f = o;
  if (f && f->tag == T_CLASS) return f;
  return find_or_make_class((f && f->cls) ? f->cls : "java/lang/Object");
}
static jboolean jf_IsInstanceOf(JNIEnv e, jobject o, jclass c) {
  (void)e;
  FakeObj *value = o, *type = c;
  if (!value || !type) return JNI_FALSE;
  const char *wanted = type->cls;
  if (!wanted || !strcmp(wanted, "java/lang/Object")) return JNI_TRUE;
  if (!strcmp(wanted, "java/lang/String")) return value->tag == T_STRING;
  if (!strcmp(wanted, "java/lang/Boolean") || !strcmp(wanted, "java/lang/Long") ||
      !strcmp(wanted, "java/lang/Integer") || !strcmp(wanted, "java/lang/Float") ||
      !strcmp(wanted, "java/lang/Double"))
    return value->cls && !strcmp(value->cls, wanted);
  return JNI_TRUE;
}
static jboolean jf_IsSameObject(JNIEnv e, jobject a, jobject b) { (void)e; return a == b; }
static jobject jf_NewRef(JNIEnv e, jobject o) { (void)e; return o; }     // global/local/weak all identity
static void    jf_DeleteRef(JNIEnv e, jobject o) { (void)e; (void)o; }   // leak; engine may retain refs
static jint    jf_EnsureLocalCapacity(JNIEnv e, jint n) { (void)e;(void)n; return 0; }
static jint    jf_PushLocalFrame(JNIEnv e, jint n) { (void)e;(void)n; return 0; }
static jobject jf_PopLocalFrame(JNIEnv e, jobject r) { (void)e; return r; }
static jobject jf_AllocObject(JNIEnv e, jclass c) {
  (void)e;
  FakeObj *cl = c;
  return obj_for_class((cl && cl->tag == T_CLASS) ? cl->cls : "java/lang/Object");
}
// NewObject(clazz, ctor, ...) -- we don't run constructors, but the object must
// be non-NULL or the engine treats it as "NewObject failed" and throws. Return
// a generic instance; any methods later called on it route through the catch-all.
//
// The engine constructs Java helper objects (e.g. Store$Product, PlayerDetails)
// via NewObject; we return an opaque generic object -- their fields are only
// read back through our own up-calls, which we answer directly. (Unlike the
// The engine audio path is OpenSL ES, so there is no AudioOutput peer to
// capture here.)
static jobject new_object_common(jclass c, jmethodID ctor, va_list ap) {
  FakeObj *cl = c;
  const char *cn = (cl && cl->tag == T_CLASS) ? cl->cls : "?";
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  debugPrintf("NewObject(%s)\n", cn);
#endif
  FakeObj *o = obj_for_class(cn);
  FakeMethod *m = ctor;
  if (o && strstr(cn, "AndroidHttpTransaction") && m && m->sig[0] == '(' && m->sig[1] == 'J') {
    o->value = va_arg(ap, jlong);
    if (!strcmp(m->sig, "(JLjava/lang/String;Ljava/lang/String;)V")) {
      FakeObj *method = va_arg(ap, FakeObj *);
      FakeObj *url = va_arg(ap, FakeObj *);
      const char *url_text = fake_string(url), *method_text = fake_string(method);
      o->str = strdup(url_text ? url_text : "");
      o->http_method = strdup(method_text ? method_text : "GET");
      o->http_timeout_ms = 5000;
    }
  }
  return o;
}
static jobject jf_NewObject(JNIEnv e, jclass c, jmethodID m, ...) {
  (void)e; va_list ap; va_start(ap, m);
  jobject o = new_object_common(c, m, ap); va_end(ap); return o;
}
static jobject jf_NewObjectV(JNIEnv e, jclass c, jmethodID m, va_list ap) {
  (void)e; return new_object_common(c, m, ap);
}
static jobject jf_NewObjectA(JNIEnv e, jclass c, jmethodID m, void *av) {
  (void)e;
  FakeObj *cl = c;
  const char *cn = (cl && cl->tag == T_CLASS) ? cl->cls : "?";
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  debugPrintf("NewObjectA(%s)\n", cn);
#endif
  FakeObj *o = obj_for_class(cn);
  FakeMethod *ctor = m;
  if (o && av && strstr(cn, "AndroidHttpTransaction") && ctor && ctor->sig[0] == '(' && ctor->sig[1] == 'J') {
    o->value = ((jvalue *)av)[0].j;
    if (!strcmp(ctor->sig, "(JLjava/lang/String;Ljava/lang/String;)V")) {
      FakeObj *method = ((jvalue *)av)[1].l;
      FakeObj *url = ((jvalue *)av)[2].l;
      const char *url_text = fake_string(url), *method_text = fake_string(method);
      o->str = strdup(url_text ? url_text : "");
      o->http_method = strdup(method_text ? method_text : "GET");
      o->http_timeout_ms = 5000;
    }
  }
  return o;
}
static jthrowable jf_ExceptionOccurred(JNIEnv e) { (void)e; return NULL; }
static void    jf_ExceptionClear(JNIEnv e) { (void)e; }
static void    jf_ExceptionDescribe(JNIEnv e) { (void)e; }
static jboolean jf_ExceptionCheck(JNIEnv e) { (void)e; return JNI_FALSE; }
static jint    jf_MonitorOp(JNIEnv e, jobject o) { (void)e; (void)o; return 0; }

static jint jf_GetJavaVM(JNIEnv e, JavaVM *vm) { (void)e; if (vm) *vm = fake_vm; return JNI_OK; }

// The engine binds its native callbacks here. We record name->fnPtr so the
// loader can resolve callbacks by their REGISTERED Java name (e.g. "nativeTick",
// "nativeLicenseResult") regardless of the underlying .dynsym symbol -- which
// matters because some are obfuscated (nativeLicenseResult -> ox94jnabared) and
// others are static. This is how Android itself binds them.
typedef struct { char *name; void *fn; } RegNat;
static RegNat  s_regnat[512];
static int     s_regnat_n = 0;

void *jni_registered(const char *name) {
  for (int i = 0; i < s_regnat_n; i++)
    if (!strcmp(s_regnat[i].name, name)) return s_regnat[i].fn;
  return NULL;
}

static jint jf_RegisterNatives(JNIEnv e, jclass c, const JNINativeMethod *m, jint n) {
  (void)e;
  FakeObj *cl = c;
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  debugPrintf("JNI RegisterNatives(%s, %d methods)\n",
              (cl && cl->tag == T_CLASS) ? cl->cls : "?", n);
#else
  (void)cl;
#endif
  for (jint i = 0; i < n && s_regnat_n < 512; i++) {
    if (!m[i].name || !m[i].fnPtr) continue;
    s_regnat[s_regnat_n].name = strdup(m[i].name);
    s_regnat[s_regnat_n].fn   = m[i].fnPtr;
    s_regnat_n++;
    const char *class_name = (cl && cl->tag == T_CLASS && cl->cls) ? cl->cls : "?";
    if (strstr(class_name, "Cloud") || strstr(class_name, "cloud") ||
        strstr(class_name, "Account") || strstr(class_name, "account") ||
        strstr(m[i].name, "Cloud") || strstr(m[i].name, "cloud") ||
        strstr(m[i].name, "Auth") || strstr(m[i].name, "auth") ||
        strstr(m[i].name, "Email") || strstr(m[i].name, "email") ||
        strstr(m[i].name, "Account") || strstr(m[i].name, "account"))
      debugPrintf("ACCOUNT JNI BIND: class=%s name=%s sig=%s fn=%p\n",
                  class_name, m[i].name, m[i].signature ? m[i].signature : "",
                  m[i].fnPtr);
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
    debugPrintf("  bind %-32s %-10s -> %p\n", m[i].name,
                m[i].signature ? m[i].signature : "", m[i].fnPtr);
#endif
  }
  return JNI_OK;
}
static jint jf_UnregisterNatives(JNIEnv e, jclass c) { (void)e;(void)c; return JNI_OK; }

// ---------------------------------------------------------------------------
// method-call dispatch: read the method token and route it to the platform layer
// ---------------------------------------------------------------------------

static jobject audio_property_value(jobject key_object, const char *call_form) {
  const char *key = jni_cstr(key_object);
  int which = key && strstr(key, "SAMPLE_RATE") ? 1 :
              key && strstr(key, "FRAMES_PER_BUFFER") ? 2 :
              g_last_audio_property;
  const char *value = which == 1 ? "48000" : which == 2 ? "256" : "";
  debugPrintf("JNI AudioManager.%s(%s) -> %s\n", call_form,
              key ? key : "(null)", value);
  return jni_make_string(value);
}

static jobject g_analytics_custom_properties;

static jvalue call_v(JNIEnv e, jobject self, jmethodID mid, va_list ap) {
  (void)e;
  jvalue r; r.j = 0;
  FakeMethod *m = mid;
  if (!m) return r;
  if (!strcmp(m->name, "getProperty"))
    return (jvalue){.l = audio_property_value(va_arg(ap, jobject), "getProperty")};

  if (!strcmp(m->name, "valueOf") && !strcmp(m->cls, "java/lang/Boolean")) {
    FakeObj *boxed = obj_for_class(m->cls);
    boxed->value = va_arg(ap, int) != 0;
    return (jvalue){.l = boxed};
  }
  if (!strcmp(m->name, "valueOf") &&
      (!strcmp(m->cls, "java/lang/Long") || !strcmp(m->cls, "java/lang/Integer"))) {
    FakeObj *boxed = obj_for_class(m->cls);
    boxed->value = !strcmp(m->cls, "java/lang/Long") ? va_arg(ap, jlong) : va_arg(ap, int);
    return (jvalue){.l = boxed};
  }
  if (!strcmp(m->name, "booleanValue"))
    return (jvalue){.z = self && ((FakeObj *)self)->value != 0};
  if (!strcmp(m->name, "longValue"))
    return (jvalue){.j = self ? ((FakeObj *)self)->value : 0};
  if (!strcmp(m->name, "intValue"))
    return (jvalue){.i = self ? (jint)((FakeObj *)self)->value : 0};
  if (!strcmp(m->name, "getCode") && strstr(m->cls, "com/ea/nimble/Error"))
    return (jvalue){.i = self ? (jint)((FakeObj *)self)->value : -1001};
  if ((!strcmp(m->name, "getMessage") || !strcmp(m->name, "toString")) &&
      (strstr(m->cls, "Throwable") || strstr(m->cls, "nimble/Error")))
    return (jvalue){.l = jni_make_string(
        "Local shop offer must use the coin/gem purchase path")};

  if (strstr(m->cls, "gluanalytics/Analytics") &&
      !strcmp(m->name, "setCustomProperties")) {
    g_analytics_custom_properties = va_arg(ap, jobject);
    FakeObj *map = g_analytics_custom_properties;
    debugPrintf("JNI Analytics.setCustomProperties(size=%d)\n",
                map && map->tag == T_MAP ? map->len : 0);
    return r;
  }
  if (strstr(m->cls, "gluanalytics/Analytics") &&
      !strcmp(m->name, "getCustomProperties")) {
    if (!g_analytics_custom_properties)
      g_analytics_custom_properties = obj_for_class("java/util/HashMap");
    FakeObj *map = g_analytics_custom_properties;
    debugPrintf("JNI Analytics.getCustomProperties(size=%d)\n",
                map && map->tag == T_MAP ? map->len : 0);
    return (jvalue){.l = g_analytics_custom_properties};
  }
  if (strstr(m->cls, "gluanalytics/Analytics") &&
      (!strcmp(m->name, "setUserIdentifier") || !strcmp(m->name, "logEvent")))
    return r;

  FakeObj *object = self;
  if (object && object->tag == T_CATALOG_ITEM) {
    debugPrintf("JNI CATALOG ITEM.%s sku=%s [V]\n", m->name,
                object->catalog_sku ? object->catalog_sku : "");
    jobject text = catalog_item_string(object, m->name);
    if (text) return (jvalue){.l = text};
    if (!strcmp(m->name, "getPriceDecimal"))
      return (jvalue){.f = object->catalog_price};
    if (!strcmp(m->name, "isFree"))
      return (jvalue){.z = object->catalog_is_free != 0};
    if (!strcmp(m->name, "getItemType"))
      return (jvalue){.l = jni_make_object_class_value(
          "com/ea/nimble/mtx/NimbleCatalogItem$ItemType",
          object->catalog_item_type)};
    if (!strcmp(m->name, "getAdditionalInfo"))
      return (jvalue){.l = catalog_item_info(object)};
  }
  if (object && object->tag == T_MAP) {
    if (!strcmp(m->name, "put")) {
      jobject key = va_arg(ap, jobject);
      jobject value = va_arg(ap, jobject);
      jobject previous = map_put(object, key, value);
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
      debugPrintf("JNI HashMap.put(%s), size=%d\n",
                  jni_cstr(key) ? jni_cstr(key) : "(object)", object->len);
#endif
      return (jvalue){.l = previous};
    }
    if (!strcmp(m->name, "get"))
      return (jvalue){.l = map_get(object, va_arg(ap, jobject))};
    if (!strcmp(m->name, "keySet"))
      return (jvalue){.l = map_view(T_SET, "java/util/Set", object)};
    if (!strcmp(m->name, "size")) return (jvalue){.i = object->len};
    if (!strcmp(m->name, "isEmpty")) return (jvalue){.z = object->len == 0};
    if (!strcmp(m->name, "containsKey"))
      return (jvalue){.z = map_index(object, va_arg(ap, jobject)) >= 0};
  }
  if (object && object->tag == T_LIST) {
    if (!strcmp(m->name, "iterator")) {
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.iterator(size=%d) [V]\n", object->len);
      return (jvalue){.l = map_view(T_ITER, "java/util/Iterator", object)};
    }
    if (!strcmp(m->name, "get")) {
      const int index = va_arg(ap, int);
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.get(%d/%d) [V]\n", index, object->len);
      return (jvalue){.l = index >= 0 && index < object->len ? object->objs[index] : NULL};
    }
    if (!strcmp(m->name, "size")) {
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.size() -> %d [V]\n", object->len);
      return (jvalue){.i = object->len};
    }
    if (!strcmp(m->name, "isEmpty")) {
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.isEmpty() -> %d [V]\n", object->len == 0);
      return (jvalue){.z = object->len == 0};
    }
  }
  if (object && object->tag == T_SET && !strcmp(m->name, "iterator"))
    return (jvalue){.l = map_view(T_ITER, "java/util/Iterator", object->data)};
  if (object && object->tag == T_ITER) {
    FakeObj *map = object->data;
    if (!strcmp(m->name, "hasNext")) {
      const int has_next = map && (map->tag == T_MAP || map->tag == T_LIST) &&
                           object->len < map->len;
      if (map && map->is_catalog_list)
        debugPrintf("JNI CATALOG ITER.hasNext(%d/%d) -> %d [V]\n",
                    object->len, map->len, has_next);
      return (jvalue){.z = has_next};
    }
    if (!strcmp(m->name, "next")) {
      if (!map || object->len >= map->len) return r;
      if (map->tag == T_LIST) {
        if (map->is_catalog_list)
          debugPrintf("JNI CATALOG ITER.next(%d/%d) [V]\n", object->len, map->len);
        return (jvalue){.l = map->objs[object->len++]};
      }
      if (map->tag == T_MAP)
        return (jvalue){.l = map->objs[object->len++ * 2]};
    }
  }
  return pvz2_upcall(m->cls, m->name, m->sig, self, ap);
}

static jvalue call_a(JNIEnv e, jobject self, jmethodID mid, const jvalue *args) {
  (void)e;
  jvalue r; r.j = 0;
  FakeMethod *m = mid;
  if (!m) return r;
  if (!strcmp(m->name, "getProperty"))
    return (jvalue){.l = audio_property_value(args ? args[0].l : NULL, "getPropertyA")};

  if (!strcmp(m->name, "valueOf") && !strcmp(m->cls, "java/lang/Boolean")) {
    FakeObj *boxed = obj_for_class(m->cls);
    boxed->value = args && args[0].z != 0;
    return (jvalue){.l = boxed};
  }
  if (!strcmp(m->name, "valueOf") &&
      (!strcmp(m->cls, "java/lang/Long") || !strcmp(m->cls, "java/lang/Integer"))) {
    FakeObj *boxed = obj_for_class(m->cls);
    boxed->value = args ? (!strcmp(m->cls, "java/lang/Long") ? args[0].j : args[0].i) : 0;
    return (jvalue){.l = boxed};
  }
  if (!strcmp(m->name, "booleanValue"))
    return (jvalue){.z = self && ((FakeObj *)self)->value != 0};
  if (!strcmp(m->name, "longValue"))
    return (jvalue){.j = self ? ((FakeObj *)self)->value : 0};
  if (!strcmp(m->name, "intValue"))
    return (jvalue){.i = self ? (jint)((FakeObj *)self)->value : 0};
  if (!strcmp(m->name, "getCode") && strstr(m->cls, "com/ea/nimble/Error"))
    return (jvalue){.i = self ? (jint)((FakeObj *)self)->value : -1001};
  if ((!strcmp(m->name, "getMessage") || !strcmp(m->name, "toString")) &&
      (strstr(m->cls, "Throwable") || strstr(m->cls, "nimble/Error")))
    return (jvalue){.l = jni_make_string(
        "Local shop offer must use the coin/gem purchase path")};

  if (strstr(m->cls, "gluanalytics/Analytics") &&
      !strcmp(m->name, "setCustomProperties")) {
    g_analytics_custom_properties = args ? args[0].l : NULL;
    return r;
  }
  if (strstr(m->cls, "gluanalytics/Analytics") &&
      !strcmp(m->name, "getCustomProperties")) {
    if (!g_analytics_custom_properties)
      g_analytics_custom_properties = obj_for_class("java/util/HashMap");
    FakeObj *map = g_analytics_custom_properties;
    debugPrintf("JNI Analytics.getCustomProperties(size=%d) [A]\n",
                map && map->tag == T_MAP ? map->len : 0);
    return (jvalue){.l = g_analytics_custom_properties};
  }
  if (strstr(m->cls, "gluanalytics/Analytics") &&
      (!strcmp(m->name, "setUserIdentifier") || !strcmp(m->name, "logEvent")))
    return r;

  FakeObj *object = self;
  if (object && object->tag == T_CATALOG_ITEM) {
    debugPrintf("JNI CATALOG ITEM.%s sku=%s [A]\n", m->name,
                object->catalog_sku ? object->catalog_sku : "");
    jobject text = catalog_item_string(object, m->name);
    if (text) return (jvalue){.l = text};
    if (!strcmp(m->name, "getPriceDecimal"))
      return (jvalue){.f = object->catalog_price};
    if (!strcmp(m->name, "isFree"))
      return (jvalue){.z = object->catalog_is_free != 0};
    if (!strcmp(m->name, "getItemType"))
      return (jvalue){.l = jni_make_object_class_value(
          "com/ea/nimble/mtx/NimbleCatalogItem$ItemType",
          object->catalog_item_type)};
    if (!strcmp(m->name, "getAdditionalInfo"))
      return (jvalue){.l = catalog_item_info(object)};
  }
  if (object && object->tag == T_MAP) {
    if (!strcmp(m->name, "put")) {
      jobject key = args ? args[0].l : NULL;
      jobject value = args ? args[1].l : NULL;
      jobject previous = map_put(object, key, value);
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
      debugPrintf("JNI HashMap.put(%s), size=%d [A]\n",
                  jni_cstr(key) ? jni_cstr(key) : "(object)", object->len);
#endif
      return (jvalue){.l = previous};
    }
    if (!strcmp(m->name, "get")) {
      jobject key = args ? args[0].l : NULL;
      jobject value = map_get(object, key);
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
      debugPrintf("JNI HashMap.get(%s) -> %s [A]\n",
                  jni_cstr(key) ? jni_cstr(key) : "(object)", value ? "hit" : "miss");
#endif
      return (jvalue){.l = value};
    }
    if (!strcmp(m->name, "keySet")) {
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
      debugPrintf("JNI HashMap.keySet(size=%d) [A]\n", object->len);
#endif
      return (jvalue){.l = map_view(T_SET, "java/util/Set", object)};
    }
    if (!strcmp(m->name, "size")) return (jvalue){.i = object->len};
    if (!strcmp(m->name, "isEmpty")) return (jvalue){.z = object->len == 0};
    if (!strcmp(m->name, "containsKey"))
      return (jvalue){.z = map_index(object, args ? args[0].l : NULL) >= 0};
  }
  if (object && object->tag == T_LIST) {
    if (!strcmp(m->name, "iterator")) {
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.iterator(size=%d) [A]\n", object->len);
      else
        debugPrintf("JNI List.iterator(size=%d) [A]\n", object->len);
      return (jvalue){.l = map_view(T_ITER, "java/util/Iterator", object)};
    }
    if (!strcmp(m->name, "get")) {
      const int index = args ? args[0].i : -1;
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.get(%d/%d) [A]\n", index, object->len);
      return (jvalue){.l = index >= 0 && index < object->len ? object->objs[index] : NULL};
    }
    if (!strcmp(m->name, "size")) {
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.size() -> %d [A]\n", object->len);
      return (jvalue){.i = object->len};
    }
    if (!strcmp(m->name, "isEmpty")) {
      if (object->is_catalog_list)
        debugPrintf("JNI CATALOG LIST.isEmpty() -> %d [A]\n", object->len == 0);
      return (jvalue){.z = object->len == 0};
    }
  }
  if (object && object->tag == T_SET && !strcmp(m->name, "iterator")) {
    FakeObj *map = object->data;
    debugPrintf("JNI Set.iterator(size=%d) [A]\n", map ? map->len : 0);
    return (jvalue){.l = map_view(T_ITER, "java/util/Iterator", map)};
  }
  if (object && object->tag == T_ITER) {
    FakeObj *map = object->data;
    if (!strcmp(m->name, "hasNext")) {
      const int has_next = map && (map->tag == T_MAP || map->tag == T_LIST) &&
                           object->len < map->len;
      if (map && map->is_catalog_list)
        debugPrintf("JNI CATALOG ITER.hasNext(%d/%d) -> %d [A]\n",
                    object->len, map->len, has_next);
      else
        debugPrintf("JNI Iterator.hasNext(%d/%d) -> %d [A]\n",
                    object->len, map ? map->len : 0, has_next);
      return (jvalue){.z = has_next};
    }
    if (!strcmp(m->name, "next")) {
      if (!map || object->len >= map->len) return r;
      jobject value = map->tag == T_LIST ? map->objs[object->len++] :
                      map->tag == T_MAP ? map->objs[object->len++ * 2] : NULL;
      FakeObj *item = value;
      if (map->is_catalog_list)
        debugPrintf("JNI CATALOG ITER.next() -> %s [A]\n",
                    item && item->tag == T_CATALOG_ITEM ? item->catalog_sku : "(object)");
      else
        debugPrintf("JNI Iterator.next() -> %s [A]\n",
                    jni_cstr(value) ? jni_cstr(value) :
                    (item && item->tag == T_CATALOG_ITEM ? item->catalog_sku : "(object)"));
      return (jvalue){.l = value};
    }
  }
  return pvz2_upcall_args(m->cls, m->name, m->sig, self, args);
}

// The A forms use the same dispatch, but receive arguments in a jvalue array.
#define CALL_FORMS(RT, FIELD, PREFIX)                                          \
  static RT jf_##PREFIX(JNIEnv e, jobject o, jmethodID m, ...) {                \
    va_list ap; va_start(ap, m); jvalue v = call_v(e, o, m, ap); va_end(ap);   \
    return v.FIELD;                                                            \
  }                                                                            \
  static RT jf_##PREFIX##V(JNIEnv e, jobject o, jmethodID m, va_list ap) {      \
    jvalue v = call_v(e, o, m, ap); return v.FIELD;                            \
  }

CALL_FORMS(jobject,  l, CallObjectMethod)
CALL_FORMS(jboolean, z, CallBooleanMethod)
CALL_FORMS(jint,     i, CallIntMethod)
CALL_FORMS(jlong,    j, CallLongMethod)
CALL_FORMS(jfloat,   f, CallFloatMethod)
static jobject jf_CallObjectMethodA(JNIEnv e, jobject o, jmethodID m,
                                    const jvalue *args) {
  return call_a(e, o, m, args).l;
}
static jboolean jf_CallBooleanMethodA(JNIEnv e, jobject o, jmethodID m,
                                      const jvalue *args) {
  return call_a(e, o, m, args).z;
}
static jint jf_CallIntMethodA(JNIEnv e, jobject o, jmethodID m,
                              const jvalue *args) {
  return call_a(e, o, m, args).i;
}
static jlong jf_CallLongMethodA(JNIEnv e, jobject o, jmethodID m,
                                const jvalue *args) {
  return call_a(e, o, m, args).j;
}
static jfloat jf_CallFloatMethodA(JNIEnv e, jobject o, jmethodID m,
                                  const jvalue *args) {
  return call_a(e, o, m, args).f;
}
static void jf_CallVoidMethodA(JNIEnv e, jobject o, jmethodID m,
                               const jvalue *args) {
  (void)call_a(e, o, m, args);
}
// void: dispatch but ignore the return
static void jf_CallVoidMethod(JNIEnv e, jobject o, jmethodID m, ...) {
  va_list ap; va_start(ap, m); call_v(e, o, m, ap); va_end(ap);
}
static void jf_CallVoidMethodV(JNIEnv e, jobject o, jmethodID m, va_list ap) {
  call_v(e, o, m, ap);
}
// static variants share the same dispatch (self is the class, ignored by fusion)
CALL_FORMS(jobject,  l, CallStaticObjectMethod)
CALL_FORMS(jboolean, z, CallStaticBooleanMethod)
CALL_FORMS(jint,     i, CallStaticIntMethod)
CALL_FORMS(jlong,    j, CallStaticLongMethod)
CALL_FORMS(jfloat,   f, CallStaticFloatMethod)
static void jf_CallStaticVoidMethod(JNIEnv e, jobject o, jmethodID m, ...) {
  va_list ap; va_start(ap, m); call_v(e, o, m, ap); va_end(ap);
}
static void jf_CallStaticVoidMethodV(JNIEnv e, jobject o, jmethodID m, va_list ap) {
  call_v(e, o, m, ap);
}

// ---------------------------------------------------------------------------
// catch-all for every slot we didn't wire (keeps stray calls non-fatal)
// ---------------------------------------------------------------------------
static jlong jni_catch_all(void) {
  static int warned;
  if (!warned) {
    warned = 1;
    debugPrintf("JNI catch-all invoked: at least one JNI slot is still unimplemented\n");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// JavaVM invoke interface
// ---------------------------------------------------------------------------
static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm; (void)version;
  if (penv) *penv = (void *)fake_env;
  return JNI_OK;
}
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args;
  if (penv) *penv = (void *)fake_env; // hand back the (two-level) env
  return JNI_OK;
}
static jint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static jint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }

static JNINativeInterface s_iface;
static JNIInvokeInterface s_invoke;
static const JNINativeInterface *s_env_ptr;
static const JNIInvokeInterface *s_vm_ptr;

void jni_init(void) {
  map_self_check();
  // fill every slot with the catch-all first, then override the real ones
  for (int i = 0; i < JNI_SLOT_COUNT; i++)
    s_iface.fn[i] = (void *)jni_catch_all;

  #define SET(name, func) s_iface.fn[J_##name] = (void *)(func)
  SET(GetVersion,               jf_GetVersion);
  SET(FindClass,                jf_FindClass);
  SET(GetMethodID,              jf_GetMethodID);
  SET(GetStaticMethodID,        jf_GetStaticMethodID);
  SET(GetFieldID,               jf_GetFieldID);
  SET(GetStaticFieldID,         jf_GetStaticFieldID);
  SET(GetStaticObjectField,     jf_GetStaticObjectField);
  SET(GetStaticIntField,        jf_GetStaticIntField);

  SET(NewStringUTF,             jf_NewStringUTF);
  SET(GetStringUTFChars,        jf_GetStringUTFChars);
  SET(ReleaseStringUTFChars,    jf_ReleaseStringUTFChars);
  SET(GetStringUTFLength,       jf_GetStringUTFLength);
  SET(GetStringLength,          jf_GetStringLength);
  SET(NewString,                jf_NewString);
  SET(GetStringChars,           jf_GetStringChars);
  SET(ReleaseStringChars,       jf_ReleaseStringChars);

  SET(GetArrayLength,           jf_GetArrayLength);
  SET(NewByteArray,             jf_NewByteArray);
  SET(NewIntArray,              jf_NewIntArray);
  SET(GetByteArrayElements,     jf_GetByteArrayElements);
  SET(SetByteArrayRegion,       jf_SetByteArrayRegion);
  SET(ReleaseByteArrayElements, jf_ReleaseByteArrayElements);
  SET(GetPrimitiveArrayCritical, jf_GetPrimitiveArrayCritical);
  SET(ReleasePrimitiveArrayCritical, jf_ReleasePrimitiveArrayCritical);
  SET(NewObjectArray,           jf_NewObjectArray);
  SET(GetObjectArrayElement,    jf_GetObjectArrayElement);
  SET(SetObjectArrayElement,    jf_SetObjectArrayElement);
  SET(NewDirectByteBuffer,      jf_NewDirectByteBuffer);
  SET(GetDirectBufferAddress,   jf_GetDirectBufferAddress);
  SET(GetDirectBufferCapacity,  jf_GetDirectBufferCapacity);

  SET(GetObjectClass,           jf_GetObjectClass);
  SET(IsInstanceOf,             jf_IsInstanceOf);
  SET(IsSameObject,             jf_IsSameObject);
  SET(NewGlobalRef,             jf_NewRef);
  SET(NewLocalRef,              jf_NewRef);
  SET(NewWeakGlobalRef,         jf_NewRef);
  SET(DeleteGlobalRef,          jf_DeleteRef);
  SET(DeleteLocalRef,           jf_DeleteRef);
  SET(DeleteWeakGlobalRef,      jf_DeleteRef);
  SET(EnsureLocalCapacity,      jf_EnsureLocalCapacity);
  SET(PushLocalFrame,           jf_PushLocalFrame);
  SET(PopLocalFrame,            jf_PopLocalFrame);
  SET(AllocObject,              jf_AllocObject);
  SET(NewObject,                jf_NewObject);
  SET(NewObjectV,               jf_NewObjectV);
  SET(NewObjectA,               jf_NewObjectA);
  SET(ExceptionOccurred,        jf_ExceptionOccurred);
  SET(ExceptionClear,           jf_ExceptionClear);
  SET(ExceptionDescribe,        jf_ExceptionDescribe);
  SET(ExceptionCheck,           jf_ExceptionCheck);
  SET(MonitorEnter,             jf_MonitorOp);
  SET(MonitorExit,              jf_MonitorOp);
  SET(GetJavaVM,                jf_GetJavaVM);
  SET(RegisterNatives,          jf_RegisterNatives);
  SET(UnregisterNatives,        jf_UnregisterNatives);

  SET(CallObjectMethod,   jf_CallObjectMethod);   SET(CallObjectMethodV, jf_CallObjectMethodV);
  SET(CallObjectMethodA,  jf_CallObjectMethodA);
  SET(CallBooleanMethod,  jf_CallBooleanMethod);  SET(CallBooleanMethodV, jf_CallBooleanMethodV);
  SET(CallBooleanMethodA, jf_CallBooleanMethodA);
  SET(CallIntMethod,      jf_CallIntMethod);      SET(CallIntMethodV, jf_CallIntMethodV);
  SET(CallIntMethodA,     jf_CallIntMethodA);
  SET(CallLongMethod,     jf_CallLongMethod);     SET(CallLongMethodV, jf_CallLongMethodV);
  SET(CallLongMethodA,    jf_CallLongMethodA);
  SET(CallFloatMethod,    jf_CallFloatMethod);    SET(CallFloatMethodV, jf_CallFloatMethodV);
  SET(CallFloatMethodA,   jf_CallFloatMethodA);
  SET(CallVoidMethod,     jf_CallVoidMethod);     SET(CallVoidMethodV, jf_CallVoidMethodV);
  SET(CallVoidMethodA,    jf_CallVoidMethodA);
  SET(CallStaticObjectMethod,  jf_CallStaticObjectMethod);  SET(CallStaticObjectMethodV, jf_CallStaticObjectMethodV);
  SET(CallStaticObjectMethodA, jf_CallObjectMethodA);
  SET(CallStaticBooleanMethod, jf_CallStaticBooleanMethod); SET(CallStaticBooleanMethodV, jf_CallStaticBooleanMethodV);
  SET(CallStaticBooleanMethodA, jf_CallBooleanMethodA);
  SET(CallStaticIntMethod,     jf_CallStaticIntMethod);     SET(CallStaticIntMethodV, jf_CallStaticIntMethodV);
  SET(CallStaticIntMethodA,    jf_CallIntMethodA);
  SET(CallStaticLongMethod,    jf_CallStaticLongMethod);    SET(CallStaticLongMethodV, jf_CallStaticLongMethodV);
  SET(CallStaticLongMethodA,   jf_CallLongMethodA);
  SET(CallStaticFloatMethod,   jf_CallStaticFloatMethod);   SET(CallStaticFloatMethodV, jf_CallStaticFloatMethodV);
  SET(CallStaticFloatMethodA,  jf_CallFloatMethodA);
  SET(CallStaticVoidMethod,    jf_CallStaticVoidMethod);    SET(CallStaticVoidMethodV, jf_CallStaticVoidMethodV);
  SET(CallStaticVoidMethodA,   jf_CallVoidMethodA);

  SET(GetIntArrayElements,     jf_GetIntArrayElements);
  SET(ReleaseIntArrayElements, jf_ReleaseIntArrayElements);
  #undef SET

  s_env_ptr = &s_iface;
  fake_env  = (JNIEnv)&s_env_ptr;   // env points at the table pointer (two-level)

  s_invoke.reserved0 = s_invoke.reserved1 = s_invoke.reserved2 = NULL;
  s_invoke.DestroyJavaVM = vm_DestroyJavaVM;
  s_invoke.AttachCurrentThread = vm_AttachCurrentThread;
  s_invoke.DetachCurrentThread = vm_DetachCurrentThread;
  s_invoke.GetEnv = vm_GetEnv;
  s_invoke.AttachCurrentThreadAsDaemon = vm_AttachCurrentThread;
  s_vm_ptr = &s_invoke;
  fake_vm  = (JavaVM)&s_vm_ptr;   // vm points at the invoke-table pointer (two-level)

  debugPrintf("jni_init: env=%p vm=%p slots=%d\n", (void*)fake_env, (void*)fake_vm, JNI_SLOT_COUNT);
}

// ---------------------------------------------------------------------------
// helpers used by main.c / audio.c
// ---------------------------------------------------------------------------

void *jni_make_thiz(void) {
  // a generic non-NULL object to stand in for the Activity/Renderer instance
  return obj_new(T_GENERIC);
}

jstring jni_make_string(const char *utf) { return jf_NewStringUTF(fake_env, utf); }

/* Read the C string out of a fake jstring (a T_STRING FakeObj). Returns NULL if
 * the handle isn't one of our strings. Used by the music path to get the file
 * name the engine passes to loadMusic(). */
const char *jni_cstr(jobject s) {
  FakeObj *o = s;
  return (o && o->tag == T_STRING) ? o->str : NULL;
}

jobjectArray jni_make_string_array(const char *const *values, int count) {
  if (count < 0) return NULL;
  FakeObj *array = obj_new(T_OBJARRAY);
  if (!array) return NULL;
  array->len = count;
  array->objs = calloc(count > 0 ? (size_t)count : 1, sizeof(*array->objs));
  if (!array->objs) { free(array); return NULL; }
  for (int i = 0; i < count; i++) array->objs[i] = jni_make_string(values[i]);
  return array;
}

int jni_object_array_length(jobject array) {
  FakeObj *o = array;
  return o && o->tag == T_OBJARRAY ? o->len : 0;
}

int jni_list_length(jobject list) {
  FakeObj *o = list;
  return o && o->tag == T_LIST ? o->len : -1;
}

jobject jni_object_array_get(jobject array, int index) {
  FakeObj *o = array;
  return o && o->tag == T_OBJARRAY && index >= 0 && index < o->len ?
      o->objs[index] : NULL;
}

// Distinct non-null object tokens handed to the engine at nativeLoad /
// nativeSurfaceCreated. Their identity doesn't matter to up-call dispatch
// (methodID carries class+name+sig), and the shims that consume them
// (AAssetManager_fromJava, ANativeWindow_fromSurface) ignore the object and
// return fixed emulated handles -- confirmed by disassembly of those entry
// points. We still hand back separate objects so the engine's own
// IsSameObject/global-ref bookkeeping stays consistent.
jobject jni_make_object(void)   { return obj_for_class("java/lang/Object"); }
jobject jni_make_object_class(const char *class_name) {
  FakeObj *c = find_or_make_class(class_name ? class_name : "java/lang/Object");
  return obj_for_class(c->cls);
}
jobject jni_make_object_class_value(const char *class_name, jlong value) {
  jobject object = jni_make_object_class(class_name);
  if (object) ((FakeObj *)object)->value = value;
  return object;
}
jobject jni_make_object_class_string(const char *class_name, const char *value) {
  jobject object = jni_make_object_class(class_name);
  if (object) ((FakeObj *)object)->str = fake_strdup(value);
  return object;
}
const char *jni_object_string(jobject object) {
  FakeObj *fake = object;
  return fake ? fake->str : NULL;
}
jobject jni_make_activity(void) { return obj_for_class("com/popcap/SexyAppFramework/SexyAppFrameworkActivity"); }
jobject jni_make_surface(void)  { return obj_for_class("com/popcap/SexyAppFramework/AndroidSurfaceView"); }

jobject jni_make_catalog_list(const JniCatalogItem *items, int count) {
  if (count < 0) return NULL;
  FakeObj *list = obj_new(T_LIST);
  if (!list) return NULL;
  list->cls = "java/util/ArrayList";
  list->is_catalog_list = 1;
  list->len = count;
  list->objs = calloc(count > 0 ? (size_t)count : 1, sizeof(*list->objs));
  if (!list->objs) { free(list); return NULL; }
  for (int i = 0; i < count; ++i) {
    const JniCatalogItem *source = &items[i];
    FakeObj *item = obj_new(T_CATALOG_ITEM);
    if (!item) continue;
    item->cls = "com/ea/nimble/mtx/NimbleCatalogItem";
    item->catalog_sku = fake_strdup(source->sku);
    item->catalog_sell_id = fake_strdup(source->sell_id);
    item->catalog_title = fake_strdup(source->title);
    item->catalog_description = fake_strdup(source->description);
    item->catalog_formatted_price = fake_strdup(source->formatted_price);
    item->catalog_metadata_url = fake_strdup(source->metadata_url);
    item->catalog_price = source->price;
    item->catalog_item_type = source->item_type;
    item->catalog_is_free = source->is_free;
    list->objs[i] = item;
  }
  return list;
}

jclass jni_find_class_c(const char *name) { return find_or_make_class(name); }

jbyteArray jni_wrap_bytearray(void *data, int len_bytes) {
  FakeObj *o = obj_new(T_BYTEARRAY);
  o->data = data; o->len = len_bytes; o->owns_data = 0; // external buffer
  return o;
}

jbyteArray jni_make_bytearray_copy(const void *data, int len_bytes) {
  if (len_bytes < 0) return NULL;
  FakeObj *o = obj_new(T_BYTEARRAY);
  if (!o) return NULL;
  o->len = len_bytes;
  o->data = malloc(len_bytes > 0 ? (size_t)len_bytes : 1);
  if (!o->data) { free(o); return NULL; }
  if (data && len_bytes) memcpy(o->data, data, (size_t)len_bytes);
  o->owns_data = 1;
  return o;
}

void jni_free_wrapper(jobject o) {
  FakeObj *f = o;
  if (!f) return;
  if (f->owns_data) free(f->data);
  free(f->str);
  free(f->http_method);
  free(f->http_headers);
  free(f->http_response_headers);
  free(f->http_response_header_value);
  free(f->http_body);
  free(f->catalog_sku);
  free(f->catalog_sell_id);
  free(f->catalog_title);
  free(f->catalog_description);
  free(f->catalog_formatted_price);
  free(f->catalog_metadata_url);
  free(f->objs);
  free(f);
}

static FakeObj *http_obj(jobject o) {
  FakeObj *f = o;
  return f && f->cls && strstr(f->cls, "AndroidHttpTransaction") ? f : NULL;
}

const char *jni_http_url(jobject o) {
  FakeObj *f = http_obj(o); return f ? f->str : NULL;
}
const char *jni_http_method(jobject o) {
  FakeObj *f = http_obj(o); return f && f->http_method ? f->http_method : "GET";
}
const char *jni_http_headers(jobject o) {
  FakeObj *f = http_obj(o); return f ? f->http_headers : NULL;
}
const void *jni_http_body(jobject o, int *len) {
  FakeObj *f = http_obj(o);
  if (len) *len = f ? f->http_body_len : 0;
  return f ? f->http_body : NULL;
}
void jni_http_set_header(jobject o, const char *name, const char *value) {
  FakeObj *f = http_obj(o);
  if (!f || !name || !value) return;
  if (!strcasecmp(name, "Content-Type"))
    debugPrintf("JNI HTTP: request header Content-Type=%s\n", value);
  else
    debugPrintf("JNI HTTP: request header %s\n", name);
  const size_t old = f->http_headers ? strlen(f->http_headers) : 0;
  const size_t add = strlen(name) + strlen(value) + 3;
  char *p = realloc(f->http_headers, old + add + 1);
  if (!p) return;
  f->http_headers = p;
  snprintf(p + old, add + 1, "%s: %s\n", name, value);
}
void jni_http_set_body(jobject o, jobject body) {
  FakeObj *f = http_obj(o), *b = body;
  if (!f || !b || b->tag != T_BYTEARRAY) return;
  unsigned char *copy = malloc(b->len > 0 ? (size_t)b->len : 1);
  if (!copy) return;
  if (b->len) memcpy(copy, b->data, (size_t)b->len);
  free(f->http_body); f->http_body = copy; f->http_body_len = b->len;
}
void jni_http_set_timeout(jobject o, int timeout_ms) {
  FakeObj *f = http_obj(o);
  if (!f || timeout_ms <= 0) return;
  f->http_timeout_ms = timeout_ms < 1000 ? timeout_ms * 1000 : timeout_ms;
  debugPrintf("JNI HTTP: timeout=%dms\n", f->http_timeout_ms);
}
int jni_http_timeout(jobject o) {
  FakeObj *f = http_obj(o);
  return f && f->http_timeout_ms > 0 ? f->http_timeout_ms : 5000;
}
void jni_http_set_status(jobject o, int status) {
  FakeObj *f = http_obj(o); if (f) f->http_status = status;
}
int jni_http_status(jobject o) {
  FakeObj *f = http_obj(o); return f ? f->http_status : 0;
}
int jni_http_take_cleanup(jobject o) {
  FakeObj *f = http_obj(o);
  if (!f || f->http_cleanup_taken) return 0;
  f->http_cleanup_taken = 1;
  return 1;
}
void jni_http_set_response_headers(jobject o, const void *data, size_t len) {
  FakeObj *f = http_obj(o);
  if (!f) return;
  free(f->http_response_headers);
  f->http_response_headers = NULL;
  free(f->http_response_header_value);
  f->http_response_header_value = NULL;
  if (!data || !len) return;
  f->http_response_headers = malloc(len + 1);
  if (!f->http_response_headers) return;
  memcpy(f->http_response_headers, data, len);
  f->http_response_headers[len] = 0;
}
const char *jni_http_get_response_header(jobject o, const char *name) {
  FakeObj *f = http_obj(o);
  if (!f || !name || !*name || !f->http_response_headers) return "";
  free(f->http_response_header_value);
  f->http_response_header_value = NULL;
  const size_t wanted = strlen(name);
  const char *line = f->http_response_headers;
  while (*line) {
    const char *end = strchr(line, '\n');
    const size_t line_len = end ? (size_t)(end - line) : strlen(line);
    const char *colon = memchr(line, ':', line_len);
    if (colon && (size_t)(colon - line) == wanted &&
        !strncasecmp(line, name, wanted)) {
      const char *value = colon + 1;
      const char *value_end = line + line_len;
      while (value < value_end && (*value == ' ' || *value == '\t')) value++;
      while (value_end > value &&
             (value_end[-1] == '\r' || value_end[-1] == ' ' || value_end[-1] == '\t'))
        value_end--;
      const size_t value_len = (size_t)(value_end - value);
      f->http_response_header_value = malloc(value_len + 1);
      if (!f->http_response_header_value) return "";
      memcpy(f->http_response_header_value, value, value_len);
      f->http_response_header_value[value_len] = 0;
    }
    if (!end) break;
    line = end + 1;
  }
  return f->http_response_header_value ? f->http_response_header_value : "";
}

int *jni_intarray_data(jobject o, int *len) {
  FakeObj *f = o;
  if (!f || f->tag != T_INTARRAY) return NULL;
  if (len) *len = f->len;
  return f->data;
}
