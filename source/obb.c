/* obb.c -- reader for PVZ2's 1BSR/PGSR asset archive. */

#include <switch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "obb.h"
#include "util.h"
#include "game_version.h"
#include "config.h"

#define OBB_TABLE_ENTRY_SIZE 0xcc
#define OBB_MAX_NAME 512

typedef struct {
  char *name;
  uint64_t offset;
  uint32_t packed_size;
  uint32_t size;
} ObbEntry;

static FILE *g_file;
static uint64_t g_file_size;
static ObbEntry *g_entries;
static size_t g_entry_count;
static size_t g_entry_capacity;
static Mutex g_lock;
static int g_lock_ready;

typedef struct {
  char name[OBB_MAX_NAME];
  uint8_t *bytes;
  size_t size;
  uint64_t stamp;
  int used;
} ObbHotAsset;

static ObbHotAsset g_hot_assets[OBB_HOT_CACHE_SLOTS];
static size_t g_hot_asset_bytes;
static uint64_t g_hot_asset_stamp;

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v) {
  wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t fnv1a32(const void *data, size_t size) {
  const uint8_t *bytes = data;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

static int read_at(uint64_t offset, void *dst, size_t size) {
  if (!g_file || offset > g_file_size || size > g_file_size - offset)
    return -1;
  if (fseek(g_file, (long)offset, SEEK_SET) != 0)
    return -1;
  return fread(dst, 1, size, g_file) == size ? 0 : -1;
}


static size_t canonicalize(const char *src, char *dst, size_t cap) {
  size_t n = 0;
  int slash = 0;
  if (!src || !cap) return 0;
  while (*src == '/' || *src == '\\') src++;
  while (*src && n + 1 < cap) {
    char c = *src++;
    if (c == '\\' || c == '/') {
      if (slash) continue;
      slash = 1;
      dst[n++] = '/';
    } else {
      slash = 0;
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      dst[n++] = c;
    }
  }
  while (n && dst[n - 1] == '/') n--;
  dst[n] = 0;
  return n;
}

static void hot_cache_clear_locked(void) {
  for (size_t i = 0; i < OBB_HOT_CACHE_SLOTS; ++i) {
    free(g_hot_assets[i].bytes);
    g_hot_assets[i] = (ObbHotAsset){0};
  }
  g_hot_asset_bytes = 0;
  g_hot_asset_stamp = 0;
}

static void *hot_cache_copy_locked(const char *key, size_t *out_size) {
  if (!key || !*key) return NULL;
  for (size_t i = 0; i < OBB_HOT_CACHE_SLOTS; ++i) {
    ObbHotAsset *asset = &g_hot_assets[i];
    if (!asset->used || strcmp(asset->name, key)) continue;
    uint8_t *copy = malloc(asset->size ? asset->size : 1);
    if (!copy) return NULL;
    if (asset->size) memcpy(copy, asset->bytes, asset->size);
    asset->stamp = ++g_hot_asset_stamp;
    if (out_size) *out_size = asset->size;
    return copy;
  }
  return NULL;
}

static size_t hot_cache_oldest_slot_locked(size_t excluded) {
  size_t selected = SIZE_MAX;
  uint64_t oldest = UINT64_MAX;
  for (size_t i = 0; i < OBB_HOT_CACHE_SLOTS; ++i) {
    if (i == excluded || !g_hot_assets[i].used) continue;
    if (g_hot_assets[i].stamp < oldest) {
      oldest = g_hot_assets[i].stamp;
      selected = i;
    }
  }
  return selected;
}

static void hot_cache_evict_locked(size_t slot) {
  if (slot >= OBB_HOT_CACHE_SLOTS || !g_hot_assets[slot].used) return;
  if (g_hot_assets[slot].size <= g_hot_asset_bytes)
    g_hot_asset_bytes -= g_hot_assets[slot].size;
  else
    g_hot_asset_bytes = 0;
  free(g_hot_assets[slot].bytes);
  g_hot_assets[slot] = (ObbHotAsset){0};
}

static void hot_cache_store_locked(const char *key, const void *bytes,
                                   size_t size) {
  if (!key || !*key || !bytes || !size ||
      size > OBB_HOT_CACHE_MAX_ASSET ||
      size > OBB_HOT_CACHE_MAX_BYTES)
    return;

  size_t slot = SIZE_MAX;
  for (size_t i = 0; i < OBB_HOT_CACHE_SLOTS; ++i) {
    if (g_hot_assets[i].used && !strcmp(g_hot_assets[i].name, key)) {
      slot = i;
      hot_cache_evict_locked(i);
      break;
    }
    if (slot == SIZE_MAX && !g_hot_assets[i].used) slot = i;
  }
  if (slot == SIZE_MAX) {
    slot = hot_cache_oldest_slot_locked(SIZE_MAX);
    hot_cache_evict_locked(slot);
  }
  while (g_hot_asset_bytes > OBB_HOT_CACHE_MAX_BYTES - size) {
    const size_t victim = hot_cache_oldest_slot_locked(slot);
    if (victim == SIZE_MAX) return;
    hot_cache_evict_locked(victim);
  }

  uint8_t *copy = malloc(size);
  if (!copy) return;
  memcpy(copy, bytes, size);
  ObbHotAsset *asset = &g_hot_assets[slot];
  snprintf(asset->name, sizeof(asset->name), "%s", key);
  asset->bytes = copy;
  asset->size = size;
  asset->stamp = ++g_hot_asset_stamp;
  asset->used = 1;
  g_hot_asset_bytes += size;
}

static int add_name(const char *name, uint64_t offset, uint32_t packed_size,
                    uint32_t size) {
  char key[OBB_MAX_NAME];
  if (!canonicalize(name, key, sizeof(key))) return 0;
  if (g_entry_count == g_entry_capacity) {
    const size_t capacity = g_entry_capacity ? g_entry_capacity * 2 : 1024;
    ObbEntry *entries = realloc(g_entries, capacity * sizeof(*entries));
    if (!entries) return -1;
    g_entries = entries;
    g_entry_capacity = capacity;
  }
  g_entries[g_entry_count].name = strdup(key);
  if (!g_entries[g_entry_count].name) return -1;
  g_entries[g_entry_count].offset = offset;
  g_entries[g_entry_count].packed_size = packed_size;
  g_entries[g_entry_count].size = size;
  g_entry_count++;
  return 0;
}

static size_t decode_name(const uint8_t *data, size_t size, size_t pos,
                          char *out, size_t cap) {
  size_t n = 0;
  while (pos + 4 <= size && n + 1 < cap) {
    uint8_t c = data[pos];
    pos += 4;
    if (!c) {
      out[n] = 0;
      return pos;
    }
    out[n++] = (char)c;
  }
  out[0] = 0;
  return 0;
}

static int add_asset_names(const char *group, const char *name,
                           uint64_t offset, uint32_t packed_size,
                           uint32_t size) {
  if (add_name(name, offset, packed_size, size) < 0)
    return -1;
  if (group && *group) {
    char full[OBB_MAX_NAME];
    const size_t group_len = strlen(group), name_len = strlen(name);
    if (group_len + 1 + name_len >= sizeof(full)) return 0;
    memcpy(full, group, group_len);
    full[group_len] = '/';
    memcpy(full + group_len + 1, name, name_len + 1);
    if (add_name(full, offset, packed_size, size) < 0)
      return -1;
  }
  return 0;
}

static int parse_pgsr(uint64_t archive_offset, uint32_t archive_size,
                      const char *group) {
  uint8_t header[80];
  if (archive_size < sizeof(header) || read_at(archive_offset, header, sizeof(header)) < 0)
    return -1;
  if (memcmp(header, "pgsr", 4) != 0)
    return -1;

  const uint32_t type = rd32(header + 16);
  const uint32_t base = rd32(header + 20);
  uint32_t stream_offset = 0, packed_size = 0, size = 0;
  for (int i = 0; i < 2; i++) {
    const uint8_t *stream = header + 24 + i * 16;
    if (rd32(stream + 8)) {
      stream_offset = rd32(stream + 0);
      packed_size = rd32(stream + 4);
      size = rd32(stream + 8);
    }
  }
  const uint32_t info_size = rd32(header + 72);
  const uint32_t info_offset = rd32(header + 76);
  /* PVZ2 stores marker containers such as DevFolderPresent as empty type-3
   * records: they have no filename and no decoded stream. They are metadata,
   * not assets, so do not reject the otherwise valid archive. */
  if (type == 3 && (!size || !info_offset)) {
    debugPrintf("obb: skip empty PGSR marker %s\n", group ? group : "(unnamed)");
    return 0;
  }
  if (!info_size || (uint64_t)info_offset + info_size > archive_size)
    return -1;

  uint8_t *info = malloc(info_size);
  if (!info || read_at(archive_offset + info_offset, info, info_size) < 0) {
    free(info);
    return -1;
  }

  int rc = 0;
  size_t pos = 0;
  if (type == 3) {
    char name[OBB_MAX_NAME];
    if (!decode_name(info, info_size, 0, name, sizeof(name)) || !size) {
      rc = -1;
    } else {
      /* Type 3 stream offsets are relative to the PGSR record itself. */
      const uint64_t data_offset = archive_offset + stream_offset;
      if (data_offset + packed_size > archive_offset + archive_size)
        rc = -1;
      else
        rc = add_asset_names(group, name, data_offset, packed_size, size);
    }
  } else if (type <= 1) {
    /* Type 0/1 records contain several files inside one PGSR stream. */
    while (pos + 16 <= info_size) {
      char name[OBB_MAX_NAME];
      size_t next = decode_name(info, info_size, pos, name, sizeof(name));
      if (!next) break;
      pos = next;
      if (pos + 12 > info_size) break;
      const uint32_t marker = rd32(info + pos);
      uint32_t inner_offset = rd32(info + pos + 4);
      uint32_t inner_size = rd32(info + pos + 8);
      pos += 12;
      if (marker) {
        if (pos + 20 > info_size) break;
        pos += 20;
        inner_offset = 0;
        inner_size = 0;
      }
      if (!inner_size) {
        inner_offset = stream_offset;
        inner_size = size;
      }
      const uint64_t data_offset = archive_offset + base + inner_offset;
      if (data_offset + inner_size > archive_offset + archive_size)
        continue;
      if (add_asset_names(group, name, data_offset, inner_size, inner_size) < 0) {
        rc = -1;
        break;
      }
    }
  } else {
    debugPrintf("obb: unsupported PGSR type %u in %s\n", type,
                group ? group : "(unnamed)");
  }
  free(info);
  return rc;
}

static void clear_entries(void) {
  for (size_t i = 0; i < g_entry_count; i++)
    free(g_entries[i].name);
  free(g_entries);
  g_entries = NULL;
  g_entry_count = 0;
  g_entry_capacity = 0;
}

static int compare_entries(const void *left, const void *right) {
  return strcmp(((const ObbEntry *)left)->name, ((const ObbEntry *)right)->name);
}

static int compare_entry_key(const void *key, const void *entry) {
  return strcmp(key, ((const ObbEntry *)entry)->name);
}

static int contains_bytes(const uint8_t *data, size_t size,
                          const char *needle) {
  const size_t length = strlen(needle);
  if (!length || length > size) return 0;
  for (size_t i = 0; i + length <= size; i++)
    if (!memcmp(data + i, needle, length)) return 1;
  return 0;
}

/* The Product/Version lines are emitted later by libPVZ2 itself, after it
 * opens the raw 1BSR file.  They are not JNI values.  Log the archive facts
 * and every plausible metadata stream here so a blank native Version can be
 * diagnosed from the next Switch log without substituting a made-up value. */
static int has_metadata_token(const char *name, const char *token) {
  const size_t length = strlen(token);
  const char *match = name;
  while ((match = strstr(match, token)) != NULL) {
    const char before = match == name ? '/' : match[-1];
    const char after = match[length];
    if ((before == '/' || before == '_' || before == '-' || before == '.') &&
        (after == 0 || after == '/' || after == '_' || after == '-' || after == '.'))
      return 1;
    match++;
  }
  return 0;
}

static void log_metadata_probe(const uint8_t *header) {
  size_t candidates = 0;
  const size_t max_candidates = 12;

  debugPrintf("obb: metadata: 1BSR bytes=%llu header[04,08,0c]=%08x,%08x,%08x; XAPK version=%s\n",
              (unsigned long long)g_file_size, rd32(header + 4), rd32(header + 8),
              rd32(header + 12), GAME_VERSION);
  for (size_t i = 0; i < g_entry_count; i++) {
    const char *name = g_entries[i].name;
    if (!has_metadata_token(name, "version") && !has_metadata_token(name, "build") &&
        !has_metadata_token(name, "product"))
      continue;
    if (candidates < max_candidates)
      debugPrintf("obb: metadata candidate: %s (%u bytes)\n", name,
                  g_entries[i].size);
    candidates++;
  }
  if (!candidates)
    debugPrintf("obb: metadata: no standalone version/build/product stream; the later blank native Version line is not a VFS or JNI lookup.\n");
  else if (candidates > max_candidates)
    debugPrintf("obb: metadata: %zu candidate streams total (%zu shown)\n",
                candidates, max_candidates);
}

static void *read_embedded_lawnstrings(size_t *out_size);

#define OBB_CACHE_PATH DATA_DIR "/No_Backup/obb_index_v1.bin"
#define OBB_CACHE_TMP  DATA_DIR "/No_Backup/obb_index_v1.bin.tmp"
#define OBB_CACHE_HEADER_SIZE 40u
#define OBB_CACHE_ENTRY_HEADER_SIZE 24u
#define OBB_CACHE_VERSION 1u
static const uint8_t k_obb_cache_magic[8] = { 'P','V','Z','2','I','D','X','1' };

/* Parsing 2,644 PGSR containers from the 1 GiB OBB costs about 1.5 seconds on
 * every launch.  Cache only the derived filename/offset table.  The cache is
 * accepted only when the archive size, container table metadata and header
 * fingerprint still match; any malformed record falls back to a full parse. */
static int obb_cache_load(const uint8_t *archive_header, uint32_t files,
                          uint32_t table_offset) {
  FILE *file = fopen(OBB_CACHE_PATH, "rb");
  if (!file) return -1;
  uint8_t header[OBB_CACHE_HEADER_SIZE];
  int ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
           !memcmp(header, k_obb_cache_magic, sizeof(k_obb_cache_magic)) &&
           rd32(header + 8) == OBB_CACHE_VERSION &&
           rd32(header + 12) == files &&
           rd32(header + 16) == table_offset &&
           rd64(header + 24) == g_file_size &&
           rd32(header + 32) == fnv1a32(archive_header, 0x30);
  const uint32_t entries = ok ? rd32(header + 20) : 0;
  if (!ok || entries == 0 || entries > 20000) {
    fclose(file);
    return -1;
  }

  clear_entries();
  char name[OBB_MAX_NAME];
  for (uint32_t i = 0; i < entries; i++) {
    uint8_t record[OBB_CACHE_ENTRY_HEADER_SIZE];
    if (fread(record, 1, sizeof(record), file) != sizeof(record)) { ok = 0; break; }
    const uint32_t name_len = rd32(record + 0);
    const uint32_t packed_size = rd32(record + 4);
    const uint32_t size = rd32(record + 8);
    const uint64_t offset = rd64(record + 16);
    if (!name_len || name_len >= sizeof(name) || !size ||
        offset > g_file_size || packed_size > g_file_size - offset ||
        fread(name, 1, name_len, file) != name_len) {
      ok = 0;
      break;
    }
    name[name_len] = 0;
    if (add_name(name, offset, packed_size, size) < 0) { ok = 0; break; }
  }
  if (ok && fgetc(file) != EOF) ok = 0;
  fclose(file);
  if (!ok || g_entry_count != entries) {
    clear_entries();
    return -1;
  }
  qsort(g_entries, g_entry_count, sizeof(*g_entries), compare_entries);
  debugPrintf("obb: index cache hit (%zu assets)\n", g_entry_count);
  return 0;
}

static void obb_cache_save(const uint8_t *archive_header, uint32_t files,
                           uint32_t table_offset) {
  if (!g_entry_count || g_entry_count > UINT32_MAX) return;
  remove(OBB_CACHE_TMP);
  FILE *file = fopen(OBB_CACHE_TMP, "wb");
  if (!file) return;

  uint8_t header[OBB_CACHE_HEADER_SIZE] = {0};
  memcpy(header, k_obb_cache_magic, sizeof(k_obb_cache_magic));
  wr32(header + 8, OBB_CACHE_VERSION);
  wr32(header + 12, files);
  wr32(header + 16, table_offset);
  wr32(header + 20, (uint32_t)g_entry_count);
  wr64(header + 24, g_file_size);
  wr32(header + 32, fnv1a32(archive_header, 0x30));
  int ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
  for (size_t i = 0; ok && i < g_entry_count; i++) {
    const ObbEntry *entry = &g_entries[i];
    const size_t name_len = strlen(entry->name);
    if (!name_len || name_len >= OBB_MAX_NAME || name_len > UINT32_MAX) { ok = 0; break; }
    uint8_t record[OBB_CACHE_ENTRY_HEADER_SIZE] = {0};
    wr32(record + 0, (uint32_t)name_len);
    wr32(record + 4, entry->packed_size);
    wr32(record + 8, entry->size);
    wr64(record + 16, entry->offset);
    ok = fwrite(record, 1, sizeof(record), file) == sizeof(record) &&
         fwrite(entry->name, 1, name_len, file) == name_len;
  }
  if (ok && fflush(file) != 0) ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok || rename(OBB_CACHE_TMP, OBB_CACHE_PATH) != 0) {
    remove(OBB_CACHE_TMP);
    return;
  }
  debugPrintf("obb: index cache saved (%zu assets)\n", g_entry_count);
}

int obb_open(const char *path) {
  obb_close();
  g_file = fopen(path, "rb");
  if (!g_file) {
    debugPrintf("obb: fopen(%s) failed\n", path ? path : "(null)");
    return -1;
  }
  if (fseek(g_file, 0, SEEK_END) != 0 || ftell(g_file) < 0) {
    obb_close();
    return -1;
  }
  g_file_size = (uint64_t)ftell(g_file);
  rewind(g_file);

  uint8_t header[0x30];
  if (g_file_size < sizeof(header) || read_at(0, header, sizeof(header)) < 0 ||
      memcmp(header, "1bsr", 4) != 0) {
    debugPrintf("obb: %s is not a 1BSR archive\n", path);
    obb_close();
    return -1;
  }
  const uint32_t files = rd32(header + 0x28);
  const uint32_t table_offset = rd32(header + 0x2c);
  if ((uint64_t)table_offset + (uint64_t)files * OBB_TABLE_ENTRY_SIZE > g_file_size) {
    obb_close();
    return -1;
  }
  if (!g_lock_ready) {
    mutexInit(&g_lock);
    g_lock_ready = 1;
  }

  if (obb_cache_load(header, files, table_offset) == 0) {
    debugPrintf("obb: opened %s (%u containers, %zu assets; cached index)\n",
                path, files, g_entry_count);
    log_metadata_probe(header);
    return 0;
  }

  uint8_t entry[OBB_TABLE_ENTRY_SIZE];
  char group[OBB_MAX_NAME];
  for (uint32_t i = 0; i < files; i++) {
    const uint64_t table_entry = table_offset + (uint64_t)i * OBB_TABLE_ENTRY_SIZE;
    if (read_at(table_entry, entry, sizeof(entry)) < 0) {
      obb_close();
      return -1;
    }
    memcpy(group, entry, 0x80);
    group[0x7f] = 0;
    group[sizeof(group) - 1] = 0;
    group[0x7f] = 0;
    uint32_t end = 0;
    while (end < 0x80 && group[end]) end++;
    group[end] = 0;
    const uint32_t offset = rd32(entry + 0x80);
    const uint32_t size = rd32(entry + 0x84);
    if (!size || (uint64_t)offset + size > g_file_size)
      continue;
    if (parse_pgsr(offset, size, group) < 0) {
      debugPrintf("obb: failed PGSR entry %u (%s)\n", i, group);
      obb_close();
      return -1;
    }
  }
  qsort(g_entries, g_entry_count, sizeof(*g_entries), compare_entries);
  obb_cache_save(header, files, table_offset);
  debugPrintf("obb: opened %s (%u containers, %zu assets)\n", path, files,
              g_entry_count);
  log_metadata_probe(header);
  return 0;
}

void obb_close(void) {
  if (g_lock_ready) mutexLock(&g_lock);
  hot_cache_clear_locked();
  clear_entries();
  if (g_file) fclose(g_file);
  g_file = NULL;
  g_file_size = 0;
  if (g_lock_ready) mutexUnlock(&g_lock);
}

static ObbEntry *find_entry(const char *name) {
  char key[OBB_MAX_NAME];
  canonicalize(name, key, sizeof(key));
  ObbEntry *entry = bsearch(key, g_entries, g_entry_count, sizeof(*g_entries),
                             compare_entry_key);
  if (entry) return entry;
  if (!strncmp(key, "assets/", 7)) {
    entry = bsearch(key + 7, g_entries, g_entry_count, sizeof(*g_entries),
                    compare_entry_key);
    if (entry) return entry;
  }
  /* Android exposes the combined package archive as packages/..., while the
   * OBB index stores it under Packages/PACKAGES/.... */
  if (!strncmp(key, "packages/", 9)) {
    char alias[OBB_MAX_NAME];
    const int length = snprintf(alias, sizeof(alias), "packages/packages/%s",
                                key + 9);
    if (length > 0 && (size_t)length < sizeof(alias))
      return bsearch(alias, g_entries, g_entry_count, sizeof(*g_entries),
                     compare_entry_key);
  }
  return NULL;
}

int obb_exists(const char *name) {
  int found;
  if (!g_lock_ready) return 0;
  mutexLock(&g_lock);
  found = find_entry(name) != NULL;
  mutexUnlock(&g_lock);
  return found;
}

void *obb_read(const char *name, size_t *out_size) {
  if (out_size) *out_size = 0;
  if (!g_file || !g_lock_ready || !name) return NULL;

  char key[OBB_MAX_NAME];
  canonicalize(name, key, sizeof(key));
  mutexLock(&g_lock);
  size_t cached_size = 0;
  void *cached = hot_cache_copy_locked(key, &cached_size);
  if (cached) {
    const size_t resident = g_hot_asset_bytes;
    if (out_size) *out_size = cached_size;
    mutexUnlock(&g_lock);
    (void)resident;
    return cached;
  }

  ObbEntry *entry = find_entry(name);
  if (!entry || !entry->size) {
    mutexUnlock(&g_lock);
    if (!strcmp(key, "packages/lawnstrings-en-us.rton"))
      return read_embedded_lawnstrings(out_size);
    return NULL;
  }
  const uint64_t offset = entry->offset;
  const uint32_t packed_size = entry->packed_size;
  const uint32_t size = entry->size;
  uint8_t *packed = malloc(packed_size ? packed_size : 1);
  uint8_t *result = malloc(size);
  if (!packed || !result || read_at(offset, packed, packed_size) < 0) {
    free(packed);
    free(result);
    mutexUnlock(&g_lock);
    debugPrintf("obb: read failed for %s\n", name);
    return NULL;
  }
  mutexUnlock(&g_lock);

  uLongf decoded_size = size;
  int rc = uncompress(result, &decoded_size, packed, packed_size);
  if (rc == Z_OK) {
    if (decoded_size != size) rc = Z_DATA_ERROR;
  } else if (packed_size == size) {
    /* Some entries are stored raw, while others happen to compress to the
     * same byte count. Only use the raw fallback after attempting zlib. */
    memcpy(result, packed, size);
    rc = Z_OK;
  }
  free(packed);
  if (rc != Z_OK) {
    free(result);
    debugPrintf("obb: decompress failed for %s (%d)\n", name, rc);
    return NULL;
  }

  mutexLock(&g_lock);
  hot_cache_store_locked(key, result, size);
  mutexUnlock(&g_lock);
  if (out_size) *out_size = size;
  return result;
}

static void *read_embedded_lawnstrings(size_t *out_size) {
  size_t package_size = 0;
  uint8_t *package = obb_read("packages/packages/arcade_config.rton",
                              &package_size);
  if (!package) return NULL;

  /* The combined RTON stream is 4 KiB aligned. Select the chunk containing
   * LawnStringsData and the English sentinel, then return only that RTON. */
  size_t start = SIZE_MAX, end = package_size;
  for (size_t pos = 0; pos + 4 <= package_size; pos += 4096) {
    if (memcmp(package + pos, "RTON", 4)) continue;
    if (start != SIZE_MAX) {
      const size_t length = pos - start;
      if (contains_bytes(package + start, length, "LawnStringsData") &&
          contains_bytes(package + start, length, "Account Deleted")) {
        end = pos;
        break;
      }
    }
    start = pos;
  }

  if (start == SIZE_MAX || end <= start) {
    free(package);
    return NULL;
  }
  const size_t size = end - start;
  void *result = malloc(size);
  if (result) memcpy(result, package + start, size);
  free(package);
  if (out_size) *out_size = result ? size : 0;
  debugPrintf("obb: embedded LawnStrings-en-us.rton -> %zu bytes\n", size);
  return result;
}
