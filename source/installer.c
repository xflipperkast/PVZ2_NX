#include <switch.h>
#include <zlib.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "installer.h"
#include "util.h"

#define INSTALL_ARCHIVE DATA_DIR "/pvz2.xapk"
#define ZIP_NAME_MAX 512
#define COPY_BUFFER_SIZE (64u * 1024u)

typedef struct {
  FILE *file;
  uint16_t entries;
  uint32_t directory_offset;
} ZipArchive;

typedef struct {
  char name[ZIP_NAME_MAX];
  uint16_t flags;
  uint16_t method;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint32_t local_offset;
} ZipEntry;

typedef int (*ZipVisitor)(ZipArchive *archive, const ZipEntry *entry, void *user);

static char g_error[256];
static int g_console_active;
static char g_progress_name[96];

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_error(const char *message) {
  snprintf(g_error, sizeof(g_error), "%s", message ? message : "Unknown installer error.");
}

const char *installer_last_error(void) {
  return g_error[0] ? g_error : "Unknown installer error.";
}

static int ends_with(const char *text, const char *suffix) {
  const size_t text_len = strlen(text);
  const size_t suffix_len = strlen(suffix);
  return text_len >= suffix_len && !strcmp(text + text_len - suffix_len, suffix);
}

static int safe_relative_path(const char *path) {
  return path && *path && path[0] != '/' && !strstr(path, "..") && !strchr(path, '\\');
}

static void progress_begin(void) {
  if (g_console_active) return;
  consoleInit(NULL);
  g_console_active = 1;
}

static void progress_update(const char *name, uint64_t done, uint64_t total) {
  if (!g_console_active) return;
  if (!name) name = "Preparing files";
  snprintf(g_progress_name, sizeof(g_progress_name), "%s", name);
  const unsigned percent = total ? (unsigned)((done * 100u) / total) : 0;
  const unsigned filled = percent * 30u / 100u;
  consoleClear();
  printf("PVZ2 first-run installer\n\n");
  printf("Extracting: %s\n\n[", g_progress_name);
  for (unsigned i = 0; i < 30; ++i) putchar(i < filled ? '#' : '-');
  printf("] %3u%%\n\n", percent > 100 ? 100 : percent);
  printf("Keep the console awake. This can take a few minutes.\n");
  consoleUpdate(NULL);
}

static void progress_finish(void) {
  if (!g_console_active) return;
  consoleClear();
  printf("PVZ2 installation complete. Starting the game...\n");
  consoleUpdate(NULL);
  svcSleepThread(700000000);
  consoleExit(NULL);
  g_console_active = 0;
}

static void progress_abort(void) {
  if (!g_console_active) return;
  consoleExit(NULL);
  g_console_active = 0;
}

static int make_parent_directories(const char *file_name) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s", file_name);
  for (char *p = path + 6; *p; ++p) {
    if (*p != '/') continue;
    *p = 0;
    if (*path && mkdir(path, 0777) < 0 && errno != EEXIST) {
      set_error("Could not create an installation directory.");
      return -1;
    }
    *p = '/';
  }
  return 0;
}

static int file_exists_with_data(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && st.st_size > 0;
}

static int required_files_present(void) {
  char path[PATH_MAX];
  const char *const required[] = {OBB_NAME, CXX_SO_NAME, NIMBLE_SO_NAME, SO_NAME};
  for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
    snprintf(path, sizeof(path), DATA_DIR "/%s", required[i]);
    if (!file_exists_with_data(path)) return 0;
  }
  return 1;
}

/* The XAPK is only an installation source. Remove it after the required game
 * files are present, including on a later launch after an earlier successful
 * install, to recover its SD-card space without risking a partial install. */
static void remove_install_archive(void) {
  if (remove(INSTALL_ARCHIVE) != 0 && errno != ENOENT)
    debugPrintf("installer: could not remove pvz2.xapk (errno=%d)\n", errno);
}

static int zip_open(ZipArchive *archive, const char *path) {
  memset(archive, 0, sizeof(*archive));
  archive->file = fopen(path, "rb");
  if (!archive->file) return -1;
  if (fseeko(archive->file, 0, SEEK_END) != 0) goto bad_archive;
  const off_t end = ftello(archive->file);
  const size_t tail_size = (size_t)(end < 0x10016 ? end : 0x10016);
  uint8_t *tail = malloc(tail_size);
  if (!tail) goto bad_archive;
  if (fseeko(archive->file, end - (off_t)tail_size, SEEK_SET) != 0 ||
      fread(tail, 1, tail_size, archive->file) != tail_size) {
    free(tail);
    goto bad_archive;
  }
  for (size_t pos = tail_size >= 22 ? tail_size - 22 : 0;; --pos) {
    if (rd32(tail + pos) == 0x06054b50u) {
      archive->entries = rd16(tail + pos + 10);
      archive->directory_offset = rd32(tail + pos + 16);
      free(tail);
      if (archive->directory_offset == UINT32_MAX) goto bad_archive;
      return 0;
    }
    if (!pos) break;
  }
  free(tail);
bad_archive:
  if (archive->file) fclose(archive->file);
  archive->file = NULL;
  return -1;
}

static void zip_close(ZipArchive *archive) {
  if (archive->file) fclose(archive->file);
  archive->file = NULL;
}

static int zip_visit(ZipArchive *archive, ZipVisitor visitor, void *user) {
  if (fseeko(archive->file, archive->directory_offset, SEEK_SET) != 0) return -1;
  for (uint16_t i = 0; i < archive->entries; ++i) {
    uint8_t header[46];
    if (fread(header, 1, sizeof(header), archive->file) != sizeof(header) ||
        rd32(header) != 0x02014b50u) return -1;
    const uint16_t name_len = rd16(header + 28);
    const uint16_t extra_len = rd16(header + 30);
    const uint16_t comment_len = rd16(header + 32);
    if (!name_len || name_len >= ZIP_NAME_MAX) return -1;
    ZipEntry entry = {
        .flags = rd16(header + 8), .method = rd16(header + 10),
        .compressed_size = rd32(header + 20), .uncompressed_size = rd32(header + 24),
        .local_offset = rd32(header + 42),
    };
    if (fread(entry.name, 1, name_len, archive->file) != name_len) return -1;
    entry.name[name_len] = 0;
    if (fseeko(archive->file, (off_t)extra_len + comment_len, SEEK_CUR) != 0) return -1;
    const off_t next = ftello(archive->file);
    if (visitor(archive, &entry, user) < 0) return -1;
    if (fseeko(archive->file, next, SEEK_SET) != 0) return -1;
  }
  return 0;
}

static int extract_entry(ZipArchive *archive, const ZipEntry *entry, const char *target) {
  uint8_t local[30];
  if (entry->flags & 1u || (entry->method != 0 && entry->method != 8) ||
      fseeko(archive->file, entry->local_offset, SEEK_SET) != 0 ||
      fread(local, 1, sizeof(local), archive->file) != sizeof(local) ||
      rd32(local) != 0x04034b50u) {
    set_error("The XAPK contains an unsupported ZIP entry.");
    return -1;
  }
  const uint16_t name_len = rd16(local + 26);
  const uint16_t extra_len = rd16(local + 28);
  if (fseeko(archive->file, (off_t)name_len + extra_len, SEEK_CUR) != 0 ||
      make_parent_directories(target) < 0) return -1;

  char temporary[PATH_MAX];
  snprintf(temporary, sizeof(temporary), "%s.tmp", target);
  FILE *output = fopen(temporary, "wb");
  if (!output) { set_error("Could not create an installation file."); return -1; }

  uint8_t *input = malloc(COPY_BUFFER_SIZE);
  uint8_t *outbuf = malloc(COPY_BUFFER_SIZE);
  int ok = input && outbuf;
  uint64_t written = 0;
  if (entry->method == 0) {
    uint32_t remaining = entry->compressed_size;
    while (ok && remaining) {
      const size_t chunk = remaining < COPY_BUFFER_SIZE ? remaining : COPY_BUFFER_SIZE;
      ok = fread(input, 1, chunk, archive->file) == chunk &&
           fwrite(input, 1, chunk, output) == chunk;
      remaining -= (uint32_t)chunk;
      written += chunk;
      progress_update(entry->name, written, entry->uncompressed_size);
    }
  } else {
    z_stream stream = {0};
    ok = inflateInit2(&stream, -MAX_WBITS) == Z_OK;
    uint32_t remaining = entry->compressed_size;
    int status = Z_OK;
    while (ok && status != Z_STREAM_END) {
      if (!stream.avail_in && remaining) {
        const size_t chunk = remaining < COPY_BUFFER_SIZE ? remaining : COPY_BUFFER_SIZE;
        if (fread(input, 1, chunk, archive->file) != chunk) { ok = 0; break; }
        remaining -= (uint32_t)chunk;
        stream.next_in = input;
        stream.avail_in = (uInt)chunk;
      }
      stream.next_out = outbuf;
      stream.avail_out = COPY_BUFFER_SIZE;
      status = inflate(&stream, remaining ? Z_NO_FLUSH : Z_FINISH);
      const size_t produced = COPY_BUFFER_SIZE - stream.avail_out;
      if (produced && fwrite(outbuf, 1, produced, output) != produced) { ok = 0; break; }
      written += produced;
      progress_update(entry->name, written, entry->uncompressed_size);
      if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) { ok = 0; break; }
      if (status == Z_BUF_ERROR && !remaining && !stream.avail_in) { ok = 0; break; }
    }
    inflateEnd(&stream);
    ok = ok && status == Z_STREAM_END;
  }
  free(input);
  free(outbuf);
  fclose(output);
  if (!ok || written != entry->uncompressed_size || rename(temporary, target) != 0) {
    remove(temporary);
    set_error("An XAPK file could not be extracted.");
    return -1;
  }
  return 0;
}

typedef struct { unsigned extracted; } ApkInstallState;

static int install_apk_entry(ZipArchive *archive, const ZipEntry *entry, void *user) {
  ApkInstallState *state = user;
  const char *asset_prefix = "assets/";
  const char *library_prefix = "lib/arm64-v8a/";
  char target[PATH_MAX];
  if (!strncmp(entry->name, asset_prefix, strlen(asset_prefix))) {
    const char *relative = entry->name + strlen(asset_prefix);
    if (!safe_relative_path(relative) || ends_with(relative, "/")) return 0;
    snprintf(target, sizeof(target), DATA_DIR "/assets/%s", relative);
  } else if (!strncmp(entry->name, library_prefix, strlen(library_prefix))) {
    const char *library = entry->name + strlen(library_prefix);
    if (strcmp(library, CXX_SO_NAME) && strcmp(library, NIMBLE_SO_NAME) && strcmp(library, SO_NAME))
      return 0;
    snprintf(target, sizeof(target), DATA_DIR "/%s", library);
  } else {
    return 0;
  }
  if (extract_entry(archive, entry, target) < 0) return -1;
  state->extracted++;
  return 0;
}

static int install_apk(const char *path) {
  ZipArchive archive;
  if (zip_open(&archive, path) < 0) { set_error("A nested APK is not a valid ZIP archive."); return -1; }
  ApkInstallState state = {0};
  const int result = zip_visit(&archive, install_apk_entry, &state);
  zip_close(&archive);
  if (result < 0) { if (!g_error[0]) set_error("Could not read a nested APK."); return -1; }
  return 0;
}

typedef struct { unsigned apk_index; } XapkInstallState;

static int install_xapk_entry(ZipArchive *archive, const ZipEntry *entry, void *user) {
  XapkInstallState *state = user;
  char target[PATH_MAX];
  if (ends_with(entry->name, ".obb")) {
    snprintf(target, sizeof(target), DATA_DIR "/%s", OBB_NAME);
    return extract_entry(archive, entry, target);
  }
  if (ends_with(entry->name, ".apk")) {
    snprintf(target, sizeof(target), DATA_DIR "/.installer_apk_%u.tmp", state->apk_index++);
    if (extract_entry(archive, entry, target) < 0) return -1;
    const int result = install_apk(target);
    remove(target);
    return result;
  }
  return 0;
}

int installer_prepare_game_files(void) {
  g_error[0] = 0;
  if (required_files_present()) {
    remove_install_archive();
    return 0;
  }
  if (!file_exists_with_data(INSTALL_ARCHIVE)) {
    set_error("Missing game files. Copy pvz2.xapk to sdmc:/switch/pvz2_nx/ and relaunch.");
    return -1;
  }
  progress_begin();
  progress_update("Opening pvz2.xapk", 0, 1);
  ZipArchive archive;
  if (zip_open(&archive, INSTALL_ARCHIVE) < 0) {
    set_error("pvz2.xapk is not a supported ZIP/XAPK archive.");
    progress_abort();
    return -1;
  }
  XapkInstallState state = {0};
  const int result = zip_visit(&archive, install_xapk_entry, &state);
  zip_close(&archive);
  if (result < 0 || !required_files_present()) {
    if (!g_error[0]) set_error("The XAPK did not contain the required PVZ2 files.");
    progress_abort();
    return -1;
  }
  remove_install_archive();
  progress_finish();
  return 0;
}
