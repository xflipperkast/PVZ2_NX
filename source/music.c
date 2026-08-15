/* music.c -- Android-compatible music path (DroidMusicManager / CustomMediaPlayer).
 *
 * PVZ2 has TWO audio paths:
 *   1. SFX  -> OpenSL ES  (opensles.c, already implemented)
 *   2. Music -> a Java "CustomMediaPlayer" driven from C++ DroidMusicManager,
 *      via JNI up-calls loadMusic()/playMusic()/setVolume()/... .
 *
 * On Android that Java class wraps android.media.MediaPlayer. There is no such
 * thing here, so those up-calls did nothing and the game was silent. This module
 * IS that media player: it decodes the requested MP3 (minimp3) fully into PCM at
 * open, then streams it. mix_music() is called from the SDL audio callback in
 * opensles.c and mixed alongside SFX, so both share the one 48 kHz device.
 *
 * The JNI bridge routes CustomMediaPlayer methods here; the jobject handle we
 * hand back to the engine is simply a Music* cast to a pointer.
 */
#define _GNU_SOURCE
#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL2/SDL.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"

#include "music.h"
#include "opensles.h"
#include "libc_shim.h"   /* AAssetManager_open_fake etc. for asset loading */

/* The device is opened at 48000 Hz stereo S16 (see opensles.c ensure_device). */
#define DEV_RATE 48000

typedef struct Music {
  uint32_t magic;        /* MUSIC_MAGIC while valid */
  int16_t *pcm;          /* decoded interleaved stereo @ src_rate */
  size_t   frames;       /* total stereo frames */
  int      src_rate;     /* decoded sample rate */
  /* playback state (guarded by g_music_lock) */
  double   pos;          /* fractional read cursor, in SOURCE frames */
  int      playing;
  int      loop;
  float    volume;       /* 0..1 */
} Music;

#define MUSIC_MAGIC 0x4D555A31u   /* "MUZ1" */

static SDL_mutex *g_music_lock = NULL;
static Music     *g_current    = NULL;   /* at most one track plays at a time */

static void ensure_lock(void) {
  if (!g_music_lock) g_music_lock = SDL_CreateMutex();
}

static Music *music_ok(void *h) {
  Music *m = h;
  return (m && m->magic == MUSIC_MAGIC) ? m : NULL;
}

/* ------------------------------------------------------------------ loading */

/* The game asks for names like "main.mp3"; loose files live under the same
 * asset roots as everything else, so reuse the AAsset loader (which serves from
 * memory and applies the sdmc: path anchoring). Returns a malloc'd buffer. */
static uint8_t *slurp_asset(const char *name, size_t *out_len) {
  void *a = AAssetManager_open_fake(NULL, name, 0 /*AASSET_MODE_BUFFER*/);
  if (!a) return NULL;
  long len = AAsset_getLength_fake(a);
  const void *buf = AAsset_getBuffer_fake(a);   /* whole file in memory */
  uint8_t *copy = NULL;
  if (buf && len > 0) {
    copy = malloc((size_t)len);
    if (copy) { memcpy(copy, buf, (size_t)len); *out_len = (size_t)len; }
  }
  AAsset_close_fake(a);
  return copy;
}

void *music_load(const char *path) {
  if (!path) return NULL;
  ensure_lock();

  size_t enc_len = 0;
  uint8_t *enc = slurp_asset(path, &enc_len);
  if (!enc) return NULL;

  static mp3dec_t dec;               /* decode is serialised (see routing) */
  mp3dec_file_info_t info;
  memset(&info, 0, sizeof(info));
  int rc = mp3dec_load_buf(&dec, enc, enc_len, &info, NULL, NULL);
  free(enc);
  if (rc || !info.buffer || info.samples == 0) {
    free(info.buffer);
    return NULL;
  }

  Music *m = calloc(1, sizeof(*m));
  if (!m) { free(info.buffer); return NULL; }
  m->magic    = MUSIC_MAGIC;
  m->src_rate = info.hz;
  m->volume   = 1.0f;

  /* minimp3 gives interleaved int16 with info.channels channels. Normalise to
   * stereo so the mixer is simple. info.samples counts individual samples. */
  if (info.channels == 2) {
    m->frames = info.samples / 2;
    m->pcm = (int16_t *)info.buffer;            /* already interleaved stereo */
  } else if (info.channels == 1) {
    m->frames = info.samples;
    m->pcm = malloc(m->frames * 2 * sizeof(int16_t));
    if (!m->pcm) { free(info.buffer); free(m); return NULL; }
    for (size_t i = 0; i < m->frames; i++) {     /* mono -> L=R */
      int16_t s = ((int16_t *)info.buffer)[i];
      m->pcm[2*i] = s; m->pcm[2*i + 1] = s;
    }
    free(info.buffer);
  } else {
    free(info.buffer); free(m); return NULL;
  }

  return m;
}

void music_unload(void *h) {
  Music *m = music_ok(h);
  if (!m) return;
  ensure_lock();
  SDL_LockMutex(g_music_lock);
  if (g_current == m) g_current = NULL;
  SDL_UnlockMutex(g_music_lock);
  m->magic = 0;
  free(m->pcm);
  free(m);
}

/* ---------------------------------------------------------------- transport */

void music_play(void *h, int loop) {
  Music *m = music_ok(h);
  if (!m) return;
  ensure_lock();
  opensles_ensure_audio();           /* music may start before any SFX opens the device */
  SDL_LockMutex(g_music_lock);
  m->pos     = 0.0;
  m->playing = 1;
  m->loop    = loop;
  g_current  = m;                    /* becomes the active track */
  SDL_UnlockMutex(g_music_lock);
}

void music_pause(void *h, int paused) {
  Music *m = music_ok(h);
  if (!m) return;
  ensure_lock();
  SDL_LockMutex(g_music_lock);
  m->playing = paused ? 0 : 1;
  SDL_UnlockMutex(g_music_lock);
}

void music_stop(void *h) {
  Music *m = music_ok(h);
  if (!m) return;
  ensure_lock();
  SDL_LockMutex(g_music_lock);
  m->playing = 0;
  m->pos     = 0.0;
  if (g_current == m) g_current = NULL;
  SDL_UnlockMutex(g_music_lock);
}

void music_set_volume(void *h, float v) {
  Music *m = music_ok(h);
  if (!m) return;
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  ensure_lock();
  SDL_LockMutex(g_music_lock);
  m->volume = v;
  SDL_UnlockMutex(g_music_lock);
}

/* ------------------------------------------------------------------ mixing */

/* Called from the SDL audio callback (opensles.c). Adds the active track into
 * the accumulation buffer, resampling from src_rate to the 48 kHz device rate
 * with linear interpolation. acc holds `frames` interleaved-stereo int32
 * samples that the callback later clamps to int16. */
void mix_music(int32_t *acc, int frames) {
  if (!g_music_lock) return;
  SDL_LockMutex(g_music_lock);
  Music *m = g_current;
  if (!m || !m->playing || !m->pcm || m->frames == 0) {
    SDL_UnlockMutex(g_music_lock);
    return;
  }

  const double step = (double)m->src_rate / (double)DEV_RATE;
  const int vol = (int)(m->volume * 256.0f);      /* fixed-point gain */

  for (int i = 0; i < frames; i++) {
    double sp = m->pos;
    size_t i0 = (size_t)sp;
    if (i0 >= m->frames) {
      if (m->loop) { m->pos = 0.0; sp = 0.0; i0 = 0; }
      else { m->playing = 0; break; }
    }
    size_t i1 = i0 + 1; if (i1 >= m->frames) i1 = m->loop ? 0 : i0;
    const double frac = sp - (double)i0;

    /* linear-interpolate L and R */
    int l0 = m->pcm[2*i0], l1 = m->pcm[2*i1];
    int r0 = m->pcm[2*i0 + 1], r1 = m->pcm[2*i1 + 1];
    int l = (int)(l0 + (l1 - l0) * frac);
    int r = (int)(r0 + (r1 - r0) * frac);

    acc[2*i]     += (l * vol) >> 8;
    acc[2*i + 1] += (r * vol) >> 8;

    m->pos += step;
  }

  SDL_UnlockMutex(g_music_lock);
}
