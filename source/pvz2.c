/* Minimal Java shell for PopCap's SexyAppFramework. Android-only services stay
 * local, while the HTTP transaction used by the CDN/config services is real. */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
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

#define PVZ2_ENABLE_TOUCH_TRACE 0

extern int screen_width;
extern int screen_height;
extern void watchdog_set_suspended(int suspended);
extern void note_official_server_time(long long epoch,
                                      const char *date_header,
                                      const char *url);
extern int nimble_probe_native_component(const char *name, const char *stage);


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
  /* classes2.dex Network.Status ordinals: UNKNOWN=0, NONE=1, DEAD=2, OK=3. */
  int nimble_ordinal;
} SwitchNetworkInfo;

static Mutex switch_network_mutex;
static int switch_network_initialized;
static Result switch_network_init_result;
static int switch_network_cache_valid;
static u64 switch_network_next_refresh_tick;
static SwitchNetworkInfo switch_network_cache = {
  .is_connected = 0,
  .android_net_type = -1,
  .type_name = "NONE",
  .nimble_ordinal = 1,
};

static jint nimble_log_threshold_level = PVZ2_NIMBLE_LOG_MIN_LEVEL;

static SwitchNetworkInfo get_switch_network_info(void) {
  const u64 now = armGetSystemTick();
  mutexLock(&switch_network_mutex);
  if (!switch_network_initialized) {
    switch_network_init_result = nifmInitialize(NifmServiceType_User);
    switch_network_initialized = 1;
  }

  if (!switch_network_cache_valid || now >= switch_network_next_refresh_tick) {
    SwitchNetworkInfo net = {
      .is_connected = 0,
      .android_net_type = -1,
      .type_name = "NONE",
      .nimble_ordinal = 1,
    };
    if (R_SUCCEEDED(switch_network_init_result)) {
      /* Initialize output-only libnx values defensively. This is invisible on
       * successful Horizon calls and prevents stale stack data from becoming a
       * network type if a future libnx implementation returns an error after
       * partially touching its output parameters. */
      NifmInternetConnectionType conn_type = 0;
      NifmInternetConnectionStatus conn_status = 0;
      u32 wifi_strength = 0;
      if (R_SUCCEEDED(nifmGetInternetConnectionStatus(
              &conn_type, &wifi_strength, &conn_status)) &&
          conn_status == NifmInternetConnectionStatus_Connected) {
        net.is_connected = 1;
        /* classes2.dex: connected -> Network.Status.OK (ordinal 3). */
        net.nimble_ordinal = 3;
        if (conn_type == NifmInternetConnectionType_Ethernet) {
          net.android_net_type = 9;
          net.type_name = "ETHERNET";
        } else {
          net.android_net_type = 1;
          net.type_name = "WIFI";
        }
      }
    }
    switch_network_cache = net;
    switch_network_cache_valid = 1;
    const u64 hz = armGetSystemTickFreq();
    switch_network_next_refresh_tick = now +
        (hz * (u64)PVZ2_NETWORK_STATUS_CACHE_MS) / 1000u;
  }

  const SwitchNetworkInfo result = switch_network_cache;
  mutexUnlock(&switch_network_mutex);
  return result;
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

/* diagnostic: faithful backing for stock Android UUID helpers.  These UUIDs are
 * ephemeral values returned to callers of UUID-generation APIs; they are NOT
 * the persistent Switch install UUID and are never synthesized from a fixed
 * fallback.  If Horizon's CSRNG is unavailable, fail closed with an empty
 * string so stock reconciliation can decide what to do. */
static int stock_generate_uuid_v4(char out[37], const char *caller) {
  unsigned char bytes[16];
  if (!out) return 0;
  out[0] = 0;
  const Result rc = csrngGetRandomBytes(bytes, sizeof(bytes));
  if (R_FAILED(rc)) {
    debugPrintf("compat: %s UUID-v4 CSRNG failed rc=0x%x; returning empty\n",
                caller ? caller : "stock", rc);
    return 0;
  }

  /* java.util.UUID.randomUUID() uses an RFC 4122 version-4 UUID. */
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  snprintf(out, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           (unsigned)bytes[0], (unsigned)bytes[1], (unsigned)bytes[2], (unsigned)bytes[3],
           (unsigned)bytes[4], (unsigned)bytes[5], (unsigned)bytes[6], (unsigned)bytes[7],
           (unsigned)bytes[8], (unsigned)bytes[9], (unsigned)bytes[10], (unsigned)bytes[11],
           (unsigned)bytes[12], (unsigned)bytes[13], (unsigned)bytes[14], (unsigned)bytes[15]);
  (void)caller;
  return 1;
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

/*  continue the exact DEX first-install SynergyEnvironmentUpdater
 * sequence through getAnonUid and the successful SynergyEnvironmentImpl
 * completion transition.  Only server-returned device/environment/anonymous
 * identity state is accepted.  The legacy Switch install UUID remains an
 * AndroidId compatibility input only and is never promoted to Synergy/player
 * identity. */
static volatile int synergy_direction_pending;
static volatile int synergy_direction_in_flight;
static volatile int synergy_direction_ready;
static volatile int synergy_device_id_pending;
static volatile int synergy_device_id_in_flight;
static volatile int synergy_device_id_done;
static int synergy_cache_blocked;
static char synergy_session_id[33];
static char synergy_hw_id[64];
static char synergy_bootstrap_sell_id[64];
static char synergy_user_url[1024];
static char synergy_ea_device_id[256];
static volatile int synergy_anon_pending;
static volatile int synergy_anon_in_flight;
static volatile int synergy_anon_done;
static volatile int synergy_environment_live;
static volatile int synergy_update_in_progress;
static volatile int synergy_id_manager_startup_seen;
static int synergy_main_application_active;
static char synergy_anonymous_id[256];
static char synergy_id_manager_anonymous_id[256];
static char synergy_current_id[256];
static char synergy_authenticator[160];
static char *synergy_direction_json;
static size_t synergy_direction_json_length;
static long long synergy_direction_timestamp_ms;
static char synergy_direction_language[32];

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
      (PendingJavaCallback){
          .callback = callback, .self = self, .argument = argument,
          .boolean_argument = -1, .data = NULL, .data_length = 0,
          .status = 0});
}


static void queue_java_bool_callback(const char *callback, jobject self,
                                     jlong argument, jboolean value) {
  (void)enqueue_pending_callback(
      (PendingJavaCallback){
          .callback = callback, .self = self, .argument = argument,
          .boolean_argument = value, .data = NULL, .data_length = 0,
          .status = 0});
}


static void queue_java_string_callback(const char *callback, jobject self,
                                       jlong argument, const char *value) {
  char *copy = strdup(value ? value : "");
  if (!copy) return;
  PendingJavaCallback pending = {
      .callback = callback, .self = self, .argument = argument,
      .boolean_argument = -2, .data = copy, .data_length = 0, .status = 0};
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
  PendingJavaCallback pending = {
      .callback = callback, .self = self, .argument = argument,
      .boolean_argument = -1, .data = copy, .data_length = len, .status = 0};
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

/* Extract one response header from libcurl's raw header buffer without
 * assuming NUL termination.  Redirects may contribute more than one header
 * block, so retain the last matching field (the final response). */
static int http_response_header_copy(const HttpBuffer *headers,
                                     const char *name,
                                     char *out, size_t out_size) {
  if (!headers || !headers->data || !headers->size || !name || !*name ||
      !out || out_size < 2)
    return 0;
  const size_t name_len = strlen(name);
  const char *p = (const char *)headers->data;
  const char *const end = p + headers->size;
  int found = 0;
  out[0] = 0;
  while (p < end) {
    const char *line_end = p;
    while (line_end < end && *line_end != '\n') ++line_end;
    const char *value = p;
    const char *colon = NULL;
    for (const char *q = p; q < line_end; ++q) {
      if (*q == ':') { colon = q; break; }
    }
    if (colon && (size_t)(colon - p) == name_len &&
        !strncasecmp(p, name, name_len)) {
      value = colon + 1;
      while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
      const char *value_end = line_end;
      while (value_end > value &&
             (value_end[-1] == '\r' || value_end[-1] == ' ' ||
              value_end[-1] == '\t'))
        --value_end;
      size_t length = (size_t)(value_end - value);
      if (length >= out_size) length = out_size - 1;
      memcpy(out, value, length);
      out[length] = 0;
      found = 1;
    }
    p = line_end < end ? line_end + 1 : end;
  }
  return found;
}

static const char *bootstrap_ipv4(const char *host) {
  size_t length = strlen(host);
  while (length && host[length - 1] == '.') --length;
  /* diagnostic: PvZ2Web is now allowed to reach the live backend for the
   * fire-and-forget PlayerInfo provisioning request.  Do not pin this AWS
   * service to the old bootstrap address; let the working public-DNS fallback
   * below obtain its current A record, just like the Joust host. */
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

/* libnx's resolver does not reliably follow the CNAME chains used by some
 * legacy PopCap/EA service hosts. Keep the known stable bootstrap entries
 * above, but fall back to a tiny recursive DNS client for hosts that are not
 * in that table. This avoids hard-coding the Arena/Joust service to a single
 * AWS address and fixes other awspopcap.com hosts that use the same pattern. */
static uint16_t dns_read_u16(const unsigned char *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int dns_skip_name(const unsigned char *packet, size_t packet_size, size_t *offset) {
  size_t pos = *offset;
  size_t labels = 0;
  while (pos < packet_size) {
    unsigned char n = packet[pos++];
    if (n == 0) {
      *offset = pos;
      return 1;
    }
    if ((n & 0xc0) == 0xc0) {
      if (pos >= packet_size) return 0;
      *offset = pos + 1;
      return 1;
    }
    if ((n & 0xc0) != 0 || n > 63 || pos + n > packet_size) return 0;
    pos += n;
    if (++labels > 127) return 0;
  }
  return 0;
}

static int dns_query_ipv4_server(const char *host, uint32_t server_be,
                                 struct in_addr *address) {
  unsigned char query[512];
  unsigned char reply[1500];
  memset(query, 0, sizeof(query));

  static uint16_t s_dns_id = 0x4e58;
  uint16_t id = ++s_dns_id;
  query[0] = (unsigned char)(id >> 8);
  query[1] = (unsigned char)id;
  query[2] = 0x01; /* recursion desired */
  query[5] = 0x01; /* QDCOUNT = 1 */

  size_t q = 12;
  const char *label = host;
  for (const char *p = host;; ++p) {
    if (*p != '.' && *p != '\0') continue;
    size_t len = (size_t)(p - label);
    if (!len || len > 63 || q + 1 + len + 5 > sizeof(query)) return 0;
    query[q++] = (unsigned char)len;
    memcpy(query + q, label, len);
    q += len;
    if (!*p) break;
    label = p + 1;
  }
  query[q++] = 0;
  query[q++] = 0; query[q++] = 1; /* QTYPE A */
  query[q++] = 0; query[q++] = 1; /* QCLASS IN */

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return 0;

  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = (uint16_t)((53u << 8) | (53u >> 8));
  dst.sin_addr.s_addr = server_be;

  ssize_t sent = sendto(fd, query, q, 0, (struct sockaddr *)&dst, sizeof(dst));
  if (sent != (ssize_t)q) {
    close(fd);
    return 0;
  }
  ssize_t got = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
  close(fd);
  if (got < 12) return 0;

  size_t n = (size_t)got;
  if (dns_read_u16(reply) != id || !(reply[2] & 0x80) || (reply[3] & 0x0f) != 0)
    return 0;

  uint16_t qd = dns_read_u16(reply + 4);
  uint16_t an = dns_read_u16(reply + 6);
  uint16_t ns = dns_read_u16(reply + 8);
  uint16_t ar = dns_read_u16(reply + 10);
  size_t off = 12;

  for (uint16_t i = 0; i < qd; ++i) {
    if (!dns_skip_name(reply, n, &off) || off + 4 > n) return 0;
    off += 4;
  }

  /* A records can appear after one or more CNAME records. Scan every RR in
   * answer/authority/additional and accept the first IPv4 IN record. */
  unsigned total = (unsigned)an + (unsigned)ns + (unsigned)ar;
  for (unsigned i = 0; i < total; ++i) {
    if (!dns_skip_name(reply, n, &off) || off + 10 > n) return 0;
    uint16_t type = dns_read_u16(reply + off);
    uint16_t klass = dns_read_u16(reply + off + 2);
    uint16_t rdlen = dns_read_u16(reply + off + 8);
    off += 10;
    if (off + rdlen > n) return 0;
    if (type == 1 && klass == 1 && rdlen == 4) {
      memcpy(&address->s_addr, reply + off, 4);
      return 1;
    }
    off += rdlen;
  }
  return 0;
}

static int dns_public_ipv4(const char *host, struct in_addr *address) {
  /* Network-order bytes for 1.1.1.1 and 8.8.8.8, built without inet_aton so
   * this path cannot recurse back through the wrapped resolver. */
  static const unsigned char servers[][4] = {{1,1,1,1}, {8,8,8,8}};
  for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); ++i) {
    uint32_t server_be;
    memcpy(&server_be, servers[i], sizeof(server_be));
    if (dns_query_ipv4_server(host, server_be, address)) return 1;
  }
  return 0;
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

/*  libPVZ2 is Android/Bionic AArch64, while this port is built
 * against devkitA64/newlib.  The two ABIs do not share sockaddr layout:
 * Bionic/Linux stores a 16-bit sa_family at offset 0, while the Horizon
 * socket headers use their native BSD/newlib representation.  Keep objects
 * returned to stock curl in the exact Bionic layout and translate only at
 * the host socket boundary. */
#define BIONIC_AF_UNSPEC 0
#define BIONIC_AF_INET   2
#define BIONIC_AF_INET6  10

#define BIONIC_EAI_FAMILY  5
#define BIONIC_EAI_MEMORY  6
#define BIONIC_EAI_NONAME  8
#define BIONIC_EAI_SERVICE 9

typedef struct {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  unsigned char sin_zero[8];
} BionicSockaddrIn;

typedef struct {
  uint16_t sin6_family;
  uint16_t sin6_port;
  uint32_t sin6_flowinfo;
  unsigned char sin6_addr[16];
  uint32_t sin6_scope_id;
} BionicSockaddrIn6;

typedef struct BionicAddrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  uint32_t ai_addrlen;
  char *ai_canonname;
  void *ai_addr;
  struct BionicAddrinfo *ai_next;
} BionicAddrinfo;

_Static_assert(sizeof(BionicSockaddrIn) == 16,
               "Bionic sockaddr_in must be 16 bytes");
_Static_assert(sizeof(BionicSockaddrIn6) == 28,
               "Bionic sockaddr_in6 must be 28 bytes");
_Static_assert(offsetof(BionicAddrinfo, ai_canonname) == 24,
               "Bionic addrinfo ai_canonname offset");
_Static_assert(offsetof(BionicAddrinfo, ai_addr) == 32,
               "Bionic addrinfo ai_addr offset");
_Static_assert(sizeof(BionicAddrinfo) == 48,
               "Bionic AArch64 addrinfo must be 48 bytes");

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
  char dynamic_target[16];
  if (target) {
    if (!parse_ipv4(target, &address)) return EAI_NONAME;
  } else {
    debugPrintf("dns: no bootstrap mapping for %s; trying public DNS\n", node);
    if (!dns_public_ipv4(node, &address)) {
      debugPrintf("dns: public DNS failed for %s\n", node);
      return EAI_NONAME;
    }
    const unsigned char *b = (const unsigned char *)&address.s_addr;
    snprintf(dynamic_target, sizeof(dynamic_target), "%u.%u.%u.%u",
             (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
    target = dynamic_target;
    debugPrintf("dns: public DNS resolved %s -> %s\n", node, target);
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


/*  The compatibility layer correctly modeled Android/Bionic addrinfo for stock
 * libPVZ2, but reused the process-wide __wrap_getaddrinfo entry.  The port's
 * own Horizon/newlib libcurl also calls that wrapper and therefore received
 * Bionic objects it cannot interpret.  Keep the host wrapper in native
 * newlib layout and expose a separate Bionic ABI entry only to libPVZ2's
 * import table.  This restores the pre-The compatibility layer JNI/CDN/Synergy transport
 * while preserving the stock Nimble sockaddr ABI correction. */
int bionic_getaddrinfo_fake(const char *node, const char *service, const struct addrinfo *hints_host,
                       struct addrinfo **result_host) {
  const BionicAddrinfo *hints = (const BionicAddrinfo *)(const void *)hints_host;
  BionicAddrinfo **result = (BionicAddrinfo **)(void *)result_host;
  if (!result || !node) return BIONIC_EAI_NONAME;
  *result = NULL;

  const int family = hints ? hints->ai_family : BIONIC_AF_UNSPEC;
  debugPrintf("dns: node=%s service=%s family=%d\n", node, service ? service : "", family);
  if (family != BIONIC_AF_UNSPEC && family != BIONIC_AF_INET)
    return BIONIC_EAI_FAMILY;

  struct in_addr address;
  const char *target = bootstrap_ipv4(node);
  char dynamic_target[16];
  if (target) {
    if (!parse_ipv4(target, &address)) return BIONIC_EAI_NONAME;
  } else {
    debugPrintf("dns: no bootstrap mapping for %s; trying public DNS\n", node);
    if (!dns_public_ipv4(node, &address)) {
      debugPrintf("dns: public DNS failed for %s\n", node);
      return BIONIC_EAI_NONAME;
    }
    const unsigned char *b = (const unsigned char *)&address.s_addr;
    snprintf(dynamic_target, sizeof(dynamic_target), "%u.%u.%u.%u",
             (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
    target = dynamic_target;
    debugPrintf("dns: public DNS resolved %s -> %s\n", node, target);
  }

  unsigned long port = 0;
  if (service && *service) {
    char *endp = NULL;
    port = strtoul(service, &endp, 10);
    if (!endp || *endp || port > 65535) return BIONIC_EAI_SERVICE;
  }

  BionicAddrinfo *info = calloc(1, sizeof(*info));
  BionicSockaddrIn *socket_address = calloc(1, sizeof(*socket_address));
  if (!info || !socket_address) {
    free(info);
    free(socket_address);
    return BIONIC_EAI_MEMORY;
  }

  socket_address->sin_family = BIONIC_AF_INET;
  socket_address->sin_port = htons((uint16_t)port);
  socket_address->sin_addr = address.s_addr;
  info->ai_flags = hints ? hints->ai_flags : 0;
  info->ai_family = BIONIC_AF_INET;
  info->ai_socktype = hints ? hints->ai_socktype : 0;
  info->ai_protocol = hints ? hints->ai_protocol : 0;
  info->ai_addrlen = (uint32_t)sizeof(*socket_address);
  info->ai_canonname = NULL;
  info->ai_addr = socket_address;
  info->ai_next = NULL;
  *result = info;

  debugPrintf("dns: %s -> %s:%lu\n", node, target, port);
  (void)0;
  return 0;
}

void bionic_freeaddrinfo_fake(struct addrinfo *info_host) {
  BionicAddrinfo *info = (BionicAddrinfo *)(void *)info_host;
  while (info) {
    BionicAddrinfo *next = info->ai_next;
    free(info->ai_addr);
    free(info->ai_canonname);
    free(info);
    info = next;
  }
}


/*  The compatibility layer exposed curl's exact failure text:
 * "getaddrinfo() thread failed to start". In curl 8.7.1 the threaded resolver
 * creates a pipe/socketpair wakeup primitive before pthread_create; the old
 * offline port hard-failed both. The same embedded curl then needs its normal
 * POSIX socket surface. Restore that transport only; no Nexus/auth response is
 * fabricated here.
 *
 * Android/Bionic socket calls also cannot be forwarded numerically without an
 * ABI shim: devkitA64/newlib uses the generic newlib socket errno and
 * SOL_SOCKET option numbers, while Bionic uses Linux numbers. Translate the
 * values that curl observes (most importantly EINPROGRESS and SO_ERROR). */
#define BIONIC_SOCK_NONBLOCK 00004000
#define BIONIC_SOCK_CLOEXEC 02000000
#define BIONIC_SOL_SOCKET 1
#define BIONIC_SO_DEBUG 1
#define BIONIC_SO_REUSEADDR 2
#define BIONIC_SO_TYPE 3
#define BIONIC_SO_ERROR 4
#define BIONIC_SO_DONTROUTE 5
#define BIONIC_SO_BROADCAST 6
#define BIONIC_SO_SNDBUF 7
#define BIONIC_SO_RCVBUF 8
#define BIONIC_SO_KEEPALIVE 9
#define BIONIC_SO_OOBINLINE 10
#define BIONIC_SO_LINGER 13
#define BIONIC_SO_REUSEPORT 15
#define BIONIC_SO_RCVLOWAT 18
#define BIONIC_SO_SNDLOWAT 19
#define BIONIC_SO_RCVTIMEO 20
#define BIONIC_SO_SNDTIMEO 21

static int bionic_errno_from_host(int e) {
  /* Generic newlib network errno values used by devkitA64. Values not listed
   * here either already match Bionic below the network range or are passed
   * through so an unexpected failure remains observable on hardware. */
  switch (e) {
    case 95:  return 95;  /* EOPNOTSUPP */
    case 104: return 104; /* ECONNRESET */
    case 105: return 105; /* ENOBUFS */
    case 106: return 97;  /* EAFNOSUPPORT */
    case 107: return 91;  /* EPROTOTYPE */
    case 108: return 88;  /* ENOTSOCK */
    case 109: return 92;  /* ENOPROTOOPT */
    case 111: return 111; /* ECONNREFUSED */
    case 112: return 98;  /* EADDRINUSE */
    case 113: return 103; /* ECONNABORTED */
    case 114: return 101; /* ENETUNREACH */
    case 115: return 100; /* ENETDOWN */
    case 116: return 110; /* ETIMEDOUT */
    case 117: return 112; /* EHOSTDOWN */
    case 118: return 113; /* EHOSTUNREACH */
    case 119: return 115; /* EINPROGRESS */
    case 120: return 114; /* EALREADY */
    case 121: return 89;  /* EDESTADDRREQ */
    case 122: return 90;  /* EMSGSIZE */
    case 123: return 93;  /* EPROTONOSUPPORT */
    case 125: return 99;  /* EADDRNOTAVAIL */
    case 126: return 102; /* ENETRESET */
    case 127: return 106; /* EISCONN */
    case 128: return 107; /* ENOTCONN */
    default: return e;
  }
}

static void bionic_translate_errno_after_failure(int rc) {
  if (rc < 0) errno = bionic_errno_from_host(errno);
}

static int bionic_host_sockopt(int level, int opt) {
  if (level != BIONIC_SOL_SOCKET) return opt;
  switch (opt) {
    case BIONIC_SO_DEBUG: return SO_DEBUG;
    case BIONIC_SO_REUSEADDR: return SO_REUSEADDR;
    case BIONIC_SO_TYPE: return SO_TYPE;
    case BIONIC_SO_ERROR: return SO_ERROR;
    case BIONIC_SO_DONTROUTE: return SO_DONTROUTE;
    case BIONIC_SO_BROADCAST: return SO_BROADCAST;
    case BIONIC_SO_SNDBUF: return SO_SNDBUF;
    case BIONIC_SO_RCVBUF: return SO_RCVBUF;
    case BIONIC_SO_KEEPALIVE: return SO_KEEPALIVE;
    case BIONIC_SO_OOBINLINE: return SO_OOBINLINE;
    case BIONIC_SO_LINGER: return SO_LINGER;
#ifdef SO_REUSEPORT
    case BIONIC_SO_REUSEPORT: return SO_REUSEPORT;
#endif
    case BIONIC_SO_RCVLOWAT: return SO_RCVLOWAT;
    case BIONIC_SO_SNDLOWAT: return SO_SNDLOWAT;
    case BIONIC_SO_RCVTIMEO: return SO_RCVTIMEO;
    case BIONIC_SO_SNDTIMEO: return SO_SNDTIMEO;
    default: return opt;
  }
}

static int bionic_socketpair_loopback(int sv[2]) {
  int listener = -1, client = -1, server = -1;
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  int one = 1;
  if (!sv) { errno = EINVAL; return -1; }
  sv[0] = sv[1] = -1;
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener < 0) goto fail;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
  if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) < 0) goto fail;
  if (listen(listener, 1) < 0) goto fail;
  client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client < 0) goto fail;
  if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
  server = accept(listener, NULL, NULL);
  if (server < 0) goto fail;
  close(listener);
  sv[0] = server;
  sv[1] = client;
  (void)0;
  return 0;
fail:
  {
    int saved = errno;
    if (listener >= 0) close(listener);
    if (client >= 0) close(client);
    if (server >= 0) close(server);
    errno = bionic_errno_from_host(saved);
    debugPrintf("compat: curl wakeup pair failed host_errno=%d bionic_errno=%d\n",
                saved, errno);
    return -1;
  }
}

int bionic_pipe_fake(int fds[2]) { return bionic_socketpair_loopback(fds); }
int bionic_socketpair_fake(int domain, int type, int protocol, int sv[2]) {
  (void)domain; (void)type; (void)protocol;
  return bionic_socketpair_loopback(sv);
}
#define BIONIC_EAFNOSUPPORT 97

static int bionic_host_af_from_bionic(int family) {
  if (family == BIONIC_AF_UNSPEC) return AF_UNSPEC;
  if (family == BIONIC_AF_INET) return AF_INET;
#ifdef AF_INET6
  if (family == BIONIC_AF_INET6) return AF_INET6;
#endif
  return -1;
}

static int bionic_sockaddr_to_host(const void *address, uint32_t address_len,
                                            struct sockaddr_storage *host_storage,
                                            socklen_t *host_len) {
  if (!address || !host_storage || !host_len || address_len < sizeof(uint16_t)) {
    errno = EINVAL;
    return -1;
  }

  uint16_t family = 0;
  memcpy(&family, address, sizeof(family));
  memset(host_storage, 0, sizeof(*host_storage));

  if (family == BIONIC_AF_INET) {
    if (address_len < sizeof(BionicSockaddrIn)) {
      errno = EINVAL;
      return -1;
    }
    const BionicSockaddrIn *source = (const BionicSockaddrIn *)address;
    struct sockaddr_in host;
    memset(&host, 0, sizeof(host));
    host.sin_family = AF_INET;
    host.sin_port = source->sin_port;
    memcpy(&host.sin_addr.s_addr, &source->sin_addr, sizeof(source->sin_addr));
    memcpy(host_storage, &host, sizeof(host));
    *host_len = (socklen_t)sizeof(host);
    return 0;
  }

#ifdef AF_INET6
  if (family == BIONIC_AF_INET6) {
    if (address_len < sizeof(BionicSockaddrIn6)) {
      errno = EINVAL;
      return -1;
    }
    const BionicSockaddrIn6 *source = (const BionicSockaddrIn6 *)address;
    struct sockaddr_in6 host;
    memset(&host, 0, sizeof(host));
    host.sin6_family = AF_INET6;
    host.sin6_port = source->sin6_port;
    host.sin6_flowinfo = source->sin6_flowinfo;
    memcpy(&host.sin6_addr, source->sin6_addr, sizeof(source->sin6_addr));
    host.sin6_scope_id = source->sin6_scope_id;
    memcpy(host_storage, &host, sizeof(host));
    *host_len = (socklen_t)sizeof(host);
    return 0;
  }
#endif

  errno = BIONIC_EAFNOSUPPORT;
  return -1;
}

static int bionic_host_sockaddr_to_bionic(const struct sockaddr *host, socklen_t host_len,
                                            void *address, uint32_t *address_len) {
  if (!host || !address_len) {
    errno = EINVAL;
    return -1;
  }

  const uint32_t capacity = *address_len;
  if (host->sa_family == AF_INET && host_len >= sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *source = (const struct sockaddr_in *)(const void *)host;
    BionicSockaddrIn out;
    memset(&out, 0, sizeof(out));
    out.sin_family = BIONIC_AF_INET;
    out.sin_port = source->sin_port;
    memcpy(&out.sin_addr, &source->sin_addr.s_addr, sizeof(out.sin_addr));
    *address_len = (uint32_t)sizeof(out);
    if (!address || capacity < sizeof(out)) {
      errno = EINVAL;
      return -1;
    }
    memcpy(address, &out, sizeof(out));
    return 0;
  }

#ifdef AF_INET6
  if (host->sa_family == AF_INET6 && host_len >= sizeof(struct sockaddr_in6)) {
    const struct sockaddr_in6 *source = (const struct sockaddr_in6 *)(const void *)host;
    BionicSockaddrIn6 out;
    memset(&out, 0, sizeof(out));
    out.sin6_family = BIONIC_AF_INET6;
    out.sin6_port = source->sin6_port;
    out.sin6_flowinfo = source->sin6_flowinfo;
    memcpy(out.sin6_addr, &source->sin6_addr, sizeof(out.sin6_addr));
    out.sin6_scope_id = source->sin6_scope_id;
    *address_len = (uint32_t)sizeof(out);
    if (!address || capacity < sizeof(out)) {
      errno = EINVAL;
      return -1;
    }
    memcpy(address, &out, sizeof(out));
    return 0;
  }
#endif

  errno = BIONIC_EAFNOSUPPORT;
  return -1;
}

int bionic_socket_fake(int domain, int type, int protocol) {
  int want_nonblock = (type & BIONIC_SOCK_NONBLOCK) != 0;
  int base_type = type & ~(BIONIC_SOCK_NONBLOCK | BIONIC_SOCK_CLOEXEC);
  const int host_domain = bionic_host_af_from_bionic(domain);
  if (host_domain < 0) {
    errno = BIONIC_EAFNOSUPPORT;
    return -1;
  }
  int fd = socket(host_domain, base_type, protocol);
  if (fd >= 0 && want_nonblock) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
  bionic_translate_errno_after_failure(fd);
  return fd;
}
int bionic_bind_fake(int fd, const struct sockaddr *a, socklen_t n) {
  struct sockaddr_storage host;
  socklen_t host_len = 0;
  if (bionic_sockaddr_to_host(a, (uint32_t)n, &host, &host_len) < 0) return -1;
  int r = bind(fd, (const struct sockaddr *)(const void *)&host, host_len);
  bionic_translate_errno_after_failure(r);
  return r;
}
int bionic_connect_fake(int fd, const struct sockaddr *a, socklen_t n) {
  struct sockaddr_storage host;
  socklen_t host_len = 0;
  if (bionic_sockaddr_to_host(a, (uint32_t)n, &host, &host_len) < 0) return -1;
  int r = connect(fd, (const struct sockaddr *)(const void *)&host, host_len);
  if (r < 0) {
    const int host_error = errno;
    bionic_translate_errno_after_failure(r);
    debugPrintf("compat: connect bionic_family=%u host_family=%d host_errno=%d bionic_errno=%d\n",
                a ? (unsigned)*(const uint16_t *)(const void *)a : 0u,
                host.ss_family, host_error, errno);
  }
  return r;
}
int bionic_listen_fake(int fd, int backlog) {
  int r = listen(fd, backlog); bionic_translate_errno_after_failure(r); return r;
}
int bionic_accept_fake(int fd, struct sockaddr *a, socklen_t *n) {
  if (!a || !n) {
    int r = accept(fd, NULL, NULL);
    bionic_translate_errno_after_failure(r);
    return r;
  }
  struct sockaddr_storage host;
  socklen_t host_len = sizeof(host);
  int r = accept(fd, (struct sockaddr *)(void *)&host, &host_len);
  if (r < 0) {
    bionic_translate_errno_after_failure(r);
    return r;
  }
  uint32_t out_len = (uint32_t)*n;
  if (bionic_host_sockaddr_to_bionic((const struct sockaddr *)(const void *)&host,
                                      host_len, a, &out_len) < 0) {
    close(r);
    return -1;
  }
  *n = (socklen_t)out_len;
  return r;
}
int bionic_getsockname_fake(int fd, struct sockaddr *a, socklen_t *n) {
  if (!a || !n) { errno = EINVAL; return -1; }
  struct sockaddr_storage host;
  socklen_t host_len = sizeof(host);
  int r = getsockname(fd, (struct sockaddr *)(void *)&host, &host_len);
  if (r < 0) { bionic_translate_errno_after_failure(r); return r; }
  uint32_t out_len = (uint32_t)*n;
  r = bionic_host_sockaddr_to_bionic((const struct sockaddr *)(const void *)&host,
                                      host_len, a, &out_len);
  *n = (socklen_t)out_len;
  return r;
}
int bionic_getpeername_fake(int fd, struct sockaddr *a, socklen_t *n) {
  if (!a || !n) { errno = EINVAL; return -1; }
  struct sockaddr_storage host;
  socklen_t host_len = sizeof(host);
  int r = getpeername(fd, (struct sockaddr *)(void *)&host, &host_len);
  if (r < 0) { bionic_translate_errno_after_failure(r); return r; }
  uint32_t out_len = (uint32_t)*n;
  r = bionic_host_sockaddr_to_bionic((const struct sockaddr *)(const void *)&host,
                                      host_len, a, &out_len);
  *n = (socklen_t)out_len;
  return r;
}
int bionic_getsockopt_fake(int fd, int level, int opt, void *v, socklen_t *n) {
  int host_opt = bionic_host_sockopt(level, opt);
  int host_level = level == BIONIC_SOL_SOCKET ? SOL_SOCKET : level;
  int r = getsockopt(fd, host_level, host_opt, v, n);
  if (r == 0 && level == BIONIC_SOL_SOCKET &&
      opt == BIONIC_SO_ERROR && v && n && *n >= sizeof(int)) {
    int *socket_error = (int *)v;
    *socket_error = bionic_errno_from_host(*socket_error);
  }
  bionic_translate_errno_after_failure(r);
  return r;
}
int bionic_setsockopt_fake(int fd, int level, int opt, const void *v, socklen_t n) {
  int host_opt = bionic_host_sockopt(level, opt);
  int host_level = level == BIONIC_SOL_SOCKET ? SOL_SOCKET : level;
  int r = setsockopt(fd, host_level, host_opt, v, n);
  bionic_translate_errno_after_failure(r);
  return r;
}
int bionic_shutdown_fake(int fd, int how) {
  int r = shutdown(fd, how); bionic_translate_errno_after_failure(r); return r;
}
long bionic_send_fake(int fd, const void *b, size_t n, int f) {
  long r = (long)send(fd, b, n, f); bionic_translate_errno_after_failure((int)r); return r;
}
long bionic_recv_fake(int fd, void *b, size_t n, int f) {
  long r = (long)recv(fd, b, n, f); bionic_translate_errno_after_failure((int)r); return r;
}
long bionic_sendto_fake(int fd, const void *b, size_t n, int f,
                         const struct sockaddr *a, socklen_t alen) {
  struct sockaddr_storage host;
  socklen_t host_len = 0;
  if (bionic_sockaddr_to_host(a, (uint32_t)alen, &host, &host_len) < 0) return -1;
  long r = (long)sendto(fd, b, n, f, (const struct sockaddr *)(const void *)&host, host_len);
  bionic_translate_errno_after_failure((int)r);
  return r;
}
long bionic_recvfrom_fake(int fd, void *b, size_t n, int f,
                           struct sockaddr *a, socklen_t *alen) {
  if (!a || !alen) {
    long r = (long)recvfrom(fd, b, n, f, NULL, NULL);
    bionic_translate_errno_after_failure((int)r);
    return r;
  }
  struct sockaddr_storage host;
  socklen_t host_len = sizeof(host);
  long r = (long)recvfrom(fd, b, n, f, (struct sockaddr *)(void *)&host, &host_len);
  if (r < 0) { bionic_translate_errno_after_failure((int)r); return r; }
  uint32_t out_len = (uint32_t)*alen;
  if (bionic_host_sockaddr_to_bionic((const struct sockaddr *)(const void *)&host,
                                      host_len, a, &out_len) < 0) return -1;
  *alen = (socklen_t)out_len;
  return r;
}
int bionic_poll_fake(struct pollfd *fds, nfds_t n, int timeout) {
  int r = poll(fds, n, timeout); bionic_translate_errno_after_failure(r); return r;
}
int bionic_select_fake(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
  int out = select(n, r, w, e, tv); bionic_translate_errno_after_failure(out); return out;
}
int bionic_gethostname_fake(char *name, size_t n) {
  int r = gethostname(name, n); bionic_translate_errno_after_failure(r); return r;
}
unsigned long bionic_inet_addr_fake(const char *text) { return (unsigned long)inet_addr(text); }
const char *bionic_inet_ntop_fake(int af, const void *src, char *dst, socklen_t n) {
  const int host_af = bionic_host_af_from_bionic(af);
  if (host_af < 0) { errno = BIONIC_EAFNOSUPPORT; return NULL; }
  const char *r = inet_ntop(host_af, src, dst, n);
  if (!r) errno = bionic_errno_from_host(errno);
  return r;
}
int bionic_inet_pton_fake(int af, const char *src, void *dst) {
  const int host_af = bionic_host_af_from_bionic(af);
  if (host_af < 0) { errno = BIONIC_EAFNOSUPPORT; return -1; }
  int r = inet_pton(host_af, src, dst);
  bionic_translate_errno_after_failure(r);
  return r;
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

static jstring read_asset_string_contract(jobject argument,
                                          int missing_returns_null,
                                          int trace_result) {
  const char *name = jni_cstr(argument);
  if (!name || !*name)
    return missing_returns_null ? NULL : jni_make_string("");
  static const char prefix[] = "file:///android_asset/";
  if (!strncmp(name, prefix, sizeof(prefix) - 1)) name += sizeof(prefix) - 1;

  /* Both Android call sites use Activity.getAssets().open(). Route through the
   * port's AssetManager bridge so mod overrides, the packed OBB, and extracted
   * assets keep the same precedence as every other native asset read. */
  void *asset = AAssetManager_open_fake(NULL, name, 0);
  if (!asset) {
    if (trace_result)
      debugPrintf("JNI asset string: %s -> missing\n", name);
    return missing_returns_null ? NULL : jni_make_string("");
  }

  const long length = AAsset_getLength_fake(asset);
  if (length < 0 || length > INT_MAX) {
    AAsset_close_fake(asset);
    return missing_returns_null ? NULL : jni_make_string("");
  }
  char *text = malloc((size_t)length + 1u);
  if (!text) {
    AAsset_close_fake(asset);
    return missing_returns_null ? NULL : jni_make_string("");
  }
  const int got = AAsset_read_fake(asset, text, (size_t)length);
  AAsset_close_fake(asset);
  if (got != length) {
    free(text);
    return missing_returns_null ? NULL : jni_make_string("");
  }

  text[length] = '\0';
  if (trace_result)
    debugPrintf("JNI asset string: %s -> %d bytes\n", name, got);
  jstring result = jni_make_string(text);
  free(text);
  return result;
}

static jstring read_asset_as_string(jobject argument) {
  return read_asset_string_contract(
      argument, 0, PVZ2_ENABLE_ASSET_TRACE || PVZ2_ENABLE_VERBOSE_RUNTIME_LOG);
}

/* classes2.dex Utility.readFile(String) opens the application AssetManager,
 * decodes the complete asset as UTF-8, and returns NULL on IOException. This
 * differs from Glu AndroidPlatform.readAssetAsString(), whose callers expect an
 * empty string for a missing optional asset. */
static jstring nimble_utility_read_file(jobject argument) {
  return read_asset_string_contract(argument, 1, 0);
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

/* Historical builds could persist this deterministic fallback ID. It is never
 * accepted as a player identity; device_uuid remains a separate install ID. */
#define LEGACY_PLAYER_ID "53776974-6368-4076-9A32-4C6F63616C31"


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

/* In-memory Java maps need true remove semantics.  This deliberately performs
 * no disk I/O; callers decide whether the backing structure is persistent. */
static int store_remove_raw(PropertyEntry *store, int count, const char *scope,
                            const char *key) {
  if (!scope) scope = "";
  if (!key) return 0;
  for (int i = 0; i < count; ++i) {
    if (!store[i].scope || strcmp(store[i].scope, scope) ||
        strcmp(store[i].key, key))
      continue;
    free(store[i].scope);
    free(store[i].key);
    free(store[i].value);
    memset(&store[i], 0, sizeof(store[i]));
    return 1;
  }
  return 0;
}

/*  The compatibility layer the native path showed the historical deterministic Switch
 * fallback UUID is the persisted PopCap/Glu game identity bundle:
 *   config/pcpid + pcpid_1..5, tagsIDs/userID, and string-store UserID.
 * device_uuid is a separate Android/Synergy compatibility identity and is
 * intentionally preserved because the live Synergy/Nexus path already uses
 * it successfully. Migrate only the exact known legacy bundle and let stock
 * PlayerIdentityService/Util_GetUUIDString regenerate the local game UUID. */
static int identity_remove_legacy_game_identity_bundle(void) {
  const char *pcpid = store_get_raw(property_store, 64, "config", "pcpid");
  if (!pcpid || strcasecmp(pcpid, LEGACY_PLAYER_ID))
    return 0;

  static const struct {
    int kind;
    const char *scope;
    const char *key;
  } targets[] = {
      {0, "config", "pcpid"},
      {0, "config", "pcpid_1"},
      {0, "config", "pcpid_2"},
      {0, "config", "pcpid_3"},
      {0, "config", "pcpid_4"},
      {0, "config", "pcpid_5"},
      {0, "tagsIDs", "userID"},
      {1, "", "UserID"},
  };

  int removed = 0;
  for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
    PropertyEntry *store = targets[i].kind ? string_store : property_store;
    const int count = targets[i].kind ? 32 : 64;
    const int did_remove = store_remove_raw(store, count, targets[i].scope,
                                            targets[i].key);
    if (did_remove > 0) removed++;
  }
  return removed;
}

static void java_store_ensure_loaded(void);

static int java_store_save(void) {
  if (!java_store_loaded || java_store_loading) return 0;

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
        return 0;
      }
      payload_size += JAVA_STORE_RECORD_SIZE + scope_len + key_len + value_len;
      entry_count++;
    }
  }

  if (payload_size > JAVA_STORE_MAX_FILE - JAVA_STORE_HEADER_SIZE) return 0;
  const size_t total_size = JAVA_STORE_HEADER_SIZE + payload_size;
  unsigned char *blob = calloc(1, total_size ? total_size : 1);
  if (!blob) return 0;
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
    const int synced = fd < 0 || fsync(fd) == 0;
    const int closed = fclose_fake(file) == 0;
    if (written == total_size && flushed && synced && closed &&
        rename_fake(JAVA_STORE_TMP_PATH, JAVA_STORE_PATH) == 0)
      committed = 1;
  }
  if (!committed)
    unlink_fake(JAVA_STORE_TMP_PATH);
  free(blob);
  return committed;
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
    const int identity_removed = identity_remove_legacy_game_identity_bundle();
    if (identity_removed > 0) {
      java_store_save();
      debugLogFlush();
    }
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

static unsigned nimble_persistence_scope_requests;
static unsigned nimble_persistence_method_calls;
static unsigned nimble_persistence_accepted_writes;
static unsigned nimble_persistence_sync_calls;
static unsigned nimble_persistence_faults;
static int nimble_persistence_dirty;
static u64 nimble_persistence_save_due_tick;

/* Original Persistence.flagChange() cancels/restarts a one-shot Timer at
 * 0.5 seconds.  Reuse Horizon's system tick so fake-JNI persistence has the
 * same deferred synchronization semantics without a Java Handler. */
static void nimble_persistence_flag_change(void) {
  nimble_persistence_dirty = 1;
  const u64 hz = armGetSystemTickFreq();
  nimble_persistence_save_due_tick = armGetSystemTick() + hz / 2u;
}

static void nimble_persistence_pump_auto_sync(void) {
  if (!nimble_persistence_dirty || !nimble_persistence_save_due_tick) return;
  if (armGetSystemTick() < nimble_persistence_save_due_tick) return;
  nimble_persistence_sync_calls++;
  const int committed = java_store_save();
  if (committed) {
    nimble_persistence_dirty = 0;
    nimble_persistence_save_due_tick = 0;
  } else {
    nimble_persistence_faults++;
    const u64 hz = armGetSystemTickFreq();
    nimble_persistence_save_due_tick = armGetSystemTick() + hz / 2u;
  }
  debugPrintf("compat: Persistence.flagChange auto-synchronize committed=%d syncs=%u faults=%u dirty=%d\n",
              committed, nimble_persistence_sync_calls,
              nimble_persistence_faults, nimble_persistence_dirty);
  debugLogFlush();
}

int pvz2_nimble_persistence_preflight_ok(void) {
  return nimble_persistence_scope_requests > 0 &&
      nimble_persistence_method_calls > 0 &&
      nimble_persistence_faults == 0 &&
      !nimble_persistence_dirty;
}

void pvz2_nimble_persistence_log_summary(void) {
  debugPrintf("compat: Persistence preflight scopes=%u methods=%u writes=%u syncs=%u faults=%u dirty=%d ok=%d\n",
              nimble_persistence_scope_requests,
              nimble_persistence_method_calls,
              nimble_persistence_accepted_writes,
              nimble_persistence_sync_calls,
              nimble_persistence_faults,
              nimble_persistence_dirty,
              pvz2_nimble_persistence_preflight_ok());
  debugLogFlush();
}

/*  The compatibility layer proves stock Nimble setup already asks for the Java
 * Persistence wrapper. Exact EA-account restore AArch64 constructs the
 * accessToken, userId, and loggedIn keys before restoring an existing session.
 * Keep this implementation inside the existing checksummed Java preference
 * file, namespaced by Storage enum and component ID. It never touches pp.dat,
 * local_profiles, profile/Joust structures, or invents a token/player ID.
 * Only values supplied by stock Nimble setValue() are persisted. */
static jobject nimble_persistence_for_component(jobject component_argument,
                                                jobject storage_argument) {
  const char *component = jni_cstr(component_argument);
  const char *storage = jni_object_string(storage_argument);
  if (!component || !*component || !storage ||
      (strcmp(storage, "DOCUMENT") && strcmp(storage, "CACHE"))) {
    nimble_persistence_faults++;
    debugPrintf("compat: PersistenceService request rejected component=%s storage=%s faults=%u\n",
                component ? component : "(null)",
                storage ? storage : "(null)", nimble_persistence_faults);
    debugLogFlush();
    return NULL;
  }

  char scope[256];
  const int written = snprintf(scope, sizeof(scope),
                               "nimble.persistence.%s.%s",
                               storage, component);
  if (written < 0 || (size_t)written >= sizeof(scope)) {
    nimble_persistence_faults++;
    debugPrintf("compat: PersistenceService scope overflow component_len=%zu storage=%s faults=%u\n",
                strlen(component), storage, nimble_persistence_faults);
    debugLogFlush();
    return NULL;
  }

  jobject persistence = jni_make_object_class_string(
      "com/ea/nimble/Persistence", scope);
  if (!persistence) {
    nimble_persistence_faults++;
    debugPrintf("compat: PersistenceService wrapper allocation failed component=%s storage=%s faults=%u\n",
                component, storage, nimble_persistence_faults);
    debugLogFlush();
    return NULL;
  }
  nimble_persistence_scope_requests++;
  (void)0;
  debugLogFlush();
  return persistence;
}

static const char *nimble_persistence_serialize(jobject object,
                                                char *scalar_buffer,
                                                size_t scalar_buffer_size,
                                                const char **type_name) {
  if (type_name) *type_name = "unsupported";
  if (!object || !scalar_buffer || scalar_buffer_size < 32) return NULL;

  /* JNI strings already own stable storage for the duration of this upcall.
   * Return that pointer directly so genuine JWT/access-token values are not
   * truncated by an arbitrary small bridge buffer. */
  const char *text = jni_cstr(object);
  if (!text) {
    const char *class_name = jni_object_class_name(object);
    if (class_name &&
        (!strcmp(class_name, "java/lang/String") ||
         !strcmp(class_name, "java.lang.String")))
      text = jni_object_string(object);
  }
  if (text) {
    if (type_name) *type_name = "String";
    return text;
  }

  const char *class_name = jni_object_class_name(object);
  const jlong value = jni_object_long(object);
  if (class_name &&
      (!strcmp(class_name, "java/lang/Boolean") ||
       !strcmp(class_name, "java.lang.Boolean"))) {
    snprintf(scalar_buffer, scalar_buffer_size, "%s",
             value ? "true" : "false");
    if (type_name) *type_name = "Boolean";
    return scalar_buffer;
  }
  if (class_name &&
      (!strcmp(class_name, "java/lang/Integer") ||
       !strcmp(class_name, "java.lang.Integer") ||
       !strcmp(class_name, "java/lang/Long") ||
       !strcmp(class_name, "java.lang.Long"))) {
    snprintf(scalar_buffer, scalar_buffer_size, "%lld", (long long)value);
    if (type_name) *type_name = strstr(class_name, "Long") ? "Long" : "Integer";
    return scalar_buffer;
  }
  return NULL;
}

/* diagnostic: player identity must come from stock/EA reconciliation, never from
 * the Switch install UUID.  Keep device_uuid() only for Android device/install
 * compatibility.  Player-ID values accepted here must have been supplied by
 * the native Nimble/EA path or observed in a live PopCap response. */
#define EA_ID_SCOPE "ea.playerIdentity"

/* ApplicationEnvironmentImpl.m_playerIdMap is process-local.  Only the
 * DEX-defined gamePlayerId bridge is persisted separately through DOCUMENT. */
static PropertyEntry ea_player_id_map[32];
static int ea_identity_legacy_alias_seen;
static int identity_legacy_persistence_scanned;
static int identity_persona_persistence_scanned;

static void nimble_queue_empty_broadcast_action(const char *action);
static unsigned nimble_queue_serializable_broadcast(jobject action_object, jobject extras);
static void synergy_id_manager_on_startup_finished(void);

static int ea_identity_is_synthetic(const char *value) {
  if (!value || !*value) return 0;

  /* Never call device_uuid() here. Identity validation must not create a
   * local UUID as a side effect on a clean install. Reject the legacy
   * deterministic fallback explicitly, then compare against an install UUID
   * only if one already exists on disk. */
  if (!strcasecmp(value, LEGACY_PLAYER_ID))
    return 1;

  char install[37] = {0};
  FILE *file = fopen(DATA_DIR "/No_Backup/device_uuid", "rb");
  if (!file) return 0;
  const size_t size = fread(install, 1, 36, file);
  fclose(file);
  if (size != 36) return 0;
  install[36] = 0;
  return !strcasecmp(value, install);
}

static int ea_identity_value_ok(const char *value) {
  if (!value || !*value || strlen(value) >= 160) return 0;
  return !ea_identity_is_synthetic(value);
}


static const char *ea_identity_get(const char *key) {
  return store_get(string_store, 32, EA_ID_SCOPE, key);
}

static int ea_identity_store_map_value(const char *key, const char *value,
                                       const char *source) {
  /* DEX ApplicationEnvironmentImpl.setPlayerId(): invalid/empty keys are
   * ignored; old/new are compared through Utility.safeString(); empty values
   * remove an existing entry; only a real change broadcasts mapChange. */
  if (!key || !*key) return 0;
  const char *old = store_get_raw(ea_player_id_map, 32, "", key);
  const char *safe_old = old ? old : "";
  const char *safe_new = value ? value : "";
  if (!strcmp(safe_old, safe_new)) {
    (void)0;
    return 0;
  }

  int changed = 0;
  if (*safe_new) {
    if (!ea_identity_value_ok(safe_new)) {
      debugPrintf("compat: rejected synthetic/invalid playerIdMap key=%s source=%s value=%s\n",
                  key, source ? source : "?", safe_new);
      return 0;
    }
    changed = store_set_raw(ea_player_id_map, 32, "", key, safe_new) > 0;
  } else {
    changed = store_remove_raw(ea_player_id_map, 32, "", key);
  }

  if (!changed) return 0;
  (void)0;
  nimble_queue_empty_broadcast_action("nimble.notification.playerIdMapChange");
  return 1;
}

static const char *ea_identity_get_map_value(const char *key) {
  return key ? store_get_raw(ea_player_id_map, 32, "", key) : NULL;
}

static jobject ea_identity_make_player_id_map(void) {
  const char *keys[32];
  const char *values[32];
  int used = 0;
  for (int i = 0; i < 32 && used < 32; ++i) {
    const PropertyEntry *entry = &ea_player_id_map[i];
    if (!entry->scope || !entry->key || !entry->value || !*entry->value)
      continue;
    keys[used] = entry->key;
    values[used] = entry->value;
    used++;
  }
  (void)0;
  return jni_make_string_map(keys, values, used);
}

static void compat_persist_game_specified_player_id(const char *value) {
  static const char scope[] =
      "nimble.persistence.DOCUMENT.com.ea.nimble.applicationEnvironment";
  static const char key[] =
      "nimble_applicationenvironment_game_specified_id";
  java_store_ensure_loaded();
  /* DEX Persistence.setValue(): a null Serializable removes the key; an
   * allocated empty String remains a present empty value. Preserve that
   * distinction rather than serializing null as "". */
  const int stored = value
      ? store_set_raw(property_store, 64, scope, key, value)
      : store_remove_raw(property_store, 64, scope, key);
  if (stored < 0) {
    nimble_persistence_faults++;
    debugPrintf("compat: gamePlayerId DOCUMENT persistence failed faults=%u\n",
                nimble_persistence_faults);
    return;
  }
  nimble_persistence_accepted_writes++;
  if (stored > 0) nimble_persistence_flag_change();
  (void)0;
}

static void set_game_specified_player_id(const char *value,
                                                   const char *source) {
  if (value && *value && !ea_identity_value_ok(value)) {
    debugPrintf("compat: rejected synthetic/invalid gamePlayerId source=%s value=%s\n",
                source ? source : "?", value);
    return;
  }
  (void)ea_identity_store_map_value("gamePlayerId", value, source);
  /* DEX setGameSpecifiedPlayerId always calls DOCUMENT setValue after the map
   * update, even when the map value was unchanged. */
  compat_persist_game_specified_player_id(value);
}

static void compat_application_environment_restore(void) {
  static const char scope[] =
      "nimble.persistence.DOCUMENT.com.ea.nimble.applicationEnvironment";
  static const char key[] =
      "nimble_applicationenvironment_game_specified_id";
  const char *value = store_get(property_store, 64, scope, key);
  if (!value || !*value) {
    (void)0;
    return;
  }
  if (!ea_identity_value_ok(value)) {
    debugPrintf("compat: ApplicationEnvironment.restore rejected synthetic/invalid persisted gamePlayerId\n");
    return;
  }
  (void)0;
  (void)ea_identity_store_map_value("gamePlayerId", value,
                                    "DOCUMENT-restore");
}

static int synergy_store_nullable(PropertyEntry *store, int count,
                                  const char *scope, const char *key,
                                  const char *value) {
  return (value && *value)
      ? store_set_raw(store, count, scope, key, value)
      : store_remove_raw(store, count, scope, key);
}

static void synergy_id_manager_save(void) {
  static const char doc_scope[] =
      "nimble.persistence.DOCUMENT.com.ea.nimble.synergyidmanager.anonymousId";
  static const char cache_scope[] =
      "nimble.persistence.CACHE.com.ea.nimble.synergyidmanager";
  java_store_ensure_loaded();
  int ok = 1;
  if (store_set_raw(property_store, 64, doc_scope, "dataVersion", "1.0.0") < 0)
    ok = 0;
  if (synergy_store_nullable(property_store, 64, doc_scope, "anonymousId",
                             synergy_id_manager_anonymous_id) < 0)
    ok = 0;
  if (store_set_raw(property_store, 64, cache_scope, "dataVersion", "1.0.0") < 0)
    ok = 0;
  if (synergy_store_nullable(property_store, 64, cache_scope, "currentId",
                             synergy_current_id) < 0)
    ok = 0;
  if (synergy_store_nullable(property_store, 64, cache_scope, "authenticator",
                             synergy_authenticator) < 0)
    ok = 0;
  const int committed = ok ? java_store_save() : 0;
  if (!committed) nimble_persistence_faults++;
  (void)0;
}

static void synergy_queue_id_change(const char *action,
                                    const char *previous,
                                    const char *current) {
  const char *keys[] = {"previousSynergyId", "currentSynergyId"};
  const char *values[] = {previous ? previous : "", current ? current : ""};
  jobject extras = jni_make_string_map(keys, values, 2);
  jobject action_object = jni_make_string(action);
  if (extras && action_object)
    (void)nimble_queue_serializable_broadcast(action_object, extras);
  if (action_object) jni_free_wrapper(action_object);
}

static void synergy_id_manager_set_current(const char *value) {
  char previous[sizeof(synergy_current_id)];
  snprintf(previous, sizeof(previous), "%s", synergy_current_id);
  if (value && *value)
    snprintf(synergy_current_id, sizeof(synergy_current_id), "%s", value);
  else
    synergy_current_id[0] = 0;
  synergy_id_manager_save();
  if (strcmp(previous, synergy_current_id)) {
    synergy_queue_id_change(
        "nimble.synergyidmanager.notification.synergy_id_changed",
        previous, synergy_current_id);
    (void)0;
  }
}

static void synergy_id_manager_set_anonymous(const char *value) {
  char previous[sizeof(synergy_id_manager_anonymous_id)];
  snprintf(previous, sizeof(previous), "%s", synergy_id_manager_anonymous_id);
  if (value && *value)
    snprintf(synergy_id_manager_anonymous_id,
             sizeof(synergy_id_manager_anonymous_id), "%s", value);
  else if (!synergy_id_manager_anonymous_id[0])
    synergy_id_manager_anonymous_id[0] = 0;
  else
    return; /* exact guard: do not replace an existing valid ID with invalid */
  synergy_id_manager_save();
  if (strcmp(previous, synergy_id_manager_anonymous_id)) {
    synergy_queue_id_change(
        "nimble.synergyidmanager.notification.anonymous_synergy_id_changed",
        previous, synergy_id_manager_anonymous_id);
    (void)0;
  }
  /* DEX setAnonymousSynergyId(): when no authenticator owns the current ID,
   * anonymous becomes current immediately. */
  if (!synergy_authenticator[0])
    synergy_id_manager_set_current(synergy_id_manager_anonymous_id);
}

static void synergy_id_manager_wakeup(void) {
  static const char doc_scope[] =
      "nimble.persistence.DOCUMENT.com.ea.nimble.synergyidmanager.anonymousId";
  static const char cache_scope[] =
      "nimble.persistence.CACHE.com.ea.nimble.synergyidmanager";
  java_store_ensure_loaded();
  const char *anon = store_get(property_store, 64, doc_scope, "anonymousId");
  const char *current = store_get(property_store, 64, cache_scope, "currentId");
  const char *auth = store_get(property_store, 64, cache_scope, "authenticator");
  snprintf(synergy_id_manager_anonymous_id,
           sizeof(synergy_id_manager_anonymous_id), "%s", anon ? anon : "");
  snprintf(synergy_current_id,
           sizeof(synergy_current_id), "%s", current ? current : "");
  snprintf(synergy_authenticator,
           sizeof(synergy_authenticator), "%s", auth ? auth : "");
  (void)0;
  if (!synergy_current_id[0] && synergy_id_manager_anonymous_id[0])
    synergy_id_manager_set_current(synergy_id_manager_anonymous_id);
}

static void synergy_id_manager_on_startup_finished(void) {
  if (!synergy_environment_live || !synergy_anonymous_id[0])
    return;
  (void)0;
  synergy_id_manager_set_anonymous(synergy_anonymous_id);
  if (!synergy_current_id[0])
    synergy_id_manager_set_current(synergy_id_manager_anonymous_id);
}


static const char *ea_identity_effective_player_id(void) {
  /* ApplicationEnvironmentImpl has no separate GameSpecifiedPlayerId slot. */
  const char *value = ea_identity_get_map_value("gamePlayerId");
  if (ea_identity_value_ok(value)) return value;
  value = ea_identity_get("ServerPlayerId");
  return ea_identity_value_ok(value) ? value : NULL;
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
}

static jstring get_string_store(jobject key_argument) {
  const char *key = jni_cstr(key_argument);
  const char *value = key ? store_get(string_store, 32, "", key) : NULL;
  if (!value && key && !strcmp(key, "LocationISOCode")) {
    /* Keep Glu's region view consistent with the Switch platform data we
     * already expose through CountryCode/DeviceCountry. */
    value = get_switch_locale().country;
  }
  return value ? jni_make_string(value) : NULL;
}

static void set_string_store(jobject key_argument, jobject value_argument) {
  const char *key = jni_cstr(key_argument);
  const char *value = jni_cstr(value_argument);
  store_set(string_store, 32, "", key, value);
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
  HTTP_SYNERGY_BOOTSTRAP_DIRECTION,
  HTTP_SYNERGY_BOOTSTRAP_DEVICE_ID,
  HTTP_SYNERGY_BOOTSTRAP_ANON_UID,
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
  int body_length;
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

static void ea_identity_prepare_joust_request(const HttpRequest *request,
                                              const char *url, const void *body,
                                              int body_length,
                                              char *url_out, size_t url_cap,
                                              char *body_out, size_t body_cap,
                                              const char **effective_url,
                                              const void **effective_body,
                                              int *effective_body_length) {
  *effective_url = url;
  *effective_body = body;
  *effective_body_length = body_length;
  if (!request || request->kind != HTTP_ANDROID || !url ||
      !strstr(url, "/joust/v1/"))
    return;
  const char *authoritative = ea_identity_effective_player_id();
  if (!ea_identity_value_ok(authoritative)) return;

  const char *parameter = strstr(url, "playerId=");
  if (parameter) {
    const char *value = parameter + strlen("playerId=");
    const char *after = strchr(value, '&');
    if (!after) after = url + strlen(url);
    const size_t prefix = (size_t)(value - url);
    if (prefix < url_cap) {
      const int written = snprintf(url_out, url_cap, "%.*s%s%s",
                                   (int)prefix, url, authoritative, after);
      if (written > 0 && (size_t)written < url_cap) {
        *effective_url = url_out;
        (void)0;
      }
    }
  }

  if (body && body_length > 0 && body_cap > 0 &&
      strstr(url, "/joust/v1/registerForTournament")) {
    const char *text = (const char *)body;
    const char marker[] = "\"playerId\":\"";
    const char *start = NULL;
    for (int i = 0; i + (int)sizeof(marker) - 1 <= body_length; ++i) {
      if (!memcmp(text + i, marker, sizeof(marker) - 1)) {
        start = text + i + sizeof(marker) - 1;
        break;
      }
    }
    if (start) {
      const char *limit = text + body_length;
      const char *finish = start;
      while (finish < limit && *finish != '"') ++finish;
      if (finish < limit) {
        const size_t prefix = (size_t)(start - text);
        const size_t suffix = (size_t)(limit - finish);
        const size_t needed = prefix + strlen(authoritative) + suffix;
        if (needed < body_cap) {
          memcpy(body_out, text, prefix);
          memcpy(body_out + prefix, authoritative, strlen(authoritative));
          memcpy(body_out + prefix + strlen(authoritative), finish, suffix);
          *effective_body = body_out;
          *effective_body_length = (int)needed;
          (void)0;
        }
      }
    }
  }
}

static int http_request_start(HttpRequest *request, const char *url,
                              const char *method, const char *raw_headers,
                              const void *body, int body_length, long timeout_ms) {
  if (!request || !url || !*url || body_length < 0 ||
      http_request_count >= MAX_HTTP_REQUESTS)
    return 0;
  char identity_url[2048];
  char identity_body[512];
  const char *effective_url = url;
  const void *effective_body = body;
  int effective_body_length = body_length;
  ea_identity_prepare_joust_request(request, url, body, body_length,
                                    identity_url, sizeof(identity_url),
                                    identity_body, sizeof(identity_body),
                                    &effective_url, &effective_body,
                                    &effective_body_length);
  if (!curl_ready && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
  curl_ready = 1;
  if (!http_multi && !(http_multi = curl_multi_init())) return 0;

  request->url = strdup(effective_url);
  request->method = strdup(method && *method ? method : "GET");
  request->body_length = effective_body_length;
  if (effective_body_length > 0) {
    request->body = malloc((size_t)effective_body_length);
    if (request->body) memcpy(request->body, effective_body, (size_t)effective_body_length);
  }
  if (!request->url || !request->method ||
      (effective_body_length > 0 && !request->body))
    return 0;
  log_account_http_request(request->url, request->method, effective_body_length);
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
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
  /* A normal SD install has no Android/system CA bundle for switch-curl. */
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  if (effective_body_length > 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)effective_body_length);
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

/* 13.3.1 classes2.dex: ApplicationEnvironmentImpl maps these accessors to
 * Android Build/package fields. Horizon has no android.os.Build, so expose the
 * closest real platform facts rather than the old Google-emulator placeholders.
 * These values describe the platform only; none is a player/account/Synergy ID. */
static const char *synergy_application_bundle_id(void) {
  return "com.ea.game.pvz2_row";
}

static const char *synergy_device_manufacturer(void) {
  return "Nintendo";
}

static const char *synergy_device_model(void) {
  return "Nintendo Switch";
}

static const char *synergy_device_brand(void) {
  return "Nintendo";
}

static const char *synergy_device_codename(void) {
  return "NX";
}

static const char *synergy_device_fingerprint(void) {
  /* Android returns Build.FINGERPRINT. Horizon exposes no equivalent string in
   * this compatibility layer; identify the real OS family without pretending
   * to be an Android build. */
  return "Horizon";
}

static const char *synergy_device_string(void) {
  /* Exact DEX behavior: Build.MANUFACTURER + Build.MODEL, no separator. */
  return "NintendoNintendo Switch";
}

/* EnvironmentDataContainer.setGetDirectionResponseDictionary() converts the
 * numeric sellId/productId/hwId Integer values to strings. Preserve that
 * representation without interpreting them as identity values. */
static int synergy_json_scalar_string(const unsigned char *data, size_t length,
                                      const char *key, char *out,
                                      size_t capacity) {
  if (out && capacity) out[0] = 0;
  if (!data || !length || length > 4u * 1024u * 1024u || !key || !out ||
      capacity < 2)
    return 0;
  char *json = malloc(length + 1u);
  if (!json) return 0;
  memcpy(json, data, length);
  json[length] = 0;
  const char *limit = json + length;
  const char *value = json_value_start(json, limit, key);
  int found = 0;
  if (value && value < limit) {
    if (*value == '"') {
      found = json_copy_string_at(value, limit, out, capacity);
    } else {
      const char *end = value;
      while (end < limit && *end && *end != ',' && *end != '}' &&
             !isspace((unsigned char)*end))
        ++end;
      size_t n = (size_t)(end - value);
      if (n > 0 && n < capacity) {
        int numeric = 1;
        for (size_t i = 0; i < n; ++i) {
          if ((i == 0 && value[i] == '-') || isdigit((unsigned char)value[i]))
            continue;
          numeric = 0;
          break;
        }
        if (numeric) {
          memcpy(out, value, n);
          out[n] = 0;
          found = 1;
        }
      }
    }
  }
  free(json);
  return found;
}

static int synergy_server_url(const unsigned char *data, size_t length,
                              const char *wanted_key, char *out,
                              size_t capacity) {
  if (out && capacity) out[0] = 0;
  if (!data || !length || length > 4u * 1024u * 1024u || !wanted_key ||
      !out || capacity < 8)
    return 0;
  char *json = malloc(length + 1u);
  if (!json) return 0;
  memcpy(json, data, length);
  json[length] = 0;
  const char *limit = json + length;
  const char *server_data = json_value_start(json, limit, "serverData");
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
      char key[160];
      char value[1024];
      const char *key_names[] = {"key"};
      const char *value_names[] = {"value"};
      if (json_copy_string(cursor, object_end, key_names, 1,
                           key, sizeof(key)) &&
          !strcmp(key, wanted_key) &&
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

static int synergy_json_copy_string_value(const unsigned char *data,
                                          size_t length, const char *key,
                                          char *out, size_t capacity) {
  if (out && capacity) out[0] = 0;
  if (!data || !length || length > 4u * 1024u * 1024u || !key || !out ||
      capacity < 2)
    return 0;
  char *json = malloc(length + 1u);
  if (!json) return 0;
  memcpy(json, data, length);
  json[length] = 0;
  const char *keys[] = {key};
  const int found = json_copy_string(json, json + length, keys, 1,
                                     out, capacity);
  free(json);
  return found;
}

static void synergy_capture_direction_response(const unsigned char *data,
                                               size_t length) {
  free(synergy_direction_json);
  synergy_direction_json = NULL;
  synergy_direction_json_length = 0;
  if (!data || !length || length > 4u * 1024u * 1024u) return;
  synergy_direction_json = malloc(length + 1u);
  if (!synergy_direction_json) return;
  memcpy(synergy_direction_json, data, length);
  synergy_direction_json[length] = 0;
  synergy_direction_json_length = length;
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) == 0)
    synergy_direction_timestamp_ms =
        (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
  else
    synergy_direction_timestamp_ms = (long long)time(NULL) * 1000LL;
  const SwitchLocaleInfo locale = get_switch_locale();
  snprintf(synergy_direction_language, sizeof(synergy_direction_language),
           "%s", locale.locale);
}

static int synergy_direction_scalar(const char *key, char *out,
                                    size_t capacity) {
  return synergy_direction_json &&
         synergy_json_scalar_string((const unsigned char *)synergy_direction_json,
                                    synergy_direction_json_length, key, out, capacity);
}

static int synergy_direction_string(const char *key, char *out,
                                    size_t capacity) {
  return synergy_direction_json &&
         synergy_json_copy_string_value((const unsigned char *)synergy_direction_json,
                                        synergy_direction_json_length, key, out, capacity);
}

static int synergy_direction_server_url(const char *key, char *out,
                                        size_t capacity) {
  return synergy_direction_json &&
         synergy_server_url((const unsigned char *)synergy_direction_json,
                            synergy_direction_json_length, key, out, capacity);
}

static int synergy_direction_int(const char *key, int fallback) {
  char value[64];
  if (!synergy_direction_scalar(key, value, sizeof(value))) return fallback;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (!end || *end || parsed < INT_MIN || parsed > INT_MAX) return fallback;
  return (int)parsed;
}

static int synergy_direction_feature_disabled(const char *wanted) {
  if (!synergy_direction_json || !wanted || !*wanted) return 0;
  const char *begin = synergy_direction_json;
  const char *limit = begin + synergy_direction_json_length;
  const char *array = json_value_start(begin, limit, "disabledFeatures");
  if (!array || *array != '[') return 0;
  const char *cursor = array + 1;
  while (cursor < limit && *cursor != ']') {
    while (cursor < limit && (*cursor == ',' || isspace((unsigned char)*cursor)))
      ++cursor;
    if (cursor >= limit || *cursor == ']') break;
    if (*cursor != '"') { ++cursor; continue; }
    char feature[256];
    if (json_copy_string_at(cursor, limit, feature, sizeof(feature)) &&
        !strcmp(feature, wanted))
      return 1;
    ++cursor;
    while (cursor < limit && *cursor != '"' && *cursor != ',' && *cursor != ']')
      ++cursor;
    if (cursor < limit && *cursor == '"') ++cursor;
  }
  return 0;
}


static int synergy_start_synergy_getdeviceid(void);
static int synergy_start_synergy_getanonuid(void);

static void synergy_direction_complete(const HttpRequest *request,
                                                CURLcode code, long status,
                                                int ok) {
  synergy_direction_in_flight = 0;
  synergy_direction_ready = 1;
  const unsigned char *data = request ? request->response.data : NULL;
  const size_t length = request ? request->response.size : 0u;
  (void)0;
  if (ok) synergy_capture_direction_response(data, length);
  (void)0;

  synergy_hw_id[0] = 0;
  synergy_bootstrap_sell_id[0] = 0;
  synergy_user_url[0] = 0;
  synergy_json_scalar_string(data, length, "sellId", synergy_bootstrap_sell_id,
                             sizeof(synergy_bootstrap_sell_id));
  if (!ok ||
      !synergy_json_scalar_string(data, length, "hwId",
                                  synergy_hw_id,
                                  sizeof(synergy_hw_id)) ||
      !synergy_server_url(data, length, "synergy.user",
                          synergy_user_url,
                          sizeof(synergy_user_url))) {
    synergy_update_in_progress = 0;
    debugPrintf("compat: getDirection cannot form DEX EnvironmentDataContainer subset; getDeviceID suppressed\n");
    (void)0;
    debugLogFlush();
    return;
  }

  /* DEX loadConfig() has now supplied the private container dictionary and
   * server URL map. Do not expose clientSecret or promote any identity state. */
  (void)0;
  synergy_device_id_pending = 1;
  (void)0;
  debugLogFlush();
}

static int synergy_start_synergy_getdeviceid(void) {
  if (!synergy_device_id_pending ||
      synergy_device_id_in_flight ||
      synergy_device_id_done)
    return 0;
  if (!synergy_user_url[0] || !synergy_hw_id[0] ||
      !synergy_session_id[0]) {
    debugPrintf("compat: getDeviceID missing DEX prerequisite; suppressed\n");
    synergy_device_id_pending = 0;
    return 0;
  }

  const SwitchLocaleInfo locale = get_switch_locale();
  const char *android_id = device_uuid();
  if (!android_id || !*android_id) {
    debugPrintf("compat: getDeviceID AndroidId compatibility value unavailable\n");
    synergy_device_id_pending = 0;
    return 0;
  }

  char url[4096];
  const int base_len = snprintf(url, sizeof(url), "%s%s",
                                synergy_user_url,
                                "/user/api/android/getDeviceID");
  if (base_len < 0 || base_len >= (int)sizeof(url)) return 0;
  size_t used = (size_t)base_len;
  int first = 1;
  /* SynergyRequest.build() first adds the common application/Locale fields,
   * then the updater's apiVer/hwId/androidId map with putAll(). */
  if (!append_url_parameter(url, sizeof(url), &used, &first, "appVer", GAME_VERSION) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "appLang", locale.language) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "localization", locale.locale) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceLanguage", locale.language) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceLocale", locale.locale) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "hwId", synergy_hw_id) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "apiVer", "1.0.0") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "androidId", android_id)) {
    debugPrintf("compat: failed to build DEX getDeviceID URL\n");
    return 0;
  }

  char headers[768];
  const int header_length = snprintf(
      headers, sizeof(headers),
      "Content-Type: application/json\n"
      "SDK-VERSION: 1.64.0.27\n"
      "SDK-TYPE: Nimble\n"
      "EAM-SESSION: %s\n",
      synergy_session_id);
  HttpRequest *request = calloc(1, sizeof(*request));
  if (header_length < 0 || header_length >= (int)sizeof(headers) || !request) {
    http_request_free(request);
    return 0;
  }
  request->kind = HTTP_SYNERGY_BOOTSTRAP_DEVICE_ID;
  if (!http_request_start(request, url, "GET", headers, NULL, 0, 30000)) {
    http_request_free(request);
    debugPrintf("compat: failed to start DEX getDeviceID request\n");
    return 0;
  }
  synergy_device_id_pending = 0;
  synergy_device_id_in_flight = 1;
  (void)0;
  (void)0;
  (void)0;
  debugLogFlush();
  return 1;
}

static void synergy_device_id_complete(const HttpRequest *request,
                                               CURLcode code, long status,
                                               int ok) {
  synergy_device_id_in_flight = 0;
  const unsigned char *data = request ? request->response.data : NULL;
  const size_t length = request ? request->response.size : 0u;
  if (ok) {
    const int have_device_id = synergy_json_copy_string_value(
        data, length, "deviceId", synergy_ea_device_id,
        sizeof(synergy_ea_device_id));
    synergy_device_id_done = 1;
    (void)0;
    /* DEX callback $2 assigns response["deviceId"], then immediately calls
     * callSynergyGetAnonUid().  The compatibility layer continues only when the server supplied
     * a non-empty device ID; no local/device UUID can satisfy this transition. */
    if (have_device_id && synergy_ea_device_id[0]) {
      synergy_anon_pending = 1;
      (void)0;
    } else {
      synergy_update_in_progress = 0;
      debugPrintf("compat: getDeviceID response missing deviceId; startup sequence fails closed\n");
    }
    debugLogFlush();
    return;
  }

  /* DEX callback $2 has retry policy, but translating its Java exception
   * classification onto libcurl errors would be a new compatibility guess.
   * Keep The compatibility layer's failure path fail-closed; the hardware target is the exact
   * successful request/parse edge. */
  synergy_device_id_done = 1;
  synergy_update_in_progress = 0;
  debugPrintf("compat: GetEADeviceID failed curl=%d status=%ld bytes=%zu; no guessed retry mapping\n",
              (int)code, status, length);
  debugPrintf("compat: startup sequence remains unavailable\n");
  debugLogFlush();
}

static void synergy_queue_startup_finished_success(void) {
  const char *keys[] = {"result"};
  const char *values[] = {"1"};
  jobject extras = jni_make_string_map(keys, values, 1);
  jobject action = jni_make_string(
      "nimble.environment.notification.startup_requests_finished");
  if (!extras || !action) {
    debugPrintf("compat: startup_requests_finished allocation failed; fail closed\n");
    if (action) jni_free_wrapper(action);
    return;
  }
  if (synergy_main_application_active) {
    (void)nimble_queue_serializable_broadcast(action, extras);
    (void)0;
  } else {
    /* The DEX defers this notification until SynergyEnvironmentImpl.resume().
     * Horizon normally reaches active before the network response completes;
     * keep a pending bit for the exact inactive case instead of sending early. */
    (void)0;
    /* Reuse a dedicated sentinel through the existing live state: value 2
     * means live environment with pending success notification. */
    synergy_environment_live = 2;
  }
  jni_free_wrapper(action);
}

static void synergy_promote_environment_success(void) {
  if (!synergy_anonymous_id[0] || !synergy_direction_json ||
      !synergy_ea_device_id[0]) {
    synergy_update_in_progress = 0;
    debugPrintf("compat: EnvironmentDataContainer promotion prerequisite missing; fail closed\n");
    return;
  }

  /* SynergyEnvironmentImpl$2.callback(null): install the updater's completed
   * EnvironmentDataContainer first. isDataAvailable() is defined solely by
   * this container becoming non-null. */
  synergy_environment_live = 1;
  synergy_update_in_progress = 0;
  (void)0;

  /* The Java Persistence bridge still cannot materialize an arbitrary
   * Externalizable object. Keep the live object exact for this process and do
   * not write a fake scalar under environmentData. The exact SynergyIdManager
   * scalar persistence is handled when its startup receiver consumes the
   * success broadcast. */
  (void)0;

  /* SynergyEnvironmentImpl$2 compares the newly completed container against
   * m_previousValidEnvironmentDataContainer. On the first-install branch the
   * previous container is null, so getKeysOfDifferences() is non-null and the
   * empty startup_environment_data_changed broadcast precedes FINISHED. */
  nimble_queue_empty_broadcast_action(
      "nimble.environment.notification.startup_environment_data_changed");
  (void)0;
  synergy_queue_startup_finished_success();
  debugLogFlush();
}

static int synergy_start_synergy_getanonuid(void) {
  if (!synergy_anon_pending || synergy_anon_in_flight ||
      synergy_anon_done)
    return 0;
  if (!synergy_user_url[0] || !synergy_hw_id[0] ||
      !synergy_ea_device_id[0] || !synergy_session_id[0]) {
    debugPrintf("compat: getAnonUid missing DEX prerequisite; suppressed\n");
    synergy_anon_pending = 0;
    synergy_update_in_progress = 0;
    return 0;
  }

  /* Exact DEX fast path: a persisted anonymous ID suppresses the request and
   * is copied into the startup EnvironmentDataContainer. */
  if (synergy_id_manager_anonymous_id[0]) {
    snprintf(synergy_anonymous_id,
             sizeof(synergy_anonymous_id), "%s",
             synergy_id_manager_anonymous_id);
    synergy_anon_pending = 0;
    synergy_anon_done = 1;
    debugPrintf("compat: DEX getAnonUid skipped: anonymous Synergy ID restored from DOCUMENT len=%zu\n",
                strlen(synergy_anonymous_id));
    synergy_promote_environment_success();
    return 1;
  }

  const SwitchLocaleInfo locale = get_switch_locale();
  char url[4096];
  const int base_len = snprintf(url, sizeof(url), "%s%s",
                                synergy_user_url,
                                "/user/api/android/getAnonUid");
  if (base_len < 0 || base_len >= (int)sizeof(url)) return 0;
  size_t used = (size_t)base_len;
  int first = 1;
  if (!append_url_parameter(url, sizeof(url), &used, &first, "appVer", GAME_VERSION) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "appLang", locale.language) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "localization", locale.locale) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceLanguage", locale.language) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceLocale", locale.locale) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "apiVer", "1.0.0") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "updatePriority", "false") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "hwId", synergy_hw_id) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "eadeviceid", synergy_ea_device_id)) {
    debugPrintf("compat: failed to build DEX getAnonUid URL\n");
    return 0;
  }

  char headers[768];
  const int header_length = snprintf(
      headers, sizeof(headers),
      "Content-Type: application/json\n"
      "SDK-VERSION: 1.64.0.27\n"
      "SDK-TYPE: Nimble\n"
      "EAM-SESSION: %s\n",
      synergy_session_id);
  HttpRequest *request = calloc(1, sizeof(*request));
  if (header_length < 0 || header_length >= (int)sizeof(headers) || !request) {
    http_request_free(request);
    return 0;
  }
  request->kind = HTTP_SYNERGY_BOOTSTRAP_ANON_UID;
  if (!http_request_start(request, url, "GET", headers, NULL, 0, 30000)) {
    http_request_free(request);
    debugPrintf("compat: failed to start DEX getAnonUid request\n");
    return 0;
  }
  synergy_anon_pending = 0;
  synergy_anon_in_flight = 1;
  (void)0;
  (void)0;
  (void)0;
  debugLogFlush();
  return 1;
}

static void synergy_anon_complete(const HttpRequest *request,
                                          CURLcode code, long status,
                                          int ok) {
  synergy_anon_in_flight = 0;
  synergy_anon_done = 1;
  const unsigned char *data = request ? request->response.data : NULL;
  const size_t length = request ? request->response.size : 0u;
  if (ok) {
    /* DEX calls getJsonData().get("uid").toString(), so accept either a
     * JSON string or numeric scalar and preserve its textual value exactly. */
    const int have_uid = synergy_json_scalar_string(
        data, length, "uid", synergy_anonymous_id,
        sizeof(synergy_anonymous_id));
    (void)0;
    if (have_uid && synergy_anonymous_id[0]) {
      synergy_promote_environment_success();
      return;
    }
    synergy_update_in_progress = 0;
    debugPrintf("compat: GETANON success response missing uid; startup fails closed\n");
    debugLogFlush();
    return;
  }

  /* DEX retries selected network failures up to three times. The Java error
   * taxonomy is not 1:1 with libcurl, so do not invent an exception mapping. */
  synergy_update_in_progress = 0;
  debugPrintf("compat: GETANON failed curl=%d status=%ld bytes=%zu; no guessed retry mapping\n",
              (int)code, status, length);
  debugLogFlush();
}

static int synergy_generate_synergy_session_id(void) {
  /* classes2.dex SynergyNetworkImpl.generateSessionId(): UUID.randomUUID(),
   * toString(), remove all '-', then lowercase with Locale.US. */
  char uuid[37];
  if (!stock_generate_uuid_v4(uuid, "SynergyNetwork session")) return 0;
  size_t out = 0;
  for (size_t i = 0; uuid[i] && out < sizeof(synergy_session_id) - 1u; ++i) {
    if (uuid[i] == '-') continue;
    synergy_session_id[out++] =
        (char)tolower((unsigned char)uuid[i]);
  }
  synergy_session_id[out] = 0;
  if (out != 32u) {
    synergy_session_id[0] = 0;
    return 0;
  }
  return 1;
}

static int synergy_start_synergy_getdirection(void) {
  if (synergy_direction_in_flight || synergy_direction_ready)
    return 0;
  const SwitchNetworkInfo net = get_switch_network_info();
  if (!net.is_connected) {
    synergy_direction_pending = 1;
    (void)0;
    return 0;
  }

  if (!synergy_session_id[0] &&
      !synergy_generate_synergy_session_id()) {
    debugPrintf("compat: DEX SynergyNetwork session UUID unavailable; getDirection not started\n");
    return 0;
  }

  const SwitchLocaleInfo locale = get_switch_locale();
  char url[4096];
  size_t used = (size_t)snprintf(
      url, sizeof(url),
      "https://syn-dir.sn.eamobile.com/director/api/android/getDirectionByPackage");
  int first = 1;
  if (used >= sizeof(url) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "appVer", GAME_VERSION) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "appLang", locale.language) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "localization", locale.locale) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceLanguage", locale.language) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceLocale", locale.locale) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "packageId", synergy_application_bundle_id()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceString", synergy_device_string()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "deviceCodename", synergy_device_codename()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "manufacturer", synergy_device_manufacturer()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "model", synergy_device_model()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "brand", synergy_device_brand()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "fingerprint", synergy_device_fingerprint()) ||
      !append_url_parameter(url, sizeof(url), &used, &first, "serverEnvironment", "live") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "sdkVersion", "1.64.0.27") ||
      !append_url_parameter(url, sizeof(url), &used, &first, "apiVer", "1.0.0")) {
    debugPrintf("compat: failed to build DEX getDirection URL\n");
    return 0;
  }

  char headers[512];
  const int header_length = snprintf(
      headers, sizeof(headers),
      "Content-Type: application/json\n"
      "SDK-VERSION: 1.64.0.27\n"
      "SDK-TYPE: Nimble\n"
      "EAM-SESSION: %s\n", synergy_session_id);
  HttpRequest *request = calloc(1, sizeof(*request));
  if (header_length < 0 || header_length >= (int)sizeof(headers) || !request) {
    http_request_free(request);
    return 0;
  }
  request->kind = HTTP_SYNERGY_BOOTSTRAP_DIRECTION;
  if (!http_request_start(request, url, "GET", headers, NULL, 0, 30000)) {
    http_request_free(request);
    debugPrintf("compat: failed to start DEX getDirection request\n");
    return 0;
  }
  synergy_direction_pending = 0;
  synergy_direction_in_flight = 1;
  (void)0;
  (void)0;
  (void)0;
  debugLogFlush();
  return 1;
}

static void synergy_java_setup(void) {
  /* EnvironmentDataContainer is a Serializable Java object. The compatibility layer's
   * persistence bridge intentionally supports scalar values only. Check the
   * exact DEX cache scope/key anyway: if anything unexpected is present, fail
   * closed rather than silently treating it as a valid Android object. */
  java_store_ensure_loaded();
  const char *cached = store_get(
      property_store, 64,
      "nimble.persistence.CACHE.com.ea.nimble.synergyEnvironment",
      "environmentData");
  synergy_cache_blocked = cached != NULL;
  (void)0;
  if (synergy_cache_blocked) {
    debugPrintf("compat: unexpected environmentData cache cannot be decoded safely; Synergy bootstrap suppressed\n");
    return;
  }
  (void)0;
  if (!synergy_session_id[0])
    (void)synergy_generate_synergy_session_id();
}

static void synergy_java_restore(void) {
  (void)0;
  if (synergy_cache_blocked) return;
  synergy_update_in_progress = 1;
  /* startSynergyEnvironmentUpdateImpl() emits STARTED(result=1) before the
   * updater starts getDirection. Keep this asynchronous through the same
   * Utility broadcast queue as the rest of the Java compatibility layer. */
  {
    const char *keys[] = {"result"};
    const char *values[] = {"1"};
    jobject extras = jni_make_string_map(keys, values, 1);
    jobject action = jni_make_string(
        "nimble.environment.notification.startup_requests_started");
    if (extras && action)
      (void)nimble_queue_serializable_broadcast(action, extras);
    if (action) jni_free_wrapper(action);
    (void)0;
  }
  synergy_direction_pending = 1;
  (void)synergy_start_synergy_getdirection();
}

static const char *synergy_sell_id(void) {
  return "com.ea.pvz2";
}

static int start_synergy_director_request(void) {
  const char *uuid = ea_identity_effective_player_id();
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
  const char *uuid = ea_identity_effective_player_id();
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


/* PlayerInfo savedelta requests still reach the production backend, but their
 * responses stay isolated from OnlineDataPersistor to protect local save state. */

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
  debugPrintf("JNI HTTP: %s %s (%d-byte body) [async]\n", method, url, request_body_len);
  HttpRequest *request = calloc(1, sizeof(*request));
  if (request) {
    request->kind = HTTP_ANDROID;
    request->self = self;
    request->callback = transaction;
  }
  if (!request || !http_request_start(request, url, method, jni_http_headers(self), request_body,
                                      request_body_len, jni_http_timeout(self))) {
    http_request_free(request);
    jni_http_set_status(self, 0);
    queue_java_callback("HttpTransactionError", self, transaction);
    return;
  }
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
  if (request) {
    request->kind = HTTP_GLU;
    request->self = self;
    request->callback = argv[5].j;
  }
  const int started = request && headers && http_request_start(
      request, url, method, headers, body, body ? (int)strlen(body) : 0, (long)argv[4].j);
  if (started) {
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
  if (request) {
    request->kind = HTTP_DOWNLOAD;
    request->self = self;
    request->callback = callback;
  }
  if (!request || !(request->destination = strdup(destination)) ||
      !http_request_start(request, url, "GET", "", NULL, 0, 30000)) {
    http_request_free(request);
    queue_java_download_callback(self, callback, 0, destination);
    return;
  }
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

  const int player_savedelta = request->kind == HTTP_ANDROID &&
      is_player_savedelta_sync(request->url, request->method, request->body,
                               request->body_length);
  const int ok = code == CURLE_OK && status >= 200 && status < 400 &&
      request->response.size <= INT_MAX;
  /* Android delivers HTTP error responses through the normal response/data/
   * complete callbacks. TransactionError is reserved for transport failure. */
  const int android_http_response = request->kind == HTTP_ANDROID &&
      code == CURLE_OK && status > 0 && request->response.size <= INT_MAX;
  /* Use the official CDN Date header to seed PVZ2's native server clock. */
  if (ok && request->kind == HTTP_ANDROID && request->url &&
      !strncmp(request->url, "https://pvz2-live.ecs.popcap.com/",
               sizeof("https://pvz2-live.ecs.popcap.com/") - 1)) {
    char date_header[96];
    if (http_response_header_copy(&request->response_headers, "Date",
                                  date_header, sizeof(date_header))) {
      const time_t epoch = curl_getdate(date_header, NULL);
      if (epoch > 0)
        note_official_server_time((long long)epoch, date_header,
                                        request->url);
    }
  }


  if (request->kind == HTTP_ANDROID) {
    if (player_savedelta) {
      /* Preserve the exact local fallback visible to stock: do not expose
       * the live status, headers or body to OnlineDataPersistor. */
      jni_http_set_status(request->self, 0);
      queue_java_callback("HttpTransactionError", request->self, request->callback);
    } else {
      jni_http_set_status(request->self, (int)status);
      jni_http_set_response_headers(request->self, request->response_headers.data,
                                    request->response_headers.size);
      if (android_http_response) {
        debugPrintf("JNI HTTP: %s -> %ld (%zu bytes)\n", request->url, status,
                    request->response.size);
        queue_java_callback("HttpReceivedResponse", request->self, request->callback);
        if (request->response.size)
          queue_java_data_callback("HttpReceivedData", request->self, request->callback,
                                   request->response.data, (int)request->response.size);
        queue_java_callback("HttpTransactionComplete", request->self, request->callback);
      } else {
        debugPrintf("JNI HTTP: %s transport failed (curl=%d status=%ld)\n",
                    request->url, code, status);
        queue_java_callback("HttpTransactionError", request->self, request->callback);
      }
    }
  } else if (request->kind == HTTP_SYNERGY_BOOTSTRAP_DIRECTION) {
    synergy_direction_complete(request, code, status, ok);
  } else if (request->kind == HTTP_SYNERGY_BOOTSTRAP_DEVICE_ID) {
    synergy_device_id_complete(request, code, status, ok);
  } else if (request->kind == HTTP_SYNERGY_BOOTSTRAP_ANON_UID) {
    synergy_anon_complete(request, code, status, ok);
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
  nimble_persistence_pump_auto_sync();
  if (synergy_direction_pending &&
      !synergy_direction_in_flight &&
      !synergy_direction_ready)
    (void)synergy_start_synergy_getdirection();
  if (synergy_device_id_pending &&
      !synergy_device_id_in_flight &&
      !synergy_device_id_done)
    (void)synergy_start_synergy_getdeviceid();
  if (synergy_anon_pending &&
      !synergy_anon_in_flight &&
      !synergy_anon_done)
    (void)synergy_start_synergy_getanonuid();
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
static int nimble_java_setup_completed;

typedef void (*fn_basenativecallback_native)(JNIEnv env, jobject receiver,
                                             jint callback_id, jobjectArray args);
static fn_basenativecallback_native basenativecallback_native;
static int basenativecallback_independent_setup_delivered;

/*  BaseNativeCallback.onCallback() does not invoke native code inline.
 * The DEX routes it through Utility.runOnWorkerThread(), so keep a tiny
 * next-pump queue for platform-service callbacks such as Play Integrity. */
#define NIMBLE_DIRECT_CALLBACK_CAPACITY 8
typedef struct {
  jobject receiver;
  jint callback_id;
  jobject error;
  unsigned long long sequence;
} NimblePendingDirectCallback;
static NimblePendingDirectCallback
    nimble_direct_callback_queue[NIMBLE_DIRECT_CALLBACK_CAPACITY];
static size_t nimble_direct_callback_head;
static size_t nimble_direct_callback_count;
static unsigned long long nimble_direct_callback_sequence;

#define NIMBLE_RECEIVER_CAPACITY 64
#define NIMBLE_RECEIVER_ACTION_CAPACITY 160
typedef struct {
  char action[NIMBLE_RECEIVER_ACTION_CAPACITY];
  jobject receiver;
  jint callback_id;
} NimbleReceiverRegistration;
static NimbleReceiverRegistration nimble_receivers[NIMBLE_RECEIVER_CAPACITY];
static size_t nimble_receiver_count;

void pvz2_set_basenativecallback_native(void *fn) {
  basenativecallback_native = (fn_basenativecallback_native)fn;
}

static void nimble_record_receiver(const char *action, jobject receiver, jint callback_id) {
  if (!action || !*action || !receiver) return;
  for (size_t i = 0; i < nimble_receiver_count; ++i) {
    if (nimble_receivers[i].receiver == receiver &&
        !strcmp(nimble_receivers[i].action, action))
      return;
  }
  if (nimble_receiver_count >= NIMBLE_RECEIVER_CAPACITY) {
    (void)0;
    debugLogFlush();
    return;
  }
  NimbleReceiverRegistration *slot = &nimble_receivers[nimble_receiver_count++];
  snprintf(slot->action, sizeof(slot->action), "%s", action);
  slot->receiver = receiver;
  slot->callback_id = callback_id;
}

static size_t nimble_unregister_receiver(jobject receiver) {
  if (!receiver) return 0;
  size_t removed = 0;
  for (size_t i = 0; i < nimble_receiver_count; ) {
    if (nimble_receivers[i].receiver != receiver) {
      ++i;
      continue;
    }
    ++removed;
    for (size_t j = i + 1; j < nimble_receiver_count; ++j)
      nimble_receivers[j - 1] = nimble_receivers[j];
    --nimble_receiver_count;
  }
  return removed;
}

static int nimble_queue_integrity_unavailable(jobject receiver) {
  if (!receiver || nimble_direct_callback_count >= NIMBLE_DIRECT_CALLBACK_CAPACITY) {
    debugPrintf("compat: integrity callback queue rejected receiver=%p pending=%zu\n",
                receiver, nimble_direct_callback_count);
    debugLogFlush();
    return 0;
  }
  const size_t index = (nimble_direct_callback_head +
                        nimble_direct_callback_count) %
                       NIMBLE_DIRECT_CALLBACK_CAPACITY;
  NimblePendingDirectCallback *slot = &nimble_direct_callback_queue[index];
  memset(slot, 0, sizeof(*slot));
  slot->receiver = receiver;
  slot->callback_id = (jint)jni_object_long(receiver);
  slot->error = jni_make_nimble_error(
      103,
      "requestIntegrityToken() resulted in exception\n"
      "Cause:Google Play Integrity unavailable on Horizon");
  if (!slot->error) {
    debugPrintf("compat: integrity callback error allocation failed id=%d\n",
                slot->callback_id);
    debugLogFlush();
    return 0;
  }
  slot->sequence = ++nimble_direct_callback_sequence;
  ++nimble_direct_callback_count;
  debugPrintf("compat: queued DEX integrity fallback seq=%llu callback_id=%d token=NULL error=NimbleError(103/NOT_AVAILABLE)\n",
              slot->sequence, slot->callback_id);
  debugLogFlush();
  return 1;
}

static void nimble_pump_direct_callback(void) {
  if (!nimble_direct_callback_count) return;
  NimblePendingDirectCallback pending =
      nimble_direct_callback_queue[nimble_direct_callback_head];
  nimble_direct_callback_head = (nimble_direct_callback_head + 1) %
                                NIMBLE_DIRECT_CALLBACK_CAPACITY;
  --nimble_direct_callback_count;
  if (!basenativecallback_native) {
    debugPrintf("compat: integrity callback seq=%llu nativeCallback unavailable; fail closed\n",
                pending.sequence);
    if (pending.error) jni_free_wrapper(pending.error);
    debugLogFlush();
    return;
  }
  jobject values[2] = { NULL, pending.error };
  jobjectArray args = jni_make_object_array(values, 2);
  if (!args) {
    debugPrintf("compat: integrity callback seq=%llu Object[] allocation failed\n",
                pending.sequence);
    if (pending.error) jni_free_wrapper(pending.error);
    debugLogFlush();
    return;
  }
  debugPrintf("compat: delivering DEX integrity fallback seq=%llu callback_id=%d token=NULL domain=NimbleError code=103\n",
              pending.sequence, pending.callback_id);
  debugLogFlush();
  basenativecallback_native(fake_env, pending.receiver, pending.callback_id, args);
  (void)0;
  debugLogFlush();
  jni_free_wrapper(args);
  jni_free_wrapper(pending.error);
}

#define NIMBLE_BROADCAST_QUEUE_CAPACITY 32
typedef struct {
  char action[NIMBLE_RECEIVER_ACTION_CAPACITY];
  jobject extras;
  unsigned long long sequence;
} NimblePendingBroadcast;
static NimblePendingBroadcast nimble_broadcast_queue[NIMBLE_BROADCAST_QUEUE_CAPACITY];
static size_t nimble_broadcast_queue_head;
static size_t nimble_broadcast_queue_count;
static unsigned long long nimble_broadcast_sequence;
/* The platform transition can arrive before Base.setupNimble. Hold only the
 * latest real Switch status until the stock network receiver is safe to run.
 * callback 4 re-queries INetwork.getStatus(), so this value is diagnostic and
 * never injected into the Java payload. */
static int nimble_network_status_pending;
static int nimble_network_status_value = INT_MIN;
static jobject nimble_network_status_empty_extras;

static unsigned nimble_deliver_serializable_broadcast(const char *action, jobject extras) {
  if (!action || !*action) return 0;
  if (!strcmp(action, "nimble.environment.notification.startup_requests_finished")) {
    synergy_id_manager_startup_seen = 1;
    /* The real Java SynergyIdManager receiver is not executing on Horizon.
     * Reproduce its DEX-defined receive side effect at broadcast-delivery time,
     * before native listeners observe the same asynchronous broadcast. */
    synergy_id_manager_on_startup_finished();
  }
  if (!basenativecallback_native) {
    debugPrintf("compat: queued broadcast action=%s nativeCallback unavailable; fail closed\n",
                action);
    debugLogFlush();
    return 0;
  }

  jobject action_object = jni_make_string(action);
  if (!action_object) {
    debugPrintf("compat: queued broadcast action=%s string allocation failed\n", action);
    debugLogFlush();
    return 0;
  }
  jobject values[2] = { action_object, extras };
  jobjectArray args = jni_make_object_array(values, 2);
  if (!args) {
    debugPrintf("compat: queued broadcast action=%s Object[] allocation failed\n", action);
    jni_free_wrapper(action_object);
    debugLogFlush();
    return 0;
  }

  /* Snapshot matching registrations before entering native code.  A receiver
   * is allowed to call Utility.unregisterReceiver() from inside its callback
   * (Nexus does this after Synergy startup), so iterating the mutable registry
   * directly would invalidate the current slot. */
  NimbleReceiverRegistration matches[NIMBLE_RECEIVER_CAPACITY];
  size_t match_count = 0;
  for (size_t i = 0; i < nimble_receiver_count &&
                     match_count < NIMBLE_RECEIVER_CAPACITY; ++i) {
    if (!strcmp(nimble_receivers[i].action, action))
      matches[match_count++] = nimble_receivers[i];
  }

  unsigned delivered = 0;
  for (size_t i = 0; i < match_count; ++i) {
    const NimbleReceiverRegistration *slot = &matches[i];
    basenativecallback_native(fake_env, slot->receiver, slot->callback_id, args);
    ++delivered;
  }
  jni_free_wrapper(args);
  jni_free_wrapper(action_object);
  return delivered;
}

static unsigned nimble_queue_serializable_broadcast(jobject action_object, jobject extras) {
  const char *action = jni_cstr(action_object);
  if (!action || !*action) {
    debugPrintf("compat: Utility.sendBroadcastSerializable missing action; fail closed\n");
    debugLogFlush();
    return 0;
  }
  if (nimble_broadcast_queue_count >= NIMBLE_BROADCAST_QUEUE_CAPACITY) {
    debugPrintf("compat: Utility.sendBroadcastSerializable queue full action=%s pending=%zu; fail closed\n",
                action, nimble_broadcast_queue_count);
    debugLogFlush();
    return 0;
  }

  const size_t index = (nimble_broadcast_queue_head + nimble_broadcast_queue_count) %
                       NIMBLE_BROADCAST_QUEUE_CAPACITY;
  NimblePendingBroadcast *slot = &nimble_broadcast_queue[index];
  snprintf(slot->action, sizeof(slot->action), "%s", action);
  slot->extras = extras;
  slot->sequence = ++nimble_broadcast_sequence;
  ++nimble_broadcast_queue_count;
  /* Fake-JNI NewGlobalRef/DeleteLocalRef are identity/no-op by design, so the
   * Java Map wrapper and the boxed values it owns remain valid until this
   * later-frame delivery. This models Android sendBroadcast() returning before
   * BroadcastReceiver execution instead of re-entering native code here. */
  return 1;
}


static void nimble_queue_empty_broadcast_action(const char *action) {
  static jobject empty_extras;
  if (!action || !*action) return;
  if (!empty_extras) empty_extras = jni_make_string_map(NULL, NULL, 0);
  jobject action_object = jni_make_string(action);
  if (!action_object || !empty_extras) {
    debugPrintf("compat: Utility.sendBroadcast allocation failed action=%s\n",
                action);
    if (action_object) jni_free_wrapper(action_object);
    return;
  }
  (void)nimble_queue_serializable_broadcast(action_object, empty_extras);
  jni_free_wrapper(action_object);
}

static void nimble_queue_pending_network_status_change(void) {
  if (!nimble_network_status_pending || !nimble_java_setup_started) return;

  static const char action_text[] = "nimble.notification.networkStatusChange";
  if (!nimble_network_status_empty_extras)
    nimble_network_status_empty_extras = jni_make_string_map(NULL, NULL, 0);
  jobject action = jni_make_string(action_text);
  if (!action || !nimble_network_status_empty_extras) {
    debugPrintf("compat: networkStatusChange queue allocation failed status=%d action=%p extras=%p\n",
                nimble_network_status_value, action, nimble_network_status_empty_extras);
    if (action) jni_free_wrapper(action);
    debugLogFlush();
    return;
  }

  if (nimble_queue_serializable_broadcast(action, nimble_network_status_empty_extras)) {
    nimble_network_status_pending = 0;
  }
  jni_free_wrapper(action);
}

int pvz2_nimble_setup_ready(void) {
  return nimble_java_setup_completed;
}

void pvz2_nimble_set_main_application_active(int active) {
  synergy_main_application_active = active ? 1 : 0;
  if (active && synergy_environment_live == 2) {
    synergy_environment_live = 1;
    synergy_queue_startup_finished_success();
  }
}

void pvz2_nimble_network_status_changed(int status) {
  nimble_network_status_value = status;
  nimble_network_status_pending = 1;
  (void)0;
  debugLogFlush();
  nimble_queue_pending_network_status_change();
}

void pvz2_nimble_broadcast_pump(void) {
  /* BaseNativeCallback.runOnWorkerThread work is independent of Android
   * broadcasts; service one queued direct callback before the broadcast pump. */
  nimble_pump_direct_callback();
  if (!nimble_broadcast_queue_count) return;

  NimblePendingBroadcast pending = nimble_broadcast_queue[nimble_broadcast_queue_head];
  nimble_broadcast_queue_head = (nimble_broadcast_queue_head + 1) %
                                NIMBLE_BROADCAST_QUEUE_CAPACITY;
  --nimble_broadcast_queue_count;

  const unsigned delivered =
      nimble_deliver_serializable_broadcast(pending.action, pending.extras);
  (void)delivered;
}

static void compat_deliver_independent_setup_finished(void) {
  if (basenativecallback_independent_setup_delivered) return;
  if (!basenativecallback_native) {
    debugPrintf("compat: componentIndependentSetupFinished not delivered; stock nativeCallback entry unavailable\n");
    debugLogFlush();
    return;
  }

  /* The compatibility layer decode the exact BaseNativeCallback transport contract:
   *   args[0] = String action
   *   args[1] = Map<String,Object> extras
   * callback 1's target at +0x23FE680 ignores the converted extras, so an
   * empty map is the exact minimal Android-equivalent payload for this event.
   * No token, player id, Synergy id, status, or Joust state is synthesized. */
  static const char action_text[] =
      "nimble.notification.componentIndependentSetupFinished";
  jobject action = jni_make_string(action_text);
  jobject extras = jni_make_string_map(NULL, NULL, 0);
  jobject values[2] = { action, extras };
  jobjectArray args = (action && extras) ? jni_make_object_array(values, 2) : NULL;
  if (!args) {
    debugPrintf("compat: componentIndependentSetupFinished not delivered; Object[]{String,Map(0)} allocation failed\n");
    if (extras) jni_free_wrapper(extras);
    if (action) jni_free_wrapper(action);
    debugLogFlush();
    return;
  }

  basenativecallback_independent_setup_delivered = 1;
  basenativecallback_native(fake_env, NULL, 1, args);

  jni_free_wrapper(args);
  jni_free_wrapper(extras);
  jni_free_wrapper(action);
}

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
  const int identity_probe =
      !strcmp(component_id, "com.ea.nimble.cpp.authenticator.anonymous") ||
      !strcmp(component_id, "com.ea.nimble.cpp.nexusservice") ||
      !strcmp(component_id, "com.ea.nimble.cpp.nexus.eaaccount") ||
      !strcmp(component_id, "com.ea.nimble.cpp.nexus.jwk");
  (void)identity_probe;
  nimble_setup_cpp_component(component);
  jni_free_wrapper(component);
}

/*  classes2.dex ComponentManager.restore() restores every registered
 * component immediately after componentIndependentSetupFinished. Historical
 * The compatibility layer hardware already proved the anonymous authenticator lifecycle
 * slots are RET stubs, while the registered EA-account component has a real
 * restore thunk at libPVZ2+0x26C72C4 branching to the substantive stock body
 * at +0x26C68C8. The compatibility layer recovered that body's Java Persistence contract
 * (accessToken, userId, loggedIn) and implemented it without inventing any
 * credential or player identity, but Horizon never dispatched the restore.
 * Replay only this missing, hardware-derived identity-side restore at the exact
 * DEX ComponentManager.restore boundary, before the already-proven The compatibility layer
 * NetworkClientManager restore. No token, account, Synergy ID, gamePlayerId,
 * profile, progression, or Joust state is synthesized here. */
static int compat_eaaccount_restored;

static void compat_restore_eaaccount(void) {
  if (compat_eaaccount_restored) return;
  static const char component_id[] = "com.ea.nimble.cpp.nexus.eaaccount";

  jobject component = jni_make_object_class_string(
      "com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent",
      component_id);
  if (!component) {
    debugPrintf("compat: EA-account restore suppressed: wrapper allocation failed\n");
    debugLogFlush();
    return;
  }

  if (!nimble_probe_native_component(component_id, "nexus.pre-restore")) {
    debugPrintf("compat: EA-account restore suppressed: native component missing\n");
    debugLogFlush();
    jni_free_wrapper(component);
    return;
  }

  (void)0;
  debugLogFlush();
  nimble_restore_cpp_component(component);
  compat_eaaccount_restored = 1;
  (void)0;
  nimble_probe_native_component(component_id, "nexus.post-restore");
  pvz2_nimble_persistence_log_summary();
  debugLogFlush();
  jni_free_wrapper(component);
}

/*  The compatibility layer hardware maps the two native network component lifecycle
 * vtables. NetworkService restore/suspend/resume all enter immediate RET stubs,
 * while NetworkClientManager restore is the one substantive missing lifecycle
 * edge at libPVZ2+0x26FD7C8. classes2.dex ComponentManager.restore() invokes
 * Component.restore() for every registered component immediately after the
 * componentIndependentSetupFinished broadcast. Horizon historically skipped
 * that native restore pass and replayed only NexusService later. Restore only
 * the proven-substantive NetworkClientManager here, once, at the exact DEX
 * boundary. No request, response, token, Synergy value, or player identity is
 * synthesized. */
static int compat_network_client_manager_restored;

static void compat_restore_network_client_manager(void) {
  if (compat_network_client_manager_restored) return;
  static const char component_id[] = "com.ea.nimble.cpp.networkclientmanager";

  jobject component = jni_make_object_class_string(
      "com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent",
      component_id);
  if (!component) {
    debugPrintf("compat: NetworkClientManager restore suppressed: wrapper allocation failed\n");
    debugLogFlush();
    return;
  }

  if (!nimble_probe_native_component(component_id, "network.pre-restore")) {
    debugPrintf("compat: NetworkClientManager restore suppressed: native component missing\n");
    debugLogFlush();
    jni_free_wrapper(component);
    return;
  }

  (void)0;
  debugLogFlush();
  nimble_restore_cpp_component(component);
  compat_network_client_manager_restored = 1;
  (void)0;
  nimble_probe_native_component(component_id, "network.post-restore");
  debugLogFlush();
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
  if (nimble_java_setup_started) nimble_setup_java_component(slot);
}

static void nimble_audit_registered_native_components(void) {
  /* DEX NimbleCppComponentRegistrar queues these components into Base once;
   * ComponentManager.setup() then invokes each registered component once. The
   * old Switch second pass duplicated setup. Keep only a read-only audit. */
  static const char *const component_ids[] = {
      "com.ea.nimble.cpp.networkservice",
      "com.ea.nimble.cpp.networkclientmanager",
      "com.ea.nimble.cpp.trackingservice",
      "com.ea.nimble.cpp.agecomplianceservice",
      "com.ea.nimble.cpp.nexus.jwk",
      "com.ea.nimble.cpp.nexusservice",
      "com.ea.nimble.cpp.authenticator.anonymous",
      "com.ea.nimble.cpp.nexus.eaaccount",
  };
  (void)component_ids;
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

  /* Historical contract discovery only. Exact DEX/native contracts are now
   * known, so normal builds avoid formatting generic object/pointer traces. */


  /* Completed The compatibility layer identity method-enumeration trace. */

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
  if (has(name, "DeviceID") || has(name, "AndroidID") || has(name, "InstallID"))
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
    if (!strcmp(name, "generateUUID")) {
      char uuid[37];
      stock_generate_uuid_v4(uuid, "AndroidPlatform.generateUUID");
      return (jvalue){.l = jni_make_string(uuid)};
    }
    if (!strcmp(name, "removePrivateData"))
      return (jvalue){.z = remove_private_data(argv[0].l)};
  }
  if (has(cls, "com/popcap/SexyAppFramework/SexyAppFrameworkActivity") ||
      has(cls, "com.popcap.SexyAppFramework.SexyAppFrameworkActivity")) {
    if (!strcmp(name, "Util_GetUUIDString") &&
        (!sig || !strcmp(sig, "()Ljava/lang/String;"))) {
      char uuid[37];
      const int generated = stock_generate_uuid_v4(
          uuid, "SexyAppFrameworkActivity.Util_GetUUIDString");
      if (generated && ea_identity_legacy_alias_seen) {
        (void)0;
      }
      /* This is a stock UUID-generation result, not an EA-authoritative ID.
       * Do not persist or directly promote it to Joust; PlayerIdentityService
       * must route it through its normal Local/Nimble/Cloud/Selected path. */
      return (jvalue){.l = jni_make_string(uuid)};
    }
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
      const char *key = jni_cstr(argv[0].l);
      const char *value = jni_cstr(argv[1].l);
      store_set(property_store, 64, "config", key, value);
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

  if (has(cls, "com/ea/nimble/NimbleApplicationConfiguration") ||
      has(cls, "com.ea.nimble.NimbleApplicationConfiguration")) {
    const char *key = jni_cstr(argv[0].l);
    const int jwt_key = key && !strcmp(key, "NimbleIdentityUseJWT");

    /* diagnostic: stock Nexus registration itself asks whether this exact key
     * exists. the native path showed no deferred constructor can create nexus.jwk and
     * anonymous/nexusservice already exist, so enable only this observed stock
     * identity gate. Do not invent an ID/token: the value merely selects the
     * game's own JWT/Nexus path, which remains responsible for deviceHash,
     * Synergy identity, and any server-issued player identity. */
    if (!strcmp(name, "configValueExists")) {
      (void)0;
      debugLogFlush();
      return (jvalue){.z = jwt_key ? JNI_TRUE : JNI_FALSE};
    }

    /* The false The compatibility layer result prevented the bridge from reaching its value
     * accessor, so its exact Java return signature was not observable. Handle
     * the exact JWT key conservatively across the normal Nimble representations
     * (primitive boolean, boxed/object Boolean, or string) and log the real
     * method/signature stock requests. All other config keys retain the prior
     * typed-default behavior. */
    if (jwt_key) {
      const char *ret = sig ? strrchr(sig, ')') : NULL;
      const char *rtype = ret ? ret + 1 : "";
      (void)0;
      debugLogFlush();
      if (!strcmp(rtype, "Z"))
        return (jvalue){.z = JNI_TRUE};
      if (!strcmp(rtype, "I"))
        return (jvalue){.i = 1};
      if (!strcmp(rtype, "Ljava/lang/String;"))
        return (jvalue){.l = jni_make_string("true")};
      if (!strcmp(rtype, "Ljava/lang/Boolean;") ||
          !strcmp(rtype, "Ljava/lang/Object;") ||
          !strcmp(rtype, "Ljava/io/Serializable;"))
        return (jvalue){.l = jni_make_object_class_value("java/lang/Boolean", 1)};
    }
  }

  if ((has(cls, "com/ea/nimble/PersistenceService") ||
       has(cls, "com.ea.nimble.PersistenceService")) &&
      !strcmp(name, "getPersistenceForNimbleComponent") && sig &&
      !strcmp(sig, "(Ljava/lang/String;Lcom/ea/nimble/Persistence$Storage;)Lcom/ea/nimble/Persistence;")) {
    return (jvalue){.l =
        nimble_persistence_for_component(argv[0].l, argv[1].l)};
  }

  if ((has(cls, "com/ea/nimble/Persistence") ||
       has(cls, "com.ea.nimble.Persistence")) &&
      !has(cls, "PersistenceService") && !has(cls, "Persistence$Storage")) {
    const char *scope = jni_object_string(self);
    if (!scope || strncmp(scope, "nimble.persistence.", 19)) {
      nimble_persistence_faults++;
      debugPrintf("compat: Persistence.%s rejected invalid object=%p scope=%s faults=%u\n",
                  name ? name : "(null)", self,
                  scope ? scope : "(null)", nimble_persistence_faults);
      debugLogFlush();
      return (jvalue){0};
    }

    /*  classes2.dex Persistence.setEncryption(Z)V compares the
     * requested primitive boolean with m_encryption, stores it only when it
     * changes, then calls flagChange(). The compatibility layer exposed this exact call from
     * stock NexusEAAuth::savePersistance(). The old scalar dispatcher decoded
     * argv[0] as an object key before method dispatch, so true (1) became
     * jobject 0x1 and faulted. Handle the primitive contract before any object
     * argument decode. The fake Persistence wrapper's value field is otherwise
     * unused and therefore safely mirrors Java's per-wrapper m_encryption. */
    if (!strcmp(name, "setEncryption") && sig && !strcmp(sig, "(Z)V")) {
      nimble_persistence_method_calls++;
      const int enabled = argv[0].z ? 1 : 0;
      const int previous = jni_object_long(self) ? 1 : 0;
      if (enabled != previous) {
        jni_object_set_long(self, enabled);
        nimble_persistence_flag_change();
      }
      (void)0;
      debugLogFlush();
      return (jvalue){0};
    }

    /*  hardware reached Persistence.setBackUp(Z)V from the stock
     * anonymous-authenticator persistence path with true (w1=1). The generic
     * object-key decode below treated that primitive as jobject 0x1 and
     * jni_cstr() faulted at address 0x1. Android backup transport has no
     * equivalent in this Switch-local persistence store, so consume the exact
     * primitive contract before any jobject decode and leave value storage /
     * synchronize behavior unchanged. */
    if (!strcmp(name, "setBackUp") && sig && !strcmp(sig, "(Z)V")) {
      nimble_persistence_method_calls++;
      return (jvalue){0};
    }

    const char *key = jni_cstr(argv[0].l);

    if (!strcmp(name, "hasKey") && sig &&
        !strcmp(sig, "(Ljava/lang/String;)Z")) {
      nimble_persistence_method_calls++;
      const char *value = (scope && key)
          ? store_get(property_store, 64, scope, key) : NULL;
      (void)0;
      debugLogFlush();
      return (jvalue){.z = value ? JNI_TRUE : JNI_FALSE};
    }

    if (!strcmp(name, "getStringValue") && sig &&
        !strcmp(sig, "(Ljava/lang/String;)Ljava/lang/String;")) {
      nimble_persistence_method_calls++;
      const char *value = (scope && key)
          ? store_get(property_store, 64, scope, key) : NULL;
      (void)0;
      debugLogFlush();
      /* Preserve Java's missing-value distinction. Returning an allocated
       * empty string here would fabricate a present-but-empty persisted value;
       * the previous unimplemented bridge returned null, and hasKey() remains
       * the authoritative presence test. */
      return (jvalue){.l = value ? jni_make_string(value) : NULL};
    }

    if (!strcmp(name, "setValue") && sig &&
        !strcmp(sig, "(Ljava/lang/String;Ljava/io/Serializable;)V")) {
      nimble_persistence_method_calls++;
      const size_t key_length = key ? strlen(key) : 0;
      if (!key || !*key || key_length > 4096u) {
        nimble_persistence_faults++;
        debugPrintf("compat: Persistence.setValue rejected invalid key scope=%s key=%s key_len=%zu faults=%u\n",
                    scope, key ? key : "(null)", key_length,
                    nimble_persistence_faults);
        debugLogFlush();
        return (jvalue){0};
      }

      java_store_ensure_loaded();
      if (!argv[1].l) {
        /* Original Persistence.setValue(key, null) removes an existing value
         * and flagChange() only when something was actually removed. */
        const int stored = store_remove_raw(property_store, 64, scope, key);
        nimble_persistence_accepted_writes++;
        if (stored > 0) nimble_persistence_flag_change();
        debugPrintf("compat: Persistence.setValue scope=%s key=%s type=null/remove result=%s faults=%u\n",
                    scope, key, stored > 0 ? "changed" : "unchanged",
                    nimble_persistence_faults);
        debugLogFlush();
        return (jvalue){0};
      }

      char scalar_value[64];
      const char *type_name = NULL;
      const char *value = nimble_persistence_serialize(
          argv[1].l, scalar_value, sizeof(scalar_value), &type_name);
      const size_t value_length = value ? strlen(value) : 0;
      if (!value || value_length > 65536u) {
        const char *value_class = jni_object_class_name(argv[1].l);
        nimble_persistence_faults++;
        debugPrintf("compat: Persistence.setValue rejected scope=%s key=%s key_len=%zu class=%s type=%s value_len=%zu faults=%u\n",
                    scope, key, key_length,
                    value_class ? value_class : "(unknown)",
                    type_name ? type_name : "unsupported", value_length,
                    nimble_persistence_faults);
        debugLogFlush();
        return (jvalue){0};
      }
      const int stored = store_set_raw(property_store, 64, scope, key, value);
      if (stored < 0) {
        nimble_persistence_faults++;
      } else {
        nimble_persistence_accepted_writes++;
        if (stored > 0) nimble_persistence_flag_change();
      }
      debugPrintf("compat: Persistence.setValue scope=%s key=%s type=%s len=%zu result=%s faults=%u\n",
                  scope, key, type_name ? type_name : "?", value_length,
                  stored > 0 ? "changed" : stored == 0 ? "unchanged" : "failed",
                  nimble_persistence_faults);
      debugLogFlush();
      return (jvalue){0};
    }

    if (!strcmp(name, "synchronize") && sig && !strcmp(sig, "()V")) {
      nimble_persistence_method_calls++;
      nimble_persistence_sync_calls++;
      java_store_ensure_loaded();
      const int committed = java_store_save();
      if (!committed) {
        nimble_persistence_faults++;
      } else {
        nimble_persistence_dirty = 0;
        nimble_persistence_save_due_tick = 0;
      }
      debugPrintf("compat: Persistence.synchronize scope=%s committed=%d faults=%u\n",
                  scope ? scope : "(null)", committed,
                  nimble_persistence_faults);
      debugLogFlush();
      return (jvalue){0};
    }

    nimble_persistence_faults++;
    debugPrintf("compat: unsupported Persistence contract name=%s sig=%s scope=%s faults=%u\n",
                name ? name : "(null)", sig ? sig : "(null)", scope,
                nimble_persistence_faults);
    debugLogFlush();
    return (jvalue){0};
  }

  if ((has(cls, "com/ea/nimble/ApplicationEnvironment") ||
       has(cls, "com.ea.nimble.ApplicationEnvironment")) &&
      !strcmp(name, "isMainApplicationActive")) {
    (void)0;
    return (jvalue){.z = synergy_main_application_active ? JNI_TRUE : JNI_FALSE};
  }
  if ((has(cls, "com/ea/nimble/ApplicationEnvironment") ||
       has(cls, "com.ea.nimble.ApplicationEnvironment")) &&
      !strcmp(name, "getComponent"))
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/IApplicationEnvironment")};
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment"))) {
    const SwitchLocaleInfo synergy_locale = get_switch_locale();
    const char *synergy_value = NULL;
    if (!strcmp(name, "getApplicationBundleId"))
      synergy_value = synergy_application_bundle_id();
    else if (!strcmp(name, "getApplicationVersion"))
      synergy_value = GAME_VERSION;
    else if (!strcmp(name, "getApplicationLanguageCode"))
      synergy_value = synergy_locale.locale;
    else if (!strcmp(name, "getShortApplicationLanguageCode"))
      synergy_value = synergy_locale.language;
    else if (!strcmp(name, "getAndroidId"))
      /* Android constructor reads Settings.Secure["android_id"]. Horizon has
       * no Secure provider; use the existing persisted device/install UUID,
       * which is explicitly barred from the player-ID map. */
      synergy_value = device_uuid();
    else if (!strcmp(name, "getGoogleAdvertisingId"))
      /*  classes2.dex getGoogleAdvertisingId() is only a getter for
       * m_advertisingId. ApplicationEnvironmentImpl.setup() calls
       * retrieveAdvertisingIdImpl(); when Google Play services are absent it
       * stores the empty Java String, not NULL. Horizon has no Google Play
       * services, so expose that exact unavailable state. Never fabricate an
       * advertising identifier. */
      synergy_value = "";
    else if (!strcmp(name, "getDeviceString"))
      synergy_value = synergy_device_string();
    else if (!strcmp(name, "getDeviceCodename"))
      synergy_value = synergy_device_codename();
    else if (!strcmp(name, "getDeviceManufacturer"))
      synergy_value = synergy_device_manufacturer();
    else if (!strcmp(name, "getDeviceModel"))
      synergy_value = synergy_device_model();
    else if (!strcmp(name, "getDeviceBrand"))
      synergy_value = synergy_device_brand();
    else if (!strcmp(name, "getDeviceFingerprint"))
      synergy_value = synergy_device_fingerprint();
    if (synergy_value) {
      return (jvalue){.l = jni_make_string(synergy_value)};
    }

    /* Exact classes2.dex platform contracts. Horizon has no Android root
     * markers or telephony service, and the stock isAppCracked() body itself
     * is an explicit false placeholder. These typed results replace generic
     * JNI defaults without changing any identity, token, save, or Joust data. */
    if (!strcmp(name, "isDeviceRooted") && sig && !strcmp(sig, "()Z"))
      return (jvalue){.z = JNI_FALSE};
    if (!strcmp(name, "isAppCracked") && sig && !strcmp(sig, "()Z"))
      return (jvalue){.z = JNI_FALSE};
    if (!strcmp(name, "getCarrier") && sig &&
        !strcmp(sig, "()Ljava/lang/String;"))
      return (jvalue){.l = NULL};

  }
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment")) &&
      !strcmp(name, "requestIntegrityToken")) {
    /* classes2.dex ApplicationEnvironmentImpl.requestIntegrityToken(): Android
     * uses Google Play Integrity. If the manager/request setup throws, it
     * invokes IntegrityTokenCallback.onCallback(NULL, Error.NOT_AVAILABLE).
     * Horizon has no Google Play Integrity service, so reproduce that exact
     * unavailable branch and never fabricate an integrity token. */
    jobject callback = argv[1].l;
    nimble_queue_integrity_unavailable(callback);
    return (jvalue){0};
  }
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment")) &&
      !strcmp(name, "getParameter")) {
    /* diagnostic: the stock anonymous Nexus authenticator derives its own
     * persisted deviceHash by concatenating these platform descriptors, then
     * hashing/base64-encoding them inside libPVZ2.  Supply only real Switch
     * hardware/platform facts here; do not synthesize a player ID, account ID,
     * token, deviceIdentifier, or Synergy ID in the shim. */
    const char *key = jni_cstr(argv[0].l);
    const SwitchLocaleInfo network_locale = get_switch_locale();
    const char *value = "";
    if (key) {
      if (!strcmp(key, "platform")) value = "android";
      else if (!strcmp(key, "countryCode")) value = network_locale.country;
      else if (!strcmp(key, "deviceManufacturer")) value = "Nintendo";
      else if (!strcmp(key, "deviceProduct")) value = "Nintendo Switch";
      else if (!strcmp(key, "deviceModel")) value = "Nintendo Switch";
      else if (!strcmp(key, "deviceCodename")) value = "NX";
      else if (!strcmp(key, "cpuChipset")) value = "NVIDIA Tegra X1";
      else if (!strcmp(key, "cpuCoreCount")) value = "4";
      else if (!strcmp(key, "attributionData")) value = "";
    }
    (void)0;
    return (jvalue){.l = jni_make_string(value)};
  }
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment")) &&
      !strcmp(name, "setPlayerId")) {
    /* Stock signature is (String key, String playerId).  Earlier Switch
     * builds incorrectly kept argv[0] as the ID and discarded argv[1]. */
    const char *key = jni_cstr(argv[0].l);
    const char *value = jni_cstr(argv[1].l);
    (void)0;
    if (key && !strcmp(key, "alias_id") && ea_identity_is_synthetic(value)) {
      /* This is the exact legacy SwitchPvZ2Local1-derived alias created by
       * older port builds.  Do not mirror it into the Java/Nimble ID map; from
       * the Java side it remains absent so stock reconciliation can bootstrap
       * a replacement through its UUID path.  Native profile/save data is not
       * modified here. */
      ea_identity_legacy_alias_seen = 1;
      (void)0;
      if (!identity_legacy_persistence_scanned) {
        identity_legacy_persistence_scanned = 1;
      }
    }
    if (key && !strcmp(key, "persona") && value && *value &&
        ea_identity_value_ok(value) && !identity_persona_persistence_scanned) {
      identity_persona_persistence_scanned = 1;
    }
    ea_identity_store_map_value(key, value, "stock-setPlayerId");
    return (jvalue){0};
  }
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment")) &&
      !strcmp(name, "getPlayerIdMap"))
    return (jvalue){.l = ea_identity_make_player_id_map()};
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment")) &&
      !strcmp(name, "getGameSpecifiedPlayerId")) {
    const char *value = ea_identity_get_map_value("gamePlayerId");
    (void)0;
    return (jvalue){.l = value ? jni_make_string(value) : NULL};
  }
  if ((has(cls, "com/ea/nimble/IApplicationEnvironment") ||
       has(cls, "com.ea.nimble.IApplicationEnvironment")) &&
      !strcmp(name, "setGameSpecifiedPlayerId")) {
    const char *value = jni_cstr(argv[0].l);
    set_game_specified_player_id(value,
                                          "stock-setGameSpecifiedPlayerId");
    return (jvalue){0};
  }
  if ((has(cls, "com/ea/nimble/SynergyEnvironment") ||
       has(cls, "com.ea.nimble.SynergyEnvironment")) &&
      !strcmp(name, "getComponent"))
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/ISynergyEnvironment")};
  if ((has(cls, "com/ea/nimble/ISynergyEnvironment") ||
       has(cls, "com.ea.nimble.ISynergyEnvironment"))) {
    const int live = synergy_environment_live != 0;
    if (!strcmp(name, "isDataAvailable")) {
      static int last_logged = -1;
      if (last_logged != live) {
        (void)0;
        last_logged = live;
      }
      return (jvalue){.z = live ? JNI_TRUE : JNI_FALSE};
    }
    if (!strcmp(name, "isUpdateInProgress"))
      return (jvalue){.z = synergy_update_in_progress ? JNI_TRUE : JNI_FALSE};
    if (!strcmp(name, "getSynergyId")) {
      const char *value = live && synergy_anonymous_id[0]
          ? synergy_anonymous_id : NULL;
      static int last_logged_state = -1;
      const int state = value ? 1 : 0;
      if (last_logged_state != state) {
        (void)0;
        last_logged_state = state;
      }
      return (jvalue){.l = value ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getEADeviceId")) {
      const char *value = live && synergy_ea_device_id[0]
          ? synergy_ea_device_id : NULL;
      return (jvalue){.l = value ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getEAHardwareId")) {
      const char *value = live && synergy_hw_id[0]
          ? synergy_hw_id : NULL;
      return (jvalue){.l = value ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getSellId")) {
      const char *value = live && synergy_bootstrap_sell_id[0]
          ? synergy_bootstrap_sell_id : NULL;
      return (jvalue){.l = value ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getProductId")) {
      char value[128];
      return (jvalue){.l = live && synergy_direction_scalar(
          "productId", value, sizeof(value)) ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getGosMdmAppKey")) {
      char value[512];
      return (jvalue){.l = live && synergy_direction_string(
          "mdmAppKey", value, sizeof(value)) ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getNexusClientId")) {
      char value[512];
      const int present = live && synergy_direction_string(
          "clientId", value, sizeof(value));
      (void)0;
      return (jvalue){.l = present ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getNexusClientSecret")) {
      char value[1024];
      const int present = live && synergy_direction_string(
          "clientSecret", value, sizeof(value));
      (void)0;
      return (jvalue){.l = present ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getServerUrlWithKey")) {
      const char *key = jni_cstr(argv[0].l);
      char value[1024];
      const int present = live && key && synergy_direction_server_url(
          key, value, sizeof(value));
      (void)0;
      return (jvalue){.l = present ? jni_make_string(value) : NULL};
    }
    if (!strcmp(name, "getTrackingPostInterval"))
      return (jvalue){.i = live ? synergy_direction_int("telemetryFreq", -1) : -1};
    if (!strcmp(name, "getLatestAppVersionCheckResult")) {
      if (!live) return (jvalue){.i = 0};
      const int raw = synergy_direction_int("appUpgrade", 0);
      return (jvalue){.i = (raw == 1 || raw == 2) ? raw : 0};
    }
    if (!strcmp(name, "isFeatureDisabled")) {
      const char *key = jni_cstr(argv[0].l);
      return (jvalue){.z = live && key && synergy_direction_feature_disabled(key)
          ? JNI_TRUE : JNI_FALSE};
    }
    if (!strcmp(name, "getSynergyDirectorServerUrl")) {
      /* The running title is configured LIVE. Exact LIVE endpoint from DEX. */
      return (jvalue){.l = jni_make_string("https://syn-dir.sn.eamobile.com")};
    }
  }
  if ((has(cls, "com/ea/nimble/SynergyIdManager") ||
       has(cls, "com.ea.nimble.SynergyIdManager")) &&
      !strcmp(name, "getComponent"))
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/ISynergyIdManager")};
  if ((has(cls, "com/ea/nimble/ISynergyIdManager") ||
       has(cls, "com.ea.nimble.ISynergyIdManager")) &&
      (!strcmp(name, "getSynergyId") || !strcmp(name, "getAnonymousSynergyId"))) {
    const char *value = NULL;
    if (!strcmp(name, "getSynergyId")) {
      if (synergy_current_id[0]) value = synergy_current_id;
      else if (synergy_id_manager_anonymous_id[0]) value = synergy_id_manager_anonymous_id;
      else if (synergy_environment_live && synergy_anonymous_id[0])
        value = synergy_anonymous_id;
    } else {
      if (synergy_id_manager_anonymous_id[0]) value = synergy_id_manager_anonymous_id;
      else if (synergy_environment_live && synergy_anonymous_id[0])
        value = synergy_anonymous_id;
    }
    (void)0;
    return (jvalue){.l = value ? jni_make_string(value) : NULL};
  }

  if ((has(cls, "com/ea/nimble/Network") || has(cls, "com.ea.nimble.Network")) &&
      !strcmp(name, "getHttpProxy")) {
    /*  classes2.dex Network.getHttpProxy() reads the Java system
     * properties http.proxyHost/http.proxyPort and returns NULL when no host
     * exists (or settings cannot be read). Horizon has no Java system proxy
     * configuration, so this is the exact no-proxy result. */
    (void)0;
    return (jvalue){.l = NULL};
  }
  if ((has(cls, "com/ea/nimble/Network") || has(cls, "com.ea.nimble.Network")) &&
      !strcmp(name, "getComponent")) {
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/INetwork")};
  }
  if ((has(cls, "com/ea/nimble/INetwork") || has(cls, "com.ea.nimble.INetwork")) &&
      !strcmp(name, "getStatus")) {
    const int ordinal = nimble_network_status_ordinal();
    return (jvalue){.l = jni_make_object_class_value(
        "com/ea/nimble/Network$Status", ordinal)};
  }
  if ((has(cls, "com/ea/nimble/INetwork") || has(cls, "com.ea.nimble.INetwork")) &&
      !strcmp(name, "isNetworkWifi")) {
    /* classes2.dex NetworkImpl.isNetworkWifi() returns its cached m_isWifi. */
    return (jvalue){.z = get_switch_network_info().android_net_type == 1 ?
        JNI_TRUE : JNI_FALSE};
  }
  if ((has(cls, "java/lang/Enum") || has(cls, "java.lang.Enum")) &&
      !strcmp(name, "ordinal"))
    return (jvalue){.i = (jint)jni_object_long(self)};

  if (has(cls, "GooglePlay/GooglePlayConnect")) {
    if (!strcmp(name, "Play_IsConnected")) {
      /*  Play_IsConnected is provider/auth-session state, not generic
       * Internet connectivity.  Horizon has working Wi-Fi/Nimble networking but
       * no Google Play provider.  Returning the physical network state here was
       * hardware-proven to recursively enter the Google connector after Nexus
       * supplied the real persona, repeatedly requesting missing
       * GoogleServerClientId until the thread exhausted its stack. */
      static unsigned compat_play_checks;
      SwitchNetworkInfo net = get_switch_network_info();
      if (compat_play_checks < 16) {
        debugPrintf("compat: GooglePlay Play_IsConnected provider=unavailable "
                    "physical=%d android_type=%d type=%s return=0 call=%u\n",
                    net.is_connected, net.android_net_type, net.type_name,
                    ++compat_play_checks);
      }
      return (jvalue){.z = JNI_FALSE};
    }
    if (!strcmp(name, "Play_Connect") || !strcmp(name, "Play_Connect_Silent") ||
        !strcmp(name, "Play_Disconnect")) {
      SwitchNetworkInfo net = get_switch_network_info();
      debugPrintf("joust: class=%s method=%s sig=%s source=GooglePlay-action "
                  "physical=%d android_type=%d type=%s provider=unavailable\n",
                  cls ? cls : "?", name ? name : "?", sig ? sig : "",
                  net.is_connected, net.android_net_type, net.type_name);
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

  if ((has(cls, "cloud/Cloud") || has(cls, "cloud.Cloud")) &&
      !strcmp(name, "Cloud_SetPcpId")) {
    /*  exact libPVZ2 static registration proves this Java setter is
     * the peer of Cloud_GetPcpId. Observe the stock-supplied value only. Do
     * not persist it, change alias_id/gamePlayerId, or suppress the existing
     * unhandled-JNI behavior until the native path shows the call is required. */
    (void)argv;
  }

  if ((has(cls, "cloud/Cloud") || has(cls, "cloud.Cloud")) &&
      !strcmp(name, "Cloud_GetPcpId")) {
    /*  The compatibility layer hardware reached real OnlineIdentity validation and
     * the server returned SUCCESS with expectedId equal to the stock
     * ApplicationEnvironment alias_id. Immediately afterward native PVZ2
     * called Cloud.Cloud_GetPcpId(). Return only that already-live stock
     * alias. Never create a PCPID here and never fall back to device_uuid. */
    const char *pcpid = ea_identity_get_map_value("alias_id");
    if (!pcpid || !*pcpid || !ea_identity_value_ok(pcpid)) {
      debugPrintf("compat: Cloud_GetPcpId -> NULL (stock alias_id absent/invalid)\n");
      return (jvalue){.l = NULL};
    }
    (void)0;
    return (jvalue){.l = jni_make_string(pcpid)};
  }

  if (!strcmp(name, "Cloud_attemptSilentSync")) {
    /* diagnostic: Android always completes this asynchronous auth leg, even when
     * the user is not authenticated.  Earlier Switch builds suppressed every
     * callback, leaving PlayerIdentityService waiting forever for the cloud
     * reconciliation leg.  Report only the real platform fact -- no Google
     * Play/cloud authentication is available on Switch -- through PVZ2's own
     * Native_AuthStatusChanged(false) handler.
     *
     * Do NOT call Native_CloudStateLoaded here.  A fabricated/empty cloud
     * payload was already hardware-proven unsafe and enters the Android cloud
     * parser at +0x24005EC. */
    void *auth_changed = jni_registered("Native_AuthStatusChanged");
    static const uint32_t expected_auth_entry[4] = {
        0xd101c3ffu, /* sub sp,sp,#0x70 */
        0xa9057bfdu, /* stp x29,x30,[sp,#0x50] */
        0xa9064ff4u, /* stp x20,x19,[sp,#0x60] */
        0x910143fdu, /* add x29,sp,#0x50 */
    };
    if (!auth_changed ||
        memcmp(auth_changed, expected_auth_entry, sizeof(expected_auth_entry)) != 0) {
      debugPrintf("compat: Cloud_attemptSilentSync auth=false callback unavailable/signature-mismatch fn=%p; fail closed\n",
                  auth_changed);
      return (jvalue){0};
    }

    /* This registered native handler's stock 13.3.1 entry consumes the auth
     * byte directly in w0 (the binary's internal Android::Cloud handler ABI).
     * Calling it with false schedules PVZ2's normal game-thread auth-status
     * work; it does not synthesize a cloud state, player ID, or login token. */
    debugPrintf("compat: Cloud_attemptSilentSync provider unavailable -> stock Native_AuthStatusChanged(false)\n");
    ((void (*)(unsigned char))auth_changed)(0);
    (void)0;
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

  if (has(cls, "com.ea.nimble.Utility") && !strcmp(name, "readFile") &&
      sig && !strcmp(sig, "(Ljava/lang/String;)Ljava/lang/String;")) {
    return (jvalue){.l = nimble_utility_read_file(argv[0].l)};
  }
  if (has(cls, "com.ea.nimble.Utility") && !strcmp(name, "registerReceiver")) {
    const char *action = jni_cstr(argv[0].l);
    const char *receiver_class = jni_object_class_name(argv[1].l);
    const jlong callback_id = jni_object_long(argv[1].l);
    nimble_record_receiver(action, argv[1].l, (jint)callback_id);
    (void)receiver_class;
    return (jvalue){0};
  }
  if (has(cls, "com.ea.nimble.Utility") && !strcmp(name, "unregisterReceiver")) {
    const size_t removed = nimble_unregister_receiver(argv[0].l);
    (void)removed;
    return (jvalue){0};
  }
  if (has(cls, "com.ea.nimble.Utility") &&
      !strcmp(name, "sendBroadcastSerializable")) {
    nimble_queue_serializable_broadcast(argv[0].l, argv[1].l);
    return (jvalue){0};
  }

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
    return (jvalue){.l = jni_make_string(component_id ? component_id : "")};
  }

  if ((has(cls, "com/ea/nimble/Base") || has(cls, "com.ea.nimble.Base")) &&
      !strcmp(name, "setupNimble")) {
    nimble_java_setup_started = 1;
    for (size_t i = 0; i < nimble_java_component_count; ++i)
      nimble_setup_java_component(nimble_java_component_ids[i]);
    nimble_audit_registered_native_components();
    /* classes2.dex BaseCore.setup(): ComponentManager.setup(), then the
     * componentIndependentSetupFinished broadcast, then ComponentManager.restore().
     * Add only the missing Java Synergy component edges around the already-proven
     * native component setup/delivery. */
    synergy_java_setup();
    compat_deliver_independent_setup_finished();
    /*  exact DEX ComponentManager.restore boundary. The EA-account
     * component was registered before NetworkClientManager and its substantive
     * stock restore was the remaining identity-side lifecycle edge that the
     * Horizon shell skipped. Replay it first, then preserve The compatibility layer unchanged. */
    compat_restore_eaaccount();
    /*  exact DEX ComponentManager.restore boundary. Restore the only
     * hardware-proven substantive native network component before the Java
     * ApplicationEnvironment/Synergy restore sequence continues. */
    compat_restore_network_client_manager();
    compat_application_environment_restore();
    /* BaseCore.initialize() registers SynergyIdManager immediately before
     * SynergyEnvironment, so ComponentManager.restore() wakes the ID manager
     * here before SynergyEnvironment.restore() can call getAnonUid(). */
    synergy_id_manager_wakeup();
    synergy_java_restore();
    nimble_java_setup_completed = 1;
    (void)0;
    debugLogFlush();
    /* The initial physical network state was observed before first-frame
     * Nimble setup. Queue its stock Android notification now; the existing
     * The compatibility layer pump delivers it on a later frame, never re-entrantly. */
    nimble_queue_pending_network_status_change();
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

  if ((has(cls, "com/ea/nimble/tracking/Tracking") ||
       has(cls, "com.ea.nimble.tracking.Tracking")) &&
      !strcmp(name, "getComponent") && sig &&
      !strcmp(sig, "()Lcom/ea/nimble/tracking/ITracking;")) {
    /* classes2.dex Tracking.getComponent() returns the stable Base component,
     * not a fresh Java object on every call. Keep one interface token for the
     * process lifetime; fake JNI local/global refs are currently identity-only. */
    static jobject tracking_component;
    jobject component = __atomic_load_n(&tracking_component, __ATOMIC_ACQUIRE);
    if (!component) {
      jobject created = jni_make_object_class("com/ea/nimble/tracking/ITracking");
      jobject expected = NULL;
      if (!__atomic_compare_exchange_n(&tracking_component, &expected, created,
                                       0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        jni_free_wrapper(created);
        component = expected;
      } else {
        component = created;
      }
    }
    return (jvalue){.l = component};
  }
  if ((has(cls, "com/ea/nimble/tracking/ITracking") ||
       has(cls, "com.ea.nimble.tracking.ITracking")) && sig &&
      ((!strcmp(name, "addCustomSessionData") &&
        !strcmp(sig, "(Ljava/lang/String;Ljava/lang/String;)V")) ||
       (!strcmp(name, "logEvent") &&
        !strcmp(sig, "(Ljava/lang/String;Ljava/util/Map;)V")))) {
    /* The DEX TrackingWrangler forwards these calls to installed tracking
     * plugins. No Android Java tracking plugin is installed on Horizon, so the
     * effective empty-component behavior is a successful void return. This
     * keeps native Tracking2 progression exact without fabricating analytics
     * delivery, a session, or a backend acknowledgement. */
    return (jvalue){0};
  }

  if ((has(cls, "com/ea/nimble/Log") || has(cls, "com.ea.nimble.Log")) &&
      !strcmp(name, "getComponent")) {
    return (jvalue){.l = jni_make_object_class("com/ea/nimble/ILog")};
  }


  if ((has(cls, "com/ea/nimble/ILog") || has(cls, "com.ea.nimble.ILog")) &&
      !strcmp(name, "getThresholdLevel")) {
    /* classes2.dex LogImpl.getThresholdLevel() returns m_level. */
    return (jvalue){.i = nimble_log_threshold_level};
  }
  if ((has(cls, "com/ea/nimble/ILog") || has(cls, "com.ea.nimble.ILog")) &&
      !strcmp(name, "setThresholdLevel")) {
    /* LogImpl.setThresholdLevel stores the requested value. For this Switch
     * performance build retain the production/live ERROR floor (500), which is
     * also LogImpl.parseLevel(null)'s DEX behavior. */
    const jint requested = argv[0].i;
    const jint effective = requested < PVZ2_NIMBLE_LOG_MIN_LEVEL ?
        PVZ2_NIMBLE_LOG_MIN_LEVEL : requested;
    if (effective != nimble_log_threshold_level) {
      nimble_log_threshold_level = effective;
      debugPrintf("NIMBLE LOG: threshold requested=%d effective=%d\n",
                  requested, effective);
    }
    return (jvalue){0};
  }
  if ((has(cls, "com/ea/nimble/ILog") || has(cls, "com.ea.nimble.ILog")) &&
      !strcmp(name, "writeWithTitle")) {
    static unsigned write_with_title_calls;
    if (argv[0].i >= nimble_log_threshold_level &&
        write_with_title_calls++ < 24) {
      const char *title = jni_cstr(argv[1].l);
      const char *message = jni_cstr(argv[2].l);
      debugPrintf("JNI Nimble ILog.writeWithTitle #%u level=%d title=%.*s message=%.*s args=%p\n",
                  write_with_title_calls, argv[0].i,
                  title ? 96 : 0, title ? title : "",
                  message ? 256 : 0, message ? message : "", argv[3].l);
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
    const SwitchNetworkInfo net = get_switch_network_info();
    return (jvalue){.z = net.is_connected ? JNI_TRUE : JNI_FALSE};
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
    if (!strcmp(name, "GetStatusCode")) {
      return (jvalue){.i = jni_http_status(self)};
    }
    if (!strcmp(name, "GetResponseHeader")) {
      const char *header_name = jni_cstr(argv[0].l);
      const char *header_value = jni_http_get_response_header(self, header_name);
      return (jvalue){.l = jni_make_string(header_value)};
    }
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
