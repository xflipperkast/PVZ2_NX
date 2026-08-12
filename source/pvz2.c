/* Minimal Java shell for PopCap's SexyAppFramework. Android-only services stay
 * local, while the HTTP transaction used by the CDN/config services is real. */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <switch.h>
#include <switch/applets/web.h>
#include <curl/curl.h>
#include <mbedtls/md5.h>

#include <sys/statvfs.h>

#include "config.h"
#include "editbox.h"
#include "jni_fake.h"
#include "libc_shim.h"
#include "platform.h"
#include "pvz2.h"
#include "util.h"

#ifndef PVZ2_ENABLE_TOUCH_TRACE
#define PVZ2_ENABLE_TOUCH_TRACE 0
#endif

extern int screen_width;
extern int screen_height;
extern void watchdog_set_suspended(int suspended);

/* AndroidSurfaceView keeps a mutable GL-view scale. Native PVZ2 queries
 * CanSet, computes the fit factor during surface resize, calls Set, and then
 * reads the same value back for presentation and input conversion. */
static float s_graphics_view_scale_factor;

/* --- Switch Platform Data Helpers ---------------------------------------- */

typedef struct {
  const char *locale;   /* e.g. "en_US" */
  const char *language; /* e.g. "en" */
  const char *country;  /* e.g. "US" */
} SwitchLocaleInfo;

static SwitchLocaleInfo get_switch_locale(void) {
  static SwitchLocaleInfo cached;
  static int initialized = 0;
  if (initialized) return cached;
  initialized = 1;

  cached.locale   = "en_US";
  cached.language = "en";
  cached.country  = "US";

  u64 lang_code = 0;
  SetLanguage set_lang = SetLanguage_ENUS;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&lang_code))) {
      if (R_SUCCEEDED(setMakeLanguage(lang_code, &set_lang))) {
        switch (set_lang) {
          case SetLanguage_JA:
            cached.locale = "ja_JP"; cached.language = "ja"; cached.country = "JP"; break;
          case SetLanguage_ENUS:
            cached.locale = "en_US"; cached.language = "en"; cached.country = "US"; break;
          case SetLanguage_FR:
            cached.locale = "fr_FR"; cached.language = "fr"; cached.country = "FR"; break;
          case SetLanguage_DE:
            cached.locale = "de_DE"; cached.language = "de"; cached.country = "DE"; break;
          case SetLanguage_IT:
            cached.locale = "it_IT"; cached.language = "it"; cached.country = "IT"; break;
          case SetLanguage_ES:
            cached.locale = "es_ES"; cached.language = "es"; cached.country = "ES"; break;
          case SetLanguage_ZHCN:
          case SetLanguage_ZHHANS:
            cached.locale = "zh_CN"; cached.language = "zh"; cached.country = "CN"; break;
          case SetLanguage_KO:
            cached.locale = "ko_KR"; cached.language = "ko"; cached.country = "KR"; break;
          case SetLanguage_NL:
            cached.locale = "nl_NL"; cached.language = "nl"; cached.country = "NL"; break;
          case SetLanguage_PT:
            cached.locale = "pt_PT"; cached.language = "pt"; cached.country = "PT"; break;
          case SetLanguage_RU:
            cached.locale = "ru_RU"; cached.language = "ru"; cached.country = "RU"; break;
          case SetLanguage_ZHTW:
          case SetLanguage_ZHHANT:
            cached.locale = "zh_TW"; cached.language = "zh"; cached.country = "TW"; break;
          case SetLanguage_ENGB:
            cached.locale = "en_GB"; cached.language = "en"; cached.country = "GB"; break;
          case SetLanguage_FRCA:
            cached.locale = "fr_CA"; cached.language = "fr"; cached.country = "CA"; break;
          case SetLanguage_ES419:
            cached.locale = "es_419"; cached.language = "es"; cached.country = "MX"; break;
          case SetLanguage_PTBR:
            cached.locale = "pt_BR"; cached.language = "pt"; cached.country = "BR"; break;
          default:
            break;
        }
      }
    }
    setExit();
  }
  debugPrintf("Switch system locale: %s (lang=%s, country=%s)\n",
              cached.locale, cached.language, cached.country);
  return cached;
}

typedef struct {
  u32 percent;     /* 0..100 */
  int is_charging; /* 0 or 1 */
  int available;   /* 1 if query succeeded */
} SwitchBatteryInfo;

static SwitchBatteryInfo get_switch_battery(void) {
  SwitchBatteryInfo info = { 100, 0, 0 };
  if (R_SUCCEEDED(psmInitialize())) {
    u32 pct = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) {
      info.percent = pct > 100 ? 100 : pct;
      info.available = 1;
    }
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetChargerType(&charger))) {
      info.is_charging = (charger != PsmChargerType_Unconnected);
    }
    psmExit();
  }
  return info;
}

typedef struct {
  u64 total_ram;
  u64 used_ram;
  u64 avail_ram;
} SwitchMemoryInfo;

static SwitchMemoryInfo get_switch_memory_info(void) {
  SwitchMemoryInfo mem = {
    .total_ram = 3ULL * 1024 * 1024 * 1024,
    .used_ram  = 512ULL * 1024 * 1024,
    .avail_ram = 2ULL * 1024 * 1024 * 1024,
  };

  u64 total = 0, used = 0;
  Result r1 = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  Result r2 = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  if (R_SUCCEEDED(r1) && total > 0) {
    mem.total_ram = total;
  }
  if (R_SUCCEEDED(r2)) {
    mem.used_ram = used;
  }
  if (mem.total_ram > mem.used_ram) {
    mem.avail_ram = mem.total_ram - mem.used_ram;
  } else {
    mem.avail_ram = 0;
  }
  return mem;
}

typedef struct {
  u64 block_size;
  u64 total_blocks;
  u64 free_blocks;
  u64 total_bytes;
  u64 free_bytes;
  int available;
} SwitchStorageInfo;

static SwitchStorageInfo get_switch_storage_info(void) {
  SwitchStorageInfo st = {
    .block_size = 4096,
    .total_blocks = 512 * 1024,
    .free_blocks = 512 * 1024,
    .total_bytes = 2ULL * 1024 * 1024 * 1024,
    .free_bytes = 2ULL * 1024 * 1024 * 1024,
    .available = 0,
  };

  struct statvfs vfs;
  const char *paths[] = { DATA_DIR, "sdmc:/", "/", NULL };
  for (int i = 0; paths[i]; i++) {
    if (statvfs(paths[i], &vfs) == 0 && vfs.f_blocks > 0) {
      st.block_size   = vfs.f_frsize ? (u64)vfs.f_frsize : (u64)vfs.f_bsize;
      st.total_blocks = (u64)vfs.f_blocks;
      st.free_blocks  = (u64)vfs.f_bavail;
      st.total_bytes  = st.total_blocks * st.block_size;
      st.free_bytes   = st.free_blocks * st.block_size;
      st.available    = 1;
      break;
    }
  }
  return st;
}

typedef struct {
  int is_connected;
  int android_net_type; /* 1 = WiFi, 9 = Ethernet, -1 = None */
  const char *type_name; /* "WIFI", "ETHERNET", "NONE" */
  int nimble_ordinal; /* 0 = Connected, 1 = Offline */
} SwitchNetworkInfo;

static SwitchNetworkInfo get_switch_network_info(void) {
  static int initialized = 0;
  static Result init_result;
  if (!initialized) {
    init_result = nifmInitialize(NifmServiceType_User);
    initialized = 1;
  }

  SwitchNetworkInfo net = {
    .is_connected = 0,
    .android_net_type = -1,
    .type_name = "NONE",
    .nimble_ordinal = 1,
  };

  if (R_SUCCEEDED(init_result)) {
    NifmInternetConnectionType conn_type;
    NifmInternetConnectionStatus conn_status;
    u32 wifi_strength = 0;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&conn_type, &wifi_strength, &conn_status))) {
      if (conn_status == NifmInternetConnectionStatus_Connected) {
        net.is_connected = 1;
        net.nimble_ordinal = 0;
        if (conn_type == NifmInternetConnectionType_Ethernet) {
          net.android_net_type = 9; /* TYPE_ETHERNET */
          net.type_name = "ETHERNET";
        } else {
          net.android_net_type = 1; /* TYPE_WIFI */
          net.type_name = "WIFI";
        }
      }
    }
  }
  return net;
}

int pvz2_current_network_status(void) {
  return get_switch_network_info().android_net_type;
}

static int nimble_network_status_ordinal(void) {
  return get_switch_network_info().nimble_ordinal;
}

static const char *device_uuid(void) {
  static char uuid[37];
  if (uuid[0]) return uuid;

  FILE *file = fopen(DATA_DIR "/No_Backup/device_uuid", "rb");
  if (file) {
    const size_t size = fread(uuid, 1, 36, file);
    fclose(file);
    if (size == 36) {
      uuid[36] = 0;
      debugPrintf("JNI UUID: restored\n");
      return uuid;
    }
  }

  unsigned char bytes[16];
  if (R_FAILED(csrngGetRandomBytes(bytes, sizeof(bytes)))) {
    static const unsigned char fallback[16] =
        {0x53, 0x77, 0x69, 0x74, 0x63, 0x68, 0x50, 0x76,
         0x5a, 0x32, 0x4c, 0x6f, 0x63, 0x61, 0x6c, 0x31};
    memcpy(bytes, fallback, sizeof(bytes));
  }
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  snprintf(uuid, sizeof(uuid),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           (unsigned)bytes[0], (unsigned)bytes[1], (unsigned)bytes[2], (unsigned)bytes[3],
           (unsigned)bytes[4], (unsigned)bytes[5], (unsigned)bytes[6], (unsigned)bytes[7],
           (unsigned)bytes[8], (unsigned)bytes[9], (unsigned)bytes[10], (unsigned)bytes[11],
           (unsigned)bytes[12], (unsigned)bytes[13], (unsigned)bytes[14], (unsigned)bytes[15]);
  file = fopen(DATA_DIR "/No_Backup/device_uuid", "wb");
  if (file) {
    fwrite(uuid, 1, 36, file);
    fclose(file);
  }
  debugPrintf("JNI UUID: created\n");
  return uuid;
}

const char *pvz2_device_uuid(void) {
  return device_uuid();
}

static const char *game_version(void) {
  return GAME_VERSION;
}

static int game_version_code(void) {
  const char *number = OBB_NAME;
  if (!strncmp(number, "main.", 5)) number += 5;
  return (int)strtol(number, NULL, 10);
}

static jboolean remove_private_data(jobject argument) {
  const char *name = jni_cstr(argument);
  char path[768];
  if (!name || !*name || strstr(name, "..") || strchr(name, ':')) return JNI_FALSE;
  if (name[0] == '/') snprintf(path, sizeof(path), DATA_DIR "%s", name);
  else snprintf(path, sizeof(path), DATA_DIR "/%s", name);

  unsigned files_removed = 0;
  unsigned dirs_removed = 0;
  const int rc = remove_private_tree_fake(path, &files_removed, &dirs_removed);
  debugPrintf("JNI removePrivateData: %s -> %s (files=%u dirs=%u)\n",
              name, rc == 0 ? "cleared" : "FAILED",
              files_removed, dirs_removed);
  return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

static int parse_args(const char *sig, va_list ap, jvalue *argv, int cap) {
  const char *p = sig;
  if (*p == '(') p++;
  int n = 0;
  while (*p && *p != ')' && n < cap) {
    switch (*p) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        argv[n].i = va_arg(ap, int); p++; break;
      case 'J': argv[n].j = va_arg(ap, long long); p++; break;
      case 'F': argv[n].f = (float)va_arg(ap, double); p++; break;
      case 'D': argv[n].d = va_arg(ap, double); p++; break;
      case 'L':
        argv[n].l = va_arg(ap, void *);
        while (*p && *p != ';') p++;
        if (*p) p++;
        break;
      case '[':
        argv[n].l = va_arg(ap, void *); p++;
        if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; }
        else if (*p) p++;
        break;
      default: p++; continue;
    }
    n++;
  }
  return n;
}

static jvalue typed_default(const char *sig) {
  jvalue r; r.j = 0;
  const char *ret = strrchr(sig, ')');
  const char type = ret ? ret[1] : 'V';
  if (type == 'L') {
    if (!strncmp(ret + 1, "Ljava/lang/String;", 18)) {
      r.l = jni_make_string("");
    } else {
      const char *class_name = ret + 2; /* skip ')L' in a JNI object return */
      const char *end = strchr(class_name, ';');
      char cls[128] = "java/lang/Object";
      if (end && (size_t)(end - class_name) < sizeof(cls)) {
        memcpy(cls, class_name, (size_t)(end - class_name));
        cls[end - class_name] = 0;
      }
      r.l = jni_make_object_class(cls);
    }
  } else if (type == '[') {
    r.l = jni_make_object();
  }
  return r;
}

static int has(const char *s, const char *part) { return strstr(s, part) != NULL; }

static void md5_direct_buffers(jobject input, jobject output) {
  int input_length = 0, output_length = 0;
  const unsigned char *source = jni_buffer_data(input, &input_length);
  unsigned char *digest = jni_buffer_data(output, &output_length);
  if (!source || input_length < 0 || !digest || output_length < 16) {
    debugPrintf("JNI md5: invalid buffers (input=%d output=%d)\n",
                input_length, output_length);
    return;
  }
  const int rc = mbedtls_md5_ret(source, (size_t)input_length, digest);
  debugPrintf("JNI md5: %d bytes -> %s\n", input_length,
              rc == 0 ? "ok" : "failed");
}

static void put_bytes(unsigned char *dst, const void *value, size_t size) {
  memcpy(dst, value, size);
}

typedef struct {
  const char *callback;
  jobject self;
  jlong argument;
  int boolean_argument;
  const void *data;
  int data_length;
  int status;
} PendingJavaCallback;

#define MAX_PENDING_JAVA_CALLBACKS 64
static PendingJavaCallback pending_callbacks[MAX_PENDING_JAVA_CALLBACKS];
static int pending_callback_count;
/* HTTP and service workers enqueue while the render thread drains.  A static
 * libnx Mutex is valid in the zero/unlocked state. */
static Mutex pending_callback_mutex;

static void queue_java_callback(const char *callback, jobject self,
                                jlong argument);

/* Glu's AndroidPlatform.scheduleEvent is the Java-side equivalent of a
 * Handler/postDelayed timer. Queueing onTimerEvent immediately collapses every
 * timeout/delay to one frame and causes
 * consent/config requests to enter their timeout path before their normal
 * asynchronous responses were consumed. */
typedef struct {
  jobject self;
  jlong callback;
  u64 due_tick;
  double delay_ms;
  int active;
} ScheduledJavaTimer;

#define MAX_SCHEDULED_JAVA_TIMERS 32
static ScheduledJavaTimer scheduled_java_timers[MAX_SCHEDULED_JAVA_TIMERS];
static Mutex scheduled_java_timer_mutex;

static u64 timer_delay_to_ticks(const char *sig, const jvalue *argv,
                                double *delay_ms_out) {
  double delay_ms = 0.0;
  if (sig && sig[0] == '(') {
    switch (sig[1]) {
      case 'D': delay_ms = argv[0].d * 1000.0; break;
      case 'F': delay_ms = (double)argv[0].f * 1000.0; break;
      case 'I': delay_ms = argv[0].i; break;
      case 'J': delay_ms = (double)argv[0].j; break;
      default:  delay_ms = 0.0; break;
    }
  } else {
    delay_ms = (double)argv[0].j;
  }

  /* Integer/long Android Handler-style delays are milliseconds; floating
   * duration APIs conventionally use seconds. Reject nonsense rather than
   * overflowing the Horizon system tick. Zero remains a next-pump event. */
  if (delay_ms < 0.0) delay_ms = 0.0;
  if (delay_ms > 86400000.0) delay_ms = 86400000.0;
  if (delay_ms_out) *delay_ms_out = delay_ms;

  const u64 hz = armGetSystemTickFreq();
  const double ticks = delay_ms * (double)hz / 1000.0;
  return ticks > (double)UINT64_MAX ? UINT64_MAX : (u64)ticks;
}

static int schedule_java_timer(jobject self, jlong callback, const char *sig,
                               const jvalue *argv) {
  double delay_ms = 0.0;
  const u64 delay_ticks = timer_delay_to_ticks(sig, argv, &delay_ms);
  const u64 due = armGetSystemTick() + delay_ticks;
  int slot = -1;

  mutexLock(&scheduled_java_timer_mutex);
  for (int i = 0; i < MAX_SCHEDULED_JAVA_TIMERS; ++i) {
    if (!scheduled_java_timers[i].active) { slot = i; break; }
  }
  if (slot >= 0) {
    scheduled_java_timers[slot] = (ScheduledJavaTimer){
        .self = self, .callback = callback, .due_tick = due,
        .delay_ms = delay_ms, .active = 1};
  }
  mutexUnlock(&scheduled_java_timer_mutex);

  if (slot < 0) {
    debugPrintf("JNI timer queue full: callback=%lld delay=%.3fms sig=%s\n",
                (long long)callback, delay_ms, sig ? sig : "?");
    return 0;
  }

  return 1;
}

void pvz2_release_startup_wait_timer(void) {
  static int attempted;
  if (attempted) return;
  attempted = 1;

  const u64 now = armGetSystemTick();
  int selected = -1;
  u64 earliest_due = UINT64_MAX;

  mutexLock(&scheduled_java_timer_mutex);
  for (int i = 0; i < MAX_SCHEDULED_JAVA_TIMERS; ++i) {
    const ScheduledJavaTimer *timer = &scheduled_java_timers[i];
    if (!timer->active || timer->delay_ms < 9000.0 ||
        timer->delay_ms > 11000.0 || (s64)(timer->due_tick - now) <= 0)
      continue;
    if (timer->due_tick < earliest_due) {
      earliest_due = timer->due_tick;
      selected = i;
    }
  }
  if (selected >= 0) {
    ScheduledJavaTimer *timer = &scheduled_java_timers[selected];
    timer->due_tick = now;
  }
  mutexUnlock(&scheduled_java_timer_mutex);
}

static void pump_java_timers(void) {
  ScheduledJavaTimer due[MAX_SCHEDULED_JAVA_TIMERS];
  int due_count = 0;
  const u64 now = armGetSystemTick();

  mutexLock(&scheduled_java_timer_mutex);
  for (int i = 0; i < MAX_SCHEDULED_JAVA_TIMERS; ++i) {
    ScheduledJavaTimer *timer = &scheduled_java_timers[i];
    if (!timer->active) continue;
    /* Signed subtraction handles ordinary tick wrap safely. */
    if ((s64)(now - timer->due_tick) < 0) continue;
    due[due_count++] = *timer;
    *timer = (ScheduledJavaTimer){0};
  }
  mutexUnlock(&scheduled_java_timer_mutex);

  for (int i = 0; i < due_count; ++i) {
    queue_java_callback("onTimerEvent", due[i].self, due[i].callback);
  }
}

static int enqueue_pending_callback(PendingJavaCallback callback) {
  int queued = 0;
  mutexLock(&pending_callback_mutex);
  if (pending_callback_count < MAX_PENDING_JAVA_CALLBACKS) {
    pending_callbacks[pending_callback_count++] = callback;
    queued = 1;
  }
  mutexUnlock(&pending_callback_mutex);
  if (!queued)
    debugPrintf("JNI callback queue full: %s\n", callback.callback ? callback.callback : "?");
  return queued;
}

static void release_pending_callback_payload(PendingJavaCallback *callback) {
  if (!callback || !callback->data) return;
  free((void *)callback->data);
  callback->data = NULL;
}

static volatile int catalog_refresh_requested;
static volatile int catalog_http_start_pending;
static volatile int catalog_http_in_flight;
static volatile int catalog_refresh_ready_flag;
static volatile int catalog_refresh_failed_flag;
static jobject catalog_items;
static jobject catalog_empty_items;
static jobject catalog_local_bootstrap_items;
static char synergy_product_url[1024];
static char director_sell_id[64];
static char director_product_id[64];

static void publish_empty_catalog_result(const char *reason) {
  if (!catalog_empty_items)
    catalog_empty_items = jni_make_catalog_list(NULL, 0);
  catalog_items = catalog_empty_items;
  catalog_refresh_ready_flag = 1;
  catalog_refresh_failed_flag = 0;
  (void)reason;
}

/* PvZ2 13.3.1 does not finish its startup path after a successful refresh that
 * contains zero products. The title identifies the service but returns no
 * synergy.product route. In that case provide one structurally real
 * NimbleCatalogItem through the same Java/JNI contract used by a remote catalog.
 *
 * This is deliberately not a loader-state bypass: the native purchase driver
 * still receives refreshcatalogfinished, calls getAvailableCatalogItems(), walks
 * the Java List and converts the item into its own catalog cache.  Purchases are
 * not reported successful by this bootstrap entry.  A future local/modded shop
 * can replace this single item without changing the native startup contract. */
static void publish_local_bootstrap_catalog(const char *reason) {
  if (!catalog_local_bootstrap_items) {
    char sku[160];
    snprintf(sku, sizeof(sku), "com.ea.game.pvz2_row.switch.bootstrap%s%s",
             director_product_id[0] ? "." : "",
             director_product_id[0] ? director_product_id : "");

    const JniCatalogItem bootstrap = {
      .sku = sku,
      .sell_id = director_sell_id[0] ? director_sell_id : "865536",
      .title = "Switch Local Catalog",
      .description = "Local compatibility catalog bootstrap",
      .formatted_price = "$0.00",
      .metadata_url = "",
      .price = 0.0f,
      .item_type = 1, /* non-consumable */
      .is_free = 1,
    };
    catalog_local_bootstrap_items = jni_make_catalog_list(&bootstrap, 1);
  }

  if (!catalog_local_bootstrap_items) {
    publish_empty_catalog_result("local bootstrap allocation failed");
    return;
  }

  catalog_items = catalog_local_bootstrap_items;
  catalog_refresh_ready_flag = 1;
  catalog_refresh_failed_flag = 0;
  (void)reason;
}

void pvz2_catalog_refresh_requested(void) {
  /* Google Play/Nimble MTX is not a provider on Horizon. Avoid reopening the
   * unavailable Synergy route on every refresh and shop visit; publish the
   * local bootstrap item directly and
   * let the native purchase driver complete through its ordinary notification. */
  catalog_refresh_requested = 1;
  catalog_http_start_pending = 0;
  catalog_http_in_flight = 0;
  synergy_product_url[0] = 0;
  publish_local_bootstrap_catalog("Switch offline catalog");
}

int pvz2_take_catalog_refresh_request(void) {
  if (!catalog_refresh_requested) return 0;
  catalog_refresh_requested = 0;
  return 1;
}

int pvz2_catalog_refresh_ready(void) {
  return catalog_refresh_ready_flag != 0;
}

int pvz2_catalog_refresh_failed(void) {
  return catalog_refresh_failed_flag != 0;
}

int pvz2_catalog_item_count(void) {
  return catalog_items && catalog_refresh_ready_flag ?
      jni_list_length(catalog_items) : -1;
}

static void queue_java_callback(const char *callback, jobject self, jlong argument) {
  (void)enqueue_pending_callback(
      (PendingJavaCallback){callback, self, argument, -1, NULL, 0});
}


static void queue_java_bool_callback(const char *callback, jobject self,
                                     jlong argument, jboolean value) {
  (void)enqueue_pending_callback(
      (PendingJavaCallback){callback, self, argument, value, NULL, 0});
}


static void queue_java_string_callback(const char *callback, jobject self,
                                       jlong argument, const char *value) {
  char *copy = strdup(value ? value : "");
  if (!copy) return;
  PendingJavaCallback pending = {callback, self, argument, -2, copy, 0};
  if (!enqueue_pending_callback(pending)) free(copy);
}


/* The stock Android long age gate collects a birth year and month, not
 * a raw numeric age.  Keep the native callback contract unchanged, but derive
 * its age/underAge/teen values from the same DOB-shaped input. */
static void persist_consent_age(long age, long birth_year, long birth_month);
static int reuse_persisted_consent_age(jobject self, jlong callback);

static long parse_decimal_strict(const char *text) {
  if (!text || !text[0]) return -1;
  char *end = NULL;
  const long value = strtol(text, &end, 10);
  return (end && *end == '\0') ? value : -1;
}

static int json_int_or_default(const char *json, const char *key, int fallback) {
  if (!json || !key) return fallback;
  const char *hit = strstr(json, key);
  if (!hit) return fallback;
  hit = strchr(hit, ':');
  if (!hit) return fallback;
  while (*++hit && isspace((unsigned char)*hit)) {}
  char *end = NULL;
  long value = strtol(hit, &end, 10);
  if (end == hit || value < 0 || value > 120) return fallback;
  return (int)value;
}

static void load_consent_age_thresholds(int *age_gate_age, int *teen_age) {
  int gate = 16;
  int teen = 18;
  char json[2048] = {0};
  FILE *file = fopen(DATA_DIR "/consentformMeta.json", "rb");
  if (file) {
    const size_t got = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);
    json[got] = '\0';
    gate = json_int_or_default(json, "\"ageGateAge\"", gate);
    teen = json_int_or_default(json, "\"teenAge\"", teen);
  }
  if (gate < 1 || gate > 120) gate = 16;
  if (teen < gate || teen > 120) teen = 18;
  if (age_gate_age) *age_gate_age = gate;
  if (teen_age) *teen_age = teen;
}

static Result show_dob_number(char *output, size_t output_size,
                              const char *header, const char *subtext,
                              const char *guide, int min_len, int max_len) {
  if (!output || output_size == 0) return (Result)1;
  output[0] = '\0';
  SwkbdConfig keyboard;
  Result rc = swkbdCreate(&keyboard, 0);
  if (R_SUCCEEDED(rc)) {
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&keyboard, header);
    swkbdConfigSetSubText(&keyboard, subtext);
    swkbdConfigSetGuideText(&keyboard, guide);
    swkbdConfigSetStringLenMin(&keyboard, (u32)min_len);
    swkbdConfigSetStringLenMax(&keyboard, (u32)max_len);
    swkbdConfigSetBlurBackground(&keyboard, true);
    rc = swkbdShow(&keyboard, output, output_size);
    swkbdClose(&keyboard);
  }
  return rc;
}

static void show_consent_age(jobject self, jlong callback) {
  /* Reuse an already-recorded DOB and complete the same callback without
   * reopening the system keyboard. */
  if (reuse_persisted_consent_age(self, callback))
    return;

  time_t now = time(NULL);
  struct tm current = {0};
  if (!localtime_r(&now, &current)) {
    current.tm_year = 126; /* 2026 fallback for a broken platform clock. */
    current.tm_mon = 7;
  }
  const long current_year = (long)current.tm_year + 1900;
  const long current_month = (long)current.tm_mon + 1;

  char year_input[8] = "";
  Result rc = show_dob_number(year_input, sizeof(year_input),
                              "Date of birth - Year",
                              "Enter your birth year. Example: 2004",
                              "YYYY", 4, 4);
  const long birth_year = R_SUCCEEDED(rc) ? parse_decimal_strict(year_input) : -1;
  if (birth_year < current_year - 120 || birth_year > current_year) {
    queue_java_string_callback("onShowConsentComplete", self, callback, "{}");
    return;
  }

  char month_subtext[96];
  snprintf(month_subtext, sizeof(month_subtext),
           "Birth year: %ld. Enter birth month (1-12).", birth_year);
  char month_input[4] = "";
  rc = show_dob_number(month_input, sizeof(month_input),
                       "Date of birth - Month", month_subtext,
                       "MM", 1, 2);
  const long birth_month = R_SUCCEEDED(rc) ? parse_decimal_strict(month_input) : -1;
  if (birth_month < 1 || birth_month > 12 ||
      (birth_year == current_year && birth_month > current_month)) {
    queue_java_string_callback("onShowConsentComplete", self, callback, "{}");
    return;
  }

  long age = current_year - birth_year;
  if (current_month < birth_month) age--;
  if (age < 0 || age > 120) {
    queue_java_string_callback("onShowConsentComplete", self, callback, "{}");
    return;
  }

  int age_gate_age = 16;
  int teen_age = 18;
  load_consent_age_thresholds(&age_gate_age, &teen_age);
  const int under_age = age < age_gate_age;
  const int teen = age >= age_gate_age && age < teen_age;

  /* Store the derived age before native receives onShowConsentComplete.  The
   * stock Java path persists age-gate state; Switch shim logs showed
   * previously returned <missing> when native immediately queried agegate.age. */
  persist_consent_age(age, birth_year, birth_month);

  char result[160];
  snprintf(result, sizeof(result),
           "{\"age\":%ld,\"underAge\":%s,\"teen\":%s,\"underAgeUser\":%s,\"teenUser\":%s}",
           age, under_age ? "true" : "false", teen ? "true" : "false",
           under_age ? "true" : "false", teen ? "true" : "false");
  queue_java_string_callback("onShowConsentComplete", self, callback, result);
}

static void queue_java_data_callback(const char *callback, jobject self,
                                     jlong argument, const void *data, int len) {
  if (len < 0) return;
  void *copy = NULL;
  if (data && len > 0) {
    copy = malloc((size_t)len);
    if (!copy) return;
    memcpy(copy, data, (size_t)len);
  }
  PendingJavaCallback pending = {callback, self, argument, -1, copy, len};
  if (!enqueue_pending_callback(pending)) free(copy);
}


static void queue_java_http_callback(jobject self, jlong argument, int status,
                                     const void *data, int len) {
  if (len < 0) return;
  void *copy = NULL;
  if (len > 0) {
    copy = malloc((size_t)len);
    if (!copy) return;
    memcpy(copy, data, (size_t)len);
  }
  PendingJavaCallback pending =
      {"onHTTPResponse", self, argument, -3, copy, len, status};
  if (!enqueue_pending_callback(pending)) free(copy);
}


static void queue_java_download_callback(jobject self, jlong callback,
                                         int status, const char *path) {
  char *copy = strdup(path ? path : "");
  if (!copy) return;
  PendingJavaCallback pending =
      {"onDownloadResponse", self, callback, -5, copy, 0, status};
  if (!enqueue_pending_callback(pending)) free(copy);
}


static void dispatch_java_callbacks(void) {
  PendingJavaCallback callbacks[MAX_PENDING_JAVA_CALLBACKS];
  int count = 0;

  /* Never invoke native code while holding the queue lock.  A callback is free
   * to enqueue follow-up work, which will be dispatched on the next pump. */
  mutexLock(&pending_callback_mutex);
  count = pending_callback_count;
  if (count > 0)
    memcpy(callbacks, pending_callbacks, (size_t)count * sizeof(*callbacks));
  pending_callback_count = 0;
  mutexUnlock(&pending_callback_mutex);

  for (int i = 0; i < count; i++) {
    PendingJavaCallback *callback = &callbacks[i];
    void *native = jni_registered(callback->callback);
    if (!native) {
      debugPrintf("JNI callback unavailable: %s\n",
                  callback->callback ? callback->callback : "?");
      release_pending_callback_payload(callback);
      continue;
    }

    debugPrintf("JNI callback: %s(%lld)\n", callback->callback,
                (long long)callback->argument);
    if (callback->boolean_argument >= 0) {
      ((void (*)(JNIEnv, jobject, jlong, jboolean))native)(
          fake_env, callback->self, callback->argument,
          callback->boolean_argument);
    } else if (callback->boolean_argument == -2) {
      jstring value = jni_make_string((const char *)callback->data);
      ((void (*)(JNIEnv, jobject, jlong, jstring))native)(
          fake_env, callback->self, callback->argument, value);
      jni_free_wrapper(value);
    } else if (callback->boolean_argument == -3) {
      jbyteArray data = jni_make_bytearray_copy(callback->data,
                                                callback->data_length);
      jobject headers = jni_make_object_class("java/util/HashMap");
      ((void (*)(JNIEnv, jobject, jlong, jint, jbyteArray, jobject))native)(
          fake_env, callback->self, callback->argument, callback->status, data,
          headers);
      jni_free_wrapper(data);
      jni_free_wrapper(headers);
    } else if (callback->boolean_argument == -5) {
      jstring path = jni_make_string((const char *)callback->data);
      ((void (*)(JNIEnv, jobject, jlong, jint, jstring))native)(
          fake_env, callback->self, callback->argument, callback->status, path);
      jni_free_wrapper(path);
    } else if (callback->data) {
      jbyteArray bytes = jni_make_bytearray_copy(callback->data,
                                                 callback->data_length);
      ((void (*)(JNIEnv, jobject, jlong, jbyteArray, jint))native)(
          fake_env, callback->self, callback->argument, bytes,
          callback->data_length);
      jni_free_wrapper(bytes);
    } else {
      ((void (*)(JNIEnv, jobject, jlong))native)(
          fake_env, callback->self, callback->argument);
    }
    release_pending_callback_payload(callback);
  }
}


typedef struct {
  unsigned char *data;
  size_t size;
  size_t capacity;
} HttpBuffer;

static const char *bootstrap_ipv4(const char *host) {
  size_t length = strlen(host);
  while (length && host[length - 1] == '.') --length;
  if (length == sizeof("pvz2-prd.popcap.com") - 1 &&
      !strncasecmp(host, "pvz2-prd.popcap.com", length)) return "52.23.60.120";
  if (length == sizeof("pvz2-live.ecs.popcap.com") - 1 &&
      !strncasecmp(host, "pvz2-live.ecs.popcap.com", length)) return "23.210.17.131";
  if (length == sizeof("prd1.personalization.centech.glulive.com") - 1 &&
      !strncasecmp(host, "prd1.personalization.centech.glulive.com", length))
    return "23.76.204.156";
  /* Akamai-backed payload CDN. Verified with the exact SDK_CONFIG_CONSENT
   * consent-form URL; libnx's resolver cannot follow its CNAME chain. */
  if (length == sizeof("prd1.cdn.pengine.revtech.glulive.com") - 1 &&
      !strncasecmp(host, "prd1.cdn.pengine.revtech.glulive.com", length))
    return "2.16.106.13";
  /* The Gems hostname is a CNAME to an AWS load balancer. libnx cannot follow
   * that chain, so use the currently resolved A record. This was verified by a
   * live GET of the exact award endpoint, which returned the server's {} body. */
  if (length == sizeof("pvz2.prod.gems.gluops.com") - 1 &&
      !strncasecmp(host, "pvz2.prod.gems.gluops.com", length))
    return "34.196.244.66";
  if (length == sizeof("syn-dir.sn.eamobile.com") - 1 &&
      !strncasecmp(host, "syn-dir.sn.eamobile.com", length))
    return "52.5.25.187";
  /* GPI host used by the payment service. The Synergy catalog now
   * resolves its product host through the Director response. */
  if (length == sizeof("gpis-prod.revtech.glulive.com") - 1 &&
      !strncasecmp(host, "gpis-prod.revtech.glulive.com", length))
    return "3.211.47.68";
  return NULL;
}

static int parse_ipv4(const char *text, struct in_addr *address) {
  unsigned char octets[4];
  for (int i = 0; i < 4; i++) {
    unsigned value = 0;
    if (*text < '0' || *text > '9') return 0;
    do {
      value = value * 10 + (unsigned)(*text++ - '0');
      if (value > 255) return 0;
    } while (*text >= '0' && *text <= '9');
    octets[i] = (unsigned char)value;
    if (i != 3) {
      if (*text++ != '.') return 0;
    }
  }
  if (*text) return 0;
  memcpy(&address->s_addr, octets, sizeof(octets));
  return 1;
}

int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result) {
  if (!result || !node) return EAI_NONAME;
  *result = NULL;
  debugPrintf("dns: node=%s service=%s family=%d\n", node, service ? service : "",
              hints ? hints->ai_family : AF_UNSPEC);
  if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
    return EAI_FAMILY;

  struct in_addr address;
  const char *target = bootstrap_ipv4(node);
  if (!target || !parse_ipv4(target, &address)) {
    debugPrintf("dns: no bootstrap mapping for %s\n", node);
    return EAI_NONAME;
  }

  unsigned long port = 0;
  if (service && *service) {
    char *end = NULL;
    port = strtoul(service, &end, 10);
    if (!end || *end || port > 65535) return EAI_SERVICE;
  }
  struct addrinfo *info = calloc(1, sizeof(*info));
  struct sockaddr_in *socket_address = calloc(1, sizeof(*socket_address));
  if (!info || !socket_address) {
    free(info);
    free(socket_address);
    return EAI_MEMORY;
  }
  socket_address->sin_family = AF_INET;
  socket_address->sin_port = (uint16_t)((port << 8) | (port >> 8));
  socket_address->sin_addr = address;
  info->ai_family = AF_INET;
  info->ai_socktype = hints ? hints->ai_socktype : 0;
  info->ai_protocol = hints ? hints->ai_protocol : 0;
  info->ai_addrlen = sizeof(*socket_address);
  info->ai_addr = (struct sockaddr *)socket_address;
  *result = info;
  debugPrintf("dns: %s -> %s:%lu\n", node, target, port);
  return 0;
}

void __wrap_freeaddrinfo(struct addrinfo *info) {
  while (info) {
    struct addrinfo *next = info->ai_next;
    free(info->ai_addr);
    free(info->ai_canonname);
    free(info);
    info = next;
  }
}

static size_t http_write(void *ptr, size_t size, size_t count, void *opaque) {
  HttpBuffer *buffer = opaque;
  const size_t bytes = size * count;
  if (!bytes || bytes > SIZE_MAX - buffer->size) return 0;
  const size_t needed = buffer->size + bytes;
  if (needed > buffer->capacity) {
    size_t cap = buffer->capacity ? buffer->capacity : 4096;
    while (cap < needed) {
      if (cap > SIZE_MAX / 2) return 0;
      cap *= 2;
    }
    unsigned char *data = realloc(buffer->data, cap);
    if (!data) return 0;
    buffer->data = data; buffer->capacity = cap;
  }
  memcpy(buffer->data + buffer->size, ptr, bytes);
  buffer->size = needed;
  return bytes;
}

#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
static int http_trace(CURL *curl, curl_infotype type, char *data, size_t size, void *opaque) {
  (void)curl;
  (void)opaque;
  if (type == CURLINFO_TEXT && size)
    debugPrintf("curl: %.*s", (int)(size > 512 ? 512 : size), data);
  return 0;
}
#endif

static void http_log_body(const char *label, const void *data, size_t size) {
  if (!size) return;
  const size_t length = size < 256 ? size : 256;
  char text[257];
  const unsigned char *bytes = data;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = bytes[i];
    text[i] = c >= 0x20 && c <= 0x7e ? (char)c : '.';
  }
  text[length] = 0;
  debugPrintf("JNI HTTP: %s (%zu bytes): %s\n", label, size, text);
}

/* Account/cloud traffic is useful to diagnose, but request and response
 * bodies can contain passwords, tokens, email addresses, or save data. Keep
 * the diagnostic at URL/method/status/size level. */
static int account_http_url(const char *url) {
  if (!url) return 0;
  return strstr(url, "ea.com") || strstr(url, "account") ||
      strstr(url, "auth") || strstr(url, "login") ||
      strstr(url, "register") || strstr(url, "identity") ||
      strstr(url, "cloud") || strstr(url, "consent");
}

static void log_account_http_request(const char *url, const char *method,
                                     int body_length) {
  if (account_http_url(url))
    debugPrintf("ACCOUNT HTTP REQUEST: %s %s body=%d bytes\n",
                method && *method ? method : "GET", url, body_length);
}

static jstring read_asset_as_string(jobject argument) {
  const char *name = jni_cstr(argument);
  if (!name || !*name) return jni_make_string("");
  static const char prefix[] = "file:///android_asset/";
  if (!strncmp(name, prefix, sizeof(prefix) - 1)) name += sizeof(prefix) - 1;

  FILE *file = fopen_fake(name, "rb");
  if (!file) {
    char asset_path[512];
    snprintf(asset_path, sizeof(asset_path), DATA_DIR "/assets/%s", name);
    file = fopen_fake(asset_path, "rb");
  }
  if (!file) {
    debugPrintf("JNI asset string: %s -> missing\n", name);
    return jni_make_string("");
  }
  if (fseek_fake(file, 0, SEEK_END) != 0) {
    fclose_fake(file);
    return jni_make_string("");
  }
  const long length = ftell_fake(file);
  if (length < 0 || length > 4 * 1024 * 1024 || fseek_fake(file, 0, SEEK_SET) != 0) {
    fclose_fake(file);
    return jni_make_string("");
  }
  char *text = malloc((size_t)length + 1);
  if (!text) {
    fclose_fake(file);
    return jni_make_string("");
  }
  const size_t got = fread_fake(text, 1, (size_t)length, file);
  fclose_fake(file);
  if (got != (size_t)length) {
    free(text);
    return jni_make_string("");
  }
  text[length] = 0;
  debugPrintf("JNI asset string: %s -> %zu bytes\n", name, got);
  jstring result = jni_make_string(text);
  free(text);
  return result;
}

static jstring read_shared_property(jobject argument) {
  const char *key = jni_cstr(argument);
  /* SharedPreferences are optional on Switch. Empty means no local override. */
  debugPrintf("JNI shared property: %s -> empty\n", key ? key : "");
  return jni_make_string("");
}

typedef struct {
  char *scope;
  char *key;
  char *value;
} PropertyEntry;

static PropertyEntry property_store[64];
static PropertyEntry string_store[32];

/* Android's property/string stores are SharedPreferences-style persistent
 * storage. Earlier Switch builds kept them only in RAM, so every launch lost
 * agegate.age, consent state, AnalyticsID/UserID and related install identity.
 * The game then legitimately re-entered first-run/profile creation even though
 * pp.dat and local_profiles were still present. Keep both Java stores in one
 * small, versioned, checksummed file written through tmp+rename. */
#define JAVA_STORE_PATH     DATA_DIR "/No_Backup/java_properties_v1.bin"
#define JAVA_STORE_TMP_PATH DATA_DIR "/No_Backup/java_properties_v1.bin.tmp"
#define JAVA_STORE_MAGIC    "PVZ2JPS1"
#define JAVA_STORE_VERSION  1u
#define JAVA_STORE_HEADER_SIZE 24u
#define JAVA_STORE_RECORD_SIZE 16u
#define JAVA_STORE_MAX_FILE (1024u * 1024u)

static int java_store_loaded;
static int java_store_loading;
static int java_store_preexisting = -1;

static uint32_t java_store_read_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void java_store_write_u32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

static uint32_t java_store_fnv1a(const unsigned char *data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; i++) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

static const char *store_get_raw(PropertyEntry *store, int count,
                                 const char *scope, const char *key) {
  if (!scope) scope = "";
  if (!key) return NULL;
  for (int i = 0; i < count; i++)
    if (store[i].scope && !strcmp(store[i].scope, scope) &&
        !strcmp(store[i].key, key))
      return store[i].value;
  return NULL;
}

/* Returns 1 when the value changed, 0 when it was already identical, and -1
 * on allocation/capacity failure. It never performs disk I/O, which lets the
 * loader and multi-key writes commit only once. */
static int store_set_raw(PropertyEntry *store, int count, const char *scope,
                         const char *key, const char *value) {
  if (!scope) scope = "";
  if (!key || !value) return -1;
  for (int i = 0; i < count; i++) {
    if (store[i].scope && !strcmp(store[i].scope, scope) &&
        !strcmp(store[i].key, key)) {
      if (!strcmp(store[i].value, value)) return 0;
      char *copy = strdup(value);
      if (!copy) return -1;
      free(store[i].value);
      store[i].value = copy;
      return 1;
    }
  }
  for (int i = 0; i < count; i++) {
    if (store[i].scope) continue;
    char *scope_copy = strdup(scope);
    char *key_copy = strdup(key);
    char *value_copy = strdup(value);
    if (!scope_copy || !key_copy || !value_copy) {
      free(scope_copy); free(key_copy); free(value_copy);
      return -1;
    }
    store[i].scope = scope_copy;
    store[i].key = key_copy;
    store[i].value = value_copy;
    return 1;
  }
  return -1;
}

static void java_store_ensure_loaded(void);

static void java_store_save(void) {
  if (!java_store_loaded || java_store_loading) return;

  size_t payload_size = 0;
  uint32_t entry_count = 0;
  PropertyEntry *stores[2] = { property_store, string_store };
  const int capacities[2] = { 64, 32 };
  for (unsigned kind = 0; kind < 2; kind++) {
    for (int i = 0; i < capacities[kind]; i++) {
      const PropertyEntry *entry = &stores[kind][i];
      if (!entry->scope || !entry->key || !entry->value) continue;
      const size_t scope_len = strlen(entry->scope);
      const size_t key_len = strlen(entry->key);
      const size_t value_len = strlen(entry->value);
      if (scope_len > UINT32_MAX || key_len > UINT32_MAX || value_len > UINT32_MAX ||
          payload_size > JAVA_STORE_MAX_FILE - JAVA_STORE_RECORD_SIZE ||
          scope_len > JAVA_STORE_MAX_FILE - payload_size - JAVA_STORE_RECORD_SIZE ||
          key_len > JAVA_STORE_MAX_FILE - payload_size - JAVA_STORE_RECORD_SIZE - scope_len ||
          value_len > JAVA_STORE_MAX_FILE - payload_size - JAVA_STORE_RECORD_SIZE - scope_len - key_len) {
        return;
      }
      payload_size += JAVA_STORE_RECORD_SIZE + scope_len + key_len + value_len;
      entry_count++;
    }
  }

  if (payload_size > JAVA_STORE_MAX_FILE - JAVA_STORE_HEADER_SIZE) return;
  const size_t total_size = JAVA_STORE_HEADER_SIZE + payload_size;
  unsigned char *blob = calloc(1, total_size ? total_size : 1);
  if (!blob) return;
  memcpy(blob, JAVA_STORE_MAGIC, 8);
  java_store_write_u32(blob + 8, JAVA_STORE_VERSION);
  java_store_write_u32(blob + 12, entry_count);
  java_store_write_u32(blob + 16, (uint32_t)payload_size);

  size_t at = JAVA_STORE_HEADER_SIZE;
  for (unsigned kind = 0; kind < 2; kind++) {
    for (int i = 0; i < capacities[kind]; i++) {
      const PropertyEntry *entry = &stores[kind][i];
      if (!entry->scope || !entry->key || !entry->value) continue;
      const uint32_t scope_len = (uint32_t)strlen(entry->scope);
      const uint32_t key_len = (uint32_t)strlen(entry->key);
      const uint32_t value_len = (uint32_t)strlen(entry->value);
      java_store_write_u32(blob + at, kind); at += 4;
      java_store_write_u32(blob + at, scope_len); at += 4;
      java_store_write_u32(blob + at, key_len); at += 4;
      java_store_write_u32(blob + at, value_len); at += 4;
      memcpy(blob + at, entry->scope, scope_len); at += scope_len;
      memcpy(blob + at, entry->key, key_len); at += key_len;
      memcpy(blob + at, entry->value, value_len); at += value_len;
    }
  }
  java_store_write_u32(blob + 20,
                       java_store_fnv1a(blob + JAVA_STORE_HEADER_SIZE, payload_size));

  FILE *file = fopen_fake(JAVA_STORE_TMP_PATH, "wb");
  int committed = 0;
  if (file) {
    const size_t written = fwrite_fake(blob, 1, total_size, file);
    const int flushed = fflush(file) == 0;
    const int fd = fileno(file);
    if (fd >= 0) fsync(fd);
    const int closed = fclose_fake(file) == 0;
    if (written == total_size && flushed && closed &&
        rename_fake(JAVA_STORE_TMP_PATH, JAVA_STORE_PATH) == 0)
      committed = 1;
  }
  if (!committed)
    unlink_fake(JAVA_STORE_TMP_PATH);
  free(blob);
}

static void java_store_ensure_loaded(void) {
  if (java_store_loaded || java_store_loading) return;
  java_store_loading = 1;

  FILE *file = fopen_fake(JAVA_STORE_PATH, "rb");
  if (!file) {
    java_store_preexisting = 0;
    java_store_loaded = 1;
    java_store_loading = 0;
    return;
  }

  long length = -1;
  if (fseek(file, 0, SEEK_END) == 0)
    length = ftell(file);
  if (length >= 0) fseek(file, 0, SEEK_SET);
  unsigned char *blob = NULL;
  size_t got = 0;
  if (length >= (long)JAVA_STORE_HEADER_SIZE &&
      length <= (long)JAVA_STORE_MAX_FILE) {
    blob = malloc((size_t)length);
    if (blob) got = fread_fake(blob, 1, (size_t)length, file);
  }
  fclose_fake(file);

  int valid = blob && got == (size_t)length &&
              !memcmp(blob, JAVA_STORE_MAGIC, 8) &&
              java_store_read_u32(blob + 8) == JAVA_STORE_VERSION;
  uint32_t entry_count = 0;
  uint32_t payload_size = 0;
  if (valid) {
    entry_count = java_store_read_u32(blob + 12);
    payload_size = java_store_read_u32(blob + 16);
    valid = entry_count <= 96 &&
            payload_size == (uint32_t)((size_t)length - JAVA_STORE_HEADER_SIZE) &&
            java_store_read_u32(blob + 20) ==
                java_store_fnv1a(blob + JAVA_STORE_HEADER_SIZE, payload_size);
  }

  size_t at = JAVA_STORE_HEADER_SIZE;
  if (valid) {
    for (uint32_t i = 0; i < entry_count; i++) {
      if (at > (size_t)length - JAVA_STORE_RECORD_SIZE) { valid = 0; break; }
      const uint32_t kind = java_store_read_u32(blob + at); at += 4;
      const uint32_t scope_len = java_store_read_u32(blob + at); at += 4;
      const uint32_t key_len = java_store_read_u32(blob + at); at += 4;
      const uint32_t value_len = java_store_read_u32(blob + at); at += 4;
      const size_t data_len = (size_t)scope_len + key_len + value_len;
      if (kind > 1 || scope_len > 4096 || key_len > 4096 || value_len > 65536 ||
          data_len > (size_t)length - at) { valid = 0; break; }
      at += data_len;
    }
    if (at != (size_t)length) valid = 0;
  }

  if (valid) {
    at = JAVA_STORE_HEADER_SIZE;
    for (uint32_t i = 0; i < entry_count; i++) {
      const uint32_t kind = java_store_read_u32(blob + at); at += 4;
      const uint32_t scope_len = java_store_read_u32(blob + at); at += 4;
      const uint32_t key_len = java_store_read_u32(blob + at); at += 4;
      const uint32_t value_len = java_store_read_u32(blob + at); at += 4;
      char *scope = malloc((size_t)scope_len + 1);
      char *key = malloc((size_t)key_len + 1);
      char *value = malloc((size_t)value_len + 1);
      if (!scope || !key || !value) {
        free(scope); free(key); free(value);
        valid = 0;
        break;
      }
      memcpy(scope, blob + at, scope_len); scope[scope_len] = 0; at += scope_len;
      memcpy(key, blob + at, key_len); key[key_len] = 0; at += key_len;
      memcpy(value, blob + at, value_len); value[value_len] = 0; at += value_len;
      const int changed = kind == 0
          ? store_set_raw(property_store, 64, scope, key, value)
          : store_set_raw(string_store, 32, scope, key, value);
      free(scope); free(key); free(value);
      if (changed < 0) { valid = 0; break; }
    }
  }
  free(blob);
  java_store_preexisting = valid ? 1 : 0;
  java_store_loaded = 1;
  java_store_loading = 0;
  if (valid) {
    /* Backfill a missing ageUpToAdult field without replacing an existing
     * value persisted by the game. */
    const char *age = store_get_raw(property_store, 64, "agegate", "age");
    const char *age_up = store_get_raw(property_store, 64, "agegate", "ageUpToAdult");
    if (age && *age && !age_up &&
        store_set_raw(property_store, 64, "agegate", "ageUpToAdult", "false") > 0) {
      java_store_save();
    }
  }
}

void pvz2_java_store_ensure_loaded(void) {
  java_store_ensure_loaded();
}

static const char *store_get(PropertyEntry *store, int count,
                             const char *scope, const char *key) {
  java_store_ensure_loaded();
  return store_get_raw(store, count, scope, key);
}

static void store_set(PropertyEntry *store, int count, const char *scope,
                      const char *key, const char *value) {
  java_store_ensure_loaded();
  if (store_set_raw(store, count, scope, key, value) > 0)
    java_store_save();
}

static void persist_consent_age(long age, long birth_year, long birth_month) {
  char age_text[16];
  char year_text[16];
  char month_text[16];
  snprintf(age_text, sizeof(age_text), "%ld", age);
  snprintf(year_text, sizeof(year_text), "%ld", birth_year);
  snprintf(month_text, sizeof(month_text), "%ld", birth_month);
  java_store_ensure_loaded();
  int changed = 0;
  if (store_set_raw(property_store, 64, "agegate", "age", age_text) > 0) changed = 1;
  if (store_set_raw(property_store, 64, "agegate", "birthYear", year_text) > 0) changed = 1;
  if (store_set_raw(property_store, 64, "agegate", "birthMonth", month_text) > 0) changed = 1;
  /* Native PIM persists this separate completion flag alongside age. Earlier
   * kept age/DOB but omitted it, so the next launch legitimately treated the
   * age gate as incomplete and displayed DOB again.  A normal DOB submission
   * is not an age-up transition, so the stock value is false. */
  if (store_set_raw(property_store, 64, "agegate", "ageUpToAdult", "false") > 0) changed = 1;
  if (changed) java_store_save();
}

static int reuse_persisted_consent_age(jobject self, jlong callback) {
  java_store_ensure_loaded();
  const char *year_text =
      store_get_raw(property_store, 64, "agegate", "birthYear");
  const char *month_text =
      store_get_raw(property_store, 64, "agegate", "birthMonth");
  if (!year_text || !month_text || !*year_text || !*month_text)
    return 0;

  const long birth_year = parse_decimal_strict(year_text);
  const long birth_month = parse_decimal_strict(month_text);
  time_t now = time(NULL);
  struct tm current = {0};
  if (!localtime_r(&now, &current)) {
    current.tm_year = 126;
    current.tm_mon = 7;
  }
  const long current_year = (long)current.tm_year + 1900;
  const long current_month = (long)current.tm_mon + 1;
  if (birth_year < current_year - 120 || birth_year > current_year ||
      birth_month < 1 || birth_month > 12 ||
      (birth_year == current_year && birth_month > current_month))
    return 0;

  long age = current_year - birth_year;
  if (current_month < birth_month) age--;
  if (age < 0 || age > 120)
    return 0;

  int age_gate_age = 16;
  int teen_age = 18;
  load_consent_age_thresholds(&age_gate_age, &teen_age);
  const int under_age = age < age_gate_age;
  const int teen = age >= age_gate_age && age < teen_age;

  /* Refresh age when a birthday/month boundary has passed.  This writes only
   * the same Android preference fields that a completed DOB submission owns. */
  persist_consent_age(age, birth_year, birth_month);

  char result[160];
  snprintf(result, sizeof(result),
           "{\"age\":%ld,\"underAge\":%s,\"teen\":%s,"
           "\"underAgeUser\":%s,\"teenUser\":%s}",
           age, under_age ? "true" : "false", teen ? "true" : "false",
           under_age ? "true" : "false", teen ? "true" : "false");
  queue_java_string_callback("onShowConsentComplete", self, callback, result);
  return 1;
}

static jobjectArray read_properties(jobject scope_argument, jobject keys) {
  const char *scope = jni_cstr(scope_argument);
  if (!scope) scope = "";
  const int count = jni_object_array_length(keys);
  const char **values = calloc((size_t)count * 2 + 1, sizeof(*values));
  if (!values) return NULL;
  int used = 0;
  for (int i = 0; i < count; i++) {
    const char *key = jni_cstr(jni_object_array_get(keys, i));
    const char *value = key ? store_get(property_store, 64, scope, key) : NULL;
    if (!value) continue;
    values[used++] = key;
    values[used++] = value;
  }
  jobjectArray result = jni_make_string_array(values, used);
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  debugPrintf("JNI properties: %s -> %d entries\n", scope, used / 2);
#endif
  free(values);
  return result;
}

static void write_properties(jobject scope_argument, jobject values) {
  const char *scope = jni_cstr(scope_argument);
  if (!scope) scope = "";
  const int count = jni_object_array_length(values) & ~1;
  java_store_ensure_loaded();
  int changed = 0;
  for (int i = 0; i < count; i += 2) {
    const char *key = jni_cstr(jni_object_array_get(values, i));
    const char *value = jni_cstr(jni_object_array_get(values, i + 1));
    if (store_set_raw(property_store, 64, scope, key, value) > 0) changed = 1;
  }
  if (changed) java_store_save();
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  debugPrintf("JNI properties: %s <- %d entries\n", scope, count / 2);
#endif
}

static jstring get_string_store(jobject key_argument) {
  const char *key = jni_cstr(key_argument);
  const char *value = key ? store_get(string_store, 32, "", key) : NULL;
  if (!value && key && !strcmp(key, "LocationISOCode")) {
    /* Keep Glu's region view consistent with the Switch platform data we
     * already expose through CountryCode/DeviceCountry. */
    value = get_switch_locale().country;
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
    debugPrintf("JNI string store: LocationISOCode -> Switch country %s\n",
                value ? value : "");
#endif
  }
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  else {
    debugPrintf("JNI string store: %s -> %s\n", key ? key : "",
                value ? "hit" : "miss");
  }
#endif
  return value ? jni_make_string(value) : NULL;
}

static void set_string_store(jobject key_argument, jobject value_argument) {
  const char *key = jni_cstr(key_argument);
  const char *value = jni_cstr(value_argument);
  store_set(string_store, 32, "", key, value);
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  debugPrintf("JNI string store: %s <- %zu bytes\n", key ? key : "",
              value ? strlen(value) : 0);
#endif
}

/* Android's HTTP bridge is asynchronous. Calling curl_easy_perform directly
 * from a JNI upcall froze Native_onDrawFrame while the network stack performed
 * a TLS request. Keep the real request/response data, but drive libcurl's
 * multi interface from UI_ProcessEvents so the native frame loop never blocks. */
typedef enum {
  HTTP_ANDROID,
  HTTP_GLU,
  HTTP_DOWNLOAD,
  HTTP_SYNERGY_DIRECTOR,
  HTTP_MTX,
  HTTP_ACCOUNT_REGISTER
} HttpKind;
typedef struct {
  CURL *curl;
  struct curl_slist *headers;
  HttpBuffer response, response_headers;
  HttpKind kind;
  jobject self;
  jlong callback;
  char *url, *method, *body, *destination;
} HttpRequest;

static void log_account_http_response(const HttpRequest *request,
                                      CURLcode code, long status) {
  if (request && account_http_url(request->url))
    debugPrintf("ACCOUNT HTTP RESPONSE: %s %s curl=%d status=%ld bytes=%zu headers=%zu\n",
                request->method ? request->method : "GET",
                request->url ? request->url : "?", code, status,
                request->response.size, request->response_headers.size);
}

#define MAX_HTTP_REQUESTS 16
static CURLM *http_multi;
static HttpRequest *http_requests[MAX_HTTP_REQUESTS];
static int http_request_count;
static int curl_ready;

static void http_pump(void);

static void http_request_free(HttpRequest *request) {
  if (!request) return;
  if (request->curl) curl_easy_cleanup(request->curl);
  curl_slist_free_all(request->headers);
  free(request->response.data);
  free(request->response_headers.data);
  free(request->url); free(request->method); free(request->body);
  free(request->destination); free(request);
}

static int http_request_start(HttpRequest *request, const char *url,
                              const char *method, const char *raw_headers,
                              const void *body, int body_length, long timeout_ms) {
  if (!request || !url || !*url || body_length < 0 ||
      http_request_count >= MAX_HTTP_REQUESTS)
    return 0;
  if (!curl_ready && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
  curl_ready = 1;
  if (!http_multi && !(http_multi = curl_multi_init())) return 0;

  request->url = strdup(url);
  request->method = strdup(method && *method ? method : "GET");
  if (body_length > 0) {
    request->body = malloc((size_t)body_length);
    if (request->body) memcpy(request->body, body, (size_t)body_length);
  }
  if (!request->url || !request->method || (body_length > 0 && !request->body))
    return 0;
  log_account_http_request(request->url, request->method, body_length);
  request->curl = curl_easy_init();
  if (!request->curl) return 0;

  while (raw_headers && *raw_headers) {
    const char *end = strchr(raw_headers, '\n');
    const size_t length = end ? (size_t)(end - raw_headers) : strlen(raw_headers);
    if (length) {
      char line[512];
      const size_t copy = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
      memcpy(line, raw_headers, copy); line[copy] = 0;
      request->headers = curl_slist_append(request->headers, line);
    }
    if (!end) break;
    raw_headers = end + 1;
  }

  CURL *curl = request->curl;
  curl_easy_setopt(curl, CURLOPT_URL, request->url);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request->method);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request->headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &request->response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_write);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &request->response_headers);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "PvZ2/13.3.1 (Nintendo Switch)");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 30000L);
  /* The CDN labels some already-plain payloads as Content-Encoding: base64.
   * Keep the wire bytes untouched; the game parses the plain HTML/JSON body. */
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
  curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, http_trace);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#else
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
#endif
  /* A normal SD install has no Android/system CA bundle for switch-curl. */
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  if (body_length > 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_length);
  }
  curl_easy_setopt(curl, CURLOPT_PRIVATE, request);
  if (curl_multi_add_handle(http_multi, curl) != CURLM_OK) return 0;
  http_requests[http_request_count++] = request;
  return 1;
}

static void account_log_callback_url(const char *url) {
  if (!url || !*url) {
    debugPrintf("ACCOUNT WEB: no callback URL returned\n");
    return;
  }
  /* Never put an EA authorization code or token in the log.  Keep only the
   * scheme/host/path so the callback route can be identified safely. */
  char safe[256];
  size_t n = 0;
  while (url[n] && url[n] != '?' && url[n] != '#') {
    if (n + 1 >= sizeof(safe)) break;
    safe[n] = url[n];
    n++;
  }
  safe[n] = '\0';
  debugPrintf("ACCOUNT WEB: callback URL path=%s\n", safe);
}

static void switch_ea_login_webview(const char *url,
                                    const char *redirect_url) {
  if (!url || strncmp(url, "https://", 8) != 0) {
    debugPrintf("ACCOUNT WEB: rejected missing/non-HTTPS OAuth URL\n");
    return;
  }
  const AppletType applet_type = appletGetAppletType();
  debugPrintf("ACCOUNT WEB: applet type=%d (Application=0)\n", (int)applet_type);
  if (applet_type != AppletType_Application) {
    debugPrintf("ACCOUNT WEB: browser unavailable in applet mode; launch HBMenu "
                "through title takeover (hold R while starting a game)\n");
    return;
  }
  WebCommonConfig config;
  WebCommonReply reply;
  /* This is the exact URL produced by the bundled Nexus account component.
   * Use the normal page mode: webNewsCreate selects a news-specific applet
   * configuration which Horizon rejects for this account flow. */
  Result rc = webPageCreate(&config, url);
  if (R_FAILED(rc)) {
    debugPrintf("ACCOUNT WEB: webPageCreate failed rc=0x%x\n", (unsigned)rc);
    return;
  }

  /* OAuth redirects can pass through several EA/CDN hosts.  Keep HTTPS-only
   * navigation unrestricted for this diagnostic account flow; the initial
   * URL is still fixed to EA and no callback URL is accepted by the applet. */
  static const char *const whitelist = "^https://.*$";
  Result whitelist_rc = webConfigSetWhitelist(&config, whitelist);
  debugPrintf("ACCOUNT WEB: EA whitelist rc=0x%x\n", (unsigned)whitelist_rc);
  if (R_FAILED(whitelist_rc)) return;

  /* Do not add CallbackUrl: this firmware rejected that optional TLV with
   * 0x1759 before the browser made any EA request. */
  debugPrintf("ACCOUNT WEB: launching native Nexus OAuth URL redirect=%s\n",
              redirect_url && *redirect_url ? "present" : "missing");

  memset(&reply, 0, sizeof(reply));
  /* webConfigShow blocks the game thread while the system browser owns the
   * foreground.  That is expected and must not be reported as a PVZ2 stall. */
  watchdog_set_suspended(1);
  rc = webConfigShow(&config, &reply);
  watchdog_set_suspended(0);
  if (R_FAILED(rc)) {
    debugPrintf("ACCOUNT WEB: webConfigShow failed rc=0x%x\n", (unsigned)rc);
    return;
  }
  WebExitReason reason = WebExitReason_UnknownE;
  webReplyGetExitReason(&reply, &reason);
  char callback[1024] = {0};
  size_t callback_size = 0;
  if (R_SUCCEEDED(webReplyGetLastUrl(&reply, callback, sizeof(callback),
                                     &callback_size))) {
    account_log_callback_url(callback);
  }
  debugPrintf("ACCOUNT WEB: EA frame closed reason=%u callback_bytes=%zu\n",
              (unsigned)reason, callback_size);
  debugLogFlush();
}

/* The MTX endpoint returns ordinary JSON, but the Android implementation
 * hands the native game a list of NimbleCatalogItem objects.  This small
 * parser deliberately understands only the catalog object fields; it avoids
 * pulling a second JSON runtime into the loader and keeps malformed server
 * data from reaching the native purchase driver. */
static const char *json_value_start(const char *begin, const char *end,
                                    const char *key) {
  char needle[96];
  if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
    return NULL;
  const char *p = begin;
  while (p && p < end) {
    p = strstr(p, needle);
    if (!p || p >= end) return NULL;
    p += strlen(needle);
    while (p < end && isspace((unsigned char)*p)) ++p;
    if (p >= end || *p != ':') continue;
    ++p;
    while (p < end && isspace((unsigned char)*p)) ++p;
    return p < end ? p : NULL;
  }
  return NULL;
}

static int json_copy_string_at(const char *value, const char *end,
                               char *out, size_t capacity) {
  if (!value || value >= end || *value != '"' || capacity == 0) return 0;
  ++value;
  size_t used = 0;
  while (value < end && *value) {
    if (*value == '"') {
      out[used] = 0;
      return 1;
    }
    unsigned char c = (unsigned char)*value++;
    if (c == '\\' && value < end) {
      const char escaped = *value++;
      switch (escaped) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u':
          /* Catalog text is only used for lookup/display. Preserve a
           * printable replacement for non-ASCII JSON escapes. */
          c = '?';
          for (int i = 0; i < 4 && value < end; ++i) ++value;
          break;
        default: c = (unsigned char)escaped; break;
      }
    }
    if (used + 1 < capacity) out[used++] = (char)c;
  }
  out[used < capacity ? used : capacity - 1] = 0;
  return 0;
}

static int json_copy_string(const char *begin, const char *end,
                            const char *const *keys, int key_count,
                            char *out, size_t capacity) {
  for (int i = 0; i < key_count; ++i) {
    const char *value = json_value_start(begin, end, keys[i]);
    if (value && json_copy_string_at(value, end, out, capacity)) return 1;
  }
  if (capacity) out[0] = 0;
  return 0;
}

static int json_number(const char *begin, const char *end,
                       const char *const *keys, int key_count, double *out) {
  for (int i = 0; i < key_count; ++i) {
    const char *value = json_value_start(begin, end, keys[i]);
    if (!value || value >= end) continue;
    char *after = NULL;
    const double number = strtod(value, &after);
    if (after != value && after <= end) {
      *out = number;
      return 1;
    }
  }
  return 0;
}

static int json_boolean(const char *begin, const char *end,
                        const char *const *keys, int key_count, int *out) {
  for (int i = 0; i < key_count; ++i) {
    const char *value = json_value_start(begin, end, keys[i]);
    if (!value || value >= end) continue;
    if ((size_t)(end - value) >= 4 && !strncmp(value, "true", 4)) {
      *out = 1; return 1;
    }
    if ((size_t)(end - value) >= 5 && !strncmp(value, "false", 5)) {
      *out = 0; return 1;
    }
  }
  return 0;
}

static void capture_director_identity(const unsigned char *data, size_t length) {
  if (!data || !length || length > 4 * 1024 * 1024) return;
  char *json = malloc(length + 1);
  if (!json) return;
  memcpy(json, data, length);
  json[length] = 0;

  const char *end = json + length;
  const char *sell_keys[] = {"sellId"};
  const char *product_keys[] = {"productId"};
  char text[64];
  double number = 0;

  if (json_copy_string(json, end, sell_keys, 1, text, sizeof(text)) && text[0]) {
    snprintf(director_sell_id, sizeof(director_sell_id), "%s", text);
  } else if (json_number(json, end, sell_keys, 1, &number) && number >= 0) {
    snprintf(director_sell_id, sizeof(director_sell_id), "%.0f", number);
  }

  text[0] = 0;
  number = 0;
  if (json_copy_string(json, end, product_keys, 1, text, sizeof(text)) && text[0]) {
    snprintf(director_product_id, sizeof(director_product_id), "%s", text);
  } else if (json_number(json, end, product_keys, 1, &number) && number >= 0) {
    snprintf(director_product_id, sizeof(director_product_id), "%.0f", number);
  }

  free(json);
}

static const char *json_object_start(const char *begin, const char *at) {
  int depth = 0;
  for (const char *p = at; p > begin; ) {
    --p;
    if (*p == '}') ++depth;
    else if (*p == '{') {
      if (!depth) return p;
      --depth;
    }
  }
  return begin;
}

static const char *json_object_end(const char *start, const char *limit) {
  int depth = 0;
  int quoted = 0;
  int escaped = 0;
  for (const char *p = start; p < limit; ++p) {
    const char c = *p;
    if (quoted) {
      if (escaped) escaped = 0;
      else if (c == '\\') escaped = 1;
      else if (c == '"') quoted = 0;
      continue;
    }
    if (c == '"') quoted = 1;
    else if (c == '{') ++depth;
    else if (c == '}' && --depth == 0) return p + 1;
  }
  return limit;
}

typedef struct {
  char sku[256], sell_id[256], title[256], description[512];
  char formatted_price[128], metadata_url[512];
  JniCatalogItem item;
} ParsedCatalogItem;

static void catalog_sku_from_desc2(const char *desc2, const char *sell_id,
                                   char *out, size_t capacity) {
  if (!desc2 || !*desc2 || !capacity) return;
  char marker[320];
  snprintf(marker, sizeof(marker), "skuAlias_%s=", sell_id ? sell_id : "");
  const char *value = strstr(desc2, marker);
  if (!value) return;
  value += strlen(marker);
  size_t used = 0;
  while (value[used] && value[used] != ';' && value[used] != ',' &&
         !isspace((unsigned char)value[used]) && used + 1 < capacity) {
    out[used] = value[used];
    ++used;
  }
  out[used] = 0;
}

static jobject parse_catalog_response(const unsigned char *data, size_t length,
                                      int *item_count) {
  if (item_count) *item_count = 0;
  if (!data || !length || length > 4 * 1024 * 1024) return NULL;
  char *json = malloc(length + 1);
  if (!json) return NULL;
  memcpy(json, data, length);
  json[length] = 0;

  ParsedCatalogItem parsed[128];
  JniCatalogItem items[128];
  int count = 0;
  const char *cursor = json;
  const char *limit = json + length;
  while (count < (int)(sizeof(parsed) / sizeof(parsed[0]))) {
    const char *sell_key = strstr(cursor, "\"sellId\"");
    if (!sell_key || sell_key >= limit) break;
    const char *object = json_object_start(json, sell_key);
    const char *end = json_object_end(object, limit);
    if (end <= object) break;

    ParsedCatalogItem *entry = &parsed[count];
    memset(entry, 0, sizeof(*entry));
    const char *sell_keys[] = {"sellId", "sellID", "id"};
    const char *sku_keys[] = {"sku", "itemSku", "productId"};
    const char *title_keys[] = {"title", "name"};
    const char *description_keys[] = {"description", "desc", "desc1"};
    const char *price_text_keys[] = {"formattedPrice", "formatted_price",
                                     "priceWithCurrencyAndFormat"};
    const char *metadata_keys[] = {"packUrl", "metaDataUrl", "metadataUrl",
                                   "metadata"};
    json_copy_string(object, end, sell_keys, 3, entry->sell_id,
                     sizeof(entry->sell_id));
    if (!entry->sell_id[0]) { cursor = end; continue; }
    json_copy_string(object, end, sku_keys, 3, entry->sku, sizeof(entry->sku));
    json_copy_string(object, end, title_keys, 2, entry->title, sizeof(entry->title));
    json_copy_string(object, end, description_keys, 3, entry->description,
                     sizeof(entry->description));
    json_copy_string(object, end, price_text_keys, 3, entry->formatted_price,
                     sizeof(entry->formatted_price));
    json_copy_string(object, end, metadata_keys, 4, entry->metadata_url,
                     sizeof(entry->metadata_url));

    char desc2[1024] = {0};
    const char *desc2_keys[] = {"desc2"};
    json_copy_string(object, end, desc2_keys, 1, desc2, sizeof(desc2));
    if (!entry->sku[0]) catalog_sku_from_desc2(desc2, entry->sell_id,
                                               entry->sku, sizeof(entry->sku));
    if (!entry->sku[0]) {
      strncpy(entry->sku, entry->sell_id, sizeof(entry->sku) - 1);
      entry->sku[sizeof(entry->sku) - 1] = 0;
    }

    double price = 0;
    const char *price_keys[] = {"price", "priceDecimal", "amount"};
    json_number(object, end, price_keys, 3, &price);
    int consumable = 0;
    const char *consumable_keys[] = {"consumable"};
    json_boolean(object, end, consumable_keys, 1, &consumable);
    char offer_type[64] = {0};
    const char *offer_keys[] = {"offerType", "itemType", "type"};
    json_copy_string(object, end, offer_keys, 3, offer_type, sizeof(offer_type));
    const int subscription = !strcasecmp(offer_type, "Subscription");
    int is_free = 0;
    const char *free_keys[] = {"isFree", "free"};
    if (!json_boolean(object, end, free_keys, 2, &is_free)) is_free = price <= 0;

    entry->item.sku = entry->sku;
    entry->item.sell_id = entry->sell_id;
    entry->item.title = entry->title;
    entry->item.description = entry->description;
    entry->item.formatted_price = entry->formatted_price;
    entry->item.metadata_url = entry->metadata_url;
    entry->item.price = (float)price;
    entry->item.item_type = subscription ? 2 : (consumable ? 0 : 1);
    entry->item.is_free = is_free;

    int duplicate = 0;
    for (int i = 0; i < count; ++i)
      if (!strcmp(parsed[i].sell_id, entry->sell_id)) duplicate = 1;
    if (!duplicate) items[count++] = entry->item;
    cursor = end;
  }

  jobject list = jni_make_catalog_list(items, count);
  if (item_count) *item_count = count;
  free(json);
  return list;
}

static int copy_json_array(const unsigned char *data, size_t length,
                           const char *key, char *out, size_t capacity) {
  if (out && capacity) out[0] = 0;
  if (!data || !length || length > 4 * 1024 * 1024 || !key ||
      !out || capacity < 2)
    return 0;

  char *json = malloc(length + 1);
  if (!json) return 0;
  memcpy(json, data, length);
  json[length] = 0;
  const char *begin = json;
  const char *limit = json + length;
  const char *start = json_value_start(begin, limit, key);
  int found = 0;
  if (start && *start == '[') {
    int depth = 0;
    int quoted = 0;
    int escaped = 0;
    const char *end = start;
    for (; end < limit; ++end) {
      const char c = *end;
      if (quoted) {
        if (escaped) escaped = 0;
        else if (c == '\\') escaped = 1;
        else if (c == '"') quoted = 0;
        continue;
      }
      if (c == '"') quoted = 1;
      else if (c == '[') ++depth;
      else if (c == ']' && --depth == 0) {
        ++end;
        const size_t copy = (size_t)(end - start) < capacity - 1 ?
                            (size_t)(end - start) : capacity - 1;
        memcpy(out, start, copy);
        out[copy] = 0;
        found = 1;
        break;
      }
    }
  }
  free(json);
  return found;
}

static int parse_synergy_product(const unsigned char *data, size_t length,
                                 char *out, size_t capacity) {
  if (out && capacity) out[0] = 0;
  if (!data || !length || length > 4 * 1024 * 1024 || !out || capacity < 8)
    return 0;

  char *json = malloc(length + 1);
  if (!json) return 0;
  memcpy(json, data, length);
  json[length] = 0;

  const char *begin = json;
  const char *limit = json + length;
  const char *server_data = json_value_start(begin, limit, "serverData");
  int found = 0;
  if (server_data && *server_data == '[') {
    const char *cursor = server_data + 1;
    while (cursor < limit && *cursor != ']') {
      while (cursor < limit && (*cursor == ',' || isspace((unsigned char)*cursor)))
        ++cursor;
      if (cursor >= limit || *cursor == ']') break;
      if (*cursor != '{') { ++cursor; continue; }

      const char *object_end = json_object_end(cursor, limit);
      if (object_end <= cursor) break;
      char key[128];
      char value[1024];
      const char *key_names[] = {"key"};
      const char *value_names[] = {"value"};
      if (json_copy_string(cursor, object_end, key_names, 1,
                           key, sizeof(key)) &&
          !strcmp(key, "synergy.product") &&
          json_copy_string(cursor, object_end, value_names, 1,
                           value, sizeof(value)) &&
          (!strncmp(value, "https://", 8) || !strncmp(value, "http://", 7))) {
        snprintf(out, capacity, "%s", value);
        found = 1;
        break;
      }
      cursor = object_end;
    }
  }
  free(json);
  return found;
}

static int append_url_parameter(char *url, size_t capacity, size_t *used,
                                int *first, const char *key, const char *value) {
  static const char hex[] = "0123456789ABCDEF";
  if (!url || !used || !first || !key || !value || *used >= capacity)
    return 0;
  int count = snprintf(url + *used, capacity - *used, "%c%s=",
                       *first ? '?' : '&', key);
  if (count < 0 || (size_t)count >= capacity - *used) return 0;
  *used += (size_t)count;
  for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
    const unsigned char c = *p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      if (*used + 1 >= capacity) return 0;
      url[(*used)++] = (char)c;
    } else {
      if (*used + 3 >= capacity) return 0;
      url[(*used)++] = '%';
      url[(*used)++] = hex[c >> 4];
      url[(*used)++] = hex[c & 15];
    }
  }
  url[*used] = 0;
  *first = 0;
  return 1;
}

static const char *synergy_sell_id(void) {
  return "com.ea.pvz2";
}

static int start_synergy_director_request(void) {
  const char *uuid = pvz2_device_uuid();
  /* Android Emulator API 34 Build values. The Director's four fixed values
   * remain the game's production identity; only these six fields are varied. */
  const char *device_string = "sdk_gphone64_x86_64";
  const char *device_codename = "emu64xa";
  const char *manufacturer = "Google";
  const char *model = "sdk_gphone64_x86_64";
  const char *brand = "google";
  const char *fingerprint =
      "google/sdk_gphone64_x86_64/emu64xa:34/UE1A.230829.036.A1/11053244:userdebug/dev-keys";
  char url[2048];
  size_t used = (size_t)snprintf(
      url, sizeof(url),
      "https://syn-dir.sn.eamobile.com/director/api/android/getDirectionByPackage");
  int first = 1;
  if (used >= sizeof(url) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "packageId",
                            "com.ea.game.pvz2_row") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceString",
                            device_string) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceCodename",
                            device_codename) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "manufacturer",
                            manufacturer) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "model",
                            model) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "brand",
                            brand) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "fingerprint",
                            fingerprint) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "serverEnvironment",
                            "production") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "sdkVersion",
                            "1.64.0.27") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "apiVer", "1.0.0")) {
    catalog_http_in_flight = 0;
    publish_empty_catalog_result("Director URL could not be built");
    debugPrintf("SYNERGY DIRECTOR: failed to build request URL\n");
    return 0;
  }

  char headers[512];
  const int header_length = snprintf(
      headers, sizeof(headers),
      "Accept: application/json\n"
      "SDK-VERSION: 1.64.0.27\n"
      "SDK-TYPE: Nimble\n"
      "EA-SELL-ID: %s\n"
      "EAM-USER-ID: %s\n", synergy_sell_id(), uuid ? uuid : "0");
  HttpRequest *request = calloc(1, sizeof(*request));
  if (header_length < 0 || header_length >= (int)sizeof(headers) ||
      !request || !http_request_start(request, url, "GET", headers, NULL, 0, 30000)) {
    http_request_free(request);
    catalog_http_in_flight = 0;
    publish_empty_catalog_result("Director request could not be started");
    debugPrintf("SYNERGY DIRECTOR: failed to start request\n");
    return 0;
  }
  request->kind = HTTP_SYNERGY_DIRECTOR;
  catalog_http_in_flight = 1;
  return 1;
}

static void start_mtx_catalog_request(void) {
  const char *uuid = pvz2_device_uuid();
  const SwitchLocaleInfo locale = get_switch_locale();
  char url[2048];
  const size_t product_length = strlen(synergy_product_url);
  const int url_length = snprintf(
      url, sizeof(url), "%s%sproduct/api/core/getAvailableItems",
      synergy_product_url,
      product_length && synergy_product_url[product_length - 1] == '/' ? "" : "/");
  size_t used = url_length > 0 ? (size_t)url_length : 0;
  int first = 1;
  if (url_length < 0 || url_length >= (int)sizeof(url) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "masterSellId",
                            synergy_sell_id()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "typeSubstr", "1") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "apiVer", "1.0.0") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "ver", GAME_VERSION) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "uid", uuid ? uuid : "0") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "sdkVer", "1.64.0.27") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "langCode", locale.language) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "includeOfferType", "true")) {
    catalog_http_in_flight = 0;
    publish_empty_catalog_result("catalog URL could not be built");
    debugPrintf("SYNERGY CATALOG: failed to build request URL\n");
    return;
  }

  char headers[512];
  const int header_length = snprintf(
      headers, sizeof(headers),
      "Accept: application/json\n"
      "SDK-VERSION: 1.64.0.27\n"
      "SDK-TYPE: Nimble\n"
      "EAM-USER-ID: %s\n"
      "EA-SELL-ID: %s\n", uuid ? uuid : "0", synergy_sell_id());
  debugPrintf("SYNERGY CATALOG: base=%s masterSellId=%s typeSubstr=1 "
              "apiVer=1.0.0 ver=%s uid=%s sdkVer=1.64.0.27 langCode=%s "
              "includeOfferType=true finalURL=%s\n",
              synergy_product_url, synergy_sell_id(), GAME_VERSION,
              uuid ? uuid : "0", locale.language, url);
  HttpRequest *request = calloc(1, sizeof(*request));
  if (header_length < 0 || header_length >= (int)sizeof(headers) ||
      !request || !http_request_start(request, url, "GET", headers, NULL, 0, 30000)) {
    http_request_free(request);
    catalog_http_in_flight = 0;
    publish_empty_catalog_result("catalog request could not be started");
    debugPrintf("SYNERGY CATALOG: failed to start request\n");
    return;
  }
  request->kind = HTTP_MTX;
  catalog_http_in_flight = 1;
}

/* PvZ2Web's savedelta endpoint is an Android account/cloud synchronization
 * path, not the local save writer. On Switch there is no authenticated Google
 * Play/cloud provider, yet feeding a successful PlayerInfo savedelta response
 * back into OnlineDataPersistor causes it to merge/rewrite pp.dat before
 * MainMenu. The local PlayerInfo was already loaded and serialized into the
 * outgoing request. Treat only PlayerInfo savedelta transactions as offline so
 * PVZ2 follows its normal local-save fallback. */
static int buffer_contains_literal(const void *data, int length, const char *literal) {
  if (!data || length <= 0 || !literal || !*literal) return 0;
  const size_t needle_length = strlen(literal);
  if (!needle_length || needle_length > (size_t)length) return 0;
  const unsigned char *bytes = (const unsigned char *)data;
  for (size_t i = 0; i + needle_length <= (size_t)length; i++)
    if (!memcmp(bytes + i, literal, needle_length)) return 1;
  return 0;
}

static int is_player_savedelta_sync(const char *url, const char *method,
                                    const void *body, int body_length) {
  return url && method && !strcmp(method, "POST") &&
      !strcmp(url, "https://pvz2-prd.popcap.com/PvZ2Web") &&
      buffer_contains_literal(body, body_length, "\"execute\":\"savedelta\"") &&
      buffer_contains_literal(body, body_length, "\"objclass\":\"PlayerInfo\"");
}

static void start_http_transaction(jobject self) {
  const jlong transaction = jni_object_long(self);
  const char *url = jni_http_url(self);
  const char *method = jni_http_method(self);
  if (!transaction || !url || !*url) {
    debugPrintf("JNI HTTP: invalid transaction URL\n");
    if (transaction) queue_java_callback("HttpTransactionError", self, transaction);
    return;
  }
  int request_body_len = 0;
  const void *request_body = jni_http_body(self, &request_body_len);
  if (is_player_savedelta_sync(url, method, request_body, request_body_len)) {
    /* Model the unavailable Android account sync as a normal network failure.
     * This does not touch pp.dat, profile counts, UIDs, or progression. */
    jni_http_set_status(self, 0);
    queue_java_callback("HttpTransactionError", self, transaction);
    return;
  }
#if PVZ2_ENABLE_VERBOSE_RUNTIME_LOG
  if (request_body && request_body_len > 0 && strstr(url, "pvz2-prd.popcap.com"))
    http_log_body("bootstrap body", request_body, (size_t)request_body_len);
#endif
  debugPrintf("JNI HTTP: %s %s (%d-byte body) [async]\n", method, url, request_body_len);
  HttpRequest *request = calloc(1, sizeof(*request));
  if (!request || !http_request_start(request, url, method, jni_http_headers(self), request_body,
                                      request_body_len, jni_http_timeout(self))) {
    http_request_free(request);
    jni_http_set_status(self, 0);
    queue_java_callback("HttpTransactionError", self, transaction);
    return;
  }
  request->kind = HTTP_ANDROID;
  request->self = self;
  request->callback = transaction;
}

static char *headers_from_pairs(jobject pairs) {
  const int count = jni_object_array_length(pairs) & ~1;
  size_t size = 1;
  for (int i = 0; i < count; i += 2) {
    const char *key = jni_cstr(jni_object_array_get(pairs, i));
    const char *value = jni_cstr(jni_object_array_get(pairs, i + 1));
    if (key && value) size += strlen(key) + strlen(value) + 3;
  }
  char *headers = malloc(size);
  if (!headers) return NULL;
  size_t used = 0;
  for (int i = 0; i < count; i += 2) {
    const char *key = jni_cstr(jni_object_array_get(pairs, i));
    const char *value = jni_cstr(jni_object_array_get(pairs, i + 1));
    if (!key || !value) continue;
    used += (size_t)snprintf(headers + used, size - used, "%s: %s\n", key, value);
  }
  headers[used] = 0;
  return headers;
}

static void start_glu_http(jobject self, const jvalue *argv) {
  const char *url = jni_cstr(argv[0].l);
  const char *method = jni_cstr(argv[1].l);
  const char *body = jni_cstr(argv[3].l);

  char *headers = headers_from_pairs(argv[2].l);
  HttpRequest *request = calloc(1, sizeof(*request));
  const int started = request && headers && http_request_start(
      request, url, method, headers, body, body ? (int)strlen(body) : 0, (long)argv[4].j);
  if (started) {
    request->kind = HTTP_GLU;
    request->self = self;
    request->callback = argv[5].j;
    debugPrintf("JNI Glu HTTP: %s %s [async]\n", method ? method : "GET", url);
  } else {
    http_request_free(request);
    queue_java_http_callback(self, argv[5].j, 0, NULL, 0);
  }
  free(headers);
}

static int download_destination_is_safe(const char *path) {
  static const char prefix[] = DATA_DIR "/payloads/";
  return path && !strncmp(path, prefix, sizeof(prefix) - 1) &&
      path[sizeof(prefix) - 1] && !strstr(path, "..");
}

static void start_glu_download(jobject self, const jvalue *argv) {
  const char *url = jni_cstr(argv[0].l);
  const char *destination = jni_cstr(argv[1].l);
  /* The original Android bridge passes HTTPOptions first and the heap-backed
   * C++ completion function second. onDownloadResponse receives that second
   * long as its callback token. */
  const jlong options = argv[2].j;
  const jlong callback = argv[3].j;

  if (!url || strncmp(url, "https://", 8) ||
      !download_destination_is_safe(destination)) {
    debugPrintf("JNI downloadFile: refused url=%s destination=%s\n",
                url ? url : "(null)", destination ? destination : "(null)");
    queue_java_download_callback(self, callback, 0, destination);
    return;
  }

  debugPrintf("JNI downloadFile: %s -> %s (options=%lld callback=%lld) [async]\n",
              url, destination, (long long)options, (long long)callback);
  HttpRequest *request = calloc(1, sizeof(*request));
  if (!request || !(request->destination = strdup(destination)) ||
      !http_request_start(request, url, "GET", "", NULL, 0, 30000)) {
    http_request_free(request);
    queue_java_download_callback(self, callback, 0, destination);
    return;
  }
  request->kind = HTTP_DOWNLOAD;
  request->self = self;
  request->callback = callback;
}

static int http_save_download(HttpRequest *request, long status) {
  int result = status >= 200 && status < 400;
  char temporary[768];
  if (result && request->destination &&
      snprintf(temporary, sizeof(temporary), "%s.tmp", request->destination) <
          (int)sizeof(temporary)) {
    FILE *file = fopen_fake(temporary, "wb");
    if (!file) {
      result = 0;
    } else {
      const size_t written = fwrite(request->response.data, 1, request->response.size, file);
      const int close_result = fclose_fake(file);
      if (written != request->response.size || close_result != 0 ||
          rename_fake(temporary, request->destination) != 0)
        result = 0;
    }
    if (!result) remove_fake(temporary);
  } else {
    result = 0;
  }
  debugPrintf("JNI downloadFile: %s -> %ld (%zu bytes), %s\n", request->url, status,
              request->response.size, result ? "saved" : "failed");
  return result;
}

static void http_complete(HttpRequest *request, CURLcode code) {
  long status = 0;
  curl_easy_getinfo(request->curl, CURLINFO_RESPONSE_CODE, &status);
  log_account_http_response(request, code, status);
  const int ok = code == CURLE_OK && status >= 200 && status < 400 &&
      request->response.size <= INT_MAX;

  if (request->kind == HTTP_ANDROID) {
    jni_http_set_status(request->self, (int)status);
    jni_http_set_response_headers(request->self, request->response_headers.data,
                                  request->response_headers.size);
    if (ok) {
      debugPrintf("JNI HTTP: %s -> %ld (%zu bytes)\n", request->url, status,
                  request->response.size);
      queue_java_callback("HttpReceivedResponse", request->self, request->callback);
      if (request->response.size)
        queue_java_data_callback("HttpReceivedData", request->self, request->callback,
                                 request->response.data, (int)request->response.size);
      queue_java_callback("HttpTransactionComplete", request->self, request->callback);
    } else {
      debugPrintf("JNI HTTP: %s failed (curl=%d status=%ld)\n", request->url, code, status);
      if (code == CURLE_OK) http_log_body("error body", request->response.data, request->response.size);
      queue_java_callback("HttpTransactionError", request->self, request->callback);
    }
  } else if (request->kind == HTTP_SYNERGY_DIRECTOR) {
    char server_data[16384];
    if (!copy_json_array(request->response.data, request->response.size,
                         "serverData", server_data, sizeof(server_data)))
      snprintf(server_data, sizeof(server_data), "<missing>");
    debugPrintf("SYNERGY DIRECTOR: status=%ld\n", status);
    debugPrintf("SYNERGY DIRECTOR: serverData=%s\n", server_data);
    if (ok) capture_director_identity(request->response.data, request->response.size);
    if (!ok || !parse_synergy_product(request->response.data,
                                      request->response.size,
                                      synergy_product_url,
                                      sizeof(synergy_product_url))) {
      catalog_http_in_flight = 0;
      debugPrintf("SYNERGY DIRECTOR: synergy.product=<missing>\n");
      publish_local_bootstrap_catalog("no synergy.product");
      return;
    }
    debugPrintf("SYNERGY DIRECTOR: synergy.product=%s\n", synergy_product_url);
    start_mtx_catalog_request();
  } else if (request->kind == HTTP_MTX) {
    catalog_http_in_flight = 0;
    if (ok) {
      int item_count = 0;
      jobject list = parse_catalog_response(request->response.data,
                                            request->response.size, &item_count);
      if (list && item_count > 0) {
        catalog_items = list;
        catalog_refresh_ready_flag = 1;
        catalog_refresh_failed_flag = 0;
      } else if (list) {
        publish_local_bootstrap_catalog("catalog response contained zero items");
      } else {
        publish_local_bootstrap_catalog("catalog response could not be parsed");
      }
    } else {
      publish_local_bootstrap_catalog("catalog request unavailable");
    }
  } else if (request->kind == HTTP_GLU) {
    debugPrintf("JNI Glu HTTP: %s %s -> %ld (%zu bytes)\n", request->method, request->url,
                status, request->response.size);
    if (strstr(request->url, "name=SDK_CONFIG_") && request->response.data && request->response.size) {
      const int shown = request->response.size > 512 ? 512 : (int)request->response.size;
      debugPrintf("JNI Glu HTTP: config response (%d/%zu bytes): %.*s\n", shown,
                  request->response.size, shown, (const char *)request->response.data);
    }
    queue_java_http_callback(request->self, request->callback, ok ? (int)status : 0,
                             request->response.data, ok ? (int)request->response.size : 0);
  } else if (request->kind == HTTP_ACCOUNT_REGISTER) {
    if (ok) {
      debugPrintf("ACCOUNT REGISTER: EA accepted request status=%ld; "
                  "verification-code step is still pending\n", status);
    } else {
      debugPrintf("ACCOUNT REGISTER: EA request failed curl=%d status=%ld\n",
                  code, status);
    }
  } else {
    const int saved = ok && http_save_download(request, status);
    queue_java_download_callback(request->self, request->callback, saved ? (int)status : 0,
                                 request->destination);
  }
}

static void http_pump(void) {
  if (catalog_http_start_pending && !catalog_http_in_flight) {
    catalog_http_start_pending = 0;
    start_synergy_director_request();
  }
  if (!http_multi) return;
  int running = 0;
  CURLMcode multi;
  do { multi = curl_multi_perform(http_multi, &running); } while (multi == CURLM_CALL_MULTI_PERFORM);
  if (multi != CURLM_OK) {
    debugPrintf("JNI HTTP: curl multi pump failed (%d)\n", multi);
    return;
  }

  int messages = 0;
  CURLMsg *message;
  while ((message = curl_multi_info_read(http_multi, &messages))) {
    if (message->msg != CURLMSG_DONE) continue;
    char *private_data = NULL;
    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &private_data);
    HttpRequest *request = (HttpRequest *)private_data;
    curl_multi_remove_handle(http_multi, message->easy_handle);
    if (!request) continue;
    http_complete(request, message->data.result);
    for (int i = 0; i < http_request_count; ++i) {
      if (http_requests[i] != request) continue;
      memmove(&http_requests[i], &http_requests[i + 1],
              (size_t)(http_request_count - i - 1) * sizeof(http_requests[0]));
      http_request_count--;
      break;
    }
    http_request_free(request);
  }
}

void pvz2_http_pump(void) {
  http_pump();
}

static jboolean process_events(jobject buffer) {
  http_pump();
  pump_java_timers();
  dispatch_java_callbacks();
  int capacity = 0;
  unsigned char *bytes = jni_buffer_data(buffer, &capacity);
  if (!bytes || capacity < 16) return 0;

  int count = 0;
  size_t offset = 16;
  const uint32_t marker = 0xdeadbeef;

  if (back_edge_pressed() && offset + 16 <= (size_t)capacity) {
    const int type = 5;
    put_bytes(bytes + offset, &type, 4); offset += 4;
    for (int i = 0; i < 3; i++) { put_bytes(bytes + offset, &marker, 4); offset += 4; }
    count++;
  }

  PtrEvent events[16];
  const int room = ((size_t)capacity - offset) / 48;
  const int n = platform_poll_pointers(events, room < 16 ? room : 16);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const double timestamp = (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;

  for (int i = 0; i < n; i++) {
    const int type = 0;
    const int touch_id = events[i].id;
    const int phase = events[i].phase == PTR_DOWN ? 0 :
                      events[i].phase == PTR_MOVE ? 1 : 3;
    /* UI_ProcessEvents is the Android View callback. It expects View pixels
     * (mOrigScreenWidth/Height = 1280x720); LawnApp converts them internally
     * to its 1365x768 layout. Passing pre-scaled coordinates double-transforms
     * every hit target and breaks tutorial interaction. */
    const int values[] = { type, touch_id, (int)events[i].x, (int)events[i].y,
                           (int)events[i].previous_x, (int)events[i].previous_y, 1 };
    for (int j = 0; j < 7; j++) { put_bytes(bytes + offset, &values[j], 4); offset += 4; }
    put_bytes(bytes + offset, &timestamp, 8); offset += 8;
    put_bytes(bytes + offset, &phase, 4); offset += 4;
    put_bytes(bytes + offset, &marker, 4); offset += 4;
    put_bytes(bytes + offset, &marker, 4); offset += 4;
    count++;
  }

  put_bytes(bytes, &count, 4);
  return 0; /* false means the Java-side queue was completely drained */
}

void account_email_prompt(void) {
  const u64 now = armGetSystemTick();
  const u64 hz = armGetSystemTickFreq();
  static u64 last_prompt;
  if (last_prompt && hz && now - last_prompt < hz / 2u) {
    debugPrintf("ACCOUNT EMAIL: duplicate native prompt suppressed\n");
    return;
  }
  last_prompt = now;

  /* The native Nexus component is not constructed by the Android class
   * initializer in the fake-JNI runtime. Keep the login button useful with a
   * normal WebApplet page while that construction path is unavailable. */
  debugPrintf("ACCOUNT WEB: native account component unavailable; opening "
              "EA sign-in page\n");
  switch_ea_login_webview("https://www.ea.com/login", NULL);
  debugLogFlush();
}

#define NIMBLE_JAVA_COMPONENT_CAPACITY 64
#define NIMBLE_JAVA_COMPONENT_ID_CAPACITY 128
static char nimble_java_component_ids[NIMBLE_JAVA_COMPONENT_CAPACITY]
                                     [NIMBLE_JAVA_COMPONENT_ID_CAPACITY];
static size_t nimble_java_component_count;
static int nimble_java_setup_started;

static void nimble_setup_java_component(const char *component_id) {
  if (!component_id || !*component_id) return;
  jobject component = jni_make_object_class_string(
      "com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent",
      component_id);
  if (!component) {
    debugPrintf("NIMBLE COMPONENT JAVA: allocation failed id=%s\n",
                component_id);
    return;
  }
  debugPrintf("NIMBLE COMPONENT JAVA: setup id=%s\n", component_id);
  nimble_setup_cpp_component(component);
  jni_free_wrapper(component);
}

static void nimble_queue_java_component(jobject component_id_object) {
  const char *component_id = jni_cstr(component_id_object);
  if (!component_id || !*component_id) {
    debugPrintf("NIMBLE COMPONENT JAVA: ignored empty registrar id\n");
    return;
  }
  for (size_t i = 0; i < nimble_java_component_count; ++i) {
    if (!strcmp(nimble_java_component_ids[i], component_id)) return;
  }
  if (nimble_java_component_count >= NIMBLE_JAVA_COMPONENT_CAPACITY) {
    debugPrintf("NIMBLE COMPONENT JAVA: registrar queue full; id=%s ignored\n",
                component_id);
    return;
  }
  char *slot = nimble_java_component_ids[nimble_java_component_count++];
  snprintf(slot, NIMBLE_JAVA_COMPONENT_ID_CAPACITY, "%s", component_id);
  debugPrintf("NIMBLE COMPONENT JAVA: queued id=%s count=%zu\n",
              slot, nimble_java_component_count);
  if (nimble_java_setup_started) nimble_setup_java_component(slot);
}

static void nimble_setup_required_native_components(void) {
  /* The Android class initializer normally supplies this list to
   * NimbleCppComponentRegistrar. A fake JNI class has no DEX static
   * initializer, so reproduce only the platform-neutral components needed by
   * the Nexus account flow. Each native setup call is a no-op when the named
   * component is absent. */
  static const char *const component_ids[] = {
      "com.ea.nimble.cpp.networkservice",
      "com.ea.nimble.cpp.networkclientmanager",
      "com.ea.nimble.cpp.trackingservice",
      "com.ea.nimble.cpp.agecomplianceservice",
      "com.ea.nimble.cpp.nexus.jwk",
      "com.ea.nimble.cpp.nexusservice",
      "com.ea.nimble.cpp.nexus.eaaccount",
  };
  debugPrintf("NIMBLE COMPONENT JAVA: applying %zu required native components\n",
              sizeof(component_ids) / sizeof(component_ids[0]));
  for (size_t i = 0; i < sizeof(component_ids) / sizeof(component_ids[0]); ++i)
    nimble_setup_java_component(component_ids[i]);
}

jvalue pvz2_upcall(const char *cls, const char *name, const char *sig,
                   jobject self, va_list ap) {
  jvalue argv[12] = {{0}};
  parse_args(sig, ap, argv, 12);
  return pvz2_upcall_args(cls, name, sig, self, argv);
}

jvalue pvz2_upcall_args(const char *cls, const char *name, const char *sig,
                        jobject self, const jvalue *argv) {
  static const jvalue empty_argv[12] = {{0}};
  if (!argv) argv = empty_argv;

  /* Record account contracts without logging string contents or credentials. */
  if ((cls && (strstr(cls, "Cloud") || strstr(cls, "cloud") ||
               strstr(cls, "Account") || strstr(cls, "account"))) ||
      (name && (strstr(name, "Email") || strstr(name, "email") ||
                strstr(name, "Auth") || strstr(name, "auth")))) {
    static unsigned account_upcalls;
    if (account_upcalls++ < 96)
      debugPrintf("ACCOUNT UPCALL: class=%s name=%s sig=%s self=%p args={%p,%p,%p,%p}\n",
                  cls ? cls : "?", name ? name : "?", sig ? sig : "", self,
                  argv[0].l, argv[1].l, argv[2].l, argv[3].l);
  }

  if (!strcmp(name, "FrameworkInfo_SysGetMainExpansionFilePath")) {
    return (jvalue){.l = jni_make_string(DATA_DIR "/" OBB_NAME)};
  }

  if (has(name, "ResourceFolder"))
    return (jvalue){.l = jni_make_string(DATA_DIR "/assets")};
  if (has(name, "UserDataFolder") || has(name, "AppSupportDataFolder") ||
      has(name, "ExternalStorageDirectory") || has(name, "FilesDir") ||
      has(name, "StoragePath") || has(name, "DataDirectory"))
    return (jvalue){.l = jni_make_string(DATA_DIR)};
  if (has(name, "CacheDataFolder") || has(name, "CacheDir"))
    return (jvalue){.l = jni_make_string(DATA_DIR "/cache")};

  if (!strcmp(name, "Resources_GetAssetFileInfo") && argv[0].l) {
    const char *asset = jni_cstr(argv[0].l);
    static char path[1024];
    snprintf(path, sizeof(path), DATA_DIR "/assets/%s", asset ? asset : "");
    return (jvalue){.l = jni_make_string(path)};
  }
  if (!strcmp(name, "Resources_GetAssetFileSize") && argv[0].l) {
    FILE *f = fopen_fake(jni_cstr(argv[0].l), "rb");
    if (!f) return (jvalue){0};
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose_fake(f);
    return (jvalue){.j = size > 0 ? size : 0};
  }

  if (has(name, "PackageName") || has(name, "BundleName"))
    return (jvalue){.l = jni_make_string("com.ea.game.pvz2_row")};
  if (has(name, "ProductVersionString"))
    return (jvalue){.l = jni_make_string(game_version())};
  if (has(name, "ProductVersion")) return (jvalue){.i = game_version_code()};
  if (has(name, "UserLocale"))
    return (jvalue){.l = jni_make_string(get_switch_locale().locale)};
  if (has(name, "Language"))
    return (jvalue){.l = jni_make_string(get_switch_locale().language)};
  if (has(name, "CountryCode"))
    return (jvalue){.l = jni_make_string(get_switch_locale().country)};
  if (has(name, "CurrencyCode")) return (jvalue){.l = jni_make_string("USD")};
  if (has(name, "CurrencySymbol")) return (jvalue){.l = jni_make_string("$")};
  if (has(name, "DeviceName") || has(name, "HardwareModel"))
    return (jvalue){.l = jni_make_string("Nintendo Switch")};
  if (has(name, "OSVersion")) return (jvalue){.l = jni_make_string("Horizon")};
  if (has(name, "DeviceID") || has(name, "AndroidID") || has(name, "InstallID") ||
      has(name, "UUID"))
    return (jvalue){.l = jni_make_string(device_uuid())};
  if (has(cls, "csdk/glucentralservices/util/AndroidPlatform")) {
    if (!strcmp(name, "registerApplicationEvent")) {
      /* Android delivers this from ActivityLifecycleCallbacks.  Switch has
       * already entered its active state before the first frame, so deliver
       * the equivalent event immediately after Glu registers. */
      void *callback = jni_registered("onApplicationEvent");
      if (callback) {
        debugPrintf("JNI application event: applicationDidBecomeActive\n");
        ((void (*)(JNIEnv, jobject, jstring))callback)(
            fake_env, self, jni_make_string("applicationDidBecomeActive"));
      }
      return (jvalue){0};
    }
    if (!strcmp(name, "readAssetAsString"))
      return (jvalue){.l = read_asset_as_string(argv[0].l)};
    if (!strcmp(name, "readSharedProperty"))
      return (jvalue){.l = read_shared_property(argv[0].l)};
    if (!strcmp(name, "getDeviceTier")) return (jvalue){.i = 3};
    if (!strcmp(name, "getCpuCoreCount")) return (jvalue){.i = 4};
    if (!strcmp(name, "getGpuDeviceVendor"))
      return (jvalue){.l = jni_make_string("NVIDIA")};
    if (!strcmp(name, "getCpuName"))
      return (jvalue){.l = jni_make_string("ARM Cortex-A57")};
    if (!strcmp(name, "getScreenSize"))
      return (jvalue){.l = jni_make_string("1280x720")};
    if (!strcmp(name, "getETC2IfSupported"))
      return (jvalue){.l = jni_make_string("true")};
    if (!strcmp(name, "getFromStringStore"))
      return (jvalue){.l = get_string_store(argv[0].l)};
    if (!strcmp(name, "setToStringStore")) {
      set_string_store(argv[0].l, argv[1].l);
      return (jvalue){0};
    }
    if (!strcmp(name, "readProperties"))
      return (jvalue){.l = read_properties(argv[0].l, argv[1].l)};
    if (!strcmp(name, "writeProperties")) {
      write_properties(argv[0].l, argv[1].l);
      return (jvalue){0};
    }
    if (!strcmp(name, "sendHTTPRequest")) {
      start_glu_http(self, argv);
      return (jvalue){0};
    }
    if (!strcmp(name, "downloadFile")) {
      start_glu_download(self, argv);
      return (jvalue){0};
    }
    if (!strcmp(name, "generateUUID"))
      return (jvalue){.l = jni_make_string(device_uuid())};
    if (!strcmp(name, "removePrivateData"))
      return (jvalue){.z = remove_private_data(argv[0].l)};
  }
  if (has(cls, "com/popcap/SexyAppFramework/SexyAppFrameworkActivity") ||
      has(cls, "com.popcap.SexyAppFramework.SexyAppFrameworkActivity")) {
    if (!strcmp(name, "Config_ConfigKeyExists")) {
      const char *key = jni_cstr(argv[0].l);
      return (jvalue){.z = key && store_get(property_store, 64, "config", key) ?
          JNI_TRUE : JNI_FALSE};
    }
    if (!strcmp(name, "Config_ConfigReadString")) {
      const char *key = jni_cstr(argv[0].l);
      const char *value = key ? store_get(property_store, 64, "config", key) : NULL;
      return (jvalue){.l = jni_make_string(value ? value : "")};
    }
    if (!strcmp(name, "Config_ConfigWriteString")) {
      store_set(property_store, 64, "config", jni_cstr(argv[0].l),
                jni_cstr(argv[1].l));
      return (jvalue){.z = JNI_TRUE};
    }
    if (!strcmp(name, "Config_ConfigWriteBoolean")) {
      const char *key = jni_cstr(argv[0].l);
      if (!key) return (jvalue){.z = JNI_FALSE};
      const char *value = argv[1].z ? "1" : "0";
      store_set(property_store, 64, "config", key, value);
      debugPrintf("JNI config bool: %s <- %s\n", key,
                  argv[1].z ? "true" : "false");
      return (jvalue){.z = JNI_TRUE};
    }
    if (!strcmp(name, "Config_ConfigReadBoolean")) {
      const char *key = jni_cstr(argv[0].l);
      const char *value = key ? store_get(property_store, 64, "config", key) : NULL;
      if (!value && sig && strstr(sig, "Ljava/lang/String;Z"))
        return (jvalue){.z = argv[1].z};
      return (jvalue){.z = value && strcmp(value, "0") && strcasecmp(value, "false")};
    }
    if (!strcmp(name, "Config_ConfigWriteInteger")) {
      const char *key = jni_cstr(argv[0].l);
      char value[32];
      snprintf(value, sizeof(value), "%d", argv[1].i);
      if (!key) return (jvalue){.z = JNI_FALSE};
      store_set(property_store, 64, "config", key, value);
      return (jvalue){.z = JNI_TRUE};
    }
    if (!strcmp(name, "Config_ConfigReadInteger")) {
      const char *key = jni_cstr(argv[0].l);
      const char *value = key ? store_get(property_store, 64, "config", key) : NULL;
      if (!value && sig && strstr(sig, "Ljava/lang/String;I"))
        return (jvalue){.i = argv[1].i};
      return (jvalue){.i = value ? (jint)strtol(value, NULL, 10) : 0};
    }
    if (!strcmp(name, "getHasCrashReport")) return (jvalue){.z = JNI_FALSE};
    if (!strcmp(name, "StartTracing") || !strcmp(name, "StopTracing") ||
        !strcmp(name, "UI_DidRecieveFocus"))
      return (jvalue){0};
    if (!strcmp(name, "Info_SysGetIntentExtraDataString"))
      return (jvalue){.l = jni_make_string("")};
  }
  if (!strcmp(name, "privateFilePath"))
    return (jvalue){.l = jni_make_string(DATA_DIR)};
  if (!strcmp(name, "getApplicationID"))
    return (jvalue){.l = jni_make_string("com.ea.game.pvz2_row")};
  if (!strcmp(name, "getApplicationVersion"))
    return (jvalue){.l = jni_make_string(game_version())};
  if (!strcmp(name, "checkPrivateDirectoryExists")) return (jvalue){.z = JNI_TRUE};
  if (has(name, "Version") && sig && strstr(sig, "Ljava/lang/String;")) {
    debugPrintf("JNI version fallback: %s -> %s\n", name, game_version());
    return (jvalue){.l = jni_make_string(game_version())};
  }

  if ((has(cls, "gluanalytics/AnalyticsFactory") ||
       has(cls, "gluanalytics.AnalyticsFactory")) &&
      !strcmp(name, "createAnalytics"))
    return (jvalue){.l = jni_make_object_class("com/glu/plugins/gluanalytics/Analytics")};

  if ((has(cls, "csdk/glucentralservices/util/AndroidPlatform") ||
       has(cls, "csdk.glucentralservices.util.AndroidPlatform")) &&
      !strcmp(name, "md5")) {
    md5_direct_buffers(argv[0].l, argv[1].l);
    return (jvalue){0};
  }

  if (has(cls, "csdk/gluads/GluAdsNativeBridge") &&
      !strcmp(name, "setAdvertisingListener"))
    return (jvalue){0};

  if ((has(cls, "com/ea/nimble/ApplicationEnvironment") ||
       has(cls, "com.ea.nimble.ApplicationEnvironment")) &&
      !strcmp(name, "getComponent"))
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/IApplicationEnvironment")};
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment")) &&
      !strcmp(name, "setPlayerId")) {
    store_set(string_store, 32, "", "NimblePlayerId", jni_cstr(argv[0].l));
    return (jvalue){0};
  }
  if ((has(cls, "com/ea/nimble/SynergyEnvironment") ||
       has(cls, "com.ea.nimble.SynergyEnvironment")) &&
      !strcmp(name, "getComponent"))
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/ISynergyEnvironment")};
  if ((has(cls, "com/ea/nimble/ISynergyEnvironment") ||
       has(cls, "com.ea.nimble.ISynergyEnvironment")) &&
      !strcmp(name, "getSynergyId"))
    return (jvalue){.l = jni_make_string(device_uuid())};

  if ((has(cls, "com/ea/nimble/Network") || has(cls, "com.ea.nimble.Network")) &&
      !strcmp(name, "getComponent")) {
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/INetwork")};
  }
  if ((has(cls, "com/ea/nimble/INetwork") || has(cls, "com.ea.nimble.INetwork")) &&
      !strcmp(name, "getStatus")) {
    const int ordinal = nimble_network_status_ordinal();
    debugPrintf("JNI Nimble INetwork.getStatus -> %s\n",
                ordinal == 0 ? "connected" : "offline");
    return (jvalue){.l = jni_make_object_class_value(
        "com/ea/nimble/Network$Status", ordinal)};
  }
  if ((has(cls, "java/lang/Enum") || has(cls, "java.lang.Enum")) &&
      !strcmp(name, "ordinal"))
    return (jvalue){.i = (jint)jni_object_long(self)};

  if (has(cls, "GooglePlay/GooglePlayConnect")) {
    if (!strcmp(name, "Play_IsConnected")) {
      static int disconnected_logged;
      if (!disconnected_logged) {
        debugPrintf("JNI GooglePlayConnect.Play_IsConnected -> false (provider unavailable)\n");
        disconnected_logged = 1;
      }
      return (jvalue){.z = JNI_FALSE};
    }
    if (!strcmp(name, "Play_Connect") || !strcmp(name, "Play_Connect_Silent") ||
        !strcmp(name, "Play_Disconnect")) {
      debugPrintf("JNI GooglePlayConnect.%s -> provider unavailable\n", name);
      return (jvalue){0};
    }
  }

  if ((has(cls, "cloud/Cloud") || has(cls, "cloud.Cloud") ||
       has(cls, "Account") || has(cls, "account") || has(cls, "Auth") ||
       has(cls, "auth")) &&
      strcmp(name, "Cloud_attemptSilentSync"))
    debugPrintf("CLOUD UPCALL: class=%s method=%s sig=%s self=%p arg0=%p arg1=%p\n",
                cls ? cls : "?", name ? name : "?", sig ? sig : "", self,
                argv[0].l, argv[1].l);

  if (!strcmp(name, "Cloud_attemptSilentSync")) {
    /* No Google Play/cloud provider exists on Switch. Report that fact through
     * the log only. Calling the stock callbacks with a fabricated empty cloud
     * state enters PVZ2's Android cloud parser and faults at +0x24005ec. */
    debugPrintf("CLOUD SYNC: provider unavailable; callbacks suppressed\n");
    return (jvalue){0};
  }
  if (has(cls, "android/content/Context") && !strcmp(name, "getApplicationInfo"))
    return (jvalue){.l = jni_make_object_class("android/content/pm/ApplicationInfo")};
  if (has(cls, "android/media/AudioManager") &&
      (!strcmp(name, "isMusicActive") || !strcmp(name, "isBluetoothScoOn")))
    return (jvalue){.z = JNI_FALSE};
  if (has(cls, "AndroidSurfaceView") && !strcmp(name, "Graphics_SetFramerate"))
    return (jvalue){0};
  if (has(cls, "AndroidNotification")) {
    /* Android notification channels have no Switch equivalent.  Consume the
     * complete notification shim silently so neither the fake JNI path nor
     * the Android-side SharedNotificationManagerRequired warning reaches the
     * log. */
    return (jvalue){0};
  }

  if ((!strcmp(cls, "java/lang/Object") || !strcmp(cls, "java.lang.Object")) &&
      !strcmp(name, "load")) {
    /* The Android wrapper exposes this loader as an Object method in the
     * stripped Switch JNI surface. There is no Java library to load here;
     * consume it so account/cloud setup does not hit the generic fallback. */
    return (jvalue){0};
  }

  if (has(cls, "com.ea.nimble.Utility") && !strcmp(name, "registerReceiver"))
    return (jvalue){0};

  if ((has(cls, "NimbleCppComponentRegistrar") &&
       !has(cls, "$NimbleCppComponent")) && !strcmp(name, "register")) {
    nimble_queue_java_component(argv[0].l);
    return (jvalue){0};
  }

  if (has(cls, "NimbleCppComponentRegistrar") &&
      !strcmp(name, "getComponentId")) {
    /* Native setup calls the registrar's static helper with the Java wrapper
     * as its first argument. It is not an instance call on the wrapper. */
    const char *component_id = jni_object_string(argv[0].l ? argv[0].l : self);
    debugPrintf("NIMBLE COMPONENT JAVA: getComponentId -> %s\n",
                component_id ? component_id : "<empty>");
    return (jvalue){.l = jni_make_string(component_id ? component_id : "")};
  }

  if ((has(cls, "com/ea/nimble/Base") || has(cls, "com.ea.nimble.Base")) &&
      !strcmp(name, "setupNimble")) {
    nimble_java_setup_started = 1;
    debugPrintf("NIMBLE COMPONENT JAVA: Base.setupNimble count=%zu\n",
                nimble_java_component_count);
    for (size_t i = 0; i < nimble_java_component_count; ++i)
      nimble_setup_java_component(nimble_java_component_ids[i]);
    nimble_setup_required_native_components();
    return (jvalue){0};
  }

  if ((has(cls, "com/ea/nimble/WebView") ||
       has(cls, "com.ea.nimble.WebView")) &&
      !strcmp(name, "showAuthView")) {
    const char *oauth_url = jni_cstr(argv[0].l);
    const char *redirect_url = jni_cstr(argv[1].l);
    debugPrintf("ACCOUNT WEB: native WebView.showAuthView callback=%p\n",
                argv[2].l);
    switch_ea_login_webview(oauth_url, redirect_url);
    return (jvalue){0};
  }


  if ((has(cls, "com/ea/nimble/Message") ||
       has(cls, "com.ea.nimble.Message")) &&
      !strcmp(name, "getComponent")) {
    /* This is only the Java-facing identity used by JNI call sites. Earlier
     * separately restores the native C++ component-manager registration and
     * Message lookup required by the +0x23F474C request adapter. */
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/IMessage")};
  }
  if (has(cls, "com/ea/nimble/IMessage") ||
      has(cls, "com.ea.nimble.IMessage")) {
    static unsigned message_calls;
    if (message_calls++ < 32) {
      debugPrintf("JNI Nimble IMessage.%s sig=%s self=%p arg0=%p arg1=%p arg2=%p\n",
                  name, sig ? sig : "<null>", self,
                  argv[0].l, argv[1].l, argv[2].l);
    }
    /* Do not fabricate an asynchronous success or callback.  Unknown Message
     * methods fail closed while their exact Java contract is being identified. */
    return (jvalue){0};
  }

  if ((has(cls, "com/ea/nimble/Log") || has(cls, "com.ea.nimble.Log")) &&
      !strcmp(name, "getComponent")) {
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/ILog")};
  }
  if ((has(cls, "com/ea/nimble/ILog") || has(cls, "com.ea.nimble.ILog")) &&
      !strcmp(name, "setThresholdLevel")) {
    return (jvalue){0};
  }
  if ((has(cls, "com/ea/nimble/ILog") || has(cls, "com.ea.nimble.ILog")) &&
      !strcmp(name, "writeWithTitle")) {
    /* EA's logger passes a level, title, formatted message, and varargs
     * array. Consume the call so it does not fall through to the generic JNI
     * warning. This is intentionally verbose while diagnosing account flow. */
    static unsigned write_with_title_calls;
    if (write_with_title_calls++ < 64) {
      const char *title = jni_cstr(argv[1].l);
      const char *message = jni_cstr(argv[2].l);
      debugPrintf("JNI Nimble ILog.writeWithTitle #%u level=%d title=%.*s message=%.*s args=%p\n",
                  write_with_title_calls, argv[0].i,
                  title ? 256 : 0, title ? title : "",
                  message ? 768 : 0, message ? message : "", argv[3].l);
    }
    return (jvalue){0};
  }

  if ((has(cls, "com/ea/nimble/mtx/NimbleMTX") ||
       has(cls, "com.ea.nimble.mtx.NimbleMTX")) &&
      !strcmp(name, "getComponent")) {
    return (jvalue){.l = jni_make_object_class(
        "com/ea/nimble/mtx/INimbleMTX")};
  }
  if ((has(cls, "com/ea/nimble/mtx/INimbleMTX") ||
       has(cls, "com.ea.nimble.mtx.INimbleMTX")) &&
      !strcmp(name, "refreshAvailableCatalogItems")) {
    /* Horizon has no Google Play billing provider. Publish the local catalog
     * immediately, then preserve the native refreshcatalogfinished lifecycle. */
    pvz2_catalog_refresh_requested();
    return (jvalue){0};
  }
  if ((has(cls, "com/ea/nimble/mtx/INimbleMTX") ||
       has(cls, "com.ea.nimble.mtx.INimbleMTX")) &&
      !strcmp(name, "getRecoveredTransactions")) {
    if (!catalog_empty_items)
      catalog_empty_items = jni_make_catalog_list(NULL, 0);
    return (jvalue){.l = catalog_empty_items};
  }
  if ((has(cls, "com/ea/nimble/mtx/INimbleMTX") ||
       has(cls, "com.ea.nimble.mtx.INimbleMTX")) &&
      !strcmp(name, "getPendingTransactions")) {
    if (!catalog_empty_items)
      catalog_empty_items = jni_make_catalog_list(NULL, 0);
    return (jvalue){.l = catalog_empty_items};
  }
  if ((has(cls, "com/ea/nimble/mtx/INimbleMTX") ||
       has(cls, "com.ea.nimble.mtx.INimbleMTX")) &&
      !strcmp(name, "purchaseItem")) {
    return (jvalue){.l = jni_make_object_class_value(
        "com/ea/nimble/Error", -1001)};
  }

  if ((has(cls, "com/ea/nimble/mtx/INimbleMTX") ||
       has(cls, "com.ea.nimble.mtx.INimbleMTX")) &&
      !strcmp(name, "getAvailableCatalogItems")) {
    return (jvalue){.l = catalog_items && pvz2_catalog_refresh_ready() ? catalog_items : NULL};
  }

  /* Android reports this View's physical size for both pixel and point-space
   * queries on the 1280x720 Switch surface.  LawnApp keeps its own internal
   * 1365x768 scene and, when CanSetGLViewScaleFactor is true, asks Java to fit
   * that scene to the View. Earlier builds fabricated a 1365x768 Java View;
   * native PVZ2 then correctly requested scale 1.0, so none of the HUD moved.
   * Keep the Java contract physical and let the native scaler choose the fit. */
  const int graphics_points_w = screen_width;
  const int graphics_points_h = screen_height;
  const float graphics_point_size = 1.0f;

  if (!strcmp(name, "Graphics_GetSafeRect")) {
    int n = 0, *v = jni_intarray_data(argv[0].l, &n);
    if (v && n >= 4) {
      /* Native AndroidAppDriver/LawnApp consumes these four integers as edge
       * INSETS, not as x/y/width/height: it subtracts (left+right) from the
       * usable width and (top+bottom) from the usable height.  The Switch
       * surface has no Android status/navigation/cutout insets, so every edge
       * is zero.  Earlier builds accidentally returned [0,0,w,h], which made
       * the game lay out every safe-area-aware HUD widget against enormous
       * bottom/top margins. */
      v[0] = 0;  /* left   */
      v[1] = 0;  /* right  */
      v[2] = 0;  /* top    */
      v[3] = 0;  /* bottom */
    }
    return (jvalue){0};
  }
  if (!strcmp(name, "Graphics_GetScreenSizeInPixels")) {
    int n = 0, *v = jni_intarray_data(argv[0].l, &n);
    if (v && n >= 2) { v[0] = screen_width; v[1] = screen_height; }
    return (jvalue){0};
  }
  if (!strcmp(name, "Graphics_GetScreenSizeInPoints")) {
    int n = 0, *v = jni_intarray_data(argv[0].l, &n);
    if (v && n >= 2) { v[0] = graphics_points_w; v[1] = graphics_points_h; }
    return (jvalue){0};
  }
  if (!strcmp(name, "Graphics_GetPointSizeInPixels"))
    return (jvalue){.f = graphics_point_size};

  /* Exact matching matters: a substring match used to return the float bits
   * for 1.0 as a jboolean, whose low byte is zero.  Keep Can/Set/Get separate
   * and preserve the scale requested by native PVZ2. */
  if (!strcmp(name, "Graphics_CanSetGLViewScaleFactor")) {
    return (jvalue){.z = JNI_TRUE};
  }
  if (!strcmp(name, "Graphics_SetGLViewScaleFactor")) {
    const float requested = argv[0].f;
    const float previous = s_graphics_view_scale_factor;
    if (requested > 0.10f && requested < 4.0f)
      s_graphics_view_scale_factor = requested;
    (void)previous;
    return (jvalue){0};
  }
  if (!strcmp(name, "Graphics_GetGLViewScaleFactor")) {
    if (!(s_graphics_view_scale_factor > 0.10f &&
          s_graphics_view_scale_factor < 4.0f))
      s_graphics_view_scale_factor = 1.0f;
    return (jvalue){.f = s_graphics_view_scale_factor};
  }
  if (has(name, "GLViewSysFBO") || has(name, "CurrentUIOrientation"))
    return (jvalue){.i = 0};
  if (has(name, "IsTablet")) {
    /* Use the Switch form factor for native layout selection. */
    const jboolean tablet = is_switch_handheld() ? JNI_TRUE : JNI_FALSE;
    return (jvalue){.z = tablet};
  }
  if (has(name, "IsSupportedUIOrientation"))
    return (jvalue){.z = JNI_TRUE};

  if (has(name, "BatteryLevel") || has(name, "BatteryPercentage") || has(name, "GetBatteryLevel")) {
    if (sig && (strstr(sig, ")F") || strstr(sig, ")D")))
      return (jvalue){.f = (float)get_switch_battery().percent / 100.0f};
    return (jvalue){.i = (jint)get_switch_battery().percent};
  }
  if (has(name, "IsBatteryCharging") || has(name, "IsCharging") || has(name, "IsPluggedIn") ||
      has(name, "PowerConnected") || has(name, "IsPowerConnected")) {
    return (jvalue){.z = get_switch_battery().is_charging ? JNI_TRUE : JNI_FALSE};
  }

  if (has(name, "MemoryTotal"))
    return (jvalue){.j = (jlong)get_switch_memory_info().total_ram};
  if (has(name, "MemoryAvailable") || has(name, "FreeMemory"))
    return (jvalue){.j = (jlong)get_switch_memory_info().avail_ram};
  if (has(name, "MemoryUsed"))
    return (jvalue){.j = (jlong)get_switch_memory_info().used_ram};

  if (has(name, "BlockSize"))
    return (jvalue){.j = (jlong)get_switch_storage_info().block_size};
  if (has(name, "BlockCount") || has(name, "TotalBlocks"))
    return (jvalue){.j = (jlong)get_switch_storage_info().total_blocks};
  if (has(name, "BlocksFree") || has(name, "FreeBlocks") || has(name, "AvailableBlocks"))
    return (jvalue){.j = (jlong)get_switch_storage_info().free_blocks};
  if (has(name, "TotalDiskSpace") || has(name, "TotalStorage"))
    return (jvalue){.j = (jlong)get_switch_storage_info().total_bytes};
  if (has(name, "FreeDiskSpace") || has(name, "AvailableDiskSpace") || has(name, "FreeStorage"))
    return (jvalue){.j = (jlong)get_switch_storage_info().free_bytes};
  if (has(name, "DoLowDiskSpaceCheck") || has(name, "CheckLowDiskSpace") || has(name, "LowDiskSpace")) {
    const u64 threshold = 50ULL * 1024 * 1024;
    SwitchStorageInfo st = get_switch_storage_info();
    int is_low = (st.free_bytes < threshold);
    debugPrintf("JNI LowDiskSpaceCheck: free=%llu MB, is_low=%d\n",
                (unsigned long long)(st.free_bytes / (1024 * 1024)), is_low);
    return (jvalue){.z = is_low ? JNI_TRUE : JNI_FALSE};
  }

  if (has(name, "IsOnline") || has(name, "IsConnected") || has(name, "IsNetworkConnected") ||
      has(name, "NetworkAvailable") || has(name, "NetworkConnected") || has(name, "HasConnection") ||
      has(name, "IsInternetAvailable")) {
    return (jvalue){.z = get_switch_network_info().is_connected ? JNI_TRUE : JNI_FALSE};
  }
  if (has(name, "NetworkType") || has(name, "GetNetworkType") || has(name, "GetConnectionType")) {
    if (sig && strstr(sig, "Ljava/lang/String;")) {
      return (jvalue){.l = jni_make_string(get_switch_network_info().type_name)};
    }
    return (jvalue){.i = (jint)get_switch_network_info().android_net_type};
  }

  if (has(name, "Billing") || has(name, "Purchase") || has(name, "Ads") ||
      has(name, "Reward") || has(name, "Facebook") || has(name, "SignIn"))
    return (jvalue){0};

  if (!strcmp(name, "scheduleEvent")) {
    /* argv[0] is the requested Handler-style delay; argv[1] is the native
     * event handle returned through onTimerEvent(J).  Preserve the delay
     * instead of firing the timeout on the next frame. */
    if (!schedule_java_timer(self, argv[1].j, sig, argv)) {
      /* Failing closed is safer than inventing an early timeout. */
      debugPrintf("JNI timer: scheduleEvent dropped because timer queue is full\n");
    }
    return (jvalue){0};
  }

  if (has(cls, "AndroidHttpTransaction")) {
    if (!strcmp(name, "SetTimeout")) {
      jni_http_set_timeout(self, argv[0].i);
      return (jvalue){0};
    }
    if (!strcmp(name, "SetRequestHeader")) {
      jni_http_set_header(self, jni_cstr(argv[0].l), jni_cstr(argv[1].l));
      return (jvalue){0};
    }
    if (!strcmp(name, "SetRequestBody")) {
      jni_http_set_body(self, argv[0].l);
      return (jvalue){0};
    }
    if (!strcmp(name, "GetStatusCode"))
      return (jvalue){.i = jni_http_status(self)};
    if (!strcmp(name, "GetResponseHeader"))
      return (jvalue){.l = jni_make_string(
          jni_http_get_response_header(self, jni_cstr(argv[0].l)))};
    if (!strcmp(name, "Release") || !strcmp(name, "Cleanup")) {
      /* Android's Java transaction notifies native code after the engine has
       * released the completed request.  The response/data/complete callbacks
       * alone leave that final lifecycle edge absent on Horizon.  Keep it
       * asynchronous and one-shot so this follows the original hand-off and
       * remains safe when native calls both method names. */
      const jlong transaction = jni_object_long(self);
      if (transaction && jni_http_take_cleanup(self)) {
        debugPrintf("JNI HTTP: %s -> queue HttpTransactionCleanup(%lld)\n",
                    name, (long long)transaction);
        queue_java_callback("HttpTransactionCleanup", self, transaction);
      }
      return (jvalue){0};
    }
    if (!strcmp(name, "Start")) {
      start_http_transaction(self);
      return (jvalue){0};
    }
  }
  if (!strcmp(name, "requestCMPConsentInfo")) {
    queue_java_bool_callback("onRequestCMPConsentInfoComplete", self,
                             argv[0].j, JNI_FALSE);
    return (jvalue){0};
  }
  if (!strcmp(name, "showConsent")) {
    show_consent_age(self, argv[0].j);
    return (jvalue){0};
  }
  if (!strcmp(name, "Device_ShowKeyboard")) {
    /* This Android method only exposes the IME for an existing Java edit field.
     * There is no matching Switch text field, so retain the request as telemetry. */
    return (jvalue){0};
  }
  if (!strcmp(name, "Device_HideKeyboard")) {
    editbox_close();
    return (jvalue){0};
  }
  if (!strcmp(name, "UI_ProcessEvents") || !strcmp(name, "ProcessEvents"))
    return (jvalue){.z = process_events(argv[0].l)};
  if (!strcmp(name, "getClassLoader"))
    return (jvalue){.l = jni_find_class_c("java/lang/ClassLoader")};
  if (!strcmp(name, "loadClass")) {
    const char *class_name = jni_cstr(argv[0].l);
    return (jvalue){.l = jni_find_class_c(class_name ? class_name : "java/lang/Object")};
  }
  if (!strcmp(name, "GetActivity") || !strcmp(name, "GetAppFrameworkActivity"))
    return (jvalue){.l = jni_make_activity()};
  if (!strcmp(name, "isBluetoothA2dpOn")) return (jvalue){.z = 0};
  if (!strcmp(name, "getApplicationContext") || !strcmp(name, "getAssets") ||
      !strcmp(name, "getResources") || !strcmp(name, "getSystemService") ||
      !strcmp(name, "instance"))
    return (jvalue){.l = jni_make_object()};

  static const char *seen[128];
  static int seen_count;
  for (int i = 0; i < seen_count; i++) if (seen[i] == name) return typed_default(sig);
  if (seen_count < 128) seen[seen_count++] = name;
  debugPrintf("JNI unhandled: %s.%s %s\n", cls, name, sig);
  return typed_default(sig);
}
