/* PVZ2 v13.3.1 Android ARM64 loader and minimal SexyAppFramework lifecycle. */

#include <switch.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>

#include "config.h"
#include "error.h"
#include "imports.h"
#include "installer.h"
#include "jni_fake.h"
#include "obb.h"
#include "opensles.h"
#include "platform.h"
#include "pvz2.h"
#include "so_util.h"
#include "util.h"

#ifndef PVZ2_ENABLE_DEEP_TRACE
#define PVZ2_ENABLE_DEEP_TRACE 0
#endif
/* Keep just the loader dependency probes enabled while state 6 is unresolved.
 * They log only on changes / every 300 calls and preserve native control flow. */
#define PVZ2_ENABLE_READINESS_TRACE 0
#define PVZ2_ENABLE_BLOCKER_TRACE 0
/* A surgical write-site probe is available for focused diagnostics. */
#define PVZ2_ENABLE_BLOCKER_XREF_SCAN 0
#define PVZ2_ENABLE_TOUCH_TRACE 0

int screen_width = 1280;
int screen_height = 720;
Config config = {0, 0, 1920, 1080, "auto"};

static so_module cxx_mod;
static so_module nimble_mod;
static so_module game_mod;

uintptr_t glu_ctor_continue;
uintptr_t title_state_continue;
uintptr_t purchase_driver_continue;
uintptr_t title_state6_exit_continue;
uintptr_t title_state_setter;
uintptr_t readiness_waiter_state1_exit;
uintptr_t readiness_waiter_state1_after_input;
uintptr_t readiness_waiter_state1_after_global;
uintptr_t readiness_waiter_input_global_page;
uintptr_t readiness_waiter_state2_exit;
uintptr_t readiness_waiter_state2_after_helper;
uintptr_t readiness_waiter_state2_helper;
uintptr_t readiness_waiter_state3_failure;
uintptr_t readiness_waiter_state3_no_detail;
uintptr_t readiness_waiter_state3_continue;
uintptr_t diagnostic_state_machine_continue;
uintptr_t diagnostic_state70_writer_continue;
uintptr_t diagnostic_state70_writer_branch_target;
uintptr_t diagnostic_dispatch_return_continue;
uintptr_t diagnostic_dispatch_adrp_value;
uintptr_t diagnostic_nimble_request_adapter;
uintptr_t diagnostic_nimble_request_return;
uintptr_t nimble_message_registry_lookup_continue;
uintptr_t nimble_message_component_continue;
uintptr_t nimble_message_component_null_cleanup;
uintptr_t joust_force_update_lookup;
uintptr_t joust_version_string_copy;
uintptr_t joust_available_rejoin;
uintptr_t diagnostic_request_state2_continue;
uintptr_t diagnostic_request_state5_helper;
uintptr_t diagnostic_request_state5_continue;
uintptr_t diagnostic_request_state6_continue;
uintptr_t diagnostic_request_state4_continue;
uintptr_t diagnostic_request_state3_continue;
uintptr_t diagnostic_request_state3_branch;
uintptr_t input_state_request_continue;
static volatile uintptr_t diagnostic_current_owner;
static volatile uintptr_t diagnostic_main_experiment_owner __attribute__((unused));
static void *glu_services;
static int glu_analytics_initialized;
static void *purchase_driver;
static void *catalog_loading_screen;
static void *catalog_last_screen;
static u64 catalog_loading_frame;
static int catalog_completion_delivered;
static int title_loading_state = -1;
static long long official_server_epoch;
static void initialize_glu_analytics_id(void);
static int module_call_analysis_offset_executable(uintptr_t off);
static void module_factory_trace_runtime_scan_factory_flow(uintptr_t caller_off);
static void seedbank_trace_note_resolved_module(uintptr_t object, const char *type_name);



extern volatile unsigned long long g_frame_count;
extern void glu_ctor_probe(void);
extern void title_state_probe(void);
extern void purchase_driver_probe(void);
extern void title_state6_exit_probe(void);
extern void nimble_message_registry_lookup_probe(void);
extern void nimble_message_component_guard_probe(void);
extern void joust_status_guard_probe(void);
extern void level_module_gate_store_probe(void);
extern void account_email_bridge_probe(void);
extern void crash_log_open(void);
static void __attribute__((unused)) dump_module_gate_probe_level_module_vtable_groups(uintptr_t vtable);
static void __attribute__((unused)) install_module_gate_probe_level_module_gate_writer_probe(void);
static void __attribute__((unused)) dump_module_layout_analysis_level_module_static_analysis(void);
static void debug_dump_bytes(const char *label, uintptr_t address,
                              size_t bytes) {
  if (!address || (address & 7) || !bytes) return;
  (void)0;
  for (size_t off = 0; off < bytes; off += 0x10) {
    const unsigned char *p = (const unsigned char *)(address + off);
    const size_t row = bytes - off < 16 ? bytes - off : 16;
    unsigned char b[16] = {0};
    memcpy(b, p, row);
    char ascii[17];
    for (unsigned i = 0; i < 16; ++i) {
      const unsigned char c = i < row ? b[i] : 0;
      ascii[i] = i < row && c >= 0x20 && c < 0x7f ? (char)c : '.';
    }
    ascii[16] = 0;
    debugPrintf("    +%03zx: %02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x  |%s|%s\n",
                off, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
                ascii, row < 16 ? " (partial)" : "");
  }
}


static void debug_log_code_address(const char *label, uintptr_t address) {
  const uintptr_t game = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t cxx = (uintptr_t)cxx_mod.load_virtbase;
  const uintptr_t nimble = (uintptr_t)nimble_mod.load_virtbase;
  if (!address) {
    debugPrintf("    %s=<none>\n", label);
  } else if (address >= game && address < game + game_mod.load_size) {
    debugPrintf("    %s=%p libPVZ2+0x%lx\n", label, (void *)address,
                (unsigned long)(address - game));
  } else if (address >= cxx && address < cxx + cxx_mod.load_size) {
    debugPrintf("    %s=%p libc+++0x%lx\n", label, (void *)address,
                (unsigned long)(address - cxx));
  } else if (address >= nimble && address < nimble + nimble_mod.load_size) {
    debugPrintf("    %s=%p libNimble+0x%lx\n", label, (void *)address,
                (unsigned long)(address - nimble));
  } else {
    debugPrintf("    %s=%p outside-loaded-modules\n", label, (void *)address);
  }
}


static size_t diagnostic_owner_allocation_size(uintptr_t owner) {
  Pvz2AllocTraceInfo info;
  if (!owner || !pvz2_alloc_trace_lookup((const void *)owner, &info) ||
      owner < info.base || owner >= info.base + info.size)
    return 0;
  const size_t remaining = info.size - (size_t)(owner - info.base);
  return remaining > 0x100 ? 0x100 : remaining;
}

static int memory_range_readable(uintptr_t address, size_t size) {
  if (!address || address + size < address) return 0;
  MemoryInfo mi;
  u32 page_info = 0;
  return R_SUCCEEDED(svcQueryMemory(&mi, &page_info, address)) &&
      (mi.perm & Perm_R) && address >= mi.addr &&
      address + size <= mi.addr + mi.size;
}

static int memory_range_writable(uintptr_t address, size_t size) {
  if (!address || address + size < address) return 0;
  MemoryInfo mi;
  u32 page_info = 0;
  return R_SUCCEEDED(svcQueryMemory(&mi, &page_info, address)) &&
      (mi.perm & Perm_W) && address >= mi.addr &&
      address + size <= mi.addr + mi.size;
}

static int read_libcpp_string(uintptr_t object, char *out,
                                     size_t out_size) {
  if (!out || out_size < 2 || !memory_range_readable(object, 24)) return 0;
  const unsigned char *p = (const unsigned char *)object;
  size_t length = 0;
  const char *data = NULL;
  if ((p[0] & 1u) == 0) {
    length = p[0] >> 1;
    data = (const char *)(p + 1);
    if (length > 22) return 0;
  } else {
    /* libc++ long-string layout on this Android build: capacity|1, size, data. */
    const uintptr_t data_ptr = *(const uintptr_t *)(object + 16);
    length = *(const size_t *)(object + 8);
    if (!data_ptr || length > 4096 || !memory_range_readable(data_ptr, length + 1))
      return 0;
    data = (const char *)data_ptr;
  }
  const size_t copy = length < out_size - 1 ? length : out_size - 1;
  memcpy(out, data, copy);
  out[copy] = 0;
  return 1;
}

/* -------------------------------------------------------------------------
 * Nimble native component registry compatibility
 *
 * The Android library imports
 * NimbleCppComponentManager::registerComponent(name, shared_ptr<component>).
 * The original Switch port resolved that import to a void stub, so every
 * component registration was discarded.  The Message helper at +0x2748168
 * later performs a normal lookup and necessarily receives an empty shared_ptr.
 *
 * Keep retained copies of the real component shared_ptrs and answer the exact
 * lookup performed by the Message helper.  This restores the component-manager
 * contract; it does not synthesize a request result or touch request state.
 * ------------------------------------------------------------------------- */

#define NIMBLE_COMPONENT_REGISTRY_CAPACITY 64
#define NIMBLE_COMPONENT_NAME_CAPACITY 128

typedef struct NimbleComponentRegistration {
  char name[NIMBLE_COMPONENT_NAME_CAPACITY];
  uintptr_t object;
  uintptr_t control;
} NimbleComponentRegistration;

static NimbleComponentRegistration
    nimble_component_registry[NIMBLE_COMPONENT_REGISTRY_CAPACITY];
static size_t nimble_component_registry_count;
static Mutex nimble_component_registry_mutex;
static int nimble_component_registry_mutex_ready;
/* The original manager owns the components constructed by libPVZ2 itself.
 * Import callers are redirected through our adapter only so the local Aruba
 * fallback remains available; consult this function first. */
uintptr_t nimble_cpp_component_native_get;
uintptr_t nimble_cpp_component_native_register;

static void nimble_component_registry_init(void) {
  if (nimble_component_registry_mutex_ready) return;
  mutexInit(&nimble_component_registry_mutex);
  nimble_component_registry_mutex_ready = 1;
}

static void nimble_component_registry_lock(void) {
  /* configure_nimble_bridge initializes this before any module constructor.
   * Keep the fallback for defensive use from an unexpectedly early call. */
  nimble_component_registry_init();
  mutexLock(&nimble_component_registry_mutex);
}

static void nimble_component_registry_unlock(void) {
  mutexUnlock(&nimble_component_registry_mutex);
}

static int nimble_shared_ptr_retain(uintptr_t control) {
  if (!control) return 1;
  /* libc++ __shared_owners_ is the signed long at control+0x08.  A stored
   * shared_ptr copy owns one additional strong reference, exactly as the
   * native manager and lookup result would. */
  const uintptr_t owners = control + 0x08;
  if (!memory_range_writable(owners, sizeof(long))) return 0;
  __atomic_fetch_add((long *)owners, 1L, __ATOMIC_RELAXED);
  return 1;
}

void nimble_cpp_component_register(const void *name_object,
                                   const void *shared_ptr_object) {
  char name[NIMBLE_COMPONENT_NAME_CAPACITY];
  if (!read_libcpp_string((uintptr_t)name_object, name, sizeof(name))) {
    debugPrintf("NIMBLE COMPONENT REGISTER: invalid name=%p; ignored\n",
                name_object);
    return;
  }

  if (!memory_range_readable((uintptr_t)shared_ptr_object,
                             2 * sizeof(uintptr_t))) {
    debugPrintf("NIMBLE COMPONENT REGISTER: name=%s invalid shared_ptr=%p; ignored\n",
                name, shared_ptr_object);
    return;
  }

  /* A non-trivial by-value shared_ptr arrives as an invisible reference in
   * x1. The native caller destroys that temporary after this function returns,
   * so the registry must acquire exactly one independent strong reference. */
  const uintptr_t object = *(const uintptr_t *)shared_ptr_object;
  const uintptr_t control =
      *(const uintptr_t *)((uintptr_t)shared_ptr_object + sizeof(uintptr_t));
  if (!object || !memory_range_readable(object, sizeof(uintptr_t))) {
    debugPrintf("NIMBLE COMPONENT REGISTER: name=%s object=%p is invalid; ignored\n",
                name, (void *)object);
    return;
  }

  size_t stored_index = SIZE_MAX;
  int duplicate = 0;
  int retained = 0;
  int registry_full = 0;
  nimble_component_registry_lock();
  for (size_t i = 0; i < nimble_component_registry_count; ++i) {
    const NimbleComponentRegistration *entry = &nimble_component_registry[i];
    if (entry->object == object && entry->control == control &&
        strcmp(entry->name, name) == 0) {
      duplicate = 1;
      stored_index = i;
      break;
    }
  }
  if (!duplicate) {
    if (nimble_component_registry_count >=
        NIMBLE_COMPONENT_REGISTRY_CAPACITY) {
      registry_full = 1;
    } else {
      retained = nimble_shared_ptr_retain(control);
      if (retained) {
        stored_index = nimble_component_registry_count++;
        NimbleComponentRegistration *entry =
            &nimble_component_registry[stored_index];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        entry->object = object;
        entry->control = control;
      }
    }
  }
  const size_t count = nimble_component_registry_count;
  nimble_component_registry_unlock();

  if (duplicate) {
    debugPrintf("NIMBLE COMPONENT REGISTER: name=%s object=%p control=%p duplicate index=%zu\n",
                name, (void *)object, (void *)control, stored_index);
  } else if (stored_index != SIZE_MAX) {
    debugPrintf("NIMBLE COMPONENT REGISTER: name=%s object=%p control=%p index=%zu count=%zu\n",
                name, (void *)object, (void *)control,
                stored_index, count);
  } else if (registry_full) {
    debugPrintf("NIMBLE COMPONENT REGISTER: registry full; name=%s ignored\n",
                name);
  } else if (!retained && control) {
    debugPrintf("NIMBLE COMPONENT REGISTER: name=%s control=%p is not writable; ignored\n",
                name, (void *)control);
  } else {
    debugPrintf("NIMBLE COMPONENT REGISTER: name=%s could not be retained; ignored\n",
                name);
  }

  /* The compatibility copy serves the local Aruba fallback. The native
   * manager must also receive every original registration: its Nexus setup
   * routine looks up the account component there. */
  if (nimble_cpp_component_native_register) {
    (void)0;
    debugLogFlush();
    ((void (*)(const void *, const void *))nimble_cpp_component_native_register)(
        name_object, shared_ptr_object);
    (void)0;
    debugLogFlush();
  } else {
    debugPrintf("compat: native component-manager register unavailable name=%s\n",
                name);
    debugLogFlush();
  }
}

/* cached non-owning native NexusService pointer for live readiness. */
static uintptr_t nexus_service_object;

/* diagnostic: prove whether the identity-side components exist in PVZ2's real
 * NimbleCppComponentManager. NimbleCppComponent_setup() silently returns when
 * a requested Java ID has no native component, so a successful JNI return by
 * itself does not prove anonymous Nexus was constructed. This is a read-only
 * lookup. The returned shared_ptr is intentionally retained for this short
 * diagnostic run rather than guessing at the private control-block destructor
 * ABI. Only a handful of identity components are probed. */
int nimble_probe_native_component(const char *name, const char *stage) {
  if (!name || !*name) return 0;
  if (!nimble_cpp_component_native_get) {
    debugPrintf("compat: native component probe stage=%s id=%s -> manager unavailable\n",
                stage ? stage : "?", name);
    debugLogFlush();
    return 0;
  }

  unsigned char name_object[24] __attribute__((aligned(8)));
  memset(name_object, 0, sizeof(name_object));
  const size_t length = strlen(name);
  if (length <= 22) {
    name_object[0] = (unsigned char)(length << 1);
    memcpy(name_object + 1, name, length);
  } else {
    const size_t encoded_capacity = length | (size_t)1;
    const char *data = name;
    memcpy(name_object + 0, &encoded_capacity, sizeof(encoded_capacity));
    memcpy(name_object + 8, &length, sizeof(length));
    memcpy(name_object + 16, &data, sizeof(data));
  }

  uintptr_t result[2] = {0, 0};
  nimble_cpp_component_native_lookup(result, name_object);
  if (result[0] && !strcmp(name, "com.ea.nimble.cpp.nexusservice"))
    nexus_service_object = result[0];
  debugLogFlush();
  return result[0] != 0;
}

static uintptr_t nimble_aruba_proxy_component_base(void);

void nimble_cpp_component_lookup(void *result_shared_ptr,
                                 const void *name_object) {
  uintptr_t *result = (uintptr_t *)result_shared_ptr;
  if (!result ||
      !memory_range_writable((uintptr_t)result, 2 * sizeof(uintptr_t)))
    return;
  result[0] = 0;
  result[1] = 0;

  char name[NIMBLE_COMPONENT_NAME_CAPACITY];
  if (!read_libcpp_string((uintptr_t)name_object, name, sizeof(name))) {
    debugPrintf("NIMBLE COMPONENT LOOKUP: invalid name=%p -> miss\n",
                name_object);
    return;
  }

  uintptr_t native_result[2] = {0, 0};
  /* Aruba is intentionally handled by the local compatibility proxy below;
   * all other component identities belong to PVZ2's original manager. */
  if (nimble_cpp_component_native_get &&
      strcmp(name, "com.ea.nimble.cpp.arubaservice") != 0)
    nimble_cpp_component_native_lookup(native_result, name_object);
  if (native_result[0]) {
    result[0] = native_result[0];
    result[1] = native_result[1];
    static unsigned native_lookup_calls;
    const unsigned call = ++native_lookup_calls;
    if (call <= 32) {
      debugPrintf("NIMBLE COMPONENT LOOKUP: name=%s -> native object=%p control=%p\n",
                  name, (void *)native_result[0], (void *)native_result[1]);
    }
    return;
  }

  uintptr_t object = 0;
  uintptr_t control = 0;
  size_t found_index = SIZE_MAX;
  int retained = 0;
  nimble_component_registry_lock();
  for (size_t i = nimble_component_registry_count; i > 0; --i) {
    const NimbleComponentRegistration *entry =
        &nimble_component_registry[i - 1];
    if (strcmp(entry->name, name) != 0) continue;
    object = entry->object;
    control = entry->control;
    found_index = i - 1;
    retained = nimble_shared_ptr_retain(control);
    if (retained) {
      result[0] = object;
      result[1] = control;
    }
    break;
  }
  const size_t count = nimble_component_registry_count;
  nimble_component_registry_unlock();

  /* Base.setupNimble is a local no-op on Horizon, so Android never
   * constructs/registers com.ea.nimble.cpp.arubaservice. Earlier testing
   * proved this exact key is what +0x2748168 requests and that the registry is
   * still empty.  Provide only this missing service identity.  The helper will
   * apply its normal +0x18 interface adjustment; no request state is touched. */
  int local_aruba = 0;
  if (found_index == SIZE_MAX &&
      strcmp(name, "com.ea.nimble.cpp.arubaservice") == 0) {
    object = nimble_aruba_proxy_component_base();
    control = 0;
    if (object) {
      result[0] = object;
      result[1] = 0;
      local_aruba = 1;
    }
  }

  static unsigned lookup_calls;
  const unsigned call = ++lookup_calls;
  if (call <= 16 || found_index == SIZE_MAX || !retained) {
    debugPrintf("NIMBLE COMPONENT LOOKUP: name=%s call=%u registry=%zu -> %s object=%p control=%p%s\n",
                name, call, count,
                local_aruba ? "local-aruba" :
                    (found_index == SIZE_MAX ? "miss" :
                        (retained ? "hit" : "invalid-control")),
                (void *)object, (void *)control,
                (retained || local_aruba)
                    ? " (native helper will select interface +0x18)" : "");
  }
}

static void diagnostic_dump_owner_string_vector(uintptr_t owner, size_t owner_size) {
  if (owner_size < 0x90) return;
  const uintptr_t begin = *(const uintptr_t *)(owner + 0x78);
  const uintptr_t end = *(const uintptr_t *)(owner + 0x80);
  const uintptr_t capacity = *(const uintptr_t *)(owner + 0x88);
  if (!begin || end < begin || capacity < end || ((end - begin) % 24) != 0)
    return;
  const size_t count = (size_t)((end - begin) / 24);
  if (count > 64 || !memory_range_readable(begin, count * 24)) return;
  (void)0;
  for (size_t i = 0; i < count; ++i) {
    char value[160];
    if (read_libcpp_string(begin + i * 24, value, sizeof(value)))
      debugPrintf("    [%zu] %s\n", i, value);
  }
}

static void diagnostic_report_target_allocation(const char *label,
                                            uintptr_t target) {
  Pvz2AllocTraceInfo info;
  if (!target || !pvz2_alloc_trace_lookup((const void *)target, &info)) return;
  (void)0;
  debug_log_code_address("child allocator-caller", info.caller);
  debug_log_code_address("child parent-1", info.parent1);
  debug_log_code_address("child parent-2", info.parent2);
}

static void diagnostic_dump_target_dispatch(const char *label, uintptr_t target) {
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  if (!target || !memory_range_readable(target, sizeof(uintptr_t))) return;
  const uintptr_t table = *(const uintptr_t *)target;
  if (table < base || table >= base + game_mod.load_size ||
      !memory_range_readable(table, 12 * sizeof(uintptr_t))) return;
  /* Only call it a dispatch table if at least one early entry points back into
   * executable libPVZ2. This avoids treating arbitrary data pointers as vtables. */
  unsigned code_entries = 0;
  for (unsigned i = 0; i < 12; ++i) {
    const uintptr_t fn = ((const uintptr_t *)table)[i];
    if (fn >= base && fn < base + game_mod.load_size) ++code_entries;
  }
  if (!code_entries) return;
  (void)0;
  for (unsigned i = 0; i < 12; ++i) {
    const uintptr_t fn = ((const uintptr_t *)table)[i];
    if (fn >= base && fn < base + game_mod.load_size)
      debugPrintf("    slot[%u]=%p libPVZ2+0x%lx\n", i, (void *)fn,
                  (unsigned long)(fn - base));
    else
      debugPrintf("    slot[%u]=%p\n", i, (void *)fn);
  }
}


static void __attribute__((unused)) diagnostic_dump_owner_targets(uintptr_t owner, size_t owner_size) {
  static const unsigned fields[] = {0x00, 0x18, 0x30, 0x38, 0x40, 0x48, 0x78};
  uintptr_t seen[sizeof(fields) / sizeof(fields[0])] = {0};
  unsigned seen_count = 0;
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const unsigned off = fields[i];
    if (off + sizeof(uintptr_t) > owner_size) continue;
    const uintptr_t target = *(const uintptr_t *)(owner + off);
    if (!target || (target >= owner && target < owner + owner_size)) continue;
    int duplicate = 0;
    for (unsigned j = 0; j < seen_count; ++j)
      if (seen[j] == target) duplicate = 1;
    if (duplicate) continue;
    seen[seen_count++] = target;
    if (!memory_range_readable(target, 0x40)) continue;
    char label[48];
    snprintf(label, sizeof(label), "owner+0x%02x target", off);
    debug_dump_bytes(label, target, 0x40);
    if (off == 0x30 || off == 0x38 || off == 0x40 || off == 0x48) {
      diagnostic_report_target_allocation(label, target);
      diagnostic_dump_target_dispatch(label, target);
    }
  }
  diagnostic_dump_owner_string_vector(owner, owner_size);
}

#define DIAGNOSTIC_OWNER_SNAPSHOT_MAX 0x100
static uintptr_t diagnostic_diff_owner;
static unsigned char diagnostic_diff_bytes[DIAGNOSTIC_OWNER_SNAPSHOT_MAX];
static size_t diagnostic_diff_size;
static int diagnostic_diff_valid;

static void __attribute__((unused)) diagnostic_owner_diff(uintptr_t owner, const char *reason) {
  /* The allocation-history ring is intentionally finite. Once the owner has
   * been attached, keep the already-validated allocation size locally so later
   * polls do not silently stop diffing after unrelated allocations wrap it. */
  const size_t size = owner == diagnostic_diff_owner && diagnostic_diff_valid
      ? diagnostic_diff_size : diagnostic_owner_allocation_size(owner);
  if (!owner || !size || !memory_range_readable(owner, size)) return;
  const unsigned char *now = (const unsigned char *)owner;
  if (owner != diagnostic_diff_owner || !diagnostic_diff_valid || size != diagnostic_diff_size) {
    memcpy(diagnostic_diff_bytes, now, size);
    diagnostic_diff_owner = owner;
    diagnostic_diff_size = size;
    diagnostic_diff_valid = 1;
    (void)0;
    return;
  }
  unsigned changed = 0;
  for (size_t off = 0; off < size; ++off) {
    if (diagnostic_diff_bytes[off] != now[off]) ++changed;
  }
  if (!changed) {
    (void)0;
    return;
  }
  (void)0;
  for (size_t off = 0; off < size; ++off) {
    if (diagnostic_diff_bytes[off] == now[off]) continue;
    debugPrintf("    +0x%02zx: %02x -> %02x\n", off,
                diagnostic_diff_bytes[off], now[off]);
  }
  memcpy(diagnostic_diff_bytes, now, size);
}

typedef struct {
  uintptr_t object;
  uintptr_t frame;
  uintptr_t stack;
  uint32_t old_value;
  uint32_t new_value;
  uint32_t w20;
  uint32_t w8;
  unsigned sequence;
} DiagnosticWriterEvent;

#define DIAGNOSTIC_WRITER_HISTORY 512
static unsigned diagnostic_writer_sequence __attribute__((unused));

static void __attribute__((unused)) diagnostic_report_writer_history(uintptr_t owner) {
  (void)owner;
}

void diagnostic_state70_writer_observed(void *object_ptr, uint32_t new_value,
                                    uint32_t w20, uint32_t w8,
                                    uintptr_t frame, uintptr_t stack) {
  (void)object_ptr; (void)new_value; (void)w20; (void)w8; (void)frame; (void)stack;
}

static int __attribute__((unused)) install_request_state70_writer_probe(uintptr_t runtime_base) {
  (void)runtime_base;
  return 0;
}

static int diagnostic_is_main_experiment_owner(uintptr_t owner) {
  if (!owner || (owner & 7) || !memory_range_readable(owner, 0x90)) return 0;
  const uintptr_t begin = *(const uintptr_t *)(owner + 0x78);
  const uintptr_t end = *(const uintptr_t *)(owner + 0x80);
  const uintptr_t capacity = *(const uintptr_t *)(owner + 0x88);
  if (!begin || end != begin + 24 || capacity < end ||
      !memory_range_readable(begin, 24)) return 0;
  char value[64];
  return read_libcpp_string(begin, value, sizeof(value)) &&
      strcmp(value, "main_experiment") == 0;
}

static void __attribute__((unused)) diagnostic_log_main_experiment_record(uintptr_t owner,
                                                 const char *reason) {
  (void)owner; (void)reason;
}


void diagnostic_request_lifecycle_observed(unsigned site, void *object_ptr,
                                       uint32_t new_value, uintptr_t frame,
                                       uintptr_t stack, uintptr_t caller_lr) {
  (void)site; (void)object_ptr; (void)new_value; (void)frame;
  (void)stack; (void)caller_lr;
}

void diagnostic_request_dispatch_returned(void *object_ptr, uintptr_t result,
                                      uintptr_t frame, uintptr_t stack,
                                      uintptr_t caller_lr) {
  (void)object_ptr; (void)result; (void)frame; (void)stack; (void)caller_lr;
}

void diagnostic_nimble_request_adapter_entered(void *owner_ptr, void *arg0,
                                            void *arg1, void *arg2, void *arg3,
                                            uintptr_t frame, uintptr_t stack) {
  (void)owner_ptr; (void)arg0; (void)arg1; (void)arg2; (void)arg3;
  (void)frame; (void)stack;
}


/* -------------------------------------------------------------------------
 * Minimal Horizon ArubaService component.
 *
 * The native manager lookup key is
 * "com.ea.nimble.cpp.arubaservice" and that no Android-side component is ever
 * registered on Switch.  The request adapter calls interface vslot +0x18 with
 * a shared_ptr to PVZ2's own callback.  For the loading-screen
 * touch/main_experiment request, that callback's first result method is vslot
 * +0x30 and resolves exactly to libPVZ2+0x10196DC.
 *
 * The Switch service reports the unavailable/empty result through that real
 * callback.  It does not write owner+0x70, loader/waiter state, or invent a
 * successful experiment payload.  PVZ2 remains responsible for translating
 * the callback into its normal request lifecycle.
 * ------------------------------------------------------------------------- */

#define NIMBLE_ARUBA_CALLBACK_FILE_OFF 0x10196dcUL

typedef struct NimbleArubaProxyBase {
  uintptr_t reserved0;
  uintptr_t reserved1;
  uintptr_t reserved2;
  uintptr_t interface_vtable; /* helper returns component + 0x18 */
} NimbleArubaProxyBase;

static uintptr_t nimble_aruba_proxy_vtable[10];
static NimbleArubaProxyBase nimble_aruba_proxy;
static int nimble_aruba_proxy_ready;

static void nimble_aruba_request_proxy(void *interface_object,
                                       void *request_context,
                                       void *request_metadata,
                                       void *callback_shared_ptr,
                                       void *request_names) {
  (void)interface_object;

  uintptr_t callback = 0;
  uintptr_t callback_control = 0;
  if (memory_range_readable((uintptr_t)callback_shared_ptr, 16)) {
    callback = *(const uintptr_t *)callback_shared_ptr;
    callback_control = *(const uintptr_t *)((uintptr_t)callback_shared_ptr + 8);
  }

  uintptr_t owner = 0;
  uintptr_t table = 0;
  uintptr_t target = 0;
  if (callback && memory_range_readable(callback, 16)) {
    table = *(const uintptr_t *)callback;
    owner = *(const uintptr_t *)(callback + 8);
    if (table && memory_range_readable(table + 0x30, sizeof(uintptr_t)))
      target = *(const uintptr_t *)(table + 0x30);
  }

  const uintptr_t expected =
      (uintptr_t)game_mod.load_virtbase + NIMBLE_ARUBA_CALLBACK_FILE_OFF;
  const uint32_t state_before =
      owner && memory_range_readable(owner + 0x70, sizeof(uint32_t))
          ? *(const uint32_t *)(owner + 0x70) : UINT32_MAX;

  debugPrintf("NIMBLE ARUBA REQUEST: context=%p metadata=%p names=%p callback={%p,%p} owner=%p state=%s%u vtable=%p result-slot=%p expected=+0x10196DC\n",
              request_context, request_metadata, request_names,
              (void *)callback, (void *)callback_control, (void *)owner,
              state_before == UINT32_MAX ? "unavailable:" : "",
              state_before == UINT32_MAX ? 0 : state_before,
              (void *)table, (void *)target);

  /* Scope the compatibility behavior to the proven loading request and the
   * exact callback implementation observed in 13.3.1.  Other Aruba requests
   * remain fail-closed until their contracts are independently identified. */
  if (!diagnostic_is_main_experiment_owner(owner) || target != expected) {
    debugPrintf("NIMBLE ARUBA REQUEST: not main_experiment/exact callback; fail closed\n");
    return;
  }

  /* Complete the recovered callback contract.
   *
   * +0x10196DC transfers result1 as a shared_ptr-like {object,control} pair
   * into queued work+0x10/+0x18.  On the predicate-true branch,
   * +0x1019D60 loads result1.object and, when non-null, passes it to
   * +0x10193F0.  That function treats the object as a libc++ std::string key:
   * it decodes the short/long string representation, finds/inserts the key in
   * the owner's completion map, marks the per-key record, and only then calls
   * the native +0x10155DC/+0x10155E0 dispatcher.
   *
   * A valid result2 predicate object with result1.object=0 caused
   * the queued code skipped the completion-map insertion and the request
   * remained state 1 forever.  Supply the actual request key
   * "main_experiment" as an exact 24-byte libc++ short string (15 chars =>
   * byte0=15<<1=0x1e), with a null control pointer because the storage is
   * static.  +0x10196DC is allowed to clear the temporary shared pair; PVZ2's
   * queued work retains only the object pointer and never owns/frees it.
   *
   * result2 remains the persistent predicate marker. No request,
   * loader, waiter, board, or completion state is written here; PVZ2's native
   * map/dispatcher/state writers remain authoritative. */
  static const unsigned char main_experiment_key[24] = {
      0x1e,
      'm','a','i','n','_','e','x','p','e','r','i','m','e','n','t',
      0,0,0,0,0,0,0,0
  };
  static uintptr_t unavailable_marker_storage = 1;
  uintptr_t result1_shared[2] = {
      (uintptr_t)main_experiment_key, 0
  };
  uintptr_t unavailable_result[3] = {
      0, (uintptr_t)&unavailable_marker_storage, 0
  };
  debugPrintf("NIMBLE ARUBA CALLBACK INPUT: callback=%p owner=%p result1={%p,%p} key=main_experiment short-tag=0x%02x result2={%p,%p,%p} marker=%p value=%lu\n",
              (void *)callback, (void *)owner,
              (void *)result1_shared[0], (void *)result1_shared[1],
              (unsigned)main_experiment_key[0],
              (void *)unavailable_result[0], (void *)unavailable_result[1],
              (void *)unavailable_result[2],
              (void *)&unavailable_marker_storage,
              (unsigned long)unavailable_marker_storage);
  if (memory_range_readable(callback, 0x40))
    debug_dump_bytes("Aruba callback object before +0x10196DC", callback, 0x40);
  typedef void (*NimbleArubaResultCallback)(void *, void *, void *);
  ((NimbleArubaResultCallback)target)((void *)callback,
                                      result1_shared,
                                      unavailable_result);

  const uint32_t state_after =
      memory_range_readable(owner + 0x70, sizeof(uint32_t))
          ? *(const uint32_t *)(owner + 0x70) : UINT32_MAX;
  debugPrintf("NIMBLE ARUBA REQUEST: real +0x10196DC callback returned; state=%s%u (native queued work may transition later)\n",
              state_after == UINT32_MAX ? "unavailable:" : "",
              state_after == UINT32_MAX ? 0 : state_after);
}

static uintptr_t nimble_aruba_proxy_component_base(void) {
  if (!nimble_aruba_proxy_ready) {
    memset(nimble_aruba_proxy_vtable, 0, sizeof(nimble_aruba_proxy_vtable));
    memset(&nimble_aruba_proxy, 0, sizeof(nimble_aruba_proxy));
    /* +0x23F486C loads interface_vtable[3] (byte offset +0x18). */
    nimble_aruba_proxy_vtable[3] = (uintptr_t)nimble_aruba_request_proxy;
    nimble_aruba_proxy.interface_vtable =
        (uintptr_t)nimble_aruba_proxy_vtable;
    nimble_aruba_proxy_ready = 1;
  }
  return (uintptr_t)&nimble_aruba_proxy;
}

int nimble_message_component_guard_observed(void *component,
                                             uintptr_t reg20,
                                             uintptr_t reg21,
                                             uintptr_t reg22,
                                             uintptr_t frame,
                                             uintptr_t adapter_stack) {
  (void)reg20; (void)reg21; (void)reg22;
  (void)frame; (void)adapter_stack;
  int safe = 0;
  uintptr_t table = 0;
  uintptr_t target = 0;
  if (component &&
      memory_range_readable((uintptr_t)component, sizeof(uintptr_t))) {
    table = *(const uintptr_t *)component;
    if (table && memory_range_readable(table + 0x18, sizeof(uintptr_t))) {
      target = *(const uintptr_t *)(table + 0x18);
      MemoryInfo mi;
      u32 page_info = 0;
      if (target && R_SUCCEEDED(svcQueryMemory(&mi, &page_info, target)) &&
          (mi.perm & Perm_X) && target >= mi.addr && target < mi.addr + mi.size)
        safe = 1;
    }
  }
  return safe;
}

static int __attribute__((unused)) diagnostic_install_exact_hook(uintptr_t file_off,
                                     const uint32_t expected[4],
                                     uintptr_t probe, const char *label) {
  const uintptr_t site = (uintptr_t)game_mod.load_base + file_off;
  const uint32_t *actual = (const uint32_t *)site;
  for (unsigned i = 0; i < 4; ++i) {
    if (actual[i] != expected[i]) {
      debugPrintf("compat: %s signature mismatch +0x%lx got=%08x expected=%08x; skipped\n",
                  label, (unsigned long)(file_off + i * 4), actual[i], expected[i]);
      return 0;
    }
  }
  hook_arm64(site, probe);
  (void)0;
  return 1;
}

static void __attribute__((unused)) install_request_lifecycle_probes(uintptr_t runtime_base) {
  (void)runtime_base;
}

static uint32_t __attribute__((unused)) diagnostic_readiness_hook(void *context_ptr) {
  const uintptr_t context = (uintptr_t)context_ptr;
  const uintptr_t state = context && !(context & 7)
      ? *(const uintptr_t *)(context + 0x10) : 0;
  const uint32_t value = state && !(state & 7)
      ? *(const uint32_t *)(state + 0x70) : UINT32_MAX;
  const uint32_t result = state && (value == 0 || value >= 3);

  diagnostic_current_owner = state;

  return result;
}


void diagnostic_state_machine_entered(void *holder_ptr, uintptr_t caller) {
  (void)holder_ptr; (void)caller;
  return;
}


static void scan_request_state_writers(void) {
  return;
}

void glu_services_created(void *self) {
  glu_services = self;
  debugPrintf("glu: service instance captured\n");
  initialize_glu_analytics_id();
}

static void initialize_glu_analytics_id(void) {
  if (glu_analytics_initialized || !glu_services) return;
  /* libc++ uses the compact string layout for values up to 22 bytes. Horizon
   * has no Android advertising ID, so use the persistent install UUID prefix
   * solely to release Glu Tags' local configuration queue. */
  struct { unsigned char size; char text[23]; } id = {0};
  const char *uuid = pvz2_device_uuid();
  const size_t length = strlen(uuid);
  const size_t used = length > 22 ? 22 : length;
  id.size = (unsigned char)(used << 1);
  memcpy(id.text, uuid, used);
  ((void (*)(void *, const void *))((uintptr_t)game_mod.load_virtbase +
      0x250223c))(glu_services, &id);
  glu_analytics_initialized = 1;
  debugPrintf("glu: initialized Tags analytics ID from local install UUID\n");
}

void watchdog_start(so_module *mod);

volatile uint32_t wwise_init_result = UINT32_MAX;
uintptr_t wwise_init_success;
uintptr_t wwise_init_failure;
uintptr_t wwise_term_continue;
uintptr_t wwise_loadbank_continue;
volatile int wwise_probe_active;
static uintptr_t wwise_state_base;
extern void wwise_init_probe(void);
extern void wwise_term_trampoline(void);
extern void wwise_loadbank_probe(void);
extern void audio_event_probe(void);

typedef void (*fn_void)(JNIEnv, jobject);
typedef void (*fn_size)(JNIEnv, jobject, int, int);
typedef void (*fn_network_status)(JNIEnv, jobject, jint);
typedef jboolean (*fn_initialize)(JNIEnv, jobject, jobject, jobject, jobject,
                                  jobject, jobject, jobject, jobject, jobject);

static fn_initialize game_initialize;
static fn_void app_will_finish;
static fn_void app_did_finish;
static fn_void app_did_become_active;
static fn_void app_will_resign_active;
static fn_void app_did_enter_background;
static fn_void app_will_become_foreground;
static fn_void create_lifecycle_observer;
static fn_void audio_gained;
static fn_void audio_lost;
static fn_network_status set_current_network_status;
static int last_network_status = INT_MIN;
static fn_void surface_created;
static fn_size surface_changed;
static fn_void draw_frame;
typedef void (*fn_nimble_component_setup)(JNIEnv, jobject);
static fn_nimble_component_setup nimble_component_setup;
static fn_nimble_component_setup nimble_component_restore;
typedef void (*fn_nimble_lifecycle)(JNIEnv, jobject, jobject);
static fn_nimble_lifecycle nimble_on_application_launch;
static fn_nimble_lifecycle nimble_on_application_resume;
static int nimble_launch_edge_pending;
static int nimble_resume_edge_pending;
static int nimble_launch_edge_delivered;
static int nimble_resume_edge_delivered;
static int nimble_nexus_restore_stage_complete;

/* the stock application launch/resume JNI
 * bridges execute and return cleanly, but they do not start anonymous identity.
 * Their exact AArch64 loads the listener-vector begin/end pair from
 * libPVZ2+0x2E54260 before calling listener vtable +0x10/+0x20. Observe that
 * live vector once after Base.setupNimble, before the already-proven the compatibility layer
 * lifecycle delivery. This is read-only and does not synthesize any listener
 * or component state. */

void nimble_setup_cpp_component(jobject component) {
  if (!component || !nimble_component_setup) {
    debugPrintf("NIMBLE COMPONENT JAVA: native setup unavailable component=%p\n",
                component);
    return;
  }
  nimble_component_setup(fake_env, component);
}

void nimble_restore_cpp_component(jobject component) {
  if (!component || !nimble_component_restore) {
    debugPrintf("NIMBLE COMPONENT JAVA: native restore unavailable component=%p\n",
                component);
    return;
  }
  nimble_component_restore(fake_env, component);
}

static void nimble_message_noop(void) {}

/* Account registration safety bridge.
 *
 * The nearby address +0x23F768C is not a function entry: it executes with an active
 * frame and reaches the stock null dereference at +0x23F7700.  hook_arm64()
 * uses BR (not BL), so branching from +0x23F768C into a normal C function and
 * returning through the outer LR left the native frame un-unwound and caused
 * the observed re-entry/stall.
 *
 * The bridge hooks the exact four-instruction block beginning at the proven
 * x20 dereference.  glu_probe.s replays that block when x20 is non-null.  On
 * Horizon x20 is the unavailable Android-owned account object, so the probe
 * opens the Switch EA web applet, then returns through the original account
 * routine epilogue. It does not enter the unavailable-provider path or
 * fabricate auth/cloud success. */
uintptr_t account_email_bridge_nonnull_continue;
uintptr_t account_email_bridge_null_return;

static void install_account_email_bridge(void) {
  const uintptr_t base = (uintptr_t)game_mod.load_base;
  const uintptr_t site = base + 0x23f7700;
  static const uint32_t expected[4] = {
      0xf9400288u, /* ldr x8,[x20] -- faults when x20 is null */
      0xf9401508u, /* ldr x8,[x8,#0x28] */
      0x910003e1u, /* mov x1,sp */
      0xd10103a3u, /* sub x3,x29,#0x40 */
  };
  const uint32_t *actual = (const uint32_t *)site;
  for (unsigned i = 0; i < 4; ++i) {
    if (actual[i] != expected[i]) {
      debugPrintf("ACCOUNT BRIDGE: signature mismatch +0x%lx got=%08x expected=%08x; skipped\n",
                  0x23f7700UL + i * 4UL, actual[i], expected[i]);
      return;
    }
  }
  account_email_bridge_nonnull_continue = (uintptr_t)game_mod.load_virtbase + 0x23f7710;
  /* The native null-provider path continues into a call that assumes the
   * Android account object exists and faults at libPVZ2+0x24005ec. Return via
   * the original function epilogue after collecting the Switch email. */
  account_email_bridge_null_return = (uintptr_t)game_mod.load_virtbase + 0x23f779c;
  hook_arm64(site, (uintptr_t)account_email_bridge_probe);
  debugPrintf("ACCOUNT BRIDGE: installed at libPVZ2+0x23F7700; null Android account object -> Switch EA web applet -> native epilogue +0x23F779C\n");
}

static void nimble_message_lookup(void) {
  __asm__ volatile("stp xzr, xzr, [x8]\nret");
}

static void get_screen_size(void *self, int *width, int *height) {
  (void)self;
  *width = screen_width;
  *height = screen_height;
}

static void patch_missing_string_map_at(void) {
  const uintptr_t base = (uintptr_t)game_mod.load_base;
  const uintptr_t from = base + 0x1060f88;
  const uintptr_t to = base + 0x1060f50;
  const intptr_t words = ((intptr_t)to - (intptr_t)from) >> 2;
  *(uint32_t *)from = 0x14000000u | ((uint32_t)words & 0x03ffffffu);
  /* Two inline lookups in the startup metadata helper throw on a missing
   * entry.  Their normal success paths already store false and clean up the
   * temporary key, so branch there while x8/w8 are still zero. */
  const uintptr_t missing_a = base + 0x21cff00;
  const uintptr_t default_a = base + 0x21cff18;
  const uintptr_t missing_b = base + 0x21cff68;
  const uintptr_t default_b = base + 0x21cff80;
  *(uint32_t *)missing_a = 0x14000000u |
      ((uint32_t)(((intptr_t)default_a - (intptr_t)missing_a) >> 2) & 0x03ffffffu);
  *(uint32_t *)missing_b = 0x14000000u |
      ((uint32_t)(((intptr_t)default_b - (intptr_t)missing_b) >> 2) & 0x03ffffffu);
  debugPrintf("PVZ2 patch: missing metadata defaults false (%08x,%08x)\n",
              *(uint32_t *)missing_a, *(uint32_t *)missing_b);
}

/* ForceUpdateConfigData may be absent on Horizon. Preserve the stock
 * MinJoustVersion check when present; otherwise continue through the existing
 * Joust-available path. */



/* Retry OnlineIdentity validation once the restored Nexus credentials are
 * actually ready. The stock re-arm lambda resets only its retry/gate fields. */
static int validation_rearmed;

typedef struct {
  uintptr_t unused;
  uintptr_t online;
} ValidationRearmClosure;

static size_t libcpp_string_length(uintptr_t object) {
  if (!memory_range_readable(object, 24)) return 0;
  const unsigned char *p = (const unsigned char *)object;
  if ((p[0] & 1u) == 0) {
    const size_t length = p[0] >> 1;
    return length <= 22 ? length : 0;
  }
  const size_t length = *(const size_t *)(object + 8);
  const uintptr_t data = *(const uintptr_t *)(object + 16);
  if (!length || length > 16384 || !data || !memory_range_readable(data, 1))
    return 0;
  return length;
}

static void maintain_online_identity_validation(void) {
  static unsigned long long last_sample_frame = ULLONG_MAX;
  if (last_sample_frame != ULLONG_MAX &&
      g_frame_count - last_sample_frame < 4)
    return;
  last_sample_frame = g_frame_count;

  if (validation_rearmed || title_loading_state != 15) return;

  const uintptr_t slot = (uintptr_t)game_mod.load_virtbase + 0x2d68fc0;
  if (!memory_range_readable(slot, sizeof(uintptr_t))) return;
  const uintptr_t outer = *(const uintptr_t *)slot;
  if (!outer || !memory_range_readable(outer, 0x138)) return;

  const uintptr_t online = *(const uintptr_t *)(outer + 0x130);
  if (!online || !memory_range_readable(online, 0x50)) return;

  const uintptr_t validate_req = *(const uintptr_t *)(online + 0x08);
  const uintptr_t assoc_req = *(const uintptr_t *)(online + 0x18);
  const uint32_t retry = *(const uint32_t *)(online + 0x2c);
  const unsigned char gate = *(const unsigned char *)(online + 0x30);
  if (retry < 3 || gate || validate_req || assoc_req) return;

  const uintptr_t nexus = nexus_service_object;
  if (!nexus || !memory_range_readable(nexus + 0x120, 24) ||
      !memory_range_readable(nexus + 0xe8, sizeof(int64_t)) ||
      !memory_range_readable(nexus + 0xb8, 24) ||
      !memory_range_readable(nexus + 0x58, sizeof(uint32_t)))
    return;
  if (*(const uint32_t *)(nexus + 0x58) != 3) return;

  const size_t token_len = libcpp_string_length(nexus + 0xb8);
  const size_t persona_len = libcpp_string_length(nexus + 0x120);
  const long long expiry = *(const int64_t *)(nexus + 0xe8);
  const long long host_epoch = (long long)time(NULL);
  if (!token_len || !persona_len || expiry <= host_epoch ||
      (official_server_epoch > 0 && expiry <= official_server_epoch))
    return;

  const uintptr_t rearm = (uintptr_t)game_mod.load_virtbase + 0x1e43430;
  if (!memory_range_readable(rearm, 4)) return;
  ValidationRearmClosure closure = {0, online};
  ((void (*)(void *))rearm)(&closure);
  validation_rearmed = 1;
}

/* Seed the native server clock from the official CDN Date header.
 * PvZ2Web savedelta responses remain isolated from local save merging. */
void note_official_server_time(long long epoch, const char *date_header,
                               const char *url) {
  (void)date_header;
  (void)url;
  if (epoch >= 1500000000LL && epoch <= 4102444800LL)
    official_server_epoch = epoch;
}

static void update_joust_server_time(void) {
  typedef void (*UpdateSeasonFn)(void);
  typedef void (*SetServerTimeFn)(void *, long long);
  static unsigned attempts;
  static unsigned long long last_attempt;
  static int complete;

  if (complete || title_loading_state != 15 || !official_server_epoch) return;
  if (last_attempt && g_frame_count - last_attempt < 120) return;
  if (attempts++ >= 12) return;
  last_attempt = g_frame_count;

  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t time_slot = base + 0x2d60370;
  const uintptr_t status_slot = base + 0x2d6d648;
  UpdateSeasonFn update_current_season = (UpdateSeasonFn)(base + 0x112c738);
  SetServerTimeFn set_server_time = (SetServerTimeFn)(base + 0x1dc1f40);

  /* The stock season updater creates TimeMgr lazily. */
  update_current_season();

  uintptr_t time_obj = 0;
  if (memory_range_readable(time_slot, sizeof(uintptr_t)))
    time_obj = *(const uintptr_t *)time_slot;
  if (!time_obj || !memory_range_readable(time_obj + 0x48, sizeof(float))) return;

  float sync_marker = 0.0f;
  memcpy(&sync_marker, (const void *)(time_obj + 0x48), sizeof(sync_marker));
  if (sync_marker == 0.0f)
    set_server_time((void *)time_obj, official_server_epoch);

  update_current_season();

  if (memory_range_readable(status_slot, sizeof(uintptr_t))) {
    const uintptr_t status = *(const uintptr_t *)status_slot;
    if (status && memory_range_readable(status + 0x218, sizeof(uintptr_t)) &&
        *(const uintptr_t *)(status + 0x218))
      complete = 1;
  }
}





static void install_archive_cleanup_fix(void) {

  /* Horizon cannot safely free this transient archive timing string at this
   * point. Preserve the surrounding stock cleanup and result flow. */
  const uintptr_t cleanup_delete = (uintptr_t)game_mod.load_base + 0x1791cd0;
  const uint32_t cleanup_delete_expected = 0x944d6e58u; /* bl _ZdlPv@plt */
  if (*(const uint32_t *)cleanup_delete == cleanup_delete_expected) {
    *(uint32_t *)cleanup_delete = 0xd503201fu; /* nop */
    (void)0;
  } else {
    debugPrintf("archive: cleanup signature mismatch; patch skipped\n");
  }
}



static void install_joust_availability_guard(void) {
  const uintptr_t site = (uintptr_t)game_mod.load_base + 0x1da8450;
  static const uint32_t expected[4] = {
      0x940f0255u, /* bl +0x2168DA4 */
      0x36000234u, /* tbz w20,#0,+0x44 */
      0x91010001u, /* add x1,x0,#0x40 */
      0x910003e0u, /* mov x0,sp */
  };
  if (memcmp((const void *)site, expected, sizeof(expected)) != 0) {
    debugPrintf("joust: signature mismatch at +0x1DA8450; guard skipped\n");
    return;
  }
  joust_force_update_lookup = (uintptr_t)game_mod.load_virtbase + 0x2168da4;
  joust_version_string_copy =
      (uintptr_t)game_mod.load_virtbase + 0x1da8460;
  joust_available_rejoin =
      (uintptr_t)game_mod.load_virtbase + 0x1da8480;
  hook_arm64(site, (uintptr_t)joust_status_guard_probe);
  (void)0;
}

/* MainMenu_GameCenterControl owns the bottom-left Google Play button.  Its
 * lazy creator keeps the pointer at menu+0x110; an empty slot normally takes
 * the allocation path below.  Horizon has no Google Play provider, so use
 * the creator's own already-present exit and leave that slot empty. */
static void hide_google_play_menu_button(void) {
  const uintptr_t site = (uintptr_t)game_mod.load_base + 0x105e84c;
  const uint32_t expected = 0xb5000528u; /* cbnz x8,+0xa4 (create control) */
  if (*(const uint32_t *)site != expected) {
    debugPrintf("GOOGLE PLAY MENU: signature mismatch; button left unchanged\n");
    return;
  }
  const uintptr_t exit = (uintptr_t)game_mod.load_base + 0x105e8f0;
  const intptr_t words = ((intptr_t)exit - (intptr_t)site) >> 2;
  *(uint32_t *)site = 0x14000000u | ((uint32_t)words & 0x03ffffffu);
  debugPrintf("GOOGLE PLAY MENU: MainMenu Game Center control disabled\n");
}


static void install_nimble_message_registry_lookup(void) {
  static const uint32_t expected[4] = {
    0x910063e8u, /* add x8,sp,#0x18 */
    0x910003e0u, /* mov x0,sp */
    0x94000490u, /* bl +0x2749404 */
    0xa941a7e8u, /* ldp x8,x9,[sp,#0x18] */
  };
  const uintptr_t file_site = (uintptr_t)game_mod.load_base + 0x27481bc;
  const uint32_t *actual = (const uint32_t *)file_site;
  for (unsigned i = 0; i < 4; ++i) {
    if (actual[i] != expected[i]) {
      debugPrintf("Nimble component registry: Message lookup signature mismatch +0x%lx got=%08x expected=%08x; skipped\n",
                  (unsigned long)(0x27481bc + i * 4), actual[i], expected[i]);
      return;
    }
  }
  nimble_message_registry_lookup_continue =
      (uintptr_t)game_mod.load_virtbase + 0x27481cc;
  hook_arm64(file_site, (uintptr_t)nimble_message_registry_lookup_probe);
  debugPrintf("Nimble component registry: connected Message lookup at +0x27481BC (continue +0x27481CC)\n");
}

static void install_nimble_message_component_guard(void) {
  static const uint32_t expected[4] = {
    0xf9400268u, /* ldr x8,[x19] */
    0xf9400d08u, /* ldr x8,[x8,#0x18] */
    0x9100a3e1u, /* add x1,sp,#0x28 */
    0xd100c3a2u, /* sub x2,x29,#0x30 */
  };
  const uintptr_t file_site = (uintptr_t)game_mod.load_base + 0x23f4868;
  const uint32_t *actual = (const uint32_t *)file_site;
  for (unsigned i = 0; i < 4; ++i) {
    if (actual[i] != expected[i]) {
      debugPrintf("Nimble Message guard: signature mismatch +0x%lx got=%08x expected=%08x; skipped\n",
                  (unsigned long)(0x23f4868 + i * 4), actual[i], expected[i]);
      return;
    }
  }
  nimble_message_component_continue =
      (uintptr_t)game_mod.load_virtbase + 0x23f4878;
  /* +0x23F4888 is the original first instruction after BLR x8.  The null
   * path joins there so the adapter releases all native temporaries itself. */
  nimble_message_component_null_cleanup =
      (uintptr_t)game_mod.load_virtbase + 0x23f4888;
  hook_arm64(file_site, (uintptr_t)nimble_message_component_guard_probe);
  debugPrintf("Nimble Message guard: installed at +0x23F4868 (null -> +0x23F4888 cleanup)\n");
}


/* Skip Android-only one-time Joust unlock presentation gates while leaving
 * progression and maintenance checks intact. */
static void install_joust_unlock_presentation_fix(void) {
  const uintptr_t hbase = (uintptr_t)game_mod.load_base;
  if (!hbase) return;

  struct JoustPresentationPatch {
    uintptr_t off;
    uint32_t expected;
    const char *label;
  };
  static const struct JoustPresentationPatch patches[] = {
      {0x1DA83F4u, 0x370000b6u, "nar_joust_unlock pending branch"},
      {0x1DA83FCu, 0x37000060u, "nar_joust_unlock_anim pending branch"},
  };

  for (unsigned i = 0; i < sizeof(patches) / sizeof(patches[0]); ++i) {
    uint32_t *word = (uint32_t *)(hbase + patches[i].off);
    if (*word != patches[i].expected) {
      debugPrintf("joust: %s signature mismatch at +0x%lx got=%08x expected=%08x; not patched\n",
                  patches[i].label, (unsigned long)patches[i].off,
                  *word, patches[i].expected);
      continue;
    }
    *word = 0xd503201fu; /* nop */
    (void)0;
  }
  (void)0;
}


static void configure_nimble_bridge(void) {
  /* Keep the stock callback-object factory intact; the fake JNI layer implements
   * the Android BaseNativeCallback constructor contract. */
  nimble_component_setup = (fn_nimble_component_setup)so_find_addr_rx(
      &game_mod,
      "Java_com_ea_nimble_bridge_NimbleCppComponentRegistrar_00024NimbleCppComponent_setup");
  nimble_component_restore = (fn_nimble_component_setup)so_find_addr_rx(
      &game_mod,
      "Java_com_ea_nimble_bridge_NimbleCppComponentRegistrar_00024NimbleCppComponent_restore");
  nimble_cpp_component_native_get = so_find_addr_rx(
      &game_mod,
      "_ZN2EA6Nimble12BaseInternal25NimbleCppComponentManager12getComponentERKNSt6__ndk112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE");
  nimble_cpp_component_native_register = so_find_addr_rx(
      &game_mod,
      "_ZN2EA6Nimble12BaseInternal25NimbleCppComponentManager17registerComponentERKNSt6__ndk112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEENS3_10shared_ptrINS1_18NimbleCppComponentEEE");
  debugPrintf("Nimble bridge: component registrar setup=%p restore=%p\n",
              (void *)nimble_component_setup, (void *)nimble_component_restore);
  if (!nimble_component_setup || !nimble_component_restore)
    fatal_error("stock Nimble component setup/restore export missing.");
  debugPrintf("Nimble bridge: original component manager=%p\n",
              (void *)nimble_cpp_component_native_get);
  const uintptr_t base = (uintptr_t)game_mod.load_base;
  nimble_component_registry_init();
  debugPrintf("Nimble bridge: restored native request adapter +0x23F474C\n");
  install_nimble_message_registry_lookup();
  install_nimble_message_component_guard();
  install_account_email_bridge();
  hook_arm64(base + 0x23f49d4, (uintptr_t)nimble_message_noop);
  hook_arm64(base + 0x23f4af4, (uintptr_t)nimble_message_noop);
  hook_arm64(base + 0x23f4bb8, (uintptr_t)nimble_message_lookup);
  hook_arm64(base + 0x23f4c84, (uintptr_t)nimble_message_noop);
  install_joust_unlock_presentation_fix();
}

static void hook_glu_service_constructor(void) {
  /* Capture only after the complete object has been initialized.  Installing
   * the persistent ID here lets the native Tags gate add analyticsId to its
   * very first SDK_CONFIG_PIN request. */
  glu_ctor_continue = (uintptr_t)game_mod.load_virtbase + 0x2500bb0;
  hook_arm64((uintptr_t)game_mod.load_base + 0x2500ba0,
             (uintptr_t)glu_ctor_probe);
}

void purchase_driver_created(void *self) {
  purchase_driver = self;
  debugPrintf("PVZ2 purchase driver: captured native catalog listener\n");
}

static void hook_purchase_driver_constructor(void) {
  /* NimblePurchaseDriver subscribes to refreshcatalogfinished at +0x240a1e0.
   * Capture its fully allocated native object before the subscription is made. */
  purchase_driver_continue = (uintptr_t)game_mod.load_virtbase + 0x240a210;
  hook_arm64((uintptr_t)game_mod.load_base + 0x240a200,
             (uintptr_t)purchase_driver_probe);
}

void title_state_changed(void *screen, int state) {
  const char *name = state == 13 ? "WaitingForABTestsToLoad" :
                     state == 14 ? "Finalizing" :
                     state == 15 ? "Finished" : "other";
  debugPrintf("PVZ2 loading state -> %d (%s)\n", state, name);
  title_loading_state = state;
  catalog_last_screen = screen;
  if (state == 13)
    pvz2_release_startup_wait_timer();
  if (state == 6 && pvz2_take_catalog_refresh_request()) {
    catalog_loading_screen = screen;
    catalog_loading_frame = g_frame_count;
    catalog_completion_delivered = 0;
    debugPrintf("PVZ2 catalog: queueing native refreshcatalogfinished event\n");
  }
}

void title_state6_exit_observed(void *screen, uint32_t result) {
  (void)screen; (void)result;
  return;
  static unsigned calls;
  static unsigned previous = UINT_MAX;
  static int previous_catalog_delivery = -1;
  static uintptr_t previous_gate_root = UINTPTR_MAX;
  static uintptr_t previous_waiter = UINTPTR_MAX;
  static unsigned previous_gate_enabled = UINT_MAX;
  static unsigned previous_waiter_state = UINT_MAX;
  const uintptr_t listener = (uintptr_t)purchase_driver;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  /* +0x17678B4 is the state-6 readiness accessor.  It reads this global
   * object's +0x802 byte, then returns the inverse of the singleton's +0x08
   * state when that byte is enabled.  These are observations only: neither
   * value is modified or invoked by this tracer. */
  const uintptr_t gate_root_slot = base + 0x2dca2d0;
  const uintptr_t waiter_slot = base + 0x2d6ba08;
  const uintptr_t gate_root = base ? *(const uintptr_t *)gate_root_slot : 0;
  const uintptr_t waiter = base ? *(const uintptr_t *)waiter_slot : 0;
  const unsigned gate_enabled = gate_root && !(gate_root & 7)
      ? *(const uint8_t *)(gate_root + 0x802) : UINT_MAX;
  const unsigned waiter_state = waiter && !(waiter & 7)
      ? *(const uint32_t *)(waiter + 0x08) : UINT_MAX;
  const unsigned state = screen ? *(const uint32_t *)((uintptr_t)screen + 184) : UINT_MAX;
  const int changed = result != previous;
  const int catalog_delivery_changed =
      catalog_completion_delivered != previous_catalog_delivery;
  const int dependency_changed = gate_root != previous_gate_root ||
      waiter != previous_waiter || gate_enabled != previous_gate_enabled ||
      waiter_state != previous_waiter_state;
  calls++;
  const int periodic_after_delivery =
      catalog_completion_delivered && (calls % 300) == 0;
  previous = result;
  previous_catalog_delivery = catalog_completion_delivered;
  previous_gate_root = gate_root;
  previous_waiter = waiter;
  previous_gate_enabled = gate_enabled;
  previous_waiter_state = waiter_state;
  if (calls > 16 && !changed && !catalog_delivery_changed && !dependency_changed &&
      !periodic_after_delivery) return;

  debugPrintf("LOADER STATE-6 EXIT GATE:\n"
              "    frame=%llu call=%u screen=%p state=%u\n"
              "    predicate_w0=%u action=%s\n"
              "    catalog_ready=%d catalog_failed=%d catalog_items=%d listener_delivered=%d\n",
              (unsigned long long)g_frame_count, calls, screen, state, result,
              (result & 1) ? "nonzero -> native setState(12)" :
                             "zero -> remain in state 6",
              pvz2_catalog_refresh_ready(), pvz2_catalog_refresh_failed(),
              pvz2_catalog_item_count(), catalog_completion_delivered);
  debugPrintf("    readiness root=%p gate(+0x802)=%s%u waiter=%p state(+0x08)=%s%u\n",
              (void *)gate_root,
              gate_enabled == UINT_MAX ? "unavailable:" : "",
              gate_enabled == UINT_MAX ? 0 : gate_enabled,
              (void *)waiter,
              waiter_state == UINT_MAX ? "unavailable:" : "",
              waiter_state == UINT_MAX ? 0 : waiter_state);
  if (listener) {
    const uintptr_t begin = *(const uintptr_t *)(listener + 0x70);
    const uintptr_t end = *(const uintptr_t *)(listener + 0x78);
    const uintptr_t capacity = *(const uintptr_t *)(listener + 0x80);
    debugPrintf("    listener=%p begin=%p end=%p capacity=%p dirty=%u\n",
                purchase_driver, (void *)begin, (void *)end, (void *)capacity,
                *(const uint8_t *)(listener + 0x88));
  }
}

/* State 6 itself is only waiting for the singleton at +0x2d6ba08 to clear
 * its +0x08 state.  Its native update function (+0x14fe720) has three
 * independent predicates on the state-1 path.  The assembly probes below
 * preserve and replay each branch; this function only records their real
 * return bits. */
void readiness_waiter_predicate_observed(unsigned phase, void *waiter,
                                         uint32_t result) {
  (void)phase; (void)waiter; (void)result;
  return;
  static unsigned calls[3];
  static unsigned previous[3] = {UINT_MAX, UINT_MAX, UINT_MAX};
  if (phase < 1 || phase > 3) return;
  const unsigned index = phase - 1;
  const unsigned call = ++calls[index];
  const int changed = result != previous[index];
  previous[index] = result;
  if (call > 8 && !changed && (call % 300) != 0) return;

  const uintptr_t self = (uintptr_t)waiter;
  const unsigned state = self ? *(const uint32_t *)(self + 0x08) : UINT_MAX;
  const unsigned option_a = self ? *(const uint8_t *)(self + 0x91) : UINT_MAX;
  const unsigned option_b = self ? *(const uint8_t *)(self + 0x92) : UINT_MAX;
  static const char *const labels[] = {
    "+0x1791564 initial readiness",
    "+0x20e94c0 optional input readiness",
    "+0x14feaa8 final completion",
  };
  debugPrintf("LOADER WAITER PREDICATE:\n"
              "    phase=%u (%s) call=%u waiter=%p state=%s%u\n"
              "    w0=%u action=%s flags(+0x91,+0x92)=(%s%u,%s%u)\n",
              phase, labels[index], call, waiter,
              state == UINT_MAX ? "unavailable:" : "",
              state == UINT_MAX ? 0 : state, result,
              (result & 1) ? "pass" : "return; state remains pending",
              option_a == UINT_MAX ? "unavailable:" : "",
              option_a == UINT_MAX ? 0 : option_a,
              option_b == UINT_MAX ? "unavailable:" : "",
              option_b == UINT_MAX ? 0 : option_b);
  if (phase == 2) {
    /* +0x20e94c0 follows this chain.  Its return is false exactly while the
     * final state's value is 1 or 2; logging the real value lets us find the
     * subsystem that owns the pending state rather than faking its result. */
    const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
    const uintptr_t slot = base + 0x2d37a60;
    const uintptr_t p1 = base ? *(const uintptr_t *)slot : 0;
    const uintptr_t app = p1 && !(p1 & 7) ? *(const uintptr_t *)p1 : 0;
    const uintptr_t context = app && !(app & 7)
        ? *(const uintptr_t *)(app + 0x828) : 0;
    const uintptr_t state_owner = context && !(context & 7)
        ? *(const uintptr_t *)(context + 0x10) : 0;
    const unsigned native_state = state_owner && !(state_owner & 7)
        ? *(const uint32_t *)(state_owner + 0x70) : UINT_MAX;
    const uintptr_t context_field0 = context && !(context & 7)
        ? *(const uintptr_t *)context : 0;
    const uintptr_t state_field0 = state_owner && !(state_owner & 7)
        ? *(const uintptr_t *)state_owner : 0;
    const int context_field0_in_game = context_field0 >= base &&
        context_field0 < base + game_mod.load_size;
    const int state_field0_in_game = state_field0 >= base &&
        state_field0 < base + game_mod.load_size;
    debugPrintf("    diagnostic_context=%p field0=%p%s state_owner=%p field0=%p%s\n"
                "    diagnostic_native_state(+0x70)=%s%u expected=0-or->=3\n",
                (void *)context, (void *)context_field0,
                context_field0_in_game ? " (libPVZ2)" : "",
                (void *)state_owner, (void *)state_field0,
                state_field0_in_game ? " (libPVZ2)" : "",
                native_state == UINT_MAX ? "unavailable:" : "",
                native_state == UINT_MAX ? 0 : native_state);
  }
}

static void complete_local_catalog_refresh(void) {
  /* A second refresh can be requested while the title state remains 6; no
   * second title-state callback is guaranteed in that case. Reuse the live
   * screen rather than fabricating a completion event outside the loader. */
  if (!catalog_loading_screen && catalog_last_screen &&
      *(int *)((uintptr_t)catalog_last_screen + 184) == 6 &&
      pvz2_take_catalog_refresh_request()) {
    catalog_loading_screen = catalog_last_screen;
    catalog_loading_frame = g_frame_count;
    catalog_completion_delivered = 0;
    debugPrintf("PVZ2 catalog: tracking refresh while loader remains in state 6\n");
  }
  if (!catalog_loading_screen || g_frame_count <= catalog_loading_frame) return;
  void *screen = catalog_loading_screen;
  if (!purchase_driver || *(int *)((uintptr_t)screen + 184) != 6) {
    catalog_loading_screen = NULL;
    debugPrintf("PVZ2 catalog: native listener unavailable or state changed\n");
    return;
  }
  const int refresh_failed = pvz2_catalog_refresh_failed();
  if (!refresh_failed && !pvz2_catalog_refresh_ready()) return;
  if (!catalog_completion_delivered) {
    /* This is the captured NimblePurchaseDriver handler for
     * nimble.notification.mtx.refreshcatalogfinished.  On Android it is
     * also delivered when the asynchronous catalog refresh fails, after the
     * MTX component has exposed an empty catalog. */
    const uintptr_t listener_target =
        (uintptr_t)game_mod.load_virtbase + 0x240a574;
    ((void (*)(void *))listener_target)(purchase_driver);
    catalog_completion_delivered = 1;
    catalog_loading_frame = g_frame_count;
    return;
  }
  catalog_loading_screen = NULL;
  debugPrintf("PVZ2 catalog: listener delivered; awaiting the native state-6 gate\n");
}

extern void register_touch_gameplay_object_trampoline(void *x0, void *x1, uint32_t x2, void *x3, void *x4);
extern void unregister_touch_gameplay_object_trampoline(void *x0, void *x1);
extern void game_input_on_touch_event_trampoline(void *this_ptr, void *x1, void *x2, uint32_t w3);
extern void touch_registration_dispatch_probe(void);
extern void touch_registration_filter_probe(void);
extern void touch_registration_filter_branch_probe(void);

extern uintptr_t register_touch_continue;
extern uintptr_t unregister_touch_continue;
extern uintptr_t game_input_on_touch_continue;
extern uintptr_t touch_registration_filter_continue;
extern uintptr_t touch_registration_filter_branch_continue;
extern uintptr_t touch_registration_filter_branch_zero;

#define MAX_TRACKED_OWNERS 128
typedef struct {
  void *owner;
  void *vtable;
  uint64_t flags;
} TrackedOwner;

static int g_register_calls_alive = 0;
static int g_tracked_owners_count = 0;

static volatile int g_touch_probe_phase;
static volatile int g_touch_probe_id;
static volatile float g_touch_probe_x;
static volatile float g_touch_probe_y;
static unsigned g_touch_probe_object_dumps;


/* Remember the LevelModuleManager object observed by the state-5 tail.
 * Observation only: the port never writes this object or its +0x70 flag. */
static uintptr_t g_level_module_manager;
enum { MAX_LEVEL_MODULE_MANAGERS = 16 };
typedef struct {
  uintptr_t ptr;
  unsigned last70;
  unsigned valid;
  unsigned attached;
  unsigned long long born_frame;
  unsigned long long last_frame;
} module_manager_traceLevelManager;
#define module_manager_trace_MAX_LEVEL_MANAGERS MAX_LEVEL_MODULE_MANAGERS
static module_manager_traceLevelManager g_module_manager_trace_level_managers[module_manager_trace_MAX_LEVEL_MANAGERS];
static unsigned g_module_manager_trace_level_manager_next;
static int arm64_analysis_level_module_gate_access(uint32_t insn, unsigned *rn,
                                          unsigned *rt, const char **kind);

static uintptr_t current_input_state_object(uint32_t *state_out) {
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  if (state_out) *state_out = UINT32_MAX;
  if (!base) return 0;

  const uintptr_t slot = base + 0x2d37a60;
  if (!memory_range_readable(slot, sizeof(uintptr_t))) return 0;
  const uintptr_t p1 = *(const uintptr_t *)slot;
  if (!p1 || !memory_range_readable(p1, sizeof(uintptr_t))) return 0;
  const uintptr_t p2 = *(const uintptr_t *)p1;
  if (!p2 || !memory_range_readable(p2 + 0x808, sizeof(uintptr_t))) return 0;
  const uintptr_t obj = *(const uintptr_t *)(p2 + 0x808);
  if (obj && state_out && memory_range_readable(obj + 0xe0, sizeof(uint32_t)))
    *state_out = *(const uint32_t *)(obj + 0xe0);
  return obj;
}

uintptr_t state5_driver_entry_continue = 0;
uintptr_t state5_driver_entry_x20 = 0;
uintptr_t state5_driver_block_continue = 0;
uintptr_t state5_tail_gate_call_continue = 0;
uintptr_t state5_tail_gate_skip_continue = 0;
uintptr_t level_module_gate_store_trampoline = 0;
uintptr_t level_module_init_call_target = 0;
uintptr_t level_module_init_stack_helper = 0;
uintptr_t level_module_init_call_continue = 0;
uintptr_t module_handle_trace_active_find_continue = 0;
uintptr_t module_handle_trace_active_collect_continue = 0;

static uintptr_t g_module_init_trace_init_manager;
static unsigned g_module_init_trace_init_before70 = 0x100;

void level_module_init_call_observed(void *manager_ptr, void *board_ptr, unsigned stage) {
  const uintptr_t manager = (uintptr_t)manager_ptr;
  const uintptr_t board = (uintptr_t)board_ptr;
  if (!manager || !memory_range_readable(manager, 0x78)) return;
  const unsigned gate70 = *(const uint8_t *)(manager + 0x70);
  const uintptr_t vtable = *(const uintptr_t *)manager;
  uintptr_t attached = 0;
  if (board && memory_range_readable(board + 0x3d0, sizeof(uintptr_t)))
    attached = *(const uintptr_t *)(board + 0x3d0);
  if (stage == 0) {
    g_module_init_trace_init_manager = manager;
    g_module_init_trace_init_before70 = gate70;
  }
  debugPrintf("module_init_trace LEVEL MODULE INIT CALL: stage=%s manager=%p board=%p board+0x3d0=%p match=%d vtable=%p +0x70=%u",
              stage ? "after" : "before", manager_ptr, board_ptr, (void *)attached,
              attached == manager, (void *)vtable, gate70);
  if (stage && g_module_init_trace_init_manager == manager)
    debugPrintf(" before70=%u delta=%d", g_module_init_trace_init_before70,
                (int)gate70 - (int)g_module_init_trace_init_before70);
  debugPrintf(" frame=%llu\n", (unsigned long long)g_frame_count);
}

void level_module_ctor_complete_observed(void *manager_ptr, uintptr_t caller_lr) {
  const uintptr_t manager = (uintptr_t)manager_ptr;
  if (!manager || !memory_range_readable(manager, 0x78)) return;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t vtable = *(const uintptr_t *)manager;
  const unsigned gate70 = *(const uint8_t *)(manager + 0x70);
  const int caller_in_game = caller_lr >= base && caller_lr < base + game_mod.load_size;

  /* Refresh an existing slot when allocator reuse gives the same address. */
  module_manager_traceLevelManager *slot = NULL;
  for (unsigned i = 0; i < module_manager_trace_MAX_LEVEL_MANAGERS; ++i) {
    if (g_module_manager_trace_level_managers[i].ptr == manager) { slot = &g_module_manager_trace_level_managers[i]; break; }
  }
  if (!slot) {
    slot = &g_module_manager_trace_level_managers[g_module_manager_trace_level_manager_next++ % module_manager_trace_MAX_LEVEL_MANAGERS];
  }
  *slot = (module_manager_traceLevelManager){
      .ptr = manager, .last70 = gate70, .valid = 1, .attached = 0,
      .born_frame = g_frame_count, .last_frame = g_frame_count};

  debugPrintf("module_manager_trace LEVEL MODULE CTOR COMPLETE: manager=%p vtable=%p +0x70=%u "
              "caller=%p%s0x%lx frame=%llu\n",
              manager_ptr, (void *)vtable, gate70, (void *)caller_lr,
              caller_in_game ? " +" : " outside+",
              caller_in_game ? (unsigned long)(caller_lr - base) : 0UL,
              (unsigned long long)g_frame_count);
}


static uintptr_t g_module_factory_trace_factory_callers[8];
static unsigned g_module_factory_trace_factory_caller_count;

static void module_factory_trace_dump_runtime_code(uintptr_t caller_off) {
  const uintptr_t base=(uintptr_t)game_mod.load_virtbase;
  if (!base || caller_off>=game_mod.load_size) return;
  uintptr_t start=caller_off>0x40?caller_off-0x40:0;
  uintptr_t end=caller_off+0x180;
  if (end>game_mod.load_size) end=game_mod.load_size;
  debugPrintf("module_factory_trace FACTORY CALLER RAW +0x%lx:\n",(unsigned long)caller_off);
  for (uintptr_t off=start; off+4<=end; off+=4) {
    uintptr_t pc=base+off;
    if (!memory_range_readable(pc,4)) break;
    debugPrintf("  %c+0x%lx: %08x\n", off==caller_off?'>':' ',
                (unsigned long)off, *(const uint32_t *)pc);
  }
}

void level_module_factory_return_observed(void *manager_ptr, uintptr_t caller_lr) {
  const uintptr_t manager=(uintptr_t)manager_ptr;
  const uintptr_t base=(uintptr_t)game_mod.load_virtbase;
  if (!manager || !base || !memory_range_readable(manager,0x78)) return;
  const unsigned gate70=*(const uint8_t *)(manager+0x70);
  const int in_game=caller_lr>=base && caller_lr<base+game_mod.load_size;
  const uintptr_t off=in_game?caller_lr-base:0;
  debugPrintf("module_factory_trace LEVEL MODULE FACTORY RETURN: manager=%p +0x70=%u caller=%p%s0x%lx frame=%llu\n",
              manager_ptr,gate70,(void *)caller_lr,in_game?" +":" outside+",
              in_game?(unsigned long)off:0UL,(unsigned long long)g_frame_count);
  if (!in_game) return;
  for (unsigned i=0;i<g_module_factory_trace_factory_caller_count;i++)
    if (g_module_factory_trace_factory_callers[i]==off) return;
  if (g_module_factory_trace_factory_caller_count<8) g_module_factory_trace_factory_callers[g_module_factory_trace_factory_caller_count++]=off;
  module_factory_trace_dump_runtime_code(off);
  module_factory_trace_runtime_scan_factory_flow(off);
}

void state5_driver_observed(void *object_ptr, uintptr_t caller_lr, uint32_t stage) {
  const uintptr_t object = (uintptr_t)object_ptr;
  uint32_t global_state = UINT32_MAX;
  const uintptr_t global_object = current_input_state_object(&global_state);
  const uint32_t object_state = object &&
      memory_range_readable(object + 0xe0, sizeof(uint32_t))
      ? *(const uint32_t *)(object + 0xe0) : UINT32_MAX;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  const int caller_in_game = caller_lr >= base && caller_lr < base + game_mod.load_size;
  debugPrintf("state5_trace STATE5 DRIVER %s: object=%p state=%u global=%d global_state=%u caller=%p%s0x%lx frame=%llu\n",
              stage == 0 ? "ENTRY +0x151CC28" : "BLOCK +0x151CC70",
              object_ptr, object_state, object == global_object, global_state,
              (void *)caller_lr, caller_in_game ? " +" : " outside+",
              caller_in_game ? (unsigned long)(caller_lr - base) : 0UL,
              (unsigned long long)g_frame_count);
}

void input_state_request_observed(void *object_ptr, uint32_t requested,
                                  uintptr_t caller_lr) {
  (void)object_ptr;
  (void)requested;
  (void)caller_lr;
}

/* The real LevelModuleManager container layout uses
 * by this path. manager+0x08 is a vector of 64-bit generational engine
 * handles. manager+0x20 is a libc++ std::vector<std::string> containing the
 * resource-group names expanded for the level. Log both accurately; do not
 * treat either container as a vector of object pointers. */
typedef struct {
  uintptr_t manager;
  uint32_t state_mask;
} module_inventory_traceInventorySeen;

static int module_inventory_trace_read_itanium_type(uintptr_t object, uintptr_t *vtable_out,
                                   char *name, size_t name_size) {
  if (vtable_out) *vtable_out = 0;
  if (name && name_size) name[0] = 0;
  if (!object || !memory_range_readable(object, sizeof(uintptr_t))) return 0;
  const uintptr_t vtable = *(const uintptr_t *)object;
  if (!vtable || vtable < 16 || !memory_range_readable(vtable - 16, 24)) return 0;
  if (vtable_out) *vtable_out = vtable;
  const uintptr_t typeinfo = *(const uintptr_t *)(vtable - 8);
  if (!typeinfo || !memory_range_readable(typeinfo + 8, sizeof(uintptr_t))) return 0;
  const uintptr_t name_ptr = *(const uintptr_t *)(typeinfo + 8);
  if (!name_ptr || !name || name_size < 2 || !memory_range_readable(name_ptr, 1)) return 1;
  size_t i = 0;
  for (; i + 1 < name_size; ++i) {
    if (!memory_range_readable(name_ptr + i, 1)) break;
    const char c = *(const char *)(name_ptr + i);
    name[i] = c;
    if (!c) return 1;
  }
  name[i < name_size ? i : name_size - 1] = 0;
  return 1;
}

typedef struct {
  uintptr_t manager;
  uintptr_t handle;
  uintptr_t object;
  unsigned source;
} module_handle_traceResolvedSeen;
static module_handle_traceResolvedSeen g_module_handle_trace_resolved_seen[96];
static unsigned g_module_handle_trace_resolved_seen_count;

void module_handle_trace_active_handle_resolved_observed(void *object_ptr, void *manager_ptr,
                                           uintptr_t index, unsigned source) {
  const uintptr_t object = (uintptr_t)object_ptr;
  const uintptr_t manager = (uintptr_t)manager_ptr;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  if (!base || !object || !manager || !memory_range_readable(manager, 0x78)) return;
  if (*(const uintptr_t *)manager != base + 0x2b4e018) return;

  const uintptr_t begin = *(const uintptr_t *)(manager + 0x08);
  const uintptr_t end = *(const uintptr_t *)(manager + 0x10);
  if (!begin || end < begin || (end - begin) % sizeof(uintptr_t)) return;
  const size_t count = (size_t)((end - begin) / sizeof(uintptr_t));
  if (index >= count || !memory_range_readable(begin + index * sizeof(uintptr_t), sizeof(uintptr_t))) return;
  const uintptr_t handle = *(const uintptr_t *)(begin + index * sizeof(uintptr_t));

  for (unsigned i = 0; i < g_module_handle_trace_resolved_seen_count; ++i) {
    const module_handle_traceResolvedSeen *r = &g_module_handle_trace_resolved_seen[i];
    if (r->manager == manager && r->handle == handle && r->object == object &&
        r->source == source) return;
  }
  if (g_module_handle_trace_resolved_seen_count < sizeof(g_module_handle_trace_resolved_seen)/sizeof(g_module_handle_trace_resolved_seen[0])) {
    g_module_handle_trace_resolved_seen[g_module_handle_trace_resolved_seen_count++] =
        (module_handle_traceResolvedSeen){ manager, handle, object, source };
  }

  uintptr_t vtable = 0;
  char type_name[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(object, &vtable, type_name, sizeof(type_name));
  if (type_ok) seedbank_trace_note_resolved_module(object, type_name);
  uintptr_t slot20 = 0, slot68 = 0, slot70 = 0, slot78 = 0;
  if (vtable && memory_range_readable(vtable + 0x80, sizeof(uintptr_t))) {
    slot20 = *(const uintptr_t *)(vtable + 0x20);
    slot68 = *(const uintptr_t *)(vtable + 0x68);
    slot70 = *(const uintptr_t *)(vtable + 0x70);
    slot78 = *(const uintptr_t *)(vtable + 0x78);
  }

  uint32_t state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&state);
  uintptr_t board_manager = 0;
  if (board && memory_range_readable(board + 0x3d0, sizeof(uintptr_t)))
    board_manager = *(const uintptr_t *)(board + 0x3d0);

  debugPrintf("module_handle_trace ACTIVE HANDLE RESOLVE: source=%s manager=%p attached=%d board=%p state=%u "
              "index=%lu handle=0x%016lx tag=0x%04lx generation=%lu slot=%lu "
              "object=%p vtable=%p type_ok=%d type=%s "
              "slot20=%p%s0x%lx slot68=%p%s0x%lx slot70=%p%s0x%lx slot78=%p%s0x%lx frame=%llu\n",
              source ? "collect" : "find", manager_ptr, board_manager == manager,
              (void *)board, state, (unsigned long)index, (unsigned long)handle,
              (unsigned long)((handle >> 48) & 0xffffu),
              (unsigned long)((handle >> 16) & 0xffffu),
              (unsigned long)(handle & 0xffffu), object_ptr, (void *)vtable,
              type_ok, type_name[0] ? type_name : "<unknown>",
              (void *)slot20, (slot20 >= base && slot20 < base + game_mod.load_size) ? " +" : " outside+",
              (slot20 >= base && slot20 < base + game_mod.load_size) ? (unsigned long)(slot20 - base) : 0UL,
              (void *)slot68, (slot68 >= base && slot68 < base + game_mod.load_size) ? " +" : " outside+",
              (slot68 >= base && slot68 < base + game_mod.load_size) ? (unsigned long)(slot68 - base) : 0UL,
              (void *)slot70, (slot70 >= base && slot70 < base + game_mod.load_size) ? " +" : " outside+",
              (slot70 >= base && slot70 < base + game_mod.load_size) ? (unsigned long)(slot70 - base) : 0UL,
              (void *)slot78, (slot78 >= base && slot78 < base + game_mod.load_size) ? " +" : " outside+",
              (slot78 >= base && slot78 < base + game_mod.load_size) ? (unsigned long)(slot78 - base) : 0UL,
              (unsigned long long)g_frame_count);
}


static uintptr_t g_seedbank_trace_seedbank_module;
static uintptr_t g_seedbank_trace_tutorial_module;
static unsigned char g_seedbank_trace_seedbank_state1[0x200];
static unsigned char g_seedbank_trace_seedbank_pre_state2[0x200];
static int g_seedbank_trace_seedbank_state1_valid;
static int g_seedbank_trace_seedbank_pre_state2_valid;
static unsigned g_seedbank_trace_seedbank_snapshot_count;

static void seedbank_trace_note_resolved_module(uintptr_t object, const char *type_name) {
  if (!object || !type_name) return;
  if (!strcmp(type_name, "14SeedBankModule")) g_seedbank_trace_seedbank_module = object;
  if (!strcmp(type_name, "14TutorialLevel1")) g_seedbank_trace_tutorial_module = object;
}

static void seedbank_trace_seedbank_pointer_inventory(uintptr_t module, const char *reason) {
  if (!module || !reason || !memory_range_readable(module, 0x200)) return;
  unsigned found = 0;
  for (unsigned off = 0; off < 0x200; off += sizeof(uintptr_t)) {
    const uintptr_t candidate = *(const uintptr_t *)(module + off);
    if (!candidate || (candidate & (sizeof(uintptr_t) - 1)) ||
        !memory_range_readable(candidate, sizeof(uintptr_t))) continue;
    uintptr_t vtable = 0;
    char type_name[192] = {0};
    if (!module_inventory_trace_read_itanium_type(candidate, &vtable, type_name, sizeof(type_name))) continue;
    debugPrintf("seedbank_trace SEEDBANK PTR: reason=%s module=%p off=+0x%x ptr=%p type=%s vtable=%p frame=%llu\n",
                reason, (void *)module, off, (void *)candidate,
                type_name[0] ? type_name : "<unknown>", (void *)vtable,
                (unsigned long long)g_frame_count);
    if (++found >= 48) break;
  }
  debugPrintf("seedbank_trace SEEDBANK PTR SUMMARY: reason=%s module=%p polymorphic_fields=%u frame=%llu\n",
              reason, (void *)module, found, (unsigned long long)g_frame_count);
}

#define TUTORIAL_TRACE_TUTORIAL_MAX_BYTES 0x800
#define TUTORIAL_TRACE_SEED_VECTOR_MAX_BYTES 0x200
#define TUTORIAL_TRACE_GRAPH_MAX 64
#define TUTORIAL_TRACE_GRAPH_SCAN_BYTES 0x180
#define TUTORIAL_TRACE_GRAPH_ROOT_SCAN_BYTES 0x800
#define TUTORIAL_TRACE_GRAPH_SNAPSHOT_BYTES 0x100
#define TUTORIAL_TRACE_TOUCH_RING 128
#define TUTORIAL_TRACE_TOUCH_DESCRIPTOR_QWORDS 8

typedef struct {
  uintptr_t object;
  uintptr_t parent;
  unsigned parent_off;
  unsigned depth;
  char type_name[80];
  size_t snapshot_bytes;
  unsigned char baseline[TUTORIAL_TRACE_GRAPH_SNAPSHOT_BYTES];
} TutorialTraceGraphNode;

typedef struct {
  unsigned long long seq;
  unsigned long long frame;
  char op;
  uintptr_t handler;
  uintptr_t descriptor;
  uintptr_t flags;
  uintptr_t x3;
  uintptr_t owner;
  uintptr_t owner_vtable;
  uintptr_t interface_vtable;
  uintptr_t caller;
  char owner_type[80];
  uintptr_t descriptor_q[TUTORIAL_TRACE_TOUCH_DESCRIPTOR_QWORDS];
} TutorialTraceTouchEvent;

static uintptr_t g_tutorial_trace_tutorial;
static unsigned long long g_tutorial_trace_state1_frame;
static unsigned long long g_tutorial_trace_state2_frame;
static unsigned g_tutorial_trace_state2_milestone_mask;
static unsigned char g_tutorial_trace_tutorial_state1[TUTORIAL_TRACE_TUTORIAL_MAX_BYTES];
static unsigned char g_tutorial_trace_tutorial_state2[TUTORIAL_TRACE_TUTORIAL_MAX_BYTES];
static size_t g_tutorial_trace_tutorial_state1_bytes;
static size_t g_tutorial_trace_tutorial_state2_bytes;
static TutorialTraceGraphNode g_tutorial_trace_graph_base[TUTORIAL_TRACE_GRAPH_MAX];
static TutorialTraceGraphNode g_tutorial_trace_graph_now[TUTORIAL_TRACE_GRAPH_MAX];
static unsigned g_tutorial_trace_graph_base_count;
static uintptr_t g_tutorial_trace_seed_vector_begin;
static uintptr_t g_tutorial_trace_seed_vector_end;
static uintptr_t g_tutorial_trace_seed_vector_cap;
static size_t g_tutorial_trace_seed_vector_bytes;
static unsigned char g_tutorial_trace_seed_vector_baseline[TUTORIAL_TRACE_SEED_VECTOR_MAX_BYTES];
static int g_tutorial_trace_seed_vector_valid;
static TutorialTraceTouchEvent g_tutorial_trace_touch_ring[TUTORIAL_TRACE_TOUCH_RING];
static unsigned long long g_tutorial_trace_touch_seq;
static unsigned long long g_tutorial_trace_touch_dump_seq;
static uintptr_t g_tutorial_trace_seedpacket_updater;

static void tutorial_trace_action_milestone(const char *label);

static size_t tutorial_trace_allocation_remaining(uintptr_t object, size_t cap,
                                         uintptr_t *base_out,
                                         size_t *allocation_size_out) {
  if (base_out) *base_out = 0;
  if (allocation_size_out) *allocation_size_out = 0;
  if (!object || !cap) return 0;
  Pvz2AllocTraceInfo info;
  if (!pvz2_alloc_trace_lookup((const void *)object, &info) ||
      object < info.base || object >= info.base + info.size)
    return 0;
  const size_t remaining = info.size - (size_t)(object - info.base);
  if (base_out) *base_out = info.base;
  if (allocation_size_out) *allocation_size_out = info.size;
  return remaining < cap ? remaining : cap;
}

static unsigned tutorial_trace_diff_bytes(const char *kind, const char *label,
                                 uintptr_t object,
                                 const unsigned char *baseline,
                                 size_t baseline_bytes,
                                 unsigned max_details) {
  if (!kind || !label || !object || !baseline || !baseline_bytes) return 0;
  size_t now_bytes = tutorial_trace_allocation_remaining(object, baseline_bytes, NULL, NULL);
  if (!now_bytes) {
    debugPrintf("TUTORIAL_TRACE %s UNAVAILABLE: label=%s object=%p baseline_bytes=0x%zx frame=%llu\n",
                kind, label, (void *)object, baseline_bytes,
                (unsigned long long)g_frame_count);
    return 0;
  }
  const size_t bytes = now_bytes < baseline_bytes ? now_bytes : baseline_bytes;
  unsigned changed = 0, shown = 0;
  for (size_t off = 0; off + sizeof(uintptr_t) <= bytes; off += sizeof(uintptr_t)) {
    uintptr_t before = 0, now = 0;
    memcpy(&before, baseline + off, sizeof(before));
    memcpy(&now, (const void *)(object + off), sizeof(now));
    if (before == now) continue;
    ++changed;
    if (shown < max_details) {
      debugPrintf("TUTORIAL_TRACE %s DIFF: label=%s object=%p off=+0x%zx before=0x%016lx now=0x%016lx frame=%llu\n",
                  kind, label, (void *)object, off, (unsigned long)before,
                  (unsigned long)now, (unsigned long long)g_frame_count);
      ++shown;
    }
  }
  debugPrintf("TUTORIAL_TRACE %s DIFF SUMMARY: label=%s object=%p bytes=0x%zx changed_qwords=%u shown=%u frame=%llu\n",
              kind, label, (void *)object, bytes, changed, shown,
              (unsigned long long)g_frame_count);
  return changed;
}

static void tutorial_trace_capture_tutorial_state1(uintptr_t tutorial) {
  uintptr_t alloc_base = 0;
  size_t alloc_size = 0;
  const size_t bytes = tutorial_trace_allocation_remaining(tutorial, TUTORIAL_TRACE_TUTORIAL_MAX_BYTES,
                                                   &alloc_base, &alloc_size);
  g_tutorial_trace_tutorial_state1_bytes = 0;
  if (!bytes) {
    debugPrintf("TUTORIAL_TRACE TUTORIAL BASELINE: object=%p allocation=untracked frame=%llu\n",
                (void *)tutorial, (unsigned long long)g_frame_count);
    return;
  }
  memcpy(g_tutorial_trace_tutorial_state1, (const void *)tutorial, bytes);
  g_tutorial_trace_tutorial_state1_bytes = bytes;
  debugPrintf("TUTORIAL_TRACE TUTORIAL BASELINE: object=%p alloc_base=%p alloc_size=0x%zx object_offset=0x%lx bytes=0x%zx frame=%llu\n",
              (void *)tutorial, (void *)alloc_base, alloc_size,
              (unsigned long)(tutorial - alloc_base), bytes,
              (unsigned long long)g_frame_count);
}

static void tutorial_trace_capture_tutorial_state2(uintptr_t tutorial, const char *label) {
  if (g_tutorial_trace_tutorial_state1_bytes)
    tutorial_trace_diff_bytes("TUTORIAL STATE1", label, tutorial,
                     g_tutorial_trace_tutorial_state1, g_tutorial_trace_tutorial_state1_bytes, 48);
  const size_t bytes = tutorial_trace_allocation_remaining(tutorial, TUTORIAL_TRACE_TUTORIAL_MAX_BYTES,
                                                   NULL, NULL);
  g_tutorial_trace_tutorial_state2_bytes = 0;
  if (bytes) {
    memcpy(g_tutorial_trace_tutorial_state2, (const void *)tutorial, bytes);
    g_tutorial_trace_tutorial_state2_bytes = bytes;
    debugPrintf("TUTORIAL_TRACE TUTORIAL STATE2 BASELINE: label=%s object=%p bytes=0x%zx frame=%llu\n",
                label, (void *)tutorial, bytes,
                (unsigned long long)g_frame_count);
  }
}

static int tutorial_trace_graph_find(const TutorialTraceGraphNode *nodes, unsigned count,
                            uintptr_t object) {
  for (unsigned i = 0; i < count; ++i)
    if (nodes[i].object == object) return (int)i;
  return -1;
}

static int tutorial_trace_graph_add(TutorialTraceGraphNode *nodes, unsigned *count,
                           uintptr_t object, uintptr_t parent,
                           unsigned parent_off, unsigned depth,
                           const char *forced_name) {
  if (!nodes || !count || !object || *count >= TUTORIAL_TRACE_GRAPH_MAX ||
      tutorial_trace_graph_find(nodes, *count, object) >= 0)
    return 0;
  uintptr_t vtable = 0;
  char type_name[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(object, &vtable,
                                               type_name, sizeof(type_name));
  if (!forced_name && (!type_ok || !type_name[0])) return 0;
  TutorialTraceGraphNode *n = &nodes[(*count)++];
  memset(n, 0, sizeof(*n));
  n->object = object;
  n->parent = parent;
  n->parent_off = parent_off;
  n->depth = depth;
  snprintf(n->type_name, sizeof(n->type_name), "%s",
           (type_ok && type_name[0]) ? type_name : forced_name);
  return 1;
}

static unsigned tutorial_trace_graph_discover(TutorialTraceGraphNode *nodes,
                                     uintptr_t tutorial,
                                     uintptr_t seedbank,
                                     uintptr_t board) {
  unsigned count = 0;
  memset(nodes, 0, sizeof(TutorialTraceGraphNode) * TUTORIAL_TRACE_GRAPH_MAX);
  tutorial_trace_graph_add(nodes, &count, tutorial, 0, 0, 0, "TutorialLevel1Root");
  if (seedbank) tutorial_trace_graph_add(nodes, &count, seedbank, 0, 0, 0, "SeedBankModuleRoot");
  if (board) tutorial_trace_graph_add(nodes, &count, board, 0, 0, 0, "BoardRoot");

  const uintptr_t game_base = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t game_end = game_base + game_mod.load_size;
  for (unsigned i = 0; i < count && i < TUTORIAL_TRACE_GRAPH_MAX; ++i) {
    TutorialTraceGraphNode *n = &nodes[i];
    if (n->depth >= 2) continue;
    const size_t scan_cap = n->depth == 0 ? TUTORIAL_TRACE_GRAPH_ROOT_SCAN_BYTES
                                          : TUTORIAL_TRACE_GRAPH_SCAN_BYTES;
    const size_t scan = tutorial_trace_allocation_remaining(n->object, scan_cap, NULL, NULL);
    if (!scan) continue;
    for (unsigned off = 0; off + sizeof(uintptr_t) <= scan; off += sizeof(uintptr_t)) {
      const uintptr_t candidate = *(const uintptr_t *)(n->object + off);
      if (!candidate || (candidate & (sizeof(uintptr_t) - 1)) ||
          candidate == n->object ||
          (candidate >= game_base && candidate < game_end) ||
          !memory_range_readable(candidate, sizeof(uintptr_t)))
        continue;
      tutorial_trace_graph_add(nodes, &count, candidate, n->object, off, n->depth + 1, NULL);
      if (count >= TUTORIAL_TRACE_GRAPH_MAX) break;
    }
  }
  return count;
}

static void tutorial_trace_graph_capture_baseline(uintptr_t tutorial) {
  uint32_t input_state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  g_tutorial_trace_graph_base_count = tutorial_trace_graph_discover(g_tutorial_trace_graph_base, tutorial,
                                                   g_seedbank_trace_seedbank_module, board);
  g_tutorial_trace_seedpacket_updater = 0;
  for (unsigned i = 0; i < g_tutorial_trace_graph_base_count; ++i) {
    TutorialTraceGraphNode *n = &g_tutorial_trace_graph_base[i];
    n->snapshot_bytes = tutorial_trace_allocation_remaining(n->object,
                                                    TUTORIAL_TRACE_GRAPH_SNAPSHOT_BYTES,
                                                    NULL, NULL);
    if (n->snapshot_bytes)
      memcpy(n->baseline, (const void *)n->object, n->snapshot_bytes);
    if (strstr(n->type_name, "SeedPacketUpdaterComp"))
      g_tutorial_trace_seedpacket_updater = n->object;
    debugPrintf("TUTORIAL_TRACE GRAPH BASE: idx=%u depth=%u object=%p type=%s parent=%p parent_off=+0x%x snapshot=0x%zx frame=%llu\n",
                i, n->depth, (void *)n->object, n->type_name,
                (void *)n->parent, n->parent_off, n->snapshot_bytes,
                (unsigned long long)g_frame_count);
  }
  debugPrintf("TUTORIAL_TRACE GRAPH BASE SUMMARY: nodes=%u seedpacket_updater=%p board=%p input_state=%u frame=%llu\n",
              g_tutorial_trace_graph_base_count, (void *)g_tutorial_trace_seedpacket_updater,
              (void *)board, input_state, (unsigned long long)g_frame_count);
}

static void tutorial_trace_graph_milestone(const char *label, uintptr_t tutorial) {
  uint32_t input_state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  const unsigned now_count = tutorial_trace_graph_discover(g_tutorial_trace_graph_now, tutorial,
                                                   g_seedbank_trace_seedbank_module, board);
  unsigned new_nodes = 0, gone_nodes = 0, changed_nodes = 0;
  uintptr_t updater = 0;
  for (unsigned i = 0; i < now_count; ++i) {
    const TutorialTraceGraphNode *n = &g_tutorial_trace_graph_now[i];
    if (strstr(n->type_name, "SeedPacketUpdaterComp")) updater = n->object;
    if (tutorial_trace_graph_find(g_tutorial_trace_graph_base, g_tutorial_trace_graph_base_count, n->object) < 0) {
      ++new_nodes;
      debugPrintf("TUTORIAL_TRACE GRAPH NEW: label=%s depth=%u object=%p type=%s parent=%p parent_off=+0x%x frame=%llu\n",
                  label, n->depth, (void *)n->object, n->type_name,
                  (void *)n->parent, n->parent_off,
                  (unsigned long long)g_frame_count);
    }
  }
  for (unsigned i = 0; i < g_tutorial_trace_graph_base_count; ++i) {
    const TutorialTraceGraphNode *b = &g_tutorial_trace_graph_base[i];
    if (tutorial_trace_graph_find(g_tutorial_trace_graph_now, now_count, b->object) < 0) {
      ++gone_nodes;
      debugPrintf("TUTORIAL_TRACE GRAPH GONE: label=%s object=%p type=%s frame=%llu\n",
                  label, (void *)b->object, b->type_name,
                  (unsigned long long)g_frame_count);
      continue;
    }
    if (b->snapshot_bytes) {
      const unsigned changed = tutorial_trace_diff_bytes("GRAPH", b->type_name,
                                                b->object, b->baseline,
                                                b->snapshot_bytes, 8);
      if (changed) ++changed_nodes;
    }
  }
  if (updater) {
    g_tutorial_trace_seedpacket_updater = updater;
    uintptr_t alloc_base = 0;
    size_t alloc_size = 0;
    const size_t bytes = tutorial_trace_allocation_remaining(updater, 0x200,
                                                     &alloc_base, &alloc_size);
    debugPrintf("TUTORIAL_TRACE SEEDPACKETUPDATER: label=%s object=%p alloc_base=%p alloc_size=0x%zx bytes_visible=0x%zx frame=%llu\n",
                label, (void *)updater, (void *)alloc_base, alloc_size, bytes,
                (unsigned long long)g_frame_count);
  } else {
    debugPrintf("TUTORIAL_TRACE SEEDPACKETUPDATER: label=%s not-reachable-from-tutorial-seedbank-board depth=2 frame=%llu\n",
                label, (unsigned long long)g_frame_count);
  }
  debugPrintf("TUTORIAL_TRACE GRAPH SUMMARY: label=%s baseline=%u current=%u new=%u gone=%u changed=%u board=%p input_state=%u frame=%llu\n",
              label, g_tutorial_trace_graph_base_count, now_count, new_nodes, gone_nodes,
              changed_nodes, (void *)board, input_state,
              (unsigned long long)g_frame_count);
}

static void tutorial_trace_seed_vector_capture(void) {
  g_tutorial_trace_seed_vector_valid = 0;
  const uintptr_t module = g_seedbank_trace_seedbank_module;
  if (!module || !memory_range_readable(module + 0x70, 0x18)) return;
  const uintptr_t begin = *(const uintptr_t *)(module + 0x70);
  const uintptr_t end = *(const uintptr_t *)(module + 0x78);
  const uintptr_t cap = *(const uintptr_t *)(module + 0x80);
  if (!begin || end < begin || cap < end || end - begin > TUTORIAL_TRACE_SEED_VECTOR_MAX_BYTES ||
      !memory_range_readable(begin, (size_t)(end - begin))) {
    debugPrintf("TUTORIAL_TRACE SEED VECTOR BASE: invalid begin=%p end=%p cap=%p frame=%llu\n",
                (void *)begin, (void *)end, (void *)cap,
                (unsigned long long)g_frame_count);
    return;
  }
  g_tutorial_trace_seed_vector_begin = begin;
  g_tutorial_trace_seed_vector_end = end;
  g_tutorial_trace_seed_vector_cap = cap;
  g_tutorial_trace_seed_vector_bytes = (size_t)(end - begin);
  if (g_tutorial_trace_seed_vector_bytes)
    memcpy(g_tutorial_trace_seed_vector_baseline, (const void *)begin,
           g_tutorial_trace_seed_vector_bytes);
  g_tutorial_trace_seed_vector_valid = 1;
  debugPrintf("TUTORIAL_TRACE SEED VECTOR BASE: module=%p begin=%p end=%p cap=%p span=0x%zx frame=%llu\n",
              (void *)module, (void *)begin, (void *)end, (void *)cap,
              g_tutorial_trace_seed_vector_bytes, (unsigned long long)g_frame_count);
  if (g_tutorial_trace_seed_vector_bytes)
    debug_dump_bytes("TUTORIAL_TRACE SEED VECTOR VALID SPAN", begin,
                      g_tutorial_trace_seed_vector_bytes);
}

static void tutorial_trace_seed_vector_milestone(const char *label) {
  const uintptr_t module = g_seedbank_trace_seedbank_module;
  if (!g_tutorial_trace_seed_vector_valid || !module ||
      !memory_range_readable(module + 0x70, 0x18)) return;
  const uintptr_t begin = *(const uintptr_t *)(module + 0x70);
  const uintptr_t end = *(const uintptr_t *)(module + 0x78);
  const uintptr_t cap = *(const uintptr_t *)(module + 0x80);
  if (begin != g_tutorial_trace_seed_vector_begin || end != g_tutorial_trace_seed_vector_end ||
      cap != g_tutorial_trace_seed_vector_cap) {
    debugPrintf("TUTORIAL_TRACE SEED VECTOR PTR CHANGE: label=%s baseline=%p/%p/%p now=%p/%p/%p frame=%llu\n",
                label, (void *)g_tutorial_trace_seed_vector_begin,
                (void *)g_tutorial_trace_seed_vector_end, (void *)g_tutorial_trace_seed_vector_cap,
                (void *)begin, (void *)end, (void *)cap,
                (unsigned long long)g_frame_count);
    return;
  }
  unsigned changed = 0, shown = 0;
  for (size_t off = 0; off < g_tutorial_trace_seed_vector_bytes; ++off) {
    const unsigned char before = g_tutorial_trace_seed_vector_baseline[off];
    const unsigned char now = *(const unsigned char *)(begin + off);
    if (before == now) continue;
    ++changed;
    if (shown < 24) {
      debugPrintf("TUTORIAL_TRACE SEED VECTOR DIFF: label=%s off=+0x%zx before=%02x now=%02x frame=%llu\n",
                  label, off, before, now, (unsigned long long)g_frame_count);
      ++shown;
    }
  }
  debugPrintf("TUTORIAL_TRACE SEED VECTOR SUMMARY: label=%s span=0x%zx changed_bytes=%u shown=%u frame=%llu\n",
              label, g_tutorial_trace_seed_vector_bytes, changed, shown,
              (unsigned long long)g_frame_count);
}

static void tutorial_trace_touch_window_begin(const char *label) {
  g_tutorial_trace_touch_dump_seq = g_tutorial_trace_touch_seq;
  debugPrintf("TUTORIAL_TRACE TOUCH WINDOW BEGIN: label=%s seq=%llu frame=%llu\n",
              label, g_tutorial_trace_touch_dump_seq,
              (unsigned long long)g_frame_count);
}

static void tutorial_trace_touch_dump_new(const char *label) {
  unsigned long long first = g_tutorial_trace_touch_dump_seq + 1;
  const unsigned long long oldest = g_tutorial_trace_touch_seq > TUTORIAL_TRACE_TOUCH_RING
      ? g_tutorial_trace_touch_seq - TUTORIAL_TRACE_TOUCH_RING + 1 : 1;
  if (first < oldest) first = oldest;
  unsigned shown = 0;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t end = base + game_mod.load_size;
  for (unsigned long long seq = first; seq <= g_tutorial_trace_touch_seq; ++seq) {
    const TutorialTraceTouchEvent *e = &g_tutorial_trace_touch_ring[(seq - 1) % TUTORIAL_TRACE_TOUCH_RING];
    if (e->seq != seq) continue;
    debugPrintf("TUTORIAL_TRACE TOUCH WINDOW: label=%s seq=%llu op=%c event_frame=%llu descriptor=%p flags=0x%lx owner=%p type=%s caller=%p q0=%p q1=%p q2=%p q3=%p q4=%p q5=%p q6=%p q7=%p\n",
                label, seq, e->op, e->frame, (void *)e->descriptor,
                (unsigned long)e->flags, (void *)e->owner,
                e->owner_type[0] ? e->owner_type : "<ownerless/unknown>",
                (void *)e->caller,
                (void *)e->descriptor_q[0], (void *)e->descriptor_q[1],
                (void *)e->descriptor_q[2], (void *)e->descriptor_q[3],
                (void *)e->descriptor_q[4], (void *)e->descriptor_q[5],
                (void *)e->descriptor_q[6], (void *)e->descriptor_q[7]);
    for (unsigned q = 0; q < TUTORIAL_TRACE_TOUCH_DESCRIPTOR_QWORDS; ++q) {
      const uintptr_t v = e->descriptor_q[q];
      if (v >= base && v < end)
        debugPrintf("TUTORIAL_TRACE TOUCH CALLBACK: label=%s seq=%llu descriptor_q=%u target=%p +0x%lx\n",
                    label, seq, q, (void *)v, (unsigned long)(v - base));
    }
    ++shown;
  }
  debugPrintf("TUTORIAL_TRACE TOUCH WINDOW SUMMARY: label=%s new_events=%u seq_now=%llu frame=%llu\n",
              label, shown, g_tutorial_trace_touch_seq,
              (unsigned long long)g_frame_count);
  g_tutorial_trace_touch_dump_seq = g_tutorial_trace_touch_seq;
}

static void tutorial_trace_capture_milestone(const char *label, int make_state2_baseline) {
  const uintptr_t tutorial = g_tutorial_trace_tutorial;
  if (!tutorial) return;
  uint32_t internal = UINT32_MAX, input_state = UINT32_MAX;
  if (memory_range_readable(tutorial + 0x48, sizeof(uint32_t)))
    internal = *(const uint32_t *)(tutorial + 0x48);
  const uintptr_t board = current_input_state_object(&input_state);
  const unsigned long long delta = g_tutorial_trace_state2_frame &&
      g_frame_count >= g_tutorial_trace_state2_frame
      ? g_frame_count - g_tutorial_trace_state2_frame : 0;
  debugPrintf("TUTORIAL_TRACE MILESTONE: label=%s tutorial=%p internal=%u board=%p input_state=%u state1_frame=%llu state2_frame=%llu delta=%llu frame=%llu\n",
              label, (void *)tutorial, internal, (void *)board, input_state,
              g_tutorial_trace_state1_frame, g_tutorial_trace_state2_frame, delta,
              (unsigned long long)g_frame_count);
  if (make_state2_baseline)
    tutorial_trace_capture_tutorial_state2(tutorial, label);
  else if (g_tutorial_trace_tutorial_state2_bytes)
    tutorial_trace_diff_bytes("TUTORIAL STATE2", label, tutorial,
                     g_tutorial_trace_tutorial_state2, g_tutorial_trace_tutorial_state2_bytes, 48);
  tutorial_trace_seed_vector_milestone(label);
  tutorial_trace_graph_milestone(label, tutorial);
  tutorial_trace_touch_dump_new(label);
  tutorial_trace_action_milestone(label);
  trigger_touch_draw_capture(2);
  debugLogFlush();
}

static void tutorial_trace_begin_state1(uintptr_t tutorial) {
  g_tutorial_trace_tutorial = tutorial;
  g_tutorial_trace_state1_frame = g_frame_count;
  g_tutorial_trace_state2_frame = 0;
  g_tutorial_trace_state2_milestone_mask = 0;
  g_tutorial_trace_tutorial_state2_bytes = 0;
  debugPrintf("TUTORIAL_TRACE STATE1 BEGIN: tutorial=%p frame=%llu\n",
              (void *)tutorial, (unsigned long long)g_frame_count);
  tutorial_trace_capture_tutorial_state1(tutorial);
  tutorial_trace_seed_vector_capture();
  tutorial_trace_graph_capture_baseline(tutorial);
  tutorial_trace_touch_window_begin("STATE1");
  trigger_touch_draw_capture(2);
  debugLogFlush();
}

static void tutorial_trace_enter_state2(uintptr_t tutorial) {
  if (!tutorial || (g_tutorial_trace_state2_frame && g_tutorial_trace_tutorial == tutorial)) return;
  g_tutorial_trace_tutorial = tutorial;
  g_tutorial_trace_state2_frame = g_frame_count;
  g_tutorial_trace_state2_milestone_mask = 1u;
  debugPrintf("TUTORIAL_TRACE STATE2 ENTER: tutorial=%p frame=%llu\n",
              (void *)tutorial, (unsigned long long)g_frame_count);
  tutorial_trace_capture_milestone("STATE2 +0F", 1);
}


static void seedbank_trace_seedbank_snapshot(const char *reason, unsigned tutorial_state) {
  const uintptr_t module = g_seedbank_trace_seedbank_module;
  if (!module || !reason || g_seedbank_trace_seedbank_snapshot_count >= 8) return;
  uintptr_t vtable = 0;
  char type_name[192] = {0};
  if (!module_inventory_trace_read_itanium_type(module, &vtable, type_name, sizeof(type_name)) ||
      strcmp(type_name, "14SeedBankModule") || !memory_range_readable(module, 0x200)) return;
  ++g_seedbank_trace_seedbank_snapshot_count;

  uint32_t input_state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  debugPrintf("seedbank_trace SEEDBANK SNAPSHOT: reason=%s module=%p vtable=%p tutorial=%p tutorial_state=%u board=%p input_state=%u frame=%llu\n",
              reason, (void *)module, (void *)vtable, (void *)g_seedbank_trace_tutorial_module,
              tutorial_state, (void *)board, input_state,
              (unsigned long long)g_frame_count);
  debug_dump_bytes(reason, module, 0x200);
  seedbank_trace_seedbank_pointer_inventory(module, reason);

  if (tutorial_state == 1 && strstr(reason, "BEFORE STATE1")) {
    memcpy(g_seedbank_trace_seedbank_state1, (const void *)module, sizeof(g_seedbank_trace_seedbank_state1));
    g_seedbank_trace_seedbank_state1_valid = 1;
    debugPrintf("seedbank_trace SEEDBANK BASELINE: captured before-state1 bytes=0x%lx frame=%llu\n",
                (unsigned long)sizeof(g_seedbank_trace_seedbank_state1),
                (unsigned long long)g_frame_count);
  } else if (tutorial_state == 2 && strstr(reason, "BEFORE STATE2")) {
    memcpy(g_seedbank_trace_seedbank_pre_state2, (const void *)module, sizeof(g_seedbank_trace_seedbank_pre_state2));
    g_seedbank_trace_seedbank_pre_state2_valid = 1;
    unsigned changed = 0;
    if (g_seedbank_trace_seedbank_state1_valid) {
      for (unsigned off = 0; off < sizeof(g_seedbank_trace_seedbank_state1); off += sizeof(uintptr_t)) {
        uintptr_t before = 0, now = 0;
        memcpy(&before, g_seedbank_trace_seedbank_state1 + off, sizeof(before));
        memcpy(&now, (const void *)(module + off), sizeof(now));
        if (before == now) continue;
        debugPrintf("seedbank_trace SEEDBANK STATE1 DIFF: off=+0x%x before_state1=0x%016lx before_state2=0x%016lx frame=%llu\n",
                    off, (unsigned long)before, (unsigned long)now,
                    (unsigned long long)g_frame_count);
        if (++changed >= 64) break;
      }
    }
    debugPrintf("seedbank_trace SEEDBANK STATE1 DIFF SUMMARY: changed_qwords=%u frame=%llu\n",
                changed, (unsigned long long)g_frame_count);
  } else if (tutorial_state == 2 && g_seedbank_trace_seedbank_pre_state2_valid) {
    unsigned changed = 0;
    for (unsigned off = 0; off < sizeof(g_seedbank_trace_seedbank_pre_state2); off += sizeof(uintptr_t)) {
      uintptr_t before = 0, now = 0;
      memcpy(&before, g_seedbank_trace_seedbank_pre_state2 + off, sizeof(before));
      memcpy(&now, (const void *)(module + off), sizeof(now));
      if (before == now) continue;
      debugPrintf("seedbank_trace SEEDBANK STATE2 DIFF: off=+0x%x before_state2=0x%016lx now=0x%016lx reason=%s frame=%llu\n",
                  off, (unsigned long)before, (unsigned long)now, reason,
                  (unsigned long long)g_frame_count);
      if (++changed >= 64) break;
    }
    debugPrintf("seedbank_trace SEEDBANK STATE2 DIFF SUMMARY: reason=%s changed_qwords=%u frame=%llu\n",
                reason, changed, (unsigned long long)g_frame_count);
  }
}


typedef struct {
  uintptr_t object;
  uintptr_t caller;
  uint32_t old_state;
  uint32_t requested;
} tutorial_state_traceTutorialStateSeen;
static tutorial_state_traceTutorialStateSeen g_tutorial_state_trace_tutorial_last;
static unsigned g_tutorial_state_trace_tutorial_state_logs;

/* The watchdog/callback delivery path is deliberately disabled.
 * +0x11ED5F8 is the entry action for TutorialLevel1 internal state 11, not
 * the completion of state 1. Calling it from state 1 skipped the seed-bank
 * tutorial setup and started gameplay out of order. */

void tutorial_state_trace_tutorial_state_observed(void *object_ptr, unsigned requested, uintptr_t caller) {
  const uintptr_t object = (uintptr_t)object_ptr;
  uint32_t old_state = UINT32_MAX;
  if (object && memory_range_readable(object + 0x48, sizeof(uint32_t)))
    old_state = *(const uint32_t *)(object + 0x48);
  if (g_tutorial_state_trace_tutorial_state_logs >= 128) return;
  if (g_tutorial_state_trace_tutorial_last.object == object &&
      g_tutorial_state_trace_tutorial_last.caller == caller &&
      g_tutorial_state_trace_tutorial_last.old_state == old_state &&
      g_tutorial_state_trace_tutorial_last.requested == requested)
    return;
  g_tutorial_state_trace_tutorial_last.object = object;
  g_tutorial_state_trace_tutorial_last.caller = caller;
  g_tutorial_state_trace_tutorial_last.old_state = old_state;
  g_tutorial_state_trace_tutorial_last.requested = requested;
  ++g_tutorial_state_trace_tutorial_state_logs;

  uintptr_t vtable = 0;
  char type_name[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(object, &vtable, type_name, sizeof(type_name));
  uint32_t input_state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  const int caller_in_game = caller >= base && caller < base + game_mod.load_size;
  debugPrintf("tutorial_state_trace TUTORIAL INTERNAL STATE: object=%p type_ok=%d type=%s old=%u requested=%u caller=%p%s0x%lx board=%p input_state=%u frame=%llu\n",
              object_ptr, type_ok, type_name[0] ? type_name : "<unknown>",
              old_state, requested, (void *)caller, caller_in_game ? " +" : " outside+",
              caller_in_game ? (unsigned long)(caller - base) : 0UL,
              (void *)board, input_state, (unsigned long long)g_frame_count);

  if (type_ok && !strcmp(type_name, "14TutorialLevel1")) {
    g_seedbank_trace_tutorial_module = object;
    if (old_state == 2 && requested != 2 && g_tutorial_trace_state2_frame)
      tutorial_trace_capture_milestone("STATE2 EXIT REQUEST", 0);
    if (requested == 1) {
      seedbank_trace_seedbank_snapshot("seedbank_trace SEEDBANK BEFORE STATE1 ENTRY", 1);
      tutorial_trace_begin_state1(object);
    }
    if (requested == 2)
      seedbank_trace_seedbank_snapshot("seedbank_trace SEEDBANK BEFORE STATE2 ENTRY", 2);
  }

}

static unsigned g_state1_action_trace_state1_logs;
void state1_action_trace_state1_module_observed(void *module_ptr, void *tutorial_ptr,
                                  void *manager_ptr, unsigned stage,
                                  uintptr_t helper_result) {
  if (g_state1_action_trace_state1_logs >= 32) return;
  ++g_state1_action_trace_state1_logs;
  const uintptr_t module = (uintptr_t)module_ptr;
  const uintptr_t tutorial = (uintptr_t)tutorial_ptr;
  const uintptr_t manager = (uintptr_t)manager_ptr;
  uintptr_t vtable = 0, tutorial_vtable = 0;
  char type_name[192] = {0}, tutorial_type[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(module, &vtable, type_name, sizeof(type_name));
  const int tutorial_type_ok = module_inventory_trace_read_itanium_type(tutorial, &tutorial_vtable,
                                                        tutorial_type, sizeof(tutorial_type));
  uint32_t internal = UINT32_MAX;
  if (tutorial_type_ok && !strcmp(tutorial_type, "14TutorialLevel1") &&
      memory_range_readable(tutorial + 0x48, sizeof(uint32_t)))
    internal = *(const uint32_t *)(tutorial + 0x48);
  uint32_t input_state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  uintptr_t manager_vtable = 0;
  if (manager && memory_range_readable(manager, sizeof(uintptr_t)))
    manager_vtable = *(const uintptr_t *)manager;
  debugPrintf("state1_action_trace STATE1 MODULE ACTION: stage=%s tutorial=%p tutorial_type_ok=%d tutorial_type=%s internal=%u board=%p input_state=%u manager=%p manager_vtable=%p object=%p type_ok=%d type=%s vtable=%p helper_result=%p frame=%llu\n",
              stage ? "after" : "before", tutorial_ptr, tutorial_type_ok,
              tutorial_type[0] ? tutorial_type : "<unknown>", internal,
              (void *)board, input_state, manager_ptr, (void *)manager_vtable,
              module_ptr, type_ok, type_name[0] ? type_name : "<null/unknown>",
              (void *)vtable, (void *)helper_result,
              (unsigned long long)g_frame_count);
}


static uintptr_t g_tutorial_layout_trace_tutorial_ptr;
static unsigned long long g_tutorial_layout_trace_state2_enter_frame;
static unsigned g_tutorial_layout_trace_shared_logs;

static void tutorial_layout_trace_read_tutorial_snapshot(uintptr_t tutorial, uintptr_t out[12]) {
  memset(out, 0, 12 * sizeof(*out));
  if (!tutorial || !memory_range_readable(tutorial + 0x50, 12 * sizeof(uintptr_t))) return;
  for (unsigned i = 0; i < 12; ++i)
    out[i] = *(const uintptr_t *)(tutorial + 0x50 + i * sizeof(uintptr_t));
}

void tutorial_layout_trace_tutorial_shared_observed(void *tutorial_ptr, unsigned stage) {
  if (g_tutorial_layout_trace_shared_logs >= 64) return;
  ++g_tutorial_layout_trace_shared_logs;
  const uintptr_t tutorial = (uintptr_t)tutorial_ptr;
  uintptr_t vtable = 0;
  char type_name[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(tutorial, &vtable, type_name, sizeof(type_name));
  uint32_t internal = UINT32_MAX;
  float f4c = 0.0f;
  if (tutorial && memory_range_readable(tutorial + 0x48, 8)) {
    internal = *(const uint32_t *)(tutorial + 0x48);
    memcpy(&f4c, (const void *)(tutorial + 0x4c), sizeof(f4c));
  }
  if (type_ok && !strcmp(type_name, "14TutorialLevel1") && internal == 2) {
    g_tutorial_layout_trace_tutorial_ptr = tutorial;
    if (!g_tutorial_trace_state2_frame || g_tutorial_trace_tutorial != tutorial) {
      g_tutorial_layout_trace_state2_enter_frame = g_frame_count;
      tutorial_trace_enter_state2(tutorial);
    }
  }
  uint32_t input_state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  uintptr_t snap[12];
  tutorial_layout_trace_read_tutorial_snapshot(tutorial, snap);
  debugPrintf("tutorial_entry_trace TUTORIAL SHARED ENTRY: stage=%s tutorial=%p type_ok=%d type=%s internal=%u f4c=%.6f board=%p input_state=%u q50=%p q58=%p q60=%p q68=%p q70=%p q78=%p q80=%p q88=%p q90=%p q98=%p qa0=%p qa8=%p frame=%llu\n",
              stage ? "after" : "before", tutorial_ptr, type_ok,
              type_name[0] ? type_name : "<unknown>", internal, (double)f4c,
              (void *)board, input_state,
              (void *)snap[0], (void *)snap[1], (void *)snap[2], (void *)snap[3],
              (void *)snap[4], (void *)snap[5], (void *)snap[6], (void *)snap[7],
              (void *)snap[8], (void *)snap[9], (void *)snap[10], (void *)snap[11],
              (unsigned long long)g_frame_count);
}

static unsigned g_tutorial_layout_trace_layout_logs;
void tutorial_layout_trace_state1_layout_observed(void *tutorial_ptr, unsigned selector,
                                  int first, int second) {
  if (g_tutorial_layout_trace_layout_logs >= 16) return;
  ++g_tutorial_layout_trace_layout_logs;
  const uintptr_t tutorial = (uintptr_t)tutorial_ptr;
  uintptr_t vtable = 0;
  char type_name[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(tutorial, &vtable, type_name, sizeof(type_name));
  uint32_t internal = UINT32_MAX, input_state = UINT32_MAX;
  if (tutorial && memory_range_readable(tutorial + 0x48, sizeof(uint32_t)))
    internal = *(const uint32_t *)(tutorial + 0x48);
  const uintptr_t board = current_input_state_object(&input_state);
  debugPrintf("tutorial_entry_trace STATE1 LAYOUT RESULT: selector=%u tutorial=%p type_ok=%d type=%s internal=%u first=%d second=%d board=%p input_state=%u frame=%llu\n",
              selector, tutorial_ptr, type_ok, type_name[0] ? type_name : "<unknown>",
              internal, first, second, (void *)board, input_state,
              (unsigned long long)g_frame_count);
}



static uintptr_t g_state2_action_trace_state2_case_target;
static uintptr_t g_state2_action_trace_state2_action_hook_off;
static uintptr_t g_state2_action_trace_state2_action_call_off;
static unsigned g_state2_action_trace_state2_action_variant;
static uintptr_t g_state2_action_trace_state2_action_object;
static uintptr_t g_state2_action_trace_state2_action_aux;
static uintptr_t g_state2_action_trace_state2_action_object_initial[16];
static uintptr_t g_state2_action_trace_state2_action_aux_initial[16];
static unsigned g_state2_action_trace_state2_action_logs;
static unsigned g_state2_source_trace_state2_source_logs;
static unsigned g_state2_source_trace_action_detail_dumped;
static uintptr_t g_state2_source_trace_vtable_dumped[8];

static void state2_action_trace_snapshot_16q(uintptr_t object, uintptr_t out[16]) {
  memset(out, 0, 16 * sizeof(*out));
  if (!object || !memory_range_readable(object, 16 * sizeof(uintptr_t))) return;
  for (unsigned i = 0; i < 16; ++i)
    out[i] = *(const uintptr_t *)(object + i * sizeof(uintptr_t));
}

static void state2_source_trace_dump_vtable_once(const char *label, uintptr_t object) {
  if (!label || !object) return;
  for (unsigned i = 0; i < sizeof(g_state2_source_trace_vtable_dumped)/sizeof(g_state2_source_trace_vtable_dumped[0]); ++i) {
    if (g_state2_source_trace_vtable_dumped[i] == object) return;
    if (!g_state2_source_trace_vtable_dumped[i]) {
      g_state2_source_trace_vtable_dumped[i] = object;
      break;
    }
  }
  uintptr_t vtable = 0;
  char type_name[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(object, &vtable, type_name, sizeof(type_name));
  debugPrintf("state2_source_trace VTABLE: label=%s object=%p type_ok=%d type=%s vtable=%p\n",
              label, (void *)object, type_ok,
              type_name[0] ? type_name : "<unknown>", (void *)vtable);
  if (!vtable) return;
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  for (unsigned slot = 0; slot < 16; ++slot) {
    if (!memory_range_readable(vtable + slot * sizeof(uintptr_t), sizeof(uintptr_t))) break;
    const uintptr_t target = *(const uintptr_t *)(vtable + slot * sizeof(uintptr_t));
    if (target >= base && target < base + game_mod.load_size)
      debugPrintf("  state2_source_trace VTABLE slot=%u target=%p +0x%lx\n",
                  slot, (void *)target, (unsigned long)(target - base));
    else
      debugPrintf("  state2_source_trace VTABLE slot=%u target=%p outside-libPVZ2\n",
                  slot, (void *)target);
  }
}

void state2_source_trace_state2_source_observed(void *source_ptr, float value) {
  if (g_state2_source_trace_state2_source_logs >= 8) return;
  uint32_t internal = UINT32_MAX;
  if (g_tutorial_layout_trace_tutorial_ptr &&
      memory_range_readable(g_tutorial_layout_trace_tutorial_ptr + 0x48, sizeof(uint32_t)))
    internal = *(const uint32_t *)(g_tutorial_layout_trace_tutorial_ptr + 0x48);
  if (internal != 2) return;
  ++g_state2_source_trace_state2_source_logs;

  const uintptr_t source = (uintptr_t)source_ptr;
  uintptr_t vtable = 0;
  char type_name[192] = {0};
  const int type_ok = module_inventory_trace_read_itanium_type(source, &vtable, type_name, sizeof(type_name));
  float f10 = 0.0f, f14 = 0.0f, f18 = 0.0f, f1c = 0.0f;
  uintptr_t q0 = 0, q8 = 0, q10 = 0, q18 = 0, q20 = 0, q28 = 0;
  if (source && memory_range_readable(source, 0x30)) {
    q0 = *(const uintptr_t *)(source + 0x00);
    q8 = *(const uintptr_t *)(source + 0x08);
    q10 = *(const uintptr_t *)(source + 0x10);
    q18 = *(const uintptr_t *)(source + 0x18);
    q20 = *(const uintptr_t *)(source + 0x20);
    q28 = *(const uintptr_t *)(source + 0x28);
    memcpy(&f10, (const void *)(source + 0x10), sizeof(f10));
    memcpy(&f14, (const void *)(source + 0x14), sizeof(f14));
    memcpy(&f18, (const void *)(source + 0x18), sizeof(f18));
    memcpy(&f1c, (const void *)(source + 0x1c), sizeof(f1c));
  }
  uint32_t input_state = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  debugPrintf("state2_source_trace STATE2 S8 SOURCE: source=%p type_ok=%d type=%s vtable=%p s8=%.9f f10=%.9f f14=%.9f f18=%.9f f1c=%.9f q0=%p q8=%p q10=%p q18=%p q20=%p q28=%p tutorial=%p internal=%u board=%p input_state=%u frame=%llu\n",
              source_ptr, type_ok, type_name[0] ? type_name : "<unknown>",
              (void *)vtable, (double)value, (double)f10, (double)f14,
              (double)f18, (double)f1c, (void *)q0, (void *)q8,
              (void *)q10, (void *)q18, (void *)q20, (void *)q28,
              (void *)g_tutorial_layout_trace_tutorial_ptr, internal, (void *)board, input_state,
              (unsigned long long)g_frame_count);
  if (g_state2_source_trace_state2_source_logs == 1) {
    if (source && memory_range_readable(source, 0x100))
      debug_dump_bytes("state2_source_trace STATE2 S8 SOURCE", source, 0x100);
    state2_source_trace_dump_vtable_once("state2-s8-source", source);
  }
}

void state2_action_trace_state2_action_observed(void *object_ptr, void *aux_ptr,
                                  void *param_ptr, float duration) {
  if (g_state2_action_trace_state2_action_logs >= 24) return;
  ++g_state2_action_trace_state2_action_logs;
  const uintptr_t object = (uintptr_t)object_ptr;
  const uintptr_t aux = (uintptr_t)aux_ptr;
  const uintptr_t param = (uintptr_t)param_ptr;
  uintptr_t object_vtable = 0, aux_vtable = 0;
  char object_type[192] = {0}, aux_type[192] = {0};
  const int object_type_ok = module_inventory_trace_read_itanium_type(object, &object_vtable,
                                                      object_type, sizeof(object_type));
  const int aux_type_ok = module_inventory_trace_read_itanium_type(aux, &aux_vtable,
                                                   aux_type, sizeof(aux_type));
  uintptr_t params[8] = {0};
  if (param && memory_range_readable(param, sizeof(params)))
    memcpy(params, (const void *)param, sizeof(params));
  uint32_t input_state = UINT32_MAX, internal = UINT32_MAX;
  const uintptr_t board = current_input_state_object(&input_state);
  if (g_tutorial_layout_trace_tutorial_ptr && memory_range_readable(g_tutorial_layout_trace_tutorial_ptr + 0x48, sizeof(uint32_t)))
    internal = *(const uint32_t *)(g_tutorial_layout_trace_tutorial_ptr + 0x48);

  debugPrintf("state2_action_trace STATE2 ACTION: case=+0x%lx hook=+0x%lx call=+0x%lx variant=%u tutorial=%p internal=%u board=%p input_state=%u object=%p type_ok=%d type=%s vtable=%p aux=%p aux_type_ok=%d aux_type=%s aux_vtable=%p param=%p duration=%.9f p0=%p p1=%p p2=%p p3=%p p4=%p p5=%p p6=%p p7=%p frame=%llu\n",
              (unsigned long)g_state2_action_trace_state2_case_target,
              (unsigned long)g_state2_action_trace_state2_action_hook_off,
              (unsigned long)g_state2_action_trace_state2_action_call_off,
              g_state2_action_trace_state2_action_variant,
              (void *)g_tutorial_layout_trace_tutorial_ptr, internal, (void *)board, input_state,
              object_ptr, object_type_ok, object_type[0] ? object_type : "<unknown>",
              (void *)object_vtable, aux_ptr, aux_type_ok,
              aux_type[0] ? aux_type : "<unknown>", (void *)aux_vtable,
              param_ptr, (double)duration,
              (void *)params[0], (void *)params[1], (void *)params[2], (void *)params[3],
              (void *)params[4], (void *)params[5], (void *)params[6], (void *)params[7],
              (unsigned long long)g_frame_count);

  if (!g_state2_source_trace_action_detail_dumped) {
    g_state2_source_trace_action_detail_dumped = 1;
    const uintptr_t p3 = params[3];
    uintptr_t p3_vtable = 0;
    char p3_type[192] = {0};
    const int p3_type_ok = module_inventory_trace_read_itanium_type(p3, &p3_vtable,
                                                   p3_type, sizeof(p3_type));
    debugPrintf("state2_source_trace STATE2 ACTION DETAIL: animation_mgr=%p show_advice=%p param=%p p3=%p p3_type_ok=%d p3_type=%s p3_vtable=%p duration=%.9f frame=%llu\n",
                object_ptr, aux_ptr, param_ptr, (void *)p3, p3_type_ok,
                p3_type[0] ? p3_type : "<unknown>", (void *)p3_vtable,
                (double)duration, (unsigned long long)g_frame_count);
    state2_source_trace_dump_vtable_once("AnimationMgr", object);
    state2_source_trace_dump_vtable_once("ShowAdvice", aux);
    state2_source_trace_dump_vtable_once("state2-param-p3", p3);
    if (aux && memory_range_readable(aux, 0x100))
      debug_dump_bytes("state2_source_trace SHOWADVICE OBJECT", aux, 0x100);
    if (param && memory_range_readable(param, 0x80))
      debug_dump_bytes("state2_source_trace STATE2 ACTION PARAM", param, 0x80);
    if (p3 && memory_range_readable(p3, 0x100))
      debug_dump_bytes("state2_source_trace STATE2 ACTION P3", p3, 0x100);
  }

  if (!g_state2_action_trace_state2_action_object && object) {
    g_state2_action_trace_state2_action_object = object;
    g_state2_action_trace_state2_action_aux = aux;
    state2_action_trace_snapshot_16q(object, g_state2_action_trace_state2_action_object_initial);
    state2_action_trace_snapshot_16q(aux, g_state2_action_trace_state2_action_aux_initial);
  }
}



static void tutorial_trace_action_milestone(const char *label) {
  if (!label) return;
  if (!g_state2_action_trace_state2_action_object) {
    debugPrintf("TUTORIAL_TRACE ACTION: label=%s state2-action-not-observed-yet frame=%llu\n",
                label, (unsigned long long)g_frame_count);
    return;
  }
  uintptr_t now_obj[16], now_aux[16];
  state2_action_trace_snapshot_16q(g_state2_action_trace_state2_action_object, now_obj);
  state2_action_trace_snapshot_16q(g_state2_action_trace_state2_action_aux, now_aux);
  unsigned object_changed = 0, aux_changed = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (now_obj[i] != g_state2_action_trace_state2_action_object_initial[i]) object_changed |= 1u << i;
    if (now_aux[i] != g_state2_action_trace_state2_action_aux_initial[i]) aux_changed |= 1u << i;
  }
  uintptr_t object_vtable = 0, aux_vtable = 0;
  char object_type[192] = {0}, aux_type[192] = {0};
  const int object_ok = module_inventory_trace_read_itanium_type(g_state2_action_trace_state2_action_object,
                                                 &object_vtable,
                                                 object_type, sizeof(object_type));
  const int aux_ok = module_inventory_trace_read_itanium_type(g_state2_action_trace_state2_action_aux,
                                              &aux_vtable,
                                              aux_type, sizeof(aux_type));
  debugPrintf("TUTORIAL_TRACE ACTION: label=%s object=%p type=%s changed_mask=0x%04x aux=%p aux_type=%s aux_changed_mask=0x%04x frame=%llu\n",
              label, (void *)g_state2_action_trace_state2_action_object,
              object_ok && object_type[0] ? object_type : "<unknown>",
              object_changed, (void *)g_state2_action_trace_state2_action_aux,
              aux_ok && aux_type[0] ? aux_type : "<unknown>",
              aux_changed, (unsigned long long)g_frame_count);
  for (unsigned i = 0; i < 16; ++i) {
    if (now_obj[i] != g_state2_action_trace_state2_action_object_initial[i])
      debugPrintf("TUTORIAL_TRACE ACTION OBJECT DIFF: label=%s q=%u before=%p now=%p frame=%llu\n",
                  label, i, (void *)g_state2_action_trace_state2_action_object_initial[i],
                  (void *)now_obj[i], (unsigned long long)g_frame_count);
    if (now_aux[i] != g_state2_action_trace_state2_action_aux_initial[i])
      debugPrintf("TUTORIAL_TRACE ACTION AUX DIFF: label=%s q=%u before=%p now=%p frame=%llu\n",
                  label, i, (void *)g_state2_action_trace_state2_action_aux_initial[i],
                  (void *)now_aux[i], (unsigned long long)g_frame_count);
  }
}

void state5_tail_gate_observed(void *board_ptr, void *gate_ptr) {
  const uintptr_t board = (uintptr_t)board_ptr;
  const uintptr_t gate = (uintptr_t)gate_ptr;
  if (gate) g_level_module_manager = gate;
  uint32_t global_state = UINT32_MAX;
  const uintptr_t global_object = current_input_state_object(&global_state);
  const uint32_t state = board && memory_range_readable(board + 0xe0, sizeof(uint32_t))
      ? *(const uint32_t *)(board + 0xe0) : UINT32_MAX;
  uintptr_t board_gate = 0;
  if (board && memory_range_readable(board + 0x3d0, sizeof(uintptr_t)))
    board_gate = *(const uintptr_t *)(board + 0x3d0);
  unsigned gate70 = 0xff;
  if (gate && memory_range_readable(gate + 0x70, 1))
    gate70 = *(const uint8_t *)(gate + 0x70);

  static uintptr_t last_board, last_gate;
  static uint32_t last_state = UINT32_MAX;
  static unsigned last_gate70 = 0x100;
  static unsigned long long last_frame;
  const int same = board == last_board && gate == last_gate &&
      state == last_state && gate70 == last_gate70;
  if (same && g_frame_count - last_frame < 120) return;
  last_board = board; last_gate = gate; last_state = state;
  last_gate70 = gate70; last_frame = g_frame_count;

  debugPrintf("tail_gate_trace STATE5 TAIL GATE: board=%p state=%u global=%d global_state=%u "
              "board+0x3d0=%p gate=%p gate+0x70=%u frame=%llu\n",
              board_ptr, state, board == global_object, global_state,
              (void *)board_gate, gate_ptr, gate70,
              (unsigned long long)g_frame_count);

  if (board == global_object && (state == 3 || state == 4) && gate &&
      memory_range_readable(gate, 0x100)) {
    static uintptr_t dumped_gate;
    if (dumped_gate != gate) {
      dumped_gate = gate;
      dump_memory_hex("tail_gate_trace STATE5 GATE OBJECT", gate_ptr, 0x100);
      const uintptr_t vtable = *(const uintptr_t *)gate;
      const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
      if (vtable >= base && vtable < base + game_mod.load_size) {
        debugPrintf("module_layout_analysis LEVEL MODULE MANAGER VTABLE: object=%p vtable=%p method_count=7\n",
                    gate_ptr, (void *)vtable);
        /* LevelModuleManager's typeinfo object begins immediately after
         * seven virtual entries in this binary. Do not walk through RTTI and
         * neighboring Event/PvzCdnManager vtable groups as if they were methods. */
        if (memory_range_readable(vtable, 7 * sizeof(uintptr_t))) {
          for (unsigned slot = 0; slot < 7; ++slot) {
            const uintptr_t target = ((const uintptr_t *)vtable)[slot];
            if (target >= base && target < base + game_mod.load_size) {
              const uintptr_t toff = target - base;
              debugPrintf("  slot=%u target=%p libPVZ2+0x%lx\n", slot,
                          (void *)target, (unsigned long)toff);
              /* Associate exact +0x70 accesses with virtual methods. Stop at
               * the first RET so adjacent functions are not misattributed. */
              /* This runs after so_finalize(). The host load_base
               * staging image is no longer safe to dereference here; inspect
               * the finalized executable alias instead. */
              const uintptr_t rbase = (uintptr_t)game_mod.load_virtbase;
              for (uintptr_t d = 0; d < 0x300 && toff + d + 4 <= game_mod.load_size; d += 4) {
                const uintptr_t pc = rbase + toff + d;
                if (!memory_range_readable(pc, sizeof(uint32_t))) break;
                const uint32_t insn = *(const uint32_t *)pc;
                unsigned rn = 0, rt = 0; const char *kind = NULL;
                if (arm64_analysis_level_module_gate_access(insn, &rn, &rt, &kind))
                  debugPrintf("    arm64_analysis VTABLE +0x70 ACCESS: slot=%u pc=+0x%lx %s w%u,[x%u,#0x70] raw=%08x\n",
                              slot, (unsigned long)(toff + d), kind, rt, rn, insn);
                if (insn == 0xd65f03c0u) break;
              }
            } else
              debugPrintf("  slot=%u target=%p outside-libPVZ2\n", slot,
                          (void *)target);
          }
        }
      }
    }
  }
}

void level_module_gate_store_observed(void *target_ptr, uint32_t value,
                                      uintptr_t lr_at_store,
                                      uintptr_t x19_value,
                                      uintptr_t x20_value) {
  const uintptr_t target = (uintptr_t)target_ptr;
  uint32_t old_word = UINT32_MAX;
  unsigned old_byte = 0xff;
  if (target && memory_range_readable(target + 0x70, sizeof(uint32_t))) {
    old_word = *(const uint32_t *)(target + 0x70);
    old_byte = *(const uint8_t *)(target + 0x70);
  }

  uint32_t global_state = UINT32_MAX;
  const uintptr_t global_object = current_input_state_object(&global_state);
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  const int lr_in_game = lr_at_store >= base &&
      lr_at_store < base + game_mod.load_size;

  debugPrintf("module_gate_probe LEVEL MODULE +0x70 STORE: target=%p manager=%p match=%d "
              "old_word=%u old_byte=%u native_new_word=%u native_new_byte=%u "
              "x19=%p x20=%p lr_at_store=%p%s0x%lx global_object=%p "
              "global_state=%u frame=%llu\n",
              target_ptr, (void *)g_level_module_manager,
              target == g_level_module_manager, old_word, old_byte, value,
              value & 0xffu, (void *)x19_value, (void *)x20_value,
              (void *)lr_at_store, lr_in_game ? " +" : " outside+",
              lr_in_game ? (unsigned long)(lr_at_store - base) : 0UL,
              (void *)global_object, global_state,
              (unsigned long long)g_frame_count);
}

static int module_gate_probe_trampoline_relocation_safe(uint32_t insn) {
  /* The copied 16-byte window must not contain PC-relative control/data
   * references. Register-indirect instructions are position independent. */
  if ((insn & 0x7c000000u) == 0x14000000u) return 0; /* B / BL */
  if ((insn & 0xff000010u) == 0x54000000u) return 0; /* B.cond */
  if ((insn & 0x7e000000u) == 0x34000000u) return 0; /* CBZ / CBNZ */
  if ((insn & 0x7e000000u) == 0x36000000u) return 0; /* TBZ / TBNZ */
  if ((insn & 0x9f000000u) == 0x10000000u) return 0; /* ADR */
  if ((insn & 0x9f000000u) == 0x90000000u) return 0; /* ADRP */
  if ((insn & 0x3b000000u) == 0x18000000u) return 0; /* literal LDR/PRFM */
  return 1;
}

static void __attribute__((unused)) dump_module_gate_probe_level_module_vtable_groups(uintptr_t vtable) {
  if (!vtable || !memory_range_readable(vtable, 64 * sizeof(uintptr_t))) return;
  for (unsigned slot = 0; slot + 2 < 64; ++slot) {
    const intptr_t offset_to_top = ((const intptr_t *)vtable)[slot];
    const uintptr_t typeinfo = ((const uintptr_t *)vtable)[slot + 1];
    if (offset_to_top < -0x10000 || offset_to_top > 0x10000) continue;
    if (!typeinfo || !memory_range_readable(typeinfo, 16)) continue;
    const uintptr_t name_ptr = *(const uintptr_t *)(typeinfo + 8);
    if (!name_ptr || !memory_range_readable(name_ptr, 1)) continue;
    char name[128] = {0};
    unsigned n = 0;
    for (; n + 1 < sizeof(name); ++n) {
      if (!memory_range_readable(name_ptr + n, 1)) break;
      const unsigned char c = *(const unsigned char *)(name_ptr + n);
      if (c && (c < 0x20 || c > 0x7e)) break;
      name[n] = (char)c;
      if (!c) break;
    }
    if (!name[0]) continue;
    debugPrintf("module_gate_probe LEVEL MODULE VTABLE GROUP: header_slot=%u "
                "offset_to_top=%ld typeinfo=%p name=%s first_method_slot=%u\n",
                slot, (long)offset_to_top, (void *)typeinfo, name, slot + 2);
    ++slot; /* typeinfo belongs to this header */
  }
}

static void __attribute__((unused)) install_module_gate_probe_level_module_gate_writer_probe(void) {
  const uintptr_t target_off = 0x14fc508;
  const uintptr_t cave_off = 0x23f49e4; /* dead body behind hooked Nimble no-op */
  const uintptr_t hbase = (uintptr_t)game_mod.load_base;
  const uintptr_t vbase = (uintptr_t)game_mod.load_virtbase;
  if (!hbase || !vbase || target_off + 16 > game_mod.load_size ||
      cave_off + 24 > game_mod.load_size) {
    debugPrintf("module_gate_probe LEVEL MODULE WRITER HOOK: unavailable mapping/size\n");
    return;
  }

  uint32_t original[4];
  memcpy(original, (const void *)(hbase + target_off), sizeof(original));
  debugPrintf("module_gate_probe LEVEL MODULE WRITER WINDOW +0x14FC508: %08x %08x %08x %08x\n",
              original[0], original[1], original[2], original[3]);
  if (original[0] != 0xb90072a8u) {
    debugPrintf("module_gate_probe LEVEL MODULE WRITER HOOK: expected STR W8,[X21,#0x70] missing; skip\n");
    return;
  }
  for (unsigned i = 0; i < 4; ++i) {
    if (!module_gate_probe_trampoline_relocation_safe(original[i])) {
      debugPrintf("module_gate_probe LEVEL MODULE WRITER HOOK: word[%u]=%08x is PC-relative; safe hook skipped\n",
                  i, original[i]);
      return;
    }
    if (i < 3 && original[i] == 0xd65f03c0u) {
      debugPrintf("module_gate_probe LEVEL MODULE WRITER HOOK: early RET in 16-byte window; safe hook skipped\n");
      return;
    }
  }

  uint32_t *tr = (uint32_t *)(hbase + cave_off);
  tr[0] = 0xf84107f1u; /* ldr x17, [sp], #16 -- restore probe branch scratch */
  memcpy(&tr[1], original, sizeof(original));
  const uintptr_t branch_pc = vbase + cave_off + 20;
  const uintptr_t return_pc = vbase + target_off + 16;
  const int64_t delta = (int64_t)return_pc - (int64_t)branch_pc;
  if ((delta & 3) || delta < -(1LL << 27) || delta >= (1LL << 27)) {
    debugPrintf("module_gate_probe LEVEL MODULE WRITER HOOK: trampoline return branch out of range; skip\n");
    return;
  }
  tr[5] = 0x14000000u | ((uint32_t)(delta >> 2) & 0x03ffffffu);
  level_module_gate_store_trampoline = vbase + cave_off;
  hook_arm64(hbase + target_off, (uintptr_t)level_module_gate_store_probe);
  debugPrintf("module_gate_probe LEVEL MODULE WRITER HOOK: +0x14FC508 -> probe; trampoline=+0x23F49E4; native store/window replayed unchanged\n");
}

int get_registered_touch_objects_count(void) {
  return g_register_calls_alive;
}

int get_mapped_owners_count(void) {
  return g_tracked_owners_count;
}

void trace_touch_probe_event(int phase, int id, float x, float y) {
  (void)phase; (void)id; (void)x; (void)y;
}

static void dump_touch_probe_simd(const uint64_t *saved) {
  const float *s = (const float *)(saved + 20);
  for (int i = 0; i < 8; i++) {
    debugPrintf("    q%d={%016lx,%016lx} s={%.3f,%.3f,%.3f,%.3f}\n", i,
                (unsigned long)saved[20 + i * 2],
                (unsigned long)saved[21 + i * 2],
                s[i * 4], s[i * 4 + 1], s[i * 4 + 2], s[i * 4 + 3]);
  }
}

void touch_path_slot3_before(const uint64_t *saved, uintptr_t original_sp,
                             uintptr_t caller_lr) {
  debugPrintf("TOUCH SLOT3 BEFORE: LR=%p SP=%p phase=%d id=%d xy=(%.1f,%.1f)\n"
              "    x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n"
              "    x8=%p x9=%p x10=%p x11=%p x12=%p x13=%p x14=%p x15=%p\n",
              (void *)caller_lr, (void *)original_sp, g_touch_probe_phase,
              g_touch_probe_id, g_touch_probe_x, g_touch_probe_y,
              (void *)saved[0], (void *)saved[1], (void *)saved[2],
              (void *)saved[3], (void *)saved[4], (void *)saved[5],
              (void *)saved[6], (void *)saved[7], (void *)saved[8],
              (void *)saved[9], (void *)saved[10], (void *)saved[11],
              (void *)saved[12], (void *)saved[13], (void *)saved[14],
              (void *)saved[15]);
  dump_touch_probe_simd(saved);

  const unsigned bit = g_touch_probe_phase >= PTR_DOWN &&
                       g_touch_probe_phase <= PTR_UP
                           ? 1u << g_touch_probe_phase : 0;
  if (bit && !(g_touch_probe_object_dumps & bit) && saved[1] &&
      !(saved[1] & 7)) {
    g_touch_probe_object_dumps |= bit;
    debugPrintf("TOUCH SLOT3 OBJECT DUMP: phase=%d object=%p\n",
                g_touch_probe_phase, (void *)saved[1]);
    dump_memory_hex("TOUCH SLOT3 X1 OBJECT", (void *)saved[1], 0x100);
  }
}

void touch_path_slot3_after(const uint64_t *before, const uint64_t *after,
                            uint64_t result, uintptr_t caller_lr) {
  debugPrintf("TOUCH SLOT3 AFTER: LR=%p return_x0=%p\n",
              (void *)caller_lr, (void *)result);
  debugPrintf("    post x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n",
              (void *)after[0], (void *)after[1], (void *)after[2],
              (void *)after[3], (void *)after[4], (void *)after[5],
              (void *)after[6], (void *)after[7]);
  (void)before;
}

void touch_path_indirect_before(const uint64_t *saved, uintptr_t original_sp,
                                uintptr_t caller_lr) {
  debugPrintf("TOUCH INDIRECT BEFORE: target=%p LR=%p SP=%p\n",
              (void *)saved[8], (void *)caller_lr, (void *)original_sp);
  debugPrintf("    x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n"
              "    x8=%p x9=%p x10=%p x11=%p x12=%p x13=%p x14=%p x15=%p\n",
              (void *)saved[0], (void *)saved[1], (void *)saved[2],
              (void *)saved[3], (void *)saved[4], (void *)saved[5],
              (void *)saved[6], (void *)saved[7], (void *)saved[8],
              (void *)saved[9], (void *)saved[10], (void *)saved[11],
              (void *)saved[12], (void *)saved[13], (void *)saved[14],
              (void *)saved[15]);
  dump_touch_probe_simd(saved);
}

void touch_path_indirect_after(const uint64_t *after, uint64_t result,
                               uintptr_t caller_lr) {
  debugPrintf("TOUCH INDIRECT AFTER: target=%p return_x0=%p LR=%p\n",
              (void *)after[8], (void *)result, (void *)caller_lr);
  debugPrintf("    post x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n",
              (void *)after[0], (void *)after[1], (void *)after[2],
              (void *)after[3], (void *)after[4], (void *)after[5],
              (void *)after[6], (void *)after[7]);
}

void touch_registration_dispatch_before(const uint64_t *saved,
                                        uintptr_t caller_lr) {
  const uintptr_t record = saved[0];
  const uintptr_t f08 = record ? *(uintptr_t *)(record + 0x08) : 0;
  const uintptr_t mixed = record ? *(uintptr_t *)(record + 0x10) : 0;
  const uintptr_t f18 = record ? *(uintptr_t *)(record + 0x18) : 0;
  const uintptr_t f28 = record ? *(uintptr_t *)(record + 0x28) : 0;
  const uintptr_t f40 = record ? *(uintptr_t *)(record + 0x40) : 0;
  const uintptr_t base = f18 + (mixed >> 1);
  const uintptr_t selected = record
      ? ((mixed & 1) ? *(uintptr_t *)(*(uintptr_t *)base + f08) : f08) : 0;
  const uintptr_t game_base = (uintptr_t)game_mod.load_virtbase;
  const int in_game = selected >= game_base && selected < game_base + game_mod.load_size;
  if (record && f28 == game_base + 0x16f9730)
    debugPrintf("MODAL +28 CALLBACK TAIL: record=%p x0=%p selected=%p\n",
                (void *)record, (void *)saved[0], (void *)selected);
  debugPrintf("TOUCH REG DISPATCH: record=%p selected=%p offset=%s0x%lx LR=%p\n"
              "    +08=%p +10=%p +18=%p +28=%p +40=%p phase=%d id=%d xy=(%.1f,%.1f)\n"
              "    x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n"
              "    x8=%p x9=%p x10=%p x11=%p x12=%p x13=%p x14=%p x15=%p\n",
              (void *)record, (void *)selected, in_game ? "+" : "",
              in_game ? (unsigned long)(selected - game_base) : (unsigned long)selected,
              (void *)caller_lr, (void *)f08, (void *)mixed, (void *)f18,
              (void *)f28, (void *)f40, g_touch_probe_phase, g_touch_probe_id,
              g_touch_probe_x, g_touch_probe_y,
              (void *)saved[0], (void *)saved[1], (void *)saved[2], (void *)saved[3],
              (void *)saved[4], (void *)saved[5], (void *)saved[6], (void *)saved[7],
              (void *)saved[8], (void *)saved[9], (void *)saved[10], (void *)saved[11],
              (void *)saved[12], (void *)saved[13], (void *)saved[14], (void *)saved[15]);
  dump_touch_probe_simd(saved);

  const unsigned bit = g_touch_probe_phase >= PTR_DOWN &&
                       g_touch_probe_phase <= PTR_UP
                           ? 1u << g_touch_probe_phase : 0;
  if (bit && !(g_touch_probe_object_dumps & bit) && record && !(record & 7)) {
    g_touch_probe_object_dumps |= bit;
    dump_memory_hex("TOUCH REG RECORD", (void *)record, 0x78);
  }
  static unsigned event_dumps;
  if (bit && saved[1] && !(saved[1] & 7) && !(event_dumps & bit)) {
    event_dumps |= bit;
    dump_memory_hex("TOUCH REG EVENT AREA", (void *)saved[1], 0x100);
  }
}

void touch_registration_dispatch_after(const uint64_t *saved,
                                       uint64_t result,
                                       uintptr_t caller_lr) {
  debugPrintf("TOUCH REG DISPATCH TAIL: target_return=%p q0={%016lx,%016lx} "
              "s0={%.3f,%.3f} LR=%p\n",
              (void *)result, (unsigned long)saved[20],
              (unsigned long)saved[21], *(const float *)(saved + 20),
              *(const float *)(saved + 21), (void *)caller_lr);
}

void touch_registration_filter_before(const uint64_t *saved, uintptr_t caller_lr,
                                      uint64_t nzcv) {
  const uintptr_t owner = (uintptr_t)saved[0];
  const uintptr_t event = (uintptr_t)saved[1];
  const uint32_t owner_9c = owner ? *(const uint32_t *)(owner + 0x9c) : 0;
  const uint64_t owner_d8 = owner ? *(const uint64_t *)(owner + 0xd8) : 0;
  const uint32_t event_00 = event ? *(const uint32_t *)(event + 0x00) : 0;
  const uint32_t event_30 = event ? *(const uint32_t *)(event + 0x30) : 0;

  debugPrintf("TOUCH FILTER BEFORE: x0=%p w0=0x%x LR=%p NZCV=0x%lx\n"
              "    x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n"
              "    q0={%016lx,%016lx} d0=%.6f s0=%.6f\n",
              (void *)saved[0], (unsigned)(uint32_t)saved[0],
              (void *)caller_lr, (unsigned long)nzcv,
              (void *)saved[1], (void *)saved[2], (void *)saved[3],
              (void *)saved[4], (void *)saved[5], (void *)saved[6],
              (void *)saved[7], (unsigned long)saved[20],
              (unsigned long)saved[21], *(const double *)(saved + 20),
              *(const float *)(saved + 20));
  debugPrintf("MODAL GATE: owner+0x9c=%u (0x%x) owner+0xd8=%p "
              "event+0x30=%u (0x%x) event+0x00=%u (0x%x)\n",
              owner_9c, owner_9c, (void *)owner_d8,
              event_30, event_30, event_00, event_00);
}

void touch_registration_filter_after(const uint64_t *saved, uint64_t result,
                                     uint64_t nzcv, uintptr_t caller_lr) {
  debugPrintf("TOUCH FILTER AFTER: x0=%p q0={%016lx,%016lx} d0=%.6f s0=%.6f "
              "NZCV=0x%lx LR=%p\n"
              "MODAL GLOBAL GATE: x8_at_exit=%p w8_at_exit=%u expected=5\n",
              (void *)result, (unsigned long)saved[20],
              (unsigned long)saved[21], *(const double *)(saved + 20),
              *(const float *)(saved + 20), (unsigned long)nzcv,
              (void *)caller_lr, (void *)saved[8], (unsigned)(uint32_t)saved[8]);
}

void touch_registration_filter_branch(uint64_t *saved, uintptr_t x19,
                                      uintptr_t x20, uintptr_t x21,
                                      uintptr_t x22, uintptr_t x23,
                                      uintptr_t original_sp) {
  const int nonzero = (uint32_t)saved[0] != 0;
  debugPrintf("TOUCH FILTER BRANCH: result=%p w0=0x%x decision=%s\n"
              "    x19=%p x20=%p x21=%p x22=%p x23=%p SP=%p\n",
              (void *)saved[0], (unsigned)(uint32_t)saved[0],
              nonzero ? "CONTINUE (+0x10DFD24)" : "SKIP (+0x10DFCF0)",
              (void *)x19, (void *)x20, (void *)x21, (void *)x22,
              (void *)x23, (void *)original_sp);
}


static int arm64_analysis_level_module_gate_access(uint32_t insn, unsigned *rn,
                                           unsigned *rt, const char **kind) {
  const uint32_t op = insn & 0xffc00000u;
  const unsigned imm12 = (insn >> 10) & 0xfffu;
  *rn = (insn >> 5) & 31u;
  *rt = insn & 31u;
  *kind = NULL;

  if ((op == 0x39000000u || op == 0x39400000u) && imm12 == 0x70u) {
    *kind = op == 0x39000000u ? "STRB" : "LDRB";
    return 1;
  }
  if ((op == 0xb9000000u || op == 0xb9400000u) && imm12 * 4u == 0x70u) {
    *kind = op == 0xb9000000u ? "STRW" : "LDRW";
    return 1;
  }

  /* Unscaled byte forms: STURB/LDURB use a signed imm9 in bits 20:12. */
  const uint32_t unscaled = insn & 0xffe00c00u;
  int imm9 = (int)((insn >> 12) & 0x1ffu);
  if (imm9 & 0x100) imm9 -= 0x200;
  if ((unscaled == 0x38000000u || unscaled == 0x38400000u) && imm9 == 0x70) {
    *kind = unscaled == 0x38000000u ? "STURB" : "LDURB";
    return 1;
  }
  return 0;
}

static int module_layout_analysis_decode_adrp(uint32_t insn, uintptr_t pc,
                             uintptr_t *page_out, unsigned *rd_out) {
  if ((insn & 0x9f000000u) != 0x90000000u) return 0;
  int64_t imm21 = (int64_t)(((insn >> 29) & 3u) |
                            (((insn >> 5) & 0x7ffffu) << 2));
  if (imm21 & (1LL << 20)) imm21 |= ~((1LL << 21) - 1);
  *page_out = (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm21 << 12);
  *rd_out = insn & 31u;
  return 1;
}

static int module_layout_analysis_decode_add_imm(uint32_t insn, uintptr_t base_value,
                                unsigned expected_rn, uintptr_t *value_out,
                                unsigned *rd_out) {
  /* 64-bit ADD (immediate), no flags. */
  if ((insn & 0xff000000u) != 0x91000000u) return 0;
  const unsigned rn = (insn >> 5) & 31u;
  if (rn != expected_rn) return 0;
  const unsigned imm12 = (insn >> 10) & 0xfffu;
  const unsigned sh = (insn >> 22) & 1u;
  *value_out = base_value + ((uintptr_t)imm12 << (sh ? 12 : 0));
  *rd_out = insn & 31u;
  return 1;
}

static void module_layout_analysis_dump_context(const char *label, uintptr_t center_off,
                               int before_words, int after_words) {
  const uintptr_t base = (uintptr_t)game_mod.load_base;
  debugPrintf("%s +0x%lx:\n", label, (unsigned long)center_off);
  for (int d = -before_words; d <= after_words; ++d) {
    const intptr_t off = (intptr_t)center_off + (intptr_t)d * 4;
    if (off < 0 || (uintptr_t)off + 4 > game_mod.load_size) continue;
    debugPrintf("  %c+0x%lx: %08x\n", d == 0 ? '>' : ' ',
                (unsigned long)off,
                *(const uint32_t *)(base + (uintptr_t)off));
  }
}

static void __attribute__((unused)) dump_module_layout_analysis_level_module_static_analysis(void) {
  const uintptr_t hbase = (uintptr_t)game_mod.load_base;
  const uintptr_t vbase = (uintptr_t)game_mod.load_virtbase;
  const uintptr_t vtable_off = 0x2b4e018;
  const uintptr_t vtable_addr = vbase + vtable_off;
  const uintptr_t vtable_symbol_addr = vtable_addr - 16;
  static const uintptr_t methods[] = {
      0x14f5e40, 0x14f612c, 0x14f92d4, 0x14f99cc,
      0x23a677c, 0x23a67b0, 0x23a67b8,
  };

  debugPrintf("module_layout_analysis LEVEL MODULE LAYOUT: vtable=%p +0x%lx symbol=%p +0x%lx "
              "typeinfo=%p method_count=7\n",
              (void *)vtable_addr, (unsigned long)vtable_off,
              (void *)vtable_symbol_addr, (unsigned long)(vtable_off - 16),
              (void *)(vbase + 0x2b4e050));

  for (unsigned m = 0; m < sizeof(methods)/sizeof(methods[0]); ++m) {
    const uintptr_t start = methods[m];
    debugPrintf("module_layout_analysis LEVEL MODULE METHOD[%u] RAW +0x%lx..+0x%lx:\n",
                m, (unsigned long)start, (unsigned long)(start + 0x80));
    for (uintptr_t off = start; off < start + 0x80 && off + 16 <= game_mod.load_size;
         off += 0x10) {
      const uint32_t *q = (const uint32_t *)(hbase + off);
      debugPrintf("  +0x%lx: %08x %08x %08x %08x\n",
                  (unsigned long)off, q[0], q[1], q[2], q[3]);
    }
  }

  /* Search PIC code/data for sequences that materialize either the Itanium
   * vtable symbol (address point - 16) or the actual object vptr address.
   * Tracking ADD-immediate chains handles the usual _ZTV + 16 construction. */
  unsigned xref_hits = 0;
  for (uintptr_t off = 0; off + 4 <= game_mod.load_size; off += 4) {
    const uint32_t insn = *(const uint32_t *)(hbase + off);
    uintptr_t value = 0;
    unsigned reg = 0;
    if (!module_layout_analysis_decode_adrp(insn, vbase + off, &value, &reg)) continue;
    uintptr_t known[32] = {0};
    uint32_t valid = 0;
    known[reg] = value;
    valid |= 1u << reg;
    for (unsigned step = 1; step <= 8 && off + step * 4 + 4 <= game_mod.load_size; ++step) {
      const uintptr_t io = off + step * 4;
      const uint32_t w = *(const uint32_t *)(hbase + io);
      const unsigned rn = (w >> 5) & 31u;
      uintptr_t nv = 0;
      unsigned rd = 0;
      if ((valid & (1u << rn)) && module_layout_analysis_decode_add_imm(w, known[rn], rn, &nv, &rd)) {
        known[rd] = nv;
        valid |= 1u << rd;
        if (nv == vtable_addr || nv == vtable_symbol_addr) {
          ++xref_hits;
          debugPrintf("module_layout_analysis LEVEL MODULE VTABLE XREF #%u: adrp=+0x%lx add=+0x%lx "
                      "reg=x%u value=%p %s\n",
                      xref_hits, (unsigned long)off, (unsigned long)io, rd,
                      (void *)nv, nv == vtable_addr ? "ADDRESS_POINT" : "SYMBOL");
          module_layout_analysis_dump_context("module_layout_analysis LEVEL MODULE VTABLE XREF CONTEXT", io, 12, 20);
        }
      }
      /* Stop tracking a register when a common instruction clearly writes it.
       * This is intentionally conservative; false negatives are preferable to
       * treating unrelated data as constructor evidence. */
      const unsigned wrd = w & 31u;
      if (((w & 0x7c000000u) == 0x14000000u) || /* branch */
          ((w & 0xff000010u) == 0x54000000u))    /* conditional branch */
        break;
      if (w == 0xd65f03c0u) break;
      (void)wrd;
    }
  }
  debugPrintf("module_layout_analysis LEVEL MODULE VTABLE XREF COUNT: %u\n", xref_hits);

  unsigned abs_hits = 0;
  for (uintptr_t off = 0; off + 8 <= game_mod.load_size; off += 8) {
    const uintptr_t q = *(const uintptr_t *)(hbase + off);
    if (q != vtable_addr && q != vtable_symbol_addr) continue;
    ++abs_hits;
    if (abs_hits <= 32)
      debugPrintf("module_layout_analysis LEVEL MODULE ABS REF #%u: +0x%lx -> %p %s\n",
                  abs_hits, (unsigned long)off, (void *)q,
                  q == vtable_addr ? "ADDRESS_POINT" : "SYMBOL");
  }
  debugPrintf("module_layout_analysis LEVEL MODULE ABS REF COUNT: %u\n", abs_hits);

  /* Constructor/destructor and closely related helpers for this class are
   * clustered around the first virtual methods. Keep this bounded and report
   * only exact +0x70 byte/word accesses. */
  unsigned local_hits = 0;
  for (uintptr_t off = 0x14f4000; off < 0x14fb000 && off + 4 <= game_mod.load_size;
       off += 4) {
    const uint32_t w = *(const uint32_t *)(hbase + off);
    unsigned rn = 0, rt = 0; const char *kind = NULL;
    if (!arm64_analysis_level_module_gate_access(w, &rn, &rt, &kind)) continue;
    ++local_hits;
    debugPrintf("module_layout_analysis LEVEL MODULE LOCAL +0x70 #%u: +0x%lx %s w%u,[x%u,#0x70] raw=%08x\n",
                local_hits, (unsigned long)off, kind, rt, rn, w);
    if (local_hits <= 32)
      module_layout_analysis_dump_context("module_layout_analysis LEVEL MODULE LOCAL ACCESS CONTEXT", off, 6, 8);
  }
  debugPrintf("module_layout_analysis LEVEL MODULE LOCAL +0x70 COUNT: %u\n", local_hits);
}

typedef struct {
  unsigned valid;
  intptr_t delta;
} module_init_analysisThisReg;

typedef struct {
  unsigned valid;
  uint32_t value;
} module_init_analysisImmReg;

static int module_init_analysis_decode_mov_x(uint32_t w, unsigned *rd, unsigned *rm) {
  /* MOV Xd,Xm is the ORR Xd,XZR,Xm alias. */
  if ((w & 0xffe0ffe0u) != 0xaa0003e0u) return 0;
  *rd = w & 31u;
  *rm = (w >> 16) & 31u;
  return 1;
}

static int module_init_analysis_decode_addsub_x_imm(uint32_t w, unsigned *rd, unsigned *rn,
                                     intptr_t *imm) {
  const uint32_t op = w & 0xff000000u;
  if (op != 0x91000000u && op != 0xd1000000u) return 0;
  *rd = w & 31u;
  *rn = (w >> 5) & 31u;
  const unsigned imm12 = (w >> 10) & 0xfffu;
  const unsigned sh = (w >> 22) & 1u;
  intptr_t v = (intptr_t)imm12 << (sh ? 12 : 0);
  if (op == 0xd1000000u) v = -v;
  *imm = v;
  return 1;
}

static int module_init_analysis_decode_movz_w(uint32_t w, unsigned *rd, uint32_t *value) {
  /* MOV Wd,#imm is normally a MOVZ alias.  Track it only when it fits in W. */
  if ((w & 0x7f800000u) != 0x52800000u) return 0;
  const unsigned hw = (w >> 21) & 3u;
  if (hw > 1) return 0;
  *rd = w & 31u;
  *value = ((w >> 5) & 0xffffu) << (hw * 16);
  return 1;
}

static int module_init_analysis_decode_store_single(uint32_t w, unsigned *rn, unsigned *rt,
                                     intptr_t *imm, unsigned *width,
                                     int *writeback, const char **kind) {
  struct UnsignedStore { uint32_t op; unsigned width; const char *name; };
  static const struct UnsignedStore uops[] = {
      {0x39000000u, 1, "STRB"}, {0x79000000u, 2, "STRH"},
      {0xb9000000u, 4, "STRW"}, {0xf9000000u, 8, "STRX"},
      {0x3d800000u, 16, "STRQ"},
  };
  const uint32_t utop = w & 0xffc00000u;
  for (unsigned i = 0; i < sizeof(uops)/sizeof(uops[0]); ++i) {
    if (utop != uops[i].op) continue;
    *rn = (w >> 5) & 31u;
    *rt = w & 31u;
    *width = uops[i].width;
    *imm = (intptr_t)((w >> 10) & 0xfffu) * (intptr_t)*width;
    *writeback = 0;
    *kind = uops[i].name;
    return 1;
  }

  struct SignedStore { uint32_t op; unsigned width; const char *name; };
  static const struct SignedStore sops[] = {
      {0x38000000u, 1, "STURB"}, {0x78000000u, 2, "STURH"},
      {0xb8000000u, 4, "STURW"}, {0xf8000000u, 8, "STURX"},
      {0x3c800000u, 16, "STURQ"},
  };
  const uint32_t stop = w & 0xffe00000u;
  for (unsigned i = 0; i < sizeof(sops)/sizeof(sops[0]); ++i) {
    if (stop != sops[i].op) continue;
    const unsigned mode = (w >> 10) & 3u;
    if (mode == 2) return 0; /* register-offset/unprivileged family, not imm9 */
    int simm = (int)((w >> 12) & 0x1ffu);
    if (simm & 0x100) simm -= 0x200;
    *rn = (w >> 5) & 31u;
    *rt = w & 31u;
    *width = sops[i].width;
    *imm = (intptr_t)simm;
    *writeback = mode == 3 ? 1 : (mode == 1 ? 2 : 0); /* pre / post */
    *kind = sops[i].name;
    return 1;
  }
  return 0;
}

typedef struct {
  uintptr_t pc;
  uintptr_t func;
  unsigned rn;
  unsigned rt;
  unsigned width;
  unsigned known_value;
  uint32_t value;
} module_manager_traceStoreCandidate;

typedef struct {
  uintptr_t target;
  unsigned arg;
  intptr_t delta;
  uintptr_t from_pc;
} module_call_analysisCallSeed;

#define module_call_analysis_MAX_CALL_SEEDS 1024

static int module_call_analysis_offset_executable(uintptr_t off) {
  for (int i=0; i<game_mod.phnum; ++i) {
    const Elf64_Phdr *p=&game_mod.phdr[i];
    if (p->p_type!=PT_LOAD || !(p->p_flags & PF_X)) continue;
    if (off >= p->p_vaddr && off < p->p_vaddr + p->p_memsz) return 1;
  }
  return 0;
}

typedef struct {
  uintptr_t pc;
  uintptr_t seed_pc;
  intptr_t seed_delta;
  intptr_t addr_delta;
  unsigned rn;
  unsigned rt;
  unsigned width;
  unsigned known_value;
  uint32_t value;
  const char *source;
} module_call_analysisExternalStore;

static int module_call_analysis_decode_ldr_x_unsigned(uint32_t w, unsigned *rn, unsigned *rt,
                                       uintptr_t *imm) {
  if ((w & 0xffc00000u) != 0xf9400000u) return 0;
  *rn = (w >> 5) & 31u;
  *rt = w & 31u;
  *imm = (uintptr_t)((w >> 10) & 0xfffu) * 8u;
  return 1;
}

static uintptr_t module_call_analysis_branch_target(uintptr_t pc_off, uint32_t w) {
  const uint32_t op=w & 0xfc000000u;
  if (op != 0x94000000u && op != 0x14000000u) return UINTPTR_MAX;
  int64_t imm26 = (int64_t)(w & 0x03ffffffu);
  if (imm26 & (1LL << 25)) imm26 -= (1LL << 26);
  return (uintptr_t)((int64_t)pc_off + (imm26 << 2));
}

static uintptr_t module_call_analysis_bl_target(uintptr_t pc_off, uint32_t w) {
  if ((w & 0xfc000000u) != 0x94000000u) return UINTPTR_MAX;
  return module_call_analysis_branch_target(pc_off,w);
}


typedef struct { uintptr_t target; unsigned arg; intptr_t delta; uintptr_t from; } module_factory_traceRuntimeSeed;

static void module_factory_trace_runtime_scan_factory_flow(uintptr_t caller_off) {
  const uintptr_t base=(uintptr_t)game_mod.load_virtbase;
  if (!base || !module_call_analysis_offset_executable(caller_off)) return;
  module_factory_traceRuntimeSeed q[64]; unsigned qn=0;
  q[qn++]=(module_factory_traceRuntimeSeed){caller_off,0,0,caller_off};
  debugPrintf("module_factory_trace FACTORY FLOW START: caller=+0x%lx seed=x0 manager_delta=0\n",(unsigned long)caller_off);

  for (unsigned qi=0; qi<qn; ++qi) {
    const module_factory_traceRuntimeSeed cur=q[qi];
    module_init_analysisThisReg regs[32]={{0}}; module_init_analysisImmReg imms[32]={{0}};
    regs[cur.arg].valid=1; regs[cur.arg].delta=cur.delta;
    uintptr_t start=cur.target, stop=start+0x400;
    if (stop>game_mod.load_size) stop=game_mod.load_size;
    debugPrintf("module_factory_trace FACTORY FLOW ANALYZE #%u: target=+0x%lx arg=x%u delta=%ld from=+0x%lx\n",
                qi+1,(unsigned long)cur.target,cur.arg,(long)cur.delta,(unsigned long)cur.from);
    for (uintptr_t off=start; off+4<=stop; off+=4) {
      uintptr_t pc=base+off; if (!memory_range_readable(pc,4)) break;
      uint32_t w=*(const uint32_t *)pc;
      unsigned rd=0,rn=0,rm=0,rt=0; intptr_t imm=0; uint32_t immv=0;
      if (module_init_analysis_decode_mov_x(w,&rd,&rm)) { if(rd<31){regs[rd]=regs[rm]; imms[rd].valid=0;} continue; }
      if (module_init_analysis_decode_addsub_x_imm(w,&rd,&rn,&imm)) {
        if(rd<31){ if(rn<31&&regs[rn].valid){regs[rd].valid=1;regs[rd].delta=regs[rn].delta+imm;}else regs[rd].valid=0; imms[rd].valid=0;} continue;
      }
      if (module_init_analysis_decode_movz_w(w,&rd,&immv)) { if(rd<31){imms[rd].valid=1;imms[rd].value=immv;regs[rd].valid=0;} continue; }
      unsigned width=0; int wb=0; const char *kind=NULL;
      if (module_init_analysis_decode_store_single(w,&rn,&rt,&imm,&width,&wb,&kind)) {
        if(rn<31&&regs[rn].valid){ intptr_t bd=regs[rn].delta; intptr_t ad=bd+(wb==2?0:imm); intptr_t hi=ad+(intptr_t)width-1;
          if(ad<=0x70&&hi>=0x70){ unsigned known=(rt==31)||(rt<31&&imms[rt].valid); unsigned val=rt==31?0u:(known?imms[rt].value:0u);
            debugPrintf("module_factory_trace FACTORY FLOW +0x70 STORE: pc=+0x%lx addr_delta=%ld base=x%u src=%u width=%u%s%u\n",
                        (unsigned long)off,(long)ad,rn,rt,width,known?" imm=":" value=unknown ",known?val:0u); }
          if(wb==1||wb==2) regs[rn].delta=bd+imm; }
        continue;
      }
      uintptr_t limm=0;
      if (module_call_analysis_decode_ldr_x_unsigned(w,&rn,&rt,&limm)) { if(rt<31){regs[rt].valid=0;imms[rt].valid=0;} continue; }
      if ((w&0x9f000000u)==0x90000000u || (w&0x9f000000u)==0x10000000u) { rd=w&31u; if(rd<31){regs[rd].valid=0;imms[rd].valid=0;} continue; }
      if ((w&0xfc000000u)==0x94000000u) {
        uintptr_t target=module_call_analysis_bl_target(off,w);
        for(unsigned a=0;a<8;a++) if(regs[a].valid){
          debugPrintf("module_factory_trace FACTORY FLOW CALL: pc=+0x%lx target=+0x%lx arg=x%u delta=%ld\n",(unsigned long)off,(unsigned long)target,a,(long)regs[a].delta);
          if(qn<64 && target<game_mod.load_size && module_call_analysis_offset_executable(target)){
            int dup=0; for(unsigned j=0;j<qn;j++) if(q[j].target==target&&q[j].arg==a&&q[j].delta==regs[a].delta){dup=1;break;}
            if(!dup) q[qn++]=(module_factory_traceRuntimeSeed){target,a,regs[a].delta,off};
          }
        }
        for(unsigned r=0;r<=17;r++){regs[r].valid=0;imms[r].valid=0;} continue;
      }
      if ((w&0xfc000000u)==0x14000000u) {
        uintptr_t target=module_call_analysis_branch_target(off,w);
        if(target>=start&&target<stop){ if(target>off){off=target-4;continue;} break; }
        for(unsigned a=0;a<8;a++) if(regs[a].valid && qn<64 && target<game_mod.load_size && module_call_analysis_offset_executable(target)) q[qn++]=(module_factory_traceRuntimeSeed){target,a,regs[a].delta,off};
        break;
      }
      if ((w&0xfffffc1fu)==0xd63f0000u) {
        unsigned brn=(w>>5)&31u;
        for(unsigned a=0;a<8;a++) if(regs[a].valid) debugPrintf("module_factory_trace FACTORY FLOW INDIRECT: pc=+0x%lx via=x%u arg=x%u delta=%ld\n",(unsigned long)off,brn,a,(long)regs[a].delta);
        for(unsigned r=0;r<=17;r++){regs[r].valid=0;imms[r].valid=0;} continue;
      }
      if(w==0xd65f03c0u) break;
    }
  }
  debugPrintf("module_factory_trace FACTORY FLOW SUMMARY: analyzed=%u queued=%u\n",qn,qn);
}

/* Kept for the linked touch probe object; no touch callback is installed. */
uintptr_t touch_interface_callback_probe(uint32_t slot, const uintptr_t *saved,
                                         uintptr_t caller_lr) {
  (void)slot; (void)saved; (void)caller_lr;
  return 0;
}

static void trace_title_loading_state(void) {
  const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  /* LoadingScreen::setState stores the new state at +184 here.  Preserve its
   * original instructions in title_state_probe and report every real state
   * transition, including the final 13 -> 14 -> 15 sequence. */
  title_state_continue = base + 0x13e61ec;
  hook_arm64((uintptr_t)game_mod.load_base + 0x13e61dc,
             (uintptr_t)title_state_probe);
scan_request_state_writers();
  /* The state-6 case reaches this branch after native catalog processing.
   * It selects the normal state-12 transition only when bit 0 of w0 is set. */
  title_state6_exit_continue = (uintptr_t)game_mod.load_virtbase + 0x13e8e28;
  title_state_setter = (uintptr_t)game_mod.load_virtbase + 0x13e6138;
  hook_arm64((uintptr_t)game_mod.load_base + 0x13e8e18,
             (uintptr_t)title_state6_exit_probe);

  /* These locations are the three conditional exits in the singleton's
   * state-1 update.  They are not a loader bypass: each probe replays the
   * native instruction sequence and follows its original branch target. */
}

static void wwise_term_probe(void) {
  wwise_term_trampoline();
}

void audio_event_observe(const char *name) {
  (void)name;
}

void wwise_loadbank_observe(const char *name, uint32_t *id_out) {
  (void)name;
  (void)id_out;
}

static void trace_wwise_init(void) {
  /* Preserve the four instructions after AK::SoundEngine::Init while recording
   * its AKRESULT. wwise_probe.s branches back to the original success/fail path. */
  wwise_init_success = (uintptr_t)game_mod.load_virtbase + 0x23f2d24;
  wwise_init_failure = (uintptr_t)game_mod.load_virtbase + 0x23f2d60;
  hook_arm64((uintptr_t)game_mod.load_base + 0x23f2d14,
             (uintptr_t)wwise_init_probe);

  /* SoundEngine::Init collapses subsystem errors to AK_Fail after calling
   * Term(). Interpose Term long enough to record which singleton was reached. */
  wwise_state_base = (uintptr_t)game_mod.load_virtbase + 0x2e53000;
  wwise_term_continue = (uintptr_t)game_mod.load_virtbase + 0x25daff4;
  hook_arm64((uintptr_t)game_mod.load_base + 0x25dafe4,
             (uintptr_t)wwise_term_probe);

}

typedef void *(*cxx_new_fn)(size_t);
typedef struct {
  size_t capacity_and_long_flag;
  size_t size;
  char *data;
} title_creditLibcppLongString;

/* These three 13.3.1 static std::string objects are initialized directly from
 * the literal [TITLESCREEN_COPYRIGHT].  Replace only when their runtime content
 * still matches that exact token, so a changed binary/version fails closed.
 * PrimeText treats non-[KEY] text as a literal, letting the game render the
 * requested port credit without modifying localization files or draw code. */
static int replace_title_token_at(uintptr_t offset, cxx_new_fn op_new) {
  static const char from[] = "[TITLESCREEN_COPYRIGHT]";
  static const char to[] = "Electronic Arts 2026 ported by flippy";
  title_creditLibcppLongString *value =
      (title_creditLibcppLongString *)((uintptr_t)game_mod.load_virtbase + offset);
  if (!op_new || !(value->capacity_and_long_flag & 1u) ||
      value->size != sizeof(from) - 1 || !value->data ||
      memcmp(value->data, from, sizeof(from) - 1) != 0)
    return 0;

  const size_t need = sizeof(to);
  const size_t allocation = (need + 15u) & ~(size_t)15u;
  char *replacement = op_new(allocation);
  if (!replacement) return 0;
  memcpy(replacement, to, sizeof(to));

  /* libc++ long-string layout is confirmed by the native initializer: it
   * allocates 0x20 bytes for the 23-byte token and stores cap|1 == 0x21. */
  value->capacity_and_long_flag = allocation | 1u;
  value->size = sizeof(to) - 1;
  value->data = replacement;
  return 1;
}

static void patch_title_credit(void) {
  /* This function runs after so_finalize(), which remaps the
   * shared-object pages to load_virtbase and makes load_base inaccessible.
   * A prior signature check at load_base+0xB668C faulted before
   * the game reached JNI_OnLoad.  Resolve operator new from the finalized RX
   * image instead; so_finalize() already rebases dynsym/dynstr for this use. */
  cxx_new_fn op_new =
      (cxx_new_fn)so_find_addr_rx(&cxx_mod, "_Znwm");
  static const uintptr_t objects[] = { 0x2d7c890, 0x2d7cd40, 0x2d7f8d0 };
  for (unsigned i = 0; i < sizeof(objects) / sizeof(objects[0]); i++)
    (void)replace_title_token_at(objects[i], op_new);
}

static void stage(const char *text) {
  static u64 previous_tick;
  const u64 now = armGetSystemTick();
  const u64 elapsed_ms = previous_tick
      ? (now - previous_tick) * 1000 / armGetSystemTickFreq() : 0;
  previous_tick = now;
  debugPrintf(">>> +%llums %s\n", (unsigned long long)elapsed_ms, text);
  /* Keep stage markers buffered. fsFileFlush after every marker was
   * synchronous SD I/O on the startup path and was included in the following
   * stage's timing. Fatal/watchdog/normal-exit paths still flush explicitly. */
}

static void initialize_wwise(void *lawn_app, jobject activity) {
  const uintptr_t app = (uintptr_t)lawn_app;
  void *driver = *(void **)(app + 0x18);
  if (!driver) fatal_error("PVZ2 did not create its Wwise audio driver.");
  if (*(uint8_t *)((uintptr_t)driver + 0x28)) {
    debugPrintf("wwise: driver already initialized\n");
    return;
  }

  /* GameAppInitialize leaves these Android-only slots uninitialized here.
   * Their non-zero garbage looks valid until Wwise dereferences the JVM. */
  JavaVM vm = fake_vm;
  jobject native_activity = activity;
  *(JavaVM *)(app + 0x840) = vm;
  *(jobject *)(app + 0x848) = native_activity;

  /* The optional StreamMgr scheduler is invalid in this Android build when
   * loaded outside ART.  Null selects Wwise's built-in scheduler. */
  uintptr_t *scheduler = (uintptr_t *)((uintptr_t)game_mod.load_virtbase +
                                       0x2e535d0 + 0x78);
  debugPrintf("wwise: StreamMgr scheduler %p -> default\n", (void *)*scheduler);
  *scheduler = 0;

  debugPrintf("wwise: driver=%p vm=%p activity=%p\n",
              driver, (void *)vm, native_activity);
  wwise_init_result = UINT32_MAX;
  wwise_probe_active = 1;
  const jboolean initialized =
      ((jboolean (*)(void *, JavaVM, jobject))
       ((uintptr_t)game_mod.load_virtbase + 0x226a6e4))
      (lawn_app, vm, native_activity);
  wwise_probe_active = 0;

  if (wwise_init_result == UINT32_MAX)
    debugPrintf("wwise: driver initialization failed before AK::SoundEngine::Init\n");
  else
    debugPrintf("wwise: driver=%u AK::SoundEngine::Init=%u\n",
                initialized, wwise_init_result);
  if (!initialized)
    fatal_error("Wwise initialization failed (AKRESULT %u).\nCheck pvz2_nx.log.",
                wwise_init_result);
}

static void *required_native(const char *name) {
  void *fn = jni_registered(name);
  if (!fn) fatal_error("PVZ2 did not register %s.\nCheck pvz2_nx.log.", name);
  return fn;
}

static void resolve_natives(void) {
  game_initialize = required_native("Native_GameAppInitialize");

  app_will_finish = jni_registered("Native_applicationWillFinishLaunching");
  app_did_finish = jni_registered("Native_applicationDidFinishLaunching");
  app_did_become_active = jni_registered("Native_applicationDidBecomeActive");
  app_will_resign_active = jni_registered("Native_applicationWillResignActive");
  app_did_enter_background = jni_registered("Native_applicationDidEnterBackground");
  app_will_become_foreground = jni_registered("Native_applicationWillBecomeForeground");
  create_lifecycle_observer = jni_registered("Native_createNativeApplicationLifecycleObserver");
  audio_gained = jni_registered("Native_NotifyAudioGainedFocus");
  audio_lost = jni_registered("Native_NotifyAudioLostFocus");
  set_current_network_status =
      (fn_network_status)jni_registered("Native_SetCurrentNetworkStatus");

  nimble_on_application_launch = (fn_nimble_lifecycle)so_find_addr_rx(
      &game_mod,
      "Java_com_ea_nimble_bridge_NimbleCppApplicationLifeCycle_onApplicationLaunch");
  nimble_on_application_resume = (fn_nimble_lifecycle)so_find_addr_rx(
      &game_mod,
      "Java_com_ea_nimble_bridge_NimbleCppApplicationLifeCycle_onApplicationResume");
  if (!nimble_on_application_launch || !nimble_on_application_resume)
    fatal_error("stock Nimble lifecycle JNI exports missing.");
  (void)0;
}

/* libPVZ2+0x272ABE4 is the Java
 * Map<String,String> -> native launch-method map converter.  A NULL Java
 * argument takes converter+0x43C directly, leaving the default-empty native
 * map initialized at entry; non-NULL input walks Java map entries and converts
 * their string key/value pairs.  Record the real Switch lifecycle edges at
 * their existing positions, but do not dispatch them until this port has
 * completed the stock Base.setupNimble component setup.  On Switch that setup
 * happens during the first native frame, after both platform edges were
 * already observed.  Replaying the pending edges here preserves
 * launch -> resume order without synthesizing any launch mode, identity, or
 * player data. */
/* proves the exact DEX Play Integrity NOT_AVAILABLE
 * callback is accepted by stock Nexus: performBootCheck() asks for
 * nexus.connect and the callback returns cleanly. The native path also shows the
 * native Nimble network service constructs HTTP clients and registers them
 * with NetworkClientManager, yet no Nexus/AgeCompliance transport reaches the
 * already-working Horizon HTTP layer. proved application-level
 * onApplicationResume does not enumerate NimbleCppComponent objects. Before
 * invoking any missing component lifecycle edge, inspect only the two native
 * network components and dump their exact setup/restore/suspend/resume vtable
 * targets. This path is observation-only: no lifecycle slot is called
 * and no request/response or identity state is synthesized. */
/* the DEX-correct anonymous environment
 * values are consumed, but no second NetworkServiceImpl::send() follows the
 * anonymous-auth field construction. Probe the anonymous component's
 * non-lifecycle vtable surface so the actual callsite can be correlated with
 * an interface entry during targeted diagnostics. No slot is invoked. */





/* proved that feeding 64 real Switch CSRNG bytes to
 * the embedded OpenSSL 3.3 RAND_seed() leaves RAND_status()==0, so the
 * provider DRBG remains unusable on this non-Android platform. the compatibility layer
 * hardware also proved that this exact libPVZ2 exports RAND_set_rand_method.
 *
 * OpenSSL's compatibility contract states that a non-default RAND_METHOD is
 * preferred even by RAND_bytes_ex()/RAND_priv_bytes_ex(). Install a minimal
 * hardware-backed RAND_METHOD before stock Nimble can enter TLS. All random
 * output comes directly from Switch csrngGetRandomBytes(); no deterministic
 * seed, fabricated entropy, token, identity, or server state is introduced. */
typedef struct compat_rand_method {
  int (*seed)(const void *buf, int num);
  int (*bytes)(unsigned char *buf, int num);
  void (*cleanup)(void);
  int (*add)(const void *buf, int num, double entropy);
  int (*pseudorand)(unsigned char *buf, int num);
  int (*status)(void);
} compat_rand_method;
_Static_assert(sizeof(compat_rand_method) == 6u * sizeof(void *),
               "OpenSSL RAND_METHOD ABI must be six function pointers");

typedef int (*compat_rand_set_method_fn)(const compat_rand_method *method);

static int compat_rand_method_attempted;
static int compat_csrng_ready;
static unsigned compat_rand_bytes_calls;

static int compat_rand_seed(const void *buf, int num) {
  (void)buf;
  (void)num;
  /* The platform CSPRNG is independently seeded by Horizon. Additional
   * application bytes are neither required nor substituted for real entropy. */
  return compat_csrng_ready;
}

static int compat_rand_fill(unsigned char *buf, int num, const char *kind) {
  if (num < 0 || (num > 0 && !buf)) return 0;

  Result rc = 0;
  if (num > 0) rc = csrngGetRandomBytes(buf, (size_t)num);
  const unsigned call = __atomic_add_fetch(&compat_rand_bytes_calls, 1u,
                                           __ATOMIC_RELAXED);
  if (call <= 8u || R_FAILED(rc)) {
    (void)0;
    debugLogFlush();
  }
  return R_SUCCEEDED(rc) ? 1 : 0;
}

static int compat_rand_bytes(unsigned char *buf, int num) {
  return compat_rand_fill(buf, num, "bytes");
}

static void compat_rand_cleanup(void) {
  /* csrng is owned for the full game lifetime by main(); nothing to free. */
}

static int compat_rand_add(const void *buf, int num, double entropy) {
  (void)buf;
  (void)num;
  (void)entropy;
  return compat_csrng_ready;
}

static int compat_rand_pseudorand(unsigned char *buf, int num) {
  return compat_rand_fill(buf, num, "pseudorand");
}

static int compat_rand_status(void) {
  return compat_csrng_ready;
}

static const compat_rand_method compat_switch_rand_method = {
  .seed = compat_rand_seed,
  .bytes = compat_rand_bytes,
  .cleanup = compat_rand_cleanup,
  .add = compat_rand_add,
  .pseudorand = compat_rand_pseudorand,
  .status = compat_rand_status,
};

static void install_embedded_openssl_rand_method(void) {
  if (compat_rand_method_attempted) return;
  compat_rand_method_attempted = 1;

  const uintptr_t set_method_addr =
      so_try_find_addr_rx(&game_mod, "RAND_set_rand_method");
  if (!set_method_addr) {
    debugPrintf("compat: OpenSSL RAND_METHOD FIX unavailable; no mutation\n");
    debugLogFlush();
    return;
  }

  /* Prove the Horizon CSRNG service is live before making it OpenSSL's
   * process-wide compatibility method. No bytes from this preflight escape. */
  unsigned char probe[32];
  const Result csrng_rc = csrngGetRandomBytes(probe, sizeof(probe));
  memset(probe, 0, sizeof(probe));
  if (R_FAILED(csrng_rc)) {
    debugPrintf("compat: OpenSSL RAND_METHOD FIX csrng preflight failed rc=0x%x; stock RAND left unchanged\n",
                (unsigned)csrng_rc);
    debugLogFlush();
    return;
  }
  compat_csrng_ready = 1;

  compat_rand_set_method_fn set_method =
      (compat_rand_set_method_fn)set_method_addr;
  const int set_rc = set_method(&compat_switch_rand_method);
  if (set_rc != 1) {
    compat_csrng_ready = 0;
    debugPrintf("compat: OpenSSL RAND_METHOD FIX install failed set_rc=%d; stock RAND retained\n",
                set_rc);
    debugLogFlush();
    return;
  }

  debugLogFlush();
}


/* hardware captured the exact OpenSSL 3 provider fetch
 * failure from SSL_CTX_new: inner_evp_generic_fetch requested HMAC in the
 * global default library context with no property query. The original APK
 * libPVZ2.so defines both OSSL_PROVIDER_load and ossl_default_provider_init,
 * so the default provider is embedded in this binary rather than an external
 * module dependency. OpenSSL's default provider is the provider that supplies
 * HMAC. Activate that exact built-in provider explicitly before stock Nimble
 * creates any TLS context, and keep the returned reference for process life.
 * Do not load legacy/FIPS providers, change properties, or fabricate crypto. */
typedef void *(*compat_provider_load_fn)(void *libctx, const char *name);

static int compat_default_provider_attempted;
static void *compat_default_provider_handle;

static void activate_embedded_openssl_default_provider(void) {
  if (compat_default_provider_attempted) return;
  compat_default_provider_attempted = 1;

  const uintptr_t load_addr =
      so_try_find_addr_rx(&game_mod, "OSSL_PROVIDER_load");
  if (!load_addr) {
    debugPrintf("compat: OpenSSL default-provider FIX unavailable OSSL_PROVIDER_load; no mutation\n");
    debugLogFlush();
    return;
  }

  compat_provider_load_fn provider_load =
      (compat_provider_load_fn)load_addr;
  void *provider = provider_load(NULL, "default");
  if (!provider) {
    debugPrintf("compat: OpenSSL default-provider FIX load failed; stock provider state preserved\n");
    debugLogFlush();
    return;
  }

  /* Keep the provider reference alive for the process lifetime. Unloading it
   * would make provider-backed algorithms unavailable again. */
  compat_default_provider_handle = provider;
  debugPrintf("compat: OpenSSL default-provider FIX loaded embedded provider=%p name=default\n",
              provider);
  debugLogFlush();
}


/* Prevent the embedded OpenSSL configuration loader from entering the Android
 * config path before PVZ2 constructors run. This uses the stock OpenSSL API
 * and does not patch OpenSSL one-time initialization state. */
#define OPENSSL_INIT_NO_LOAD_CONFIG 0x80ull

typedef int (*compat_openssl_init_crypto_fn_a)(uint64_t opts,
                                                 const void *settings);
typedef unsigned long (*compat_err_peek_last_error_fn_a)(void);
typedef void (*compat_err_error_string_n_fn_a)(unsigned long error,
                                                  char *buf, size_t len);
typedef void (*compat_err_clear_error_fn_a)(void);

static int compat_preconstructor_no_config_attempted;
static int compat_preconstructor_no_config_rc = -2;

static void compat_log_error_snapshot_a(
    const char *stage, compat_err_peek_last_error_fn_a peek,
    compat_err_error_string_n_fn_a to_string) {
  const unsigned long code = peek ? peek() : 0;
  if (!code) {
    debugPrintf("compat: %s error=0x0 queue_empty=1\n", stage);
    return;
  }
  char text[256];
  memset(text, 0, sizeof(text));
  if (to_string) to_string(code, text, sizeof(text));
  debugPrintf("compat: %s error=0x%lx text=%s\n",
              stage, code, text[0] ? text : "(unavailable)");
}


static void disable_openssl_config_before_pvz2_constructors(void) {
  if (compat_preconstructor_no_config_attempted) return;
  compat_preconstructor_no_config_attempted = 1;

  const uintptr_t init_addr =
      so_try_find_addr_rx(&game_mod, "OPENSSL_init_crypto");
  const uintptr_t peek_addr =
      so_try_find_addr_rx(&game_mod, "ERR_peek_last_error");
  const uintptr_t string_addr =
      so_try_find_addr_rx(&game_mod, "ERR_error_string_n");
  const uintptr_t clear_addr =
      so_try_find_addr_rx(&game_mod, "ERR_clear_error");
  compat_openssl_init_crypto_fn_a init_crypto =
      (compat_openssl_init_crypto_fn_a)init_addr;
  compat_err_peek_last_error_fn_a peek =
      (compat_err_peek_last_error_fn_a)peek_addr;
  compat_err_error_string_n_fn_a to_string =
      (compat_err_error_string_n_fn_a)string_addr;
  compat_err_clear_error_fn_a clear_error =
      (compat_err_clear_error_fn_a)clear_addr;

  (void)0;
  if (!init_crypto) {
    compat_preconstructor_no_config_rc = -1;
    debugPrintf("compat: preconstructor OPENSSL_init_crypto unavailable\n");
    debugLogFlush();
    return;
  }

  if (clear_error) clear_error();
  compat_preconstructor_no_config_rc =
      init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, NULL);
  (void)0;
  if (compat_preconstructor_no_config_rc != 1)
    compat_log_error_snapshot_a("preconstructor NO_LOAD_CONFIG", peek,
                                  to_string);
  if (clear_error) clear_error();
  (void)0;
  debugLogFlush();
}


/* the native path shows the embedded OpenSSL stack itself is
 * now healthy after the preconstructor NO_LOAD_CONFIG fix: OPENSSL_init_ssl,
 * EVP_MAC_fetch(HMAC), RAND_METHOD, and a direct SSL_CTX_new all succeed. Stock
 * Nimble/curl then advances to certificate verification and fails with
 * CURLE_PEER_FAILED_VERIFICATION messages (self-signed chain / unable to get
 * local issuer). Horizon Horizon ssl can verify the EA
 * Synergy host with peer-CA + hostname checks enabled and exposes a built-in
 * CA store.
 *
 * Bridge that real platform trust store into each embedded OpenSSL SSL_CTX.
 * Only Horizon certificates marked SslTrustedCertStatus_EnabledTrusted are
 * admitted. Verification remains enabled; no verify callback, hostname check,
 * server certificate, or error result is bypassed. The stock SSL_CTX_new ABI is
 * preserved by delegating to SSL_CTX_new_ex(NULL, NULL, method) and then adding
 * trusted roots to the context's X509_STORE. */
typedef void *(*compat_ssl_ctx_new_ex_fn)(void *libctx,
                                               const char *propq,
                                               const void *method);
typedef void *(*compat_ssl_ctx_get_cert_store_fn)(const void *ctx);
typedef void *(*compat_d2i_x509_fn)(void **x509,
                                        const unsigned char **data,
                                        long len);
typedef int (*compat_x509_store_add_cert_fn)(void *store, void *x509);
typedef void (*compat_x509_free_fn)(void *x509);
typedef void (*compat_err_clear_error_fn_b)(void);

static void *compat_horizon_ca_buffer;
static u32 compat_horizon_ca_buffer_size;
static u32 compat_horizon_ca_count;
static u32 compat_horizon_ca_trusted_count;
static int compat_horizon_ca_ready;
static int compat_ssl_ctx_hook_installed;
static unsigned compat_ssl_ctx_calls;

static compat_ssl_ctx_new_ex_fn compat_ssl_ctx_new_ex;
static compat_ssl_ctx_get_cert_store_fn compat_ssl_ctx_get_cert_store;
static compat_d2i_x509_fn compat_d2i_x509;
static compat_x509_store_add_cert_fn compat_x509_store_add_cert;
static compat_x509_free_fn compat_x509_free;
static compat_err_clear_error_fn_b compat_err_clear_error;

static void *ssl_ctx_new_with_horizon_trust(const void *method) {
  if (!compat_ssl_ctx_new_ex) return NULL;

  void *ctx = compat_ssl_ctx_new_ex(NULL, NULL, method);
  if (!ctx) return NULL;

  const unsigned call = ++compat_ssl_ctx_calls;
  unsigned attempted = 0;
  unsigned decoded = 0;
  unsigned added = 0;
  unsigned add_failed = 0;

  void *store = compat_ssl_ctx_get_cert_store
      ? compat_ssl_ctx_get_cert_store(ctx) : NULL;
  if (store && compat_horizon_ca_ready && compat_horizon_ca_buffer &&
      compat_d2i_x509 && compat_x509_store_add_cert &&
      compat_x509_free) {
    SslBuiltInCertificateInfo *certs =
        (SslBuiltInCertificateInfo *)compat_horizon_ca_buffer;
    for (u32 i = 0; i < compat_horizon_ca_count; ++i) {
      if (certs[i].status != SslTrustedCertStatus_EnabledTrusted ||
          !certs[i].cert_data || !certs[i].cert_size)
        continue;
      attempted++;
      const unsigned char *der = certs[i].cert_data;
      void *x509 = compat_d2i_x509(NULL, &der, (long)certs[i].cert_size);
      if (!x509) {
        if (compat_err_clear_error) compat_err_clear_error();
        continue;
      }
      decoded++;
      if (compat_x509_store_add_cert(store, x509) == 1)
        added++;
      else
        add_failed++;
      compat_x509_free(x509);
      /* Duplicate/root-store bookkeeping failures must not leak into curl's
       * later ERR_get_error() path. The trust decision itself is still made by
       * normal OpenSSL certificate verification. */
      if (compat_err_clear_error) compat_err_clear_error();
    }
  }

  if (call <= 16 || added != compat_horizon_ca_trusted_count) {
    debugPrintf("compat: SSL_CTX_new trust bridge call=%u method=%p ctx=%p store=%p horizon_ready=%d roots=%u attempted=%u decoded=%u added=%u add_failed=%u\n",
                call, method, ctx, store, compat_horizon_ca_ready,
                compat_horizon_ca_trusted_count, attempted, decoded,
                added, add_failed);
  }
  return ctx;
}

static void install_ssl_ctx_trust_hook(void) {
  if (compat_ssl_ctx_hook_installed) return;

  const uintptr_t ctx_new_addr =
      so_try_find_addr_rx(&game_mod, "SSL_CTX_new");
  const uintptr_t ctx_new_ex_addr =
      so_try_find_addr_rx(&game_mod, "SSL_CTX_new_ex");
  const uintptr_t get_store_addr =
      so_try_find_addr_rx(&game_mod, "SSL_CTX_get_cert_store");
  const uintptr_t d2i_addr =
      so_try_find_addr_rx(&game_mod, "d2i_X509");
  const uintptr_t add_addr =
      so_try_find_addr_rx(&game_mod, "X509_STORE_add_cert");
  const uintptr_t x509_free_addr =
      so_try_find_addr_rx(&game_mod, "X509_free");
  const uintptr_t clear_addr =
      so_try_find_addr_rx(&game_mod, "ERR_clear_error");

  compat_ssl_ctx_new_ex =
      (compat_ssl_ctx_new_ex_fn)ctx_new_ex_addr;
  compat_ssl_ctx_get_cert_store =
      (compat_ssl_ctx_get_cert_store_fn)get_store_addr;
  compat_d2i_x509 = (compat_d2i_x509_fn)d2i_addr;
  compat_x509_store_add_cert =
      (compat_x509_store_add_cert_fn)add_addr;
  compat_x509_free = (compat_x509_free_fn)x509_free_addr;
  compat_err_clear_error =
      (compat_err_clear_error_fn_b)clear_addr;

  (void)0;

  if (!ctx_new_addr || !ctx_new_ex_addr || !get_store_addr || !d2i_addr ||
      !add_addr || !x509_free_addr) {
    debugPrintf("compat: pre-finalize SSL_CTX_new hook skipped symbols_complete=0\n");
    debugLogFlush();
    return;
  }

  /* so_try_find_addr_rx() intentionally returns the future executable mapping.
   * Before so_finalize() that mapping is not the writable patch target. Patch
   * the corresponding byte offset in load_base; so_finalize() then maps those
   * already-patched bytes RX and the normal cache flush makes them executable. */
  const uintptr_t ctx_new_off =
      ctx_new_addr - (uintptr_t)game_mod.load_virtbase;
  const uintptr_t ctx_new_stage =
      (uintptr_t)game_mod.load_base + ctx_new_off;

  static const uint32_t expected[4] = {
      0xaa0003e2u, /* mov x2,x0 */
      0xaa1f03e0u, /* mov x0,xzr */
      0xaa1f03e1u, /* mov x1,xzr */
      0x1409c045u, /* b SSL_CTX_new_ex@plt */
  };
  const uint32_t *actual = (const uint32_t *)ctx_new_stage;
  int sig_ok = 1;
  for (unsigned i = 0; i < 4; ++i) {
    if (actual[i] != expected[i]) {
      sig_ok = 0;
      debugPrintf("compat: pre-finalize SSL_CTX_new signature mismatch +0x%x got=%08x expected=%08x\n",
                  i * 4, actual[i], expected[i]);
    }
  }

  if (sig_ok) {
    hook_arm64(ctx_new_stage,
               (uintptr_t)ssl_ctx_new_with_horizon_trust);
    compat_ssl_ctx_hook_installed = 1;
    (void)0;
  } else {
    debugPrintf("compat: pre-finalize SSL_CTX_new hook skipped signature mismatch\n");
  }
  debugLogFlush();
}

static void prepare_horizon_ca_trust(void) {
  if (compat_horizon_ca_ready) return;

  (void)0;
  Result rc = sslInitialize(3);
  (void)0;
  if (R_SUCCEEDED(rc)) {
    u32 ids[1] = { (u32)SslCaCertificateId_All };
    u32 need = 0;
    rc = sslGetCertificateBufSize(ids, 1, &need);
    (void)0;
    if (R_SUCCEEDED(rc) && need >= sizeof(SslBuiltInCertificateInfo)) {
      void *buf = calloc(1, need);
      if (buf) {
        u32 total = 0;
        rc = sslGetCertificates(buf, need, ids, 1, &total);
        (void)0;
        if (R_SUCCEEDED(rc) && total &&
            (size_t)total <= (size_t)need / sizeof(SslBuiltInCertificateInfo)) {
          SslBuiltInCertificateInfo *certs =
              (SslBuiltInCertificateInfo *)buf;
          u32 trusted = 0, not_trusted = 0, removed = 0, revoked = 0, invalid = 0;
          for (u32 i = 0; i < total; ++i) {
            switch ((SslTrustedCertStatus)certs[i].status) {
              case SslTrustedCertStatus_EnabledTrusted: trusted++; break;
              case SslTrustedCertStatus_EnabledNotTrusted: not_trusted++; break;
              case SslTrustedCertStatus_Removed: removed++; break;
              case SslTrustedCertStatus_Revoked: revoked++; break;
              default: invalid++; break;
            }
          }
          compat_horizon_ca_buffer = buf;
          compat_horizon_ca_buffer_size = need;
          compat_horizon_ca_count = total;
          compat_horizon_ca_trusted_count = trusted;
          compat_horizon_ca_ready = trusted != 0;
          buf = NULL;
          debugPrintf("compat: Horizon CA inventory trusted=%u not_trusted=%u removed=%u revoked=%u invalid=%u ready=%d\n",
                      trusted, not_trusted, removed, revoked, invalid,
                      compat_horizon_ca_ready);
        }
        free(buf);
      } else {
        debugPrintf("compat: Horizon CA allocation failed bytes=%u\n", need);
      }
    }
    sslExit();
  }

  (void)0;
  debugLogFlush();
}

/* LOGGER / FALLBACK PROBE: retain the compatibility layer's exact embedded-OpenSSL
 * base-init diagnosis and the compatibility layer's independent Horizon TLS handshake. If the
 * verified Horizon handshake succeeds, additionally prove encrypted application
 * I/O with one harmless HEAD / request to the same Synergy host. The probe logs
 * only the first HTTP response line and never calls the real getDirection API.
 * On handshake failure, also query Horizon's verify-cert error and needed server
 * certificate buffer size so certificate/protocol failures are distinguishable.
 * No auth token, identity, player ID, Synergy response, or game state is changed. */


/* the native path shows the embedded default provider is live,
 * FIPS default properties are disabled, and OSSL_OP_MAC directly advertises
 * HMAC. The native path also shows our RAND_METHOD and the compatibility layer
 * provider activation happened only after stock NetworkClientManager.restore()
 * had already initialized curl/OpenSSL and started its worker thread. Prepare
 * both proven OpenSSL compatibility contracts before Base.setupNimble can hit
 * the exact DEX NetworkClientManager restore boundary. No provider/property,
 * cipher, auth, player, profile, or Joust fabrication is introduced here. */
static int compat_openssl_pre_network_prepared;

static void prepare_embedded_openssl_before_network(void) {
  if (compat_openssl_pre_network_prepared) return;
  compat_openssl_pre_network_prepared = 1;

  (void)0;
  install_embedded_openssl_rand_method();
  activate_embedded_openssl_default_provider();
  debugPrintf("compat: OpenSSL pre-network FIX end rand_attempted=%d provider_attempted=%d provider=%p\n",
              compat_rand_method_attempted,
              compat_default_provider_attempted,
              compat_default_provider_handle);
  debugLogFlush();
}



static void pump_nimble_startup_lifecycle(void) {
  if (!pvz2_nimble_setup_ready()) return;
  if (nimble_resume_edge_delivered && nimble_nexus_restore_stage_complete &&
      !nimble_launch_edge_pending && !nimble_resume_edge_pending)
    return;

  if (nimble_launch_edge_pending && !nimble_launch_edge_delivered) {
    (void)0;
    debugLogFlush();
    nimble_on_application_launch(fake_env, NULL, NULL);
    nimble_launch_edge_pending = 0;
    nimble_launch_edge_delivered = 1;
    (void)0;
    debugLogFlush();
  }

  /* the stock Persistence bridge is the evidence-backed platform
   * prerequisite. Treat its setup-time behavior as a preflight for the one
   * active NexusService restore experiment. Any unexpected method/type,
   * capacity failure, or failed atomic commit suppresses restore and preserves
   * the already-proven application resume baseline. */
  if (nimble_launch_edge_delivered && !nimble_nexus_restore_stage_complete) {
    static const char nexus_id[] = "com.ea.nimble.cpp.nexusservice";
    pvz2_nimble_persistence_log_summary();

    if (!pvz2_nimble_persistence_preflight_ok()) {
      debugPrintf("compat: NexusService restore suppressed: Persistence preflight failed; continuing proven application resume baseline\n");
      nimble_nexus_restore_stage_complete = 1;
      debugLogFlush();
    } else {
      jobject component = jni_make_object_class_string(
          "com/ea/nimble/bridge/NimbleCppComponentRegistrar$NimbleCppComponent",
          nexus_id);
      if (!component) {
        debugPrintf("compat: NexusService restore suppressed: wrapper allocation failed\n");
        nimble_nexus_restore_stage_complete = 1;
        debugLogFlush();
      } else if (!nimble_probe_native_component(nexus_id, "pre-restore")) {
        debugPrintf("compat: NexusService restore suppressed: native component missing\n");
        nimble_nexus_restore_stage_complete = 1;
        debugLogFlush();
        jni_free_wrapper(component);
      } else {
        (void)0;
        debugLogFlush();
        nimble_restore_cpp_component(component);
        nimble_nexus_restore_stage_complete = 1;
        (void)0;
        nimble_probe_native_component(nexus_id, "post-restore");
        pvz2_nimble_persistence_log_summary();
        debugLogFlush();
        jni_free_wrapper(component);
      }
    }
  }

  if (nimble_resume_edge_pending && nimble_launch_edge_delivered &&
      nimble_nexus_restore_stage_complete && !nimble_resume_edge_delivered) {
    (void)0;
    debugLogFlush();
    nimble_on_application_resume(fake_env, NULL, NULL);
    nimble_resume_edge_pending = 0;
    nimble_resume_edge_delivered = 1;
    (void)0;
    debugLogFlush();
  }
}

static void deliver_network_status(jobject activity) {
  /* AndroidHttpProxy registers this native during applicationWillFinishLaunching,
   * which happens after the initial JNI_OnLoad native lookup. */
  if (!set_current_network_status)
    set_current_network_status =
        (fn_network_status)jni_registered("Native_SetCurrentNetworkStatus");
  if (!set_current_network_status) return;

  const int status = pvz2_current_network_status();
  if (status == last_network_status) return;

  debugPrintf("PVZ2 network status -> %d\n", status);
  (void)0;
  set_current_network_status(fake_env, activity, status);
  /* proves callback 4's target at +0x23FAB7C ignores notification
   * payload fields and re-queries Nimble's current network service before
   * notifying listeners. Preserve the real Switch status transition, then
   * let pvz2.c enqueue the stock networkStatusChange broadcast once Nimble
   * setup is ready. */
  pvz2_nimble_network_status_changed(status);
  last_network_status = status;
}

static void resolve_frame_natives(void) {
  surface_created = required_native("Native_onSurfaceCreated");
  surface_changed = required_native("Native_onSurfaceChanged");
  draw_frame = required_native("Native_onDrawFrame");
}

static void load_modules(void) {
  void *base = heap_so_base();
  const size_t total = heap_so_limit();
  if (!base) fatal_error("Could not reserve the shared-object load zone.");

  stage("load libc++");
  int rc = so_load(&cxx_mod, CXX_SO_NAME, base, total);
  if (rc < 0) fatal_error("Could not load " CXX_SO_NAME " (%d).", rc);
  stage("loaded libc++");

  void *nimble_base = (void *)ALIGN_MEM((uintptr_t)base + cxx_mod.load_size, 0x1000);
  size_t nimble_limit = total - ((uintptr_t)nimble_base - (uintptr_t)base);
  stage("load Nimble");
  rc = so_load(&nimble_mod, NIMBLE_SO_NAME, nimble_base, nimble_limit);
  if (rc < 0) fatal_error("Could not load " NIMBLE_SO_NAME " (%d).", rc);
  stage("loaded Nimble");

  void *game_base = (void *)ALIGN_MEM((uintptr_t)nimble_base + nimble_mod.load_size, 0x1000);
  size_t game_limit = total - ((uintptr_t)game_base - (uintptr_t)base);
  stage("load PVZ2");
  rc = so_load(&game_mod, SO_NAME, game_base, game_limit);
  if (rc < 0) fatal_error("Could not load " SO_NAME " (%d).", rc);
  stage("loaded PVZ2");

  stage("relocate modules");
  update_imports();
  so_relocate(&cxx_mod);
  so_relocate(&nimble_mod);
  so_relocate(&game_mod);
  stage("resolve imports");
  so_resolve(&cxx_mod, dynlib_functions, dynlib_numfunctions, 1);
  so_resolve(&nimble_mod, dynlib_functions, dynlib_numfunctions, 1);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  stage("configure Nimble bridge");
  configure_nimble_bridge();
  stage("patch string-map defaults");
  patch_missing_string_map_at();
  install_joust_availability_guard();
  install_archive_cleanup_fix();
  hide_google_play_menu_button();
  stage("initialize Glu services");
  hook_glu_service_constructor();
  stage("observe loading state");
  trace_title_loading_state();
  stage("initialize purchase driver");
  hook_purchase_driver_constructor();
  stage("initialize Wwise");
  trace_wwise_init();
  stage("touch diagnostics disabled");
  stage("provide screen size");
  hook_arm64((uintptr_t)game_mod.load_base + 0x23cf6f8,
             (uintptr_t)get_screen_size);
  install_ssl_ctx_trust_hook();
  stage("finalize modules");
  so_finalize(&cxx_mod);
  so_finalize(&nimble_mod);
  so_finalize(&game_mod);
  so_flush_caches(&cxx_mod);
  so_flush_caches(&nimble_mod);
  so_flush_caches(&game_mod);

  stage("construct libc++");
  so_execute_init_array(&cxx_mod);
  stage("construct Nimble");
  so_execute_init_array(&nimble_mod);
  disable_openssl_config_before_pvz2_constructors();
  stage("construct PVZ2");
  so_execute_init_array(&game_mod);
  patch_title_credit();
  stage("constructors complete");
  so_free_temp(&cxx_mod);
  so_free_temp(&nimble_mod);
  so_free_temp(&game_mod);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  mkdir("sdmc:/switch", 0777);
  mkdir(DATA_DIR, 0777);
  mkdir(DATA_DIR "/cache", 0777);
  mkdir(DATA_DIR "/No_Backup", 0777);
  /* Remove the obsolete mkdir-shim artifact if it is empty. Never delete a
   * non-empty root directory: rmdir simply fails safely in that case. */
  rmdir("sdmc:/No_Backup");
  mkdir(DATA_DIR "/tags", 0777);
  mkdir(DATA_DIR "/payloads", 0777);
  if (chdir(DATA_DIR) < 0) fatal_error("Could not enter " DATA_DIR ".");
  crash_log_open();
  if (installer_prepare_game_files() < 0)
    fatal_error("%s", installer_last_error());
  cpu_boost(1);
  debugPrintf("pvz2_nx build: release\n");
  debugPrintf("joust: stock AndroidHttpTransaction HTTP-error contract restored; HTTP responses use Response/Data/Complete, transport exceptions use TransactionError; PvZ2Web savedelta suppression preserved\n");
  (void)0;

  /* diagnostic: stock Android asks for its player-identity UUID during startup,
   * before the rest of the port happens to touch Horizon CSRNG.  diagnostic
   * therefore saw LibnxError_NotInitialized (0xe401) for the only
   * Util_GetUUIDString call that matters.  Own the libnx CSRNG service for
   * the full game lifetime so stock UUID APIs work from their first call. */
  const Result identity_rng_rc = csrngInitialize();
  (void)0;

  const Result net_rc = socketInitializeDefault();
  debugPrintf("socketInitializeDefault -> 0x%x\n", (unsigned)net_rc);
  debugLogFlush();
  tls_setup_guard();
  pin_current_thread();
  stage("open PVZ2 asset archive");
  if (obb_open(OBB_NAME) < 0)
    fatal_error("Could not index " OBB_NAME ".");

  stage("load modules");
  load_modules();
  stage("create EGL context");
  egl_init_context();

  jni_init();
  jint (*nimble_onload)(JavaVM, void *) =
      (void *)so_find_addr_rx(&nimble_mod, "JNI_OnLoad");
  jint (*game_onload)(JavaVM, void *) =
      (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");
  if (!nimble_onload)
    fatal_error("JNI_OnLoad was not found in " NIMBLE_SO_NAME ".");
  const jint nimble_jni_version = nimble_onload(fake_vm, NULL);
  if (nimble_jni_version != JNI_VERSION_1_6)
    fatal_error("libNimble JNI_OnLoad failed/version mismatch: 0x%x.",
                (unsigned)nimble_jni_version);

  /* libNimble 13.3.1 has only one JavaVM global. JNI_OnLoad stores x0 at
   * .bss+0x8F68 and both EA::Nimble::getEnv()/findClass() load that exact
   * slot. diagnostic's fault at findClass+0x3C (libNimble+0x9A8) proves the
   * value observed there was NULL. Verify the Android contract explicitly
   * before entering the deferred Nexus registrar. Fail closed if the stock
   * JNI_OnLoad assignment is not present; do not synthesize VM or identity state. */
  {
    JavaVM *const nimble_vm_slot =
        (JavaVM *)((uintptr_t)nimble_mod.load_virtbase + 0x8f68);
    JavaVM vm_seen = *nimble_vm_slot;
    (void)0;
    if (vm_seen != fake_vm)
      fatal_error("libNimble JavaVM slot mismatch after stock JNI_OnLoad.");

    JNIEnv (*nimble_get_env)(void) =
        (void *)so_find_addr_rx(&nimble_mod, "_ZN2EA6Nimble6getEnvEv");
    if (!nimble_get_env)
      fatal_error("EA::Nimble::getEnv was not found.");
    JNIEnv env_seen = nimble_get_env();
    (void)0;
    if (env_seen != fake_env)
      fatal_error("EA::Nimble::getEnv did not return the active fake JNIEnv.");
    debugLogFlush();
  }

  /* diagnostic: recovered the BaseNativeCallback(I) receiver IDs, but
   * libNimble/PVZ2 never called RegisterNatives for BaseNativeCallback.  Before
   * delivering any notification, enumerate the already-loaded dynamic symbol
   * tables for the exact JNI/exported callback bridge.  This is observational
   * only: no receiver is fired and no identity/player state is changed. */


  /* diagnostic: proved that callback id 1 lookup and JNI dispatch are
   * correct, but an empty Object[] reaches the genuine BridgeCallback and then
   * faults at libPVZ2+0x271AE3C (FAR=0). The callback object seen in the crash
   * uses vtable +0x2CD7208, whose first dispatch method is +0x271AE08, and the
   * callback-1 object stores +0x23FE680 as its target thunk. Do not deliver the
   * notification again yet. Dump only those two proven stock code regions and
   * nearby dynamic symbols so the next hardware log reveals the exact payload
   * contract before any further lifecycle event is fired. */
  pvz2_set_basenativecallback_native(NULL);

  /* proves callback.payload2.convert is the stock Java
   * Map<String,Object> -> Nimble map bridge: it walks entrySet/Iterator,
   * converts String keys, and recursively converts values.  Callback 1's
   * target at +0x23FE680 does not consume the converted payload.  Re-arm the
   * exact exported JNI bridge and let the Java shell deliver only callback 1
   * with Object[]{ exact action string, empty Map }. */
  {
    void *native_callback = (void *)so_find_addr_rx(
        &game_mod,
        "Java_com_ea_nimble_bridge_BaseNativeCallback_nativeCallback");
    if (!native_callback)
      fatal_error("stock BaseNativeCallback.nativeCallback export missing.");
    pvz2_set_basenativecallback_native(native_callback);
    (void)0;
    debugLogFlush();
  }

  /* Slot 1175 is the stock Nexus component registrar. Keep 1176+ quarantined,
   * but run this one only after the libNimble VM/env preflight above. */
  {
    const uintptr_t registrar_addr =
        (uintptr_t)game_mod.load_virtbase + 0x26f8b34;
    const uint32_t *registrar_sig = (const uint32_t *)registrar_addr;
    static const uint32_t expected_sig[4] = {
        0xd10183ffu, 0xa9037bfdu, 0xf90023f5u, 0xa9054ff4u
    };
    if (memcmp(registrar_sig, expected_sig, sizeof(expected_sig)) != 0)
      fatal_error("stock Nexus registrar signature mismatch at libPVZ2+0x26F8B34.");
    (void)0;
    debugLogFlush();
    ((void (*)(void))registrar_addr)();
    (void)0;
    (void)0;
    debugLogFlush();
  }

  /* every quarantined stock constructor 1176-1182
   * returns safely in this Android-correct window and registers the remaining
   * network/connector/tracking services, but none of them creates nexus.jwk.
   * Preserve that now-proven full stock constructor baseline even if the JWT
   * gate handled in pvz2.c makes JWK appear earlier during the Nexus registrar.
   * This keeps the JWT config result as the only behavioral variable versus
   * the verified path. No profile/player/Joust state is written here. */
  {
    const size_t deferred_count = so_pvz2_deferred_init_count();
    (void)0;
    int jwk_present = nimble_probe_native_component(
        "com.ea.nimble.cpp.nexus.jwk", "before-init-1176plus");
    for (size_t i = 0; i < deferred_count; ++i) {
      int slot = -1;
      uintptr_t target = 0;
      if (!so_pvz2_deferred_init_get(i, &slot, &target)) continue;
      if (slot <= 1175) continue;

      const uintptr_t base = (uintptr_t)game_mod.load_virtbase;
      const uintptr_t end = base + game_mod.load_size;
      if (target < base || target + 16 > end ||
          !memory_range_readable(target, 16)) {
        debugPrintf("compat: deferred init[%d] target=%p outside readable libPVZ2 RX image; skipped\n",
                    slot, (void *)target);
        debugLogFlush();
        continue;
      }

      debugLogFlush();
      ((void (*)(void))target)();
      (void)0;
      debugLogFlush();

      char stage_name[48];
      snprintf(stage_name, sizeof(stage_name), "after-init-%d", slot);
      const int jwk_now = nimble_probe_native_component(
          "com.ea.nimble.cpp.nexus.jwk", stage_name);
      if (!jwk_present && jwk_now) {
        (void)0;
        debugLogFlush();
      }
      if (jwk_now) jwk_present = 1;
    }
    (void)0;
    debugLogFlush();
  }

  if (!game_onload) fatal_error("JNI_OnLoad was not found in " SO_NAME ".");

  stage("JNI_OnLoad");
  game_onload(fake_vm, NULL);
  (void)0;
  debugLogFlush();
  resolve_natives();

  jobject activity = jni_make_activity();
  jobject surface_view = jni_make_surface();
  /* Preserve the Java wrapper identities from Native_GameAppInitialize.  The
   * native runtime may call GetObjectClass on these instances before looking
   * up methods; handing every service java/lang/Object loses that information. */
  jobject http = jni_make_object_class(
      "com/popcap/SexyAppFramework/AndroidHttpProxy");
  jobject cloud = jni_make_object_class(
      "com/popcap/SexyAppFramework/cloud/Cloud");
  jobject play_connect = jni_make_object_class(
      "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect");
  jobject achievements = jni_make_object_class(
      "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements");
  jobject leaderboard = jni_make_object_class(
      "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard");
  /* Switch has no Android notification manager.  Passing a fake notification
   * object makes the native Android bridge enter its warning path once per
   * channel operation (SharedNotificationManagerRequired).  NULL is the
   * platform-level unavailable-service value, so Native_GameAppInitialize
   * skips notification setup entirely. */

  stage("initialize game");
  stage("create lifecycle observer");
  if (create_lifecycle_observer) create_lifecycle_observer(fake_env, activity);
  watchdog_start(&game_mod);
  stage("will finish launching");
  if (app_will_finish) app_will_finish(fake_env, activity);
  void *lawn_app = *(void **)((uintptr_t)game_mod.load_virtbase + 0x2dca2d0);
  void *app_driver = lawn_app ? *(void **)((uintptr_t)lawn_app + 0x10) : NULL;
  if (!app_driver) fatal_error("PVZ2 did not create its Android app driver.");
  ((void (*)(void *))((uintptr_t)game_mod.load_virtbase + 0x23c92c4))(app_driver);
  stage("GameAppInitialize");
  const jboolean initialized = game_initialize(fake_env, activity, surface_view, http,
      cloud, play_connect, achievements, leaderboard, NULL, activity);
  if (!initialized) fatal_error("Native_GameAppInitialize returned false.");
  stage("GameAppInitialize complete");
  stage("deliver network status");
  deliver_network_status(activity);
  stage("initialize Wwise");
  initialize_wwise(lawn_app, activity);
  stage("resolve renderer");
  resolve_frame_natives();
  stage("did finish launching");
  if (app_did_finish) app_did_finish(fake_env, activity);
  nimble_launch_edge_pending = 1;
  (void)0;
  debugLogFlush();
  /* Base.setupNimble is entered from the surface-created path on
   * hardware. Install the proven OpenSSL compatibility contracts before that
   * call can dispatch the stock NetworkClientManager restore/curl init. */
prepare_embedded_openssl_before_network();
  prepare_horizon_ca_trust();
  stage("surface created");
  surface_created(fake_env, surface_view);
  stage("surface changed");
  surface_changed(fake_env, surface_view, screen_width, screen_height);
  stage("became active");
  if (app_did_become_active) app_did_become_active(fake_env, activity);
  pvz2_nimble_set_main_application_active(1);
  nimble_resume_edge_pending = 1;
  (void)0;
  debugLogFlush();
  stage("audio focus");
  if (audio_gained) audio_gained(fake_env, activity);

  stage("first frame");
  while (appletMainLoop() && !jni_quit_requested) {
    padUpdate_all();
    if (should_quit()) break;
    if (handle_dock_change(&screen_width, &screen_height))
      surface_changed(fake_env, surface_view, screen_width, screen_height);
    deliver_network_status(activity);
    pump_nimble_startup_lifecycle();
    /* Android sendBroadcast() returns before BroadcastReceiver.onReceive().
     * Pump queued Nimble broadcasts only after the sender's native stack has
     * unwound, never synchronously from Utility.sendBroadcastSerializable. */
    pvz2_nimble_broadcast_pump();
    /* Recycle any released audout buffers before a potentially expensive
     * render as well as after swap, reducing refill latency without changing
     * the device cadence or appending extra buffers. */
    opensles_pump();
    /* Network requests started by native hooks (including the EA email
     * bridge) cannot rely on the Android input-event callback to run again. */
    pvz2_http_pump();
    update_joust_server_time();
    maintain_online_identity_validation();
    draw_frame(fake_env, surface_view);
    complete_local_catalog_refresh();
    initialize_glu_analytics_id();
    g_frame_count++;
    /* Watchdog/crash/exit paths flush explicitly.  Avoid a synchronous SD flush
     * every five seconds during UI-heavy menus; the RAM log buffer still drains
     * automatically when full. */
    if (g_frame_count % 1800 == 0 && PVZ2_ENABLE_VERBOSE_RUNTIME_LOG)
      debugLogFlush();
    egl_swap_buffers();
    opensles_pump();
  }

  if (audio_lost) audio_lost(fake_env, activity);
  pvz2_nimble_set_main_application_active(0);
  if (app_will_resign_active) app_will_resign_active(fake_env, activity);
  if (app_did_enter_background) app_did_enter_background(fake_env, activity);
  (void)app_will_become_foreground;
  opensles_shutdown();
  obb_close();
  egl_exit_context();
  if (R_SUCCEEDED(net_rc)) socketExit();
  if (R_SUCCEEDED(identity_rng_rc)) csrngExit();
  cpu_boost(0);
  debugLogSync();
  return 0;
}
