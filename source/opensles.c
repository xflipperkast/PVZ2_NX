/* opensles.c -- minimal OpenSL ES shim backed by libnx audout
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Implements the slice of OpenSL ES 1.0.1 the SQEX "Sd" sound driver uses:
 * the Object interface (Realize/GetInterface/Destroy), the Engine interface
 * (CreateOutputMix/CreateAudioPlayer), and on each player the Play, Volume and
 * AndroidSimpleBufferQueue interfaces. Players are software-mixed into one
 * libnx audout stream; completion callbacks run from its pump thread.
 */

#include <switch.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "opensles.h"
#include "music.h"

static void ensure_device(int rate);  /* fwd */
#include "util.h"

// --- OpenSL ES constants ----------------------------------------------------

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PARAMETER_INVALID    0x0D
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef int16_t  SLint16;
typedef uint8_t  SLuint8;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t  SLmillibel;

// PCM data format (samplesPerSec is in milliHz per the spec)
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void *SLObjectItf;       // -> &obj->obj_vt
typedef void *SLInterfaceID;

// callback: (SLAndroidSimpleBufferQueueItf caller, void *pContext)
typedef void (*slBufferQueueCallback)(void *caller, void *context);

// --- interface-id sentinels -------------------------------------------------

#define DEF_IID(n) void *SL_IID_##n = &SL_IID_##n
DEF_IID(3DCOMMIT); DEF_IID(3DDOPPLER); DEF_IID(3DGROUPING); DEF_IID(3DLOCATION);
DEF_IID(3DMACROSCOPIC); DEF_IID(3DSOURCE); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(ANDROIDEFFECT); DEF_IID(ANDROIDEFFECTCAPABILITIES); DEF_IID(ANDROIDEFFECTSEND);
DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(AUDIODECODERCAPABILITIES); DEF_IID(AUDIOENCODER);
DEF_IID(AUDIOENCODERCAPABILITIES); DEF_IID(AUDIOIODEVICECAPABILITIES); DEF_IID(BASSBOOST);
DEF_IID(BUFFERQUEUE); DEF_IID(DEVICEVOLUME); DEF_IID(DYNAMICINTERFACEMANAGEMENT);
DEF_IID(DYNAMICSOURCE); DEF_IID(EFFECTSEND); DEF_IID(ENGINE); DEF_IID(ENGINECAPABILITIES);
DEF_IID(ENVIRONMENTALREVERB); DEF_IID(EQUALIZER); DEF_IID(LED); DEF_IID(METADATAEXTRACTION);
DEF_IID(METADATATRAVERSAL); DEF_IID(MIDIMESSAGE); DEF_IID(MIDIMUTESOLO); DEF_IID(MIDITEMPO);
DEF_IID(MIDITIME); DEF_IID(MUTESOLO); DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(OUTPUTMIX);
DEF_IID(PITCH); DEF_IID(PLAY); DEF_IID(PLAYBACKRATE); DEF_IID(PREFETCHSTATUS);
DEF_IID(PRESETREVERB); DEF_IID(RATEPITCH); DEF_IID(RECORD); DEF_IID(SEEK); DEF_IID(THREADSYNC);
DEF_IID(VIBRA); DEF_IID(VIRTUALIZER); DEF_IID(VISUALIZATION); DEF_IID(VOLUME);
#undef DEF_IID

// --- vtable structs (method order matches the OpenSL ES 1.0.1 spec) ---------

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *pPriority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

// only CreateAudioPlayer (slot 2) and CreateOutputMix (slot 7) are used; the
// rest keep the correct layout but are generic so a shared stub assigns
// cleanly. The engine calls each slot with its own typed vtable.
typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateAudioRecorder;
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *pMix, SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateMetadataExtractor;
  void *CreateExtensionObject;
  void *QueryNumSupportedInterfaces;
  void *QuerySupportedInterfaces;
  void *QueryNumSupportedExtensions;
  void *QuerySupportedExtension;
  void *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} SLPlayItf_;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *pState);
  SLresult (*RegisterCallback)(void *self, slBufferQueueCallback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *p);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *p);
  SLresult (*SetStereoPosition)(void *self, SLint32 perMille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *p);
} SLVolumeItf_;

// SLPlaybackRateItf (Chaos Rings 3 requests it for pitch-shifted SE). We don't
// resample, so SetRate is accepted but ignored; the spec method order matters.
typedef struct {
  SLresult (*SetRate)(void *self, SLint16 rate);
  SLresult (*GetRate)(void *self, SLint16 *p);
  SLresult (*SetPropertyConstraints)(void *self, SLuint32 c);
  SLresult (*GetProperties)(void *self, SLuint32 *p);
  SLresult (*GetCapabilitiesOfRate)(void *self, SLuint32 *p);
  SLresult (*GetRateRange)(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop);
} SLPlaybackRateItf_;

// SLAndroidConfigurationItf -- CR3 requests it with req=SL_BOOLEAN_TRUE at
// CreateAudioPlayer, so it MUST be obtainable or the engine aborts the player
// (the symptom: a player is created but never set PLAYING / never enqueues).
// It only uses SetConfiguration (stream type / usage), which we can ignore.
typedef struct {
  SLresult (*SetConfiguration)(void *self, const void *key, const void *value, SLuint32 valueSize);
  SLresult (*GetConfiguration)(void *self, const void *key, SLuint32 *pValueSize, void *value);
  SLresult (*AcquireJavaProxy)(void *self, SLuint32 proxyType, void *pProxyObj);
  SLresult (*ReleaseJavaProxy)(void *self, SLuint32 proxyType);
} SLAndroidConfigurationItf_;

// --- objects ----------------------------------------------------------------

#define MAX_PLAYERS 64
#define BQ_SLOTS 256

typedef struct {
  const void *data;
  SLuint32 size;
} BQBuffer;

typedef struct Player {
  const SLObjectItf_ *obj_vt;
  const SLPlayItf_   *play_vt;
  const SLBufferQueueItf_ *bq_vt;
  const SLVolumeItf_ *vol_vt;
  const SLPlaybackRateItf_ *rate_vt;
  const SLAndroidConfigurationItf_ *config_vt;

  int in_use;
  int channels;
  int rate;
  int playing;
  int drained;   // consecutive callbacks this playing player produced no audio
  float gain; // effective linear gain after volume + mute
  SLmillibel volume_level;
  SLboolean muted;
  SLuint32 queue_index; /* AndroidSimpleBufferQueue buffers fully consumed */

  slBufferQueueCallback cb;
  void *cb_ctx;

  // FIFO of enqueued buffers
  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail; // count = (tail - head + N) % N
  // currently draining buffer
  const uint8_t *cur;
  SLuint32 cur_size, cur_pos;
  double cur_fpos; // fractional sample index into cur (for rate conversion)

  SDL_mutex *lock;
} Player;

typedef struct {
  const SLObjectItf_ *obj_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *obj_vt;
  const SLEngineItf_ *eng_vt;
} Engine;

#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

// --- global audout device + player registry ---------------------------------

#define OUT_BUFS 6
#define OUT_FRAMES 1024
#define OUT_BYTES (OUT_FRAMES * 2 * sizeof(int16_t))

static int g_dev = 0;
static int g_dev_rate = 44100;
static AudioOutBuffer g_out[OUT_BUFS];
static void *g_out_mem[OUT_BUFS];
static Player *g_players[MAX_PLAYERS];
static int g_player_count = 0;
static SDL_mutex *g_reg_lock = NULL;

/* Diagnostic windows. Input-state changes arm dense logging for a short
 * period so Ready/Set/Plant can be followed across Wwise -> OpenSL -> audout
 * without permanently flooding the log. */
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
static volatile unsigned g_input_trace_bq_trace_remaining;
static volatile unsigned g_input_trace_mix_trace_remaining;
static volatile unsigned g_input_trace_audio_window_id;
#endif

void opensles_trace_state_window(uint32_t state, uintptr_t object) {
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  g_input_trace_bq_trace_remaining = 192;
  g_input_trace_mix_trace_remaining = 64;
  const unsigned id = ++g_input_trace_audio_window_id;
  debugPrintf("input_trace AUDIO WINDOW #%u: input_state=%u object=%p bq=192 mix=64\n",
              id, state, (void *)object);
#else
  (void)state;
  (void)object;
#endif
}

#define MOVIE_RING_FRAMES 65536
static SDL_mutex *g_movie_lock = NULL;
static int16_t *g_movie_pcm = NULL;
static int g_movie_active = 0;
static int g_movie_paused = 0;
static int g_movie_head = 0;
static int g_movie_count = 0;
static uint64_t g_movie_samples_queued = 0;
static uint64_t g_movie_samples_played = 0;

static float mb_to_linear(SLmillibel mb) {
  if (mb <= -9600) return 0.0f;
  return powf(10.0f, (float)mb / 2000.0f); // 100 mB = 1 dB
}

// Mix one player while holding its queue lock once per output block. This
// callback is pumped from the frame thread, not a hard real-time interrupt, so
// wait for the short producer critical section instead of dropping an entire
// output block whenever Wwise happens to be enqueuing. Completion callbacks
// still run unlocked because they call Enqueue themselves.
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
static unsigned g_audio_refill_sync_refill_recoveries;
#endif
static void mix_player(Player *p, int32_t *acc, int frames) {
  SDL_LockMutex(p->lock);
  if (!p->playing) { SDL_UnlockMutex(p->lock); return; }

  // A playing player with nothing queued needs a buffer callback so the audio
  // engine (Wwise) is kicked to feed the next PCM chunk.
  const int dry = (!p->cur) && (p->q_head == p->q_tail);
  if (dry) {
    if (p->drained < (1 << 20)) p->drained++;
    slBufferQueueCallback cb = p->cb;
    void *cb_ctx = p->cb_ctx;
    SDL_UnlockMutex(p->lock);
    if (cb)
      cb(&p->bq_vt, cb_ctx);

    /* Wwise often refills synchronously from this callback. Reacquire the
     * queue and consume that PCM now instead of emitting a guaranteed silent
     * output block at every refill boundary. */
    SDL_LockMutex(p->lock);
    if (!p->playing) { SDL_UnlockMutex(p->lock); return; }
    if (!p->cur && p->q_head == p->q_tail) {
      SDL_UnlockMutex(p->lock);
      return;
    }
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
    if (g_audio_refill_sync_refill_recoveries++ < 12)
      debugPrintf("audio_refill AUDIO REFILL: recovered synchronous PCM player=%p index=%u\n",
                  (void *)p, p->queue_index);
#endif
  }
  p->drained = 0;

  const float g = p->gain;
  const int stereo = (p->channels >= 2);
  const int bps = stereo ? 4 : 2;                       // bytes per input frame
  // resample the player's own rate to the device rate (players come in at 22050
  // AND 44100; without this, off-rate voices play at the wrong speed/pitch).
  const double ratio = g_dev_rate > 0 ? (double)p->rate / (double)g_dev_rate : 1.0;

  for (int i = 0; i < frames; i++) {
    // ensure cur holds a buffer whose integer sample index covers cur_fpos,
    // carrying the fractional remainder across buffer boundaries.
    //
    const int16_t *s = NULL;
    long n = 0;
    double fpos = 0.0;
    for (;;) {
      if (!p->cur) {
        if (p->q_head != p->q_tail) {
          BQBuffer b = p->q[p->q_head];
          p->q_head = (p->q_head + 1) % BQ_SLOTS;
          p->cur = b.data;
          p->cur_size = b.size;
        }
      }
      if (!p->cur) { SDL_UnlockMutex(p->lock); return; } // underrun: rest silent
      n = (long)(p->cur_size / (SLuint32)bps);
      if (n > 0 && (long)p->cur_fpos < n) {
        s = (const int16_t *)p->cur;
        fpos = p->cur_fpos;
        break;
      }
      // buffer consumed (or empty): carry remainder, drop it, notify engine
      p->cur_fpos -= (double)n;
      if (p->cur_fpos < 0.0) p->cur_fpos = 0.0;
      p->cur = NULL;
      p->cur_pos = p->cur_size = 0;
      p->queue_index++;
      slBufferQueueCallback cb = p->cb;
      void *cb_ctx = p->cb_ctx;
      SDL_UnlockMutex(p->lock);
      if (cb)
        cb(&p->bq_vt, cb_ctx);
      SDL_LockMutex(p->lock);
      if (!p->playing) { SDL_UnlockMutex(p->lock); return; }
    }

    const long idx = (long)fpos;
    const double frac = fpos - (double)idx;
    int32_t l, r;
    if (stereo) {
      const long j0 = idx * 2, j1 = (idx + 1 < n ? idx + 1 : idx) * 2;
      l = (int32_t)(s[j0]     * (1.0 - frac) + s[j1]     * frac);
      r = (int32_t)(s[j0 + 1] * (1.0 - frac) + s[j1 + 1] * frac);
    } else {
      const int16_t a = s[idx], b2 = (idx + 1 < n) ? s[idx + 1] : a;
      l = r = (int32_t)(a * (1.0 - frac) + b2 * frac);
    }
    acc[i * 2 + 0] += (int32_t)(l * g);
    acc[i * 2 + 1] += (int32_t)(r * g);
    p->cur_fpos += ratio;
  }
  SDL_UnlockMutex(p->lock);
}

static void mix_movie(int32_t *acc, int frames) {
  if (!g_movie_lock)
    return;

  SDL_LockMutex(g_movie_lock);
  if (!g_movie_active || g_movie_paused || !g_movie_pcm) {
    SDL_UnlockMutex(g_movie_lock);
    return;
  }

  const int n = g_movie_count < frames ? g_movie_count : frames;
  for (int i = 0; i < n; i++) {
    const int idx = (g_movie_head + i) % MOVIE_RING_FRAMES;
    acc[i * 2 + 0] += g_movie_pcm[idx * 2 + 0];
    acc[i * 2 + 1] += g_movie_pcm[idx * 2 + 1];
  }
  g_movie_head = (g_movie_head + n) % MOVIE_RING_FRAMES;
  g_movie_count -= n;
  g_movie_samples_played += (uint64_t)n;
  SDL_UnlockMutex(g_movie_lock);
}

static void SDLCALL audio_callback(void *ud, Uint8 *stream, int len) {
  (void)ud;

  // The engine's completion callback (fired from here via mix_player) reads its
  // stack-guard from tpidr_el0+0x28; SDL's audio thread never set that up. Install
  // the same shared guard block on this thread, once. Also pin it under
  // SINGLE_CORE so it can't run truly in parallel with the engine threads.
  static int tls_ready = 0;
  if (!tls_ready) { tls_setup_guard(); pin_current_thread(); tls_ready = 1; }

  const int frames = len / 4; // S16 stereo
  static int32_t acc[8192 * 2];
  if (frames > 8192) { memset(stream, 0, len); return; }
  memset(acc, 0, frames * 2 * sizeof(int32_t));

  /* Player handles are retained after Destroy, so snapshot the registry and
   * release its lock before mixing. Completion callbacks can then create or
   * destroy players without re-entering g_reg_lock. */
  Player *players[MAX_PLAYERS];
  int player_count = 0;
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count && player_count < MAX_PLAYERS; i++)
    if (g_players[i] && g_players[i]->in_use)
      players[player_count++] = g_players[i];
  SDL_UnlockMutex(g_reg_lock);
  for (int i = 0; i < player_count; i++)
    mix_player(players[i], acc, frames);

  mix_movie(acc, frames);
  mix_music(acc, frames);      /* streamed music path (CustomMediaPlayer) */

  int16_t *out = (int16_t *)stream;
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  static unsigned final_mix_calls;
  ++final_mix_calls;
  const int input_trace_trace = g_input_trace_mix_trace_remaining > 0;
  if (input_trace_trace) --g_input_trace_mix_trace_remaining;
  const int log_final_mix = final_mix_calls <= 4 ||
      (final_mix_calls % 1800) == 0 || input_trace_trace;
  int final_nonzero = 0;
  int final_peak = 0;
#endif
  for (int i = 0; i < frames * 2; i++) {
    int32_t v = acc[i];
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
    if (log_final_mix) {
      if (v) ++final_nonzero;
      const int magnitude = v < 0 ? (int)-v : (int)v;
      if (magnitude > final_peak) final_peak = magnitude;
    }
#endif
  }
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  if (log_final_mix)
    debugPrintf("opensles: FINAL MIX #%u frames=%d nonzero=%d/%d peak=%d players=%d input_trace=%d remain=%u\n",
                final_mix_calls, frames, final_nonzero, frames * 2, final_peak,
                player_count, input_trace_trace,
                (unsigned)g_input_trace_mix_trace_remaining);
#endif
}

void opensles_pump(void) {
  if (!g_dev)
    return;

  for (;;) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    Result rc = audoutGetReleasedAudioOutBuffer(&released, &count);
    if (R_FAILED(rc)) {
      debugPrintf("opensles: audout release query failed: %08x\n", rc);
      return;
    }
    if (!released || !count)
      return;

    audio_callback(NULL, released->buffer, OUT_BYTES);
    released->data_size = OUT_BYTES;
    released->data_offset = 0;
    rc = audoutAppendAudioOutBuffer(released);
    if (R_FAILED(rc)) {
      debugPrintf("opensles: audout append failed: %08x\n", rc);
      return;
    }
  }
}

void opensles_ensure_audio(void) { ensure_device(48000); }

static void ensure_device(int rate) {
  (void)rate; // players run at mixed rates (22050/44100); open at the Switch's
              // native 48000 and resample each player in mix_player instead of
              // letting the first player pin the device rate.
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (g_dev)
    return;
  if (R_FAILED(audoutInitialize())) {
    debugPrintf("opensles: audoutInitialize failed\n");
    return;
  }
  if (R_FAILED(audoutStartAudioOut())) {
    debugPrintf("opensles: audoutStartAudioOut failed\n");
    audoutExit();
    return;
  }
  g_dev_rate = (int)audoutGetSampleRate();
  for (int i = 0; i < OUT_BUFS; i++) {
    g_out_mem[i] = memalign(0x1000, OUT_BYTES);
    if (!g_out_mem[i]) {
      debugPrintf("opensles: audout buffer allocation failed\n");
      audoutStopAudioOut();
      audoutExit();
      for (int j = 0; j < i; j++) { free(g_out_mem[j]); g_out_mem[j] = NULL; }
      return;
    }
    memset(&g_out[i], 0, sizeof(g_out[i]));
    g_out[i].buffer = g_out_mem[i];
    g_out[i].buffer_size = OUT_BYTES;
    g_out[i].data_size = OUT_BYTES;
    memset(g_out_mem[i], 0, OUT_BYTES);
    audoutAppendAudioOutBuffer(&g_out[i]);
  }
  g_dev = 1;
  debugPrintf("opensles: audout ready (%d Hz, %u channels)\n",
              g_dev_rate, audoutGetChannelCount());
}

int opensles_movie_begin(int requested_rate) {
  if (!g_movie_lock)
    g_movie_lock = SDL_CreateMutex();
  if (!g_movie_pcm)
    g_movie_pcm = calloc(MOVIE_RING_FRAMES * 2, sizeof(int16_t));
  if (!g_movie_lock || !g_movie_pcm)
    return 0;

  ensure_device(requested_rate > 0 ? requested_rate : 44100);
  if (!g_dev)
    return 0;

  SDL_LockMutex(g_movie_lock);
  g_movie_active = 1;
  g_movie_paused = 1;
  g_movie_head = 0;
  g_movie_count = 0;
  g_movie_samples_queued = 0;
  g_movie_samples_played = 0;
  SDL_UnlockMutex(g_movie_lock);
  return g_dev_rate;
}

int opensles_movie_queue(const int16_t *pcm, int frames) {
  int done = 0;
  while (done < frames) {
    if (!g_movie_lock)
      return done;

    SDL_LockMutex(g_movie_lock);
    if (!g_movie_active || !g_movie_pcm) {
      SDL_UnlockMutex(g_movie_lock);
      return done;
    }

    const int space = MOVIE_RING_FRAMES - g_movie_count;
    int n = frames - done;
    if (n > space)
      n = space;
    for (int i = 0; i < n; i++) {
      const int idx = (g_movie_head + g_movie_count + i) % MOVIE_RING_FRAMES;
      g_movie_pcm[idx * 2 + 0] = pcm[(done + i) * 2 + 0];
      g_movie_pcm[idx * 2 + 1] = pcm[(done + i) * 2 + 1];
    }
    g_movie_count += n;
    g_movie_samples_queued += (uint64_t)n;
    SDL_UnlockMutex(g_movie_lock);

    done += n;
    if (done < frames)
      SDL_Delay(2);
  }
  return done;
}

void opensles_movie_set_paused(int paused) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  if (g_movie_active)
    g_movie_paused = paused != 0;
  SDL_UnlockMutex(g_movie_lock);
}

uint64_t opensles_movie_samples_queued(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_queued;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

uint64_t opensles_movie_samples_played(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_played;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

int opensles_movie_buffered_frames(void) {
  int ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_count;
  SDL_UnlockMutex(g_movie_lock);
  return ret;


}

void opensles_movie_end(void) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  g_movie_active = 0;
  g_movie_paused = 0;
  g_movie_head = 0;
  g_movie_count = 0;
  SDL_UnlockMutex(g_movie_lock);
}

// --- buffer queue interface -------------------------------------------------

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  const int next = (p->q_tail + 1) % BQ_SLOTS;
  if (next == p->q_head) { // full
    SDL_UnlockMutex(p->lock);
    debugPrintf("opensles: bq_Enqueue queue FULL\n");
    return SL_RESULT_PARAMETER_INVALID;
  }
  p->q[p->q_tail].data = pBuffer;
  p->q[p->q_tail].size = size;
  p->q_tail = next;
  const int q_depth = (p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS;
  SDL_UnlockMutex(p->lock);
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  static unsigned enq_count;
  const int input_trace_trace = g_input_trace_bq_trace_remaining > 0;
  if (input_trace_trace) --g_input_trace_bq_trace_remaining;
  const int log_enqueue = enq_count < 10 || (enq_count % 1800) == 0 ||
      input_trace_trace;
  if (log_enqueue) {
    const int16_t *samples = (const int16_t *)pBuffer;
    int nonzero = 0;
    int max_sample = 0;
    for (size_t i = 0; i < size / 2; ++i) {
      const int value = samples[i];
      if (value != 0) ++nonzero;
      const int magnitude = value < 0 ? -value : value;
      if (magnitude > max_sample) max_sample = magnitude;
    }
    debugPrintf("opensles: bq_Enqueue #%u player=%p rate=%d ch=%d playing=%d q=%d "
                "buf=%p size=%u nonzero=%d/%zu max=%d gain=%.2f input_trace=%d remain=%u\n",
                enq_count, (void *)p, p->rate, p->channels, p->playing, q_depth,
                pBuffer, size, nonzero, (size_t)size / 2u, max_sample, p->gain,
                input_trace_trace, (unsigned)g_input_trace_bq_trace_remaining);
  }
  ++enq_count;
#else
  (void)q_depth;
#endif
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  p->cur_pos = p->cur_size = 0;
  p->cur_fpos = 0.0;
  p->queue_index = 0;
  p->drained = 0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (pState) {
    SLBufferQueueState *st = pState;
    SDL_LockMutex(p->lock);
    st->count = (p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS + (p->cur ? 1 : 0);
    st->index = p->queue_index;
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->cb = cb;
  p->cb_ctx = ctx;
  const int kick = p->playing && !p->cur && (p->q_head == p->q_tail);
  SDL_UnlockMutex(p->lock);
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  debugPrintf("opensles: buffer callback registered (cb=%p kick=%d)\n", (void *)cb, kick);
#endif
  if (cb && kick)
    cb(&p->bq_vt, ctx);
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtable = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

// --- play interface ---------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = CONTAINER(self, Player, play_vt);
  SDL_LockMutex(p->lock);
  p->playing = (state == SL_PLAYSTATE_PLAYING);
  slBufferQueueCallback cb = (p->playing && !p->cur && (p->q_head == p->q_tail)) ? p->cb : NULL;
  void *cb_ctx = p->cb_ctx;
  SDL_UnlockMutex(p->lock);
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  debugPrintf("opensles: play state %u (kick_cb=%p)\n", state, (void *)cb);
#endif
  if (cb)
    cb(&p->bq_vt, cb_ctx);
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (pState) *pState = p->playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static const SLPlayItf_ play_vtable = {
  play_SetPlayState, play_GetPlayState, play_ret0_u32, play_ret0_u32,
  play_RegisterCallback, play_ok_u32, play_ret0_u32, play_ok_u32,
  play_ok, play_ret0_u32, play_ok_u32, play_ret0_u32,
};

// --- volume interface -------------------------------------------------------

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  Player *p = CONTAINER(self, Player, vol_vt);
  int mb = (int)level;
  if (mb > 0) mb = 0;
  if (mb < -9600) mb = -9600;
  SDL_LockMutex(p->lock);
  p->volume_level = (SLmillibel)mb;
  p->gain = p->muted ? 0.0f : mb_to_linear(p->volume_level);
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  const float gain = p->gain;
  const int muted = (int)p->muted;
#endif
  SDL_UnlockMutex(p->lock);
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  debugPrintf("opensles: vol_SetVolumeLevel(%d mB) -> stored=%d muted=%d gain=%.3f\n",
              (int)level, mb, muted, gain);
#endif
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *out) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (out) {
    SDL_LockMutex(p->lock);
    *out = p->volume_level;
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *out) {
  (void)self;
  if (out) *out = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_SetMute(void *self, SLboolean mute) {
  Player *p = CONTAINER(self, Player, vol_vt);
  SDL_LockMutex(p->lock);
  p->muted = mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  p->gain = p->muted ? 0.0f : mb_to_linear(p->volume_level);
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  const float gain = p->gain;
  const int muted = (int)p->muted;
#endif
  SDL_UnlockMutex(p->lock);
#if PVZ2_ENABLE_AUDIO_DIAGNOSTICS
  debugPrintf("opensles: vol_SetMute(%d) -> muted=%d gain=%.3f\n",
              (int)mute, muted, gain);
#endif
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(void *self, SLboolean *out) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (out) {
    SDL_LockMutex(p->lock);
    *out = p->muted;
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}
static SLresult vol_enable(void *self, SLboolean e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_isenabled(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setpos(void *self, SLint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_getpos(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLVolumeItf_ vol_vtable = {
  vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute,
  vol_GetMute, vol_enable, vol_isenabled, vol_setpos, vol_getpos,
};

// --- playback rate interface (accepted but not resampled) -------------------

static SLresult rate_SetRate(void *self, SLint16 r) { (void)self; (void)r; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRate(void *self, SLint16 *p) { (void)self; if (p) *p = 1000; return SL_RESULT_SUCCESS; }
static SLresult rate_SetProps(void *self, SLuint32 c) { (void)self; (void)c; return SL_RESULT_SUCCESS; }
static SLresult rate_GetProps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetCaps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRange(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop) {
  (void)self; (void)i;
  if (min) *min = 500;
  if (max) *max = 2000;
  if (step) *step = 1;
  if (prop) *prop = 0;
  return SL_RESULT_SUCCESS;
}
static const SLPlaybackRateItf_ rate_vtable = {
  rate_SetRate, rate_GetRate, rate_SetProps, rate_GetProps, rate_GetCaps, rate_GetRange,
};

// --- android configuration interface (accepted, ignored) --------------------

static SLresult cfg_SetConfiguration(void *self, const void *key, const void *value, SLuint32 sz) {
  (void)self; (void)key; (void)value; (void)sz; return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(void *self, const void *key, SLuint32 *psz, void *value) {
  (void)self; (void)key; (void)value; if (psz) *psz = 0; return SL_RESULT_SUCCESS;
}
static SLresult cfg_AcquireJavaProxy(void *self, SLuint32 t, void *p) {
  (void)self; (void)t; if (p) *(void **)p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult cfg_ReleaseJavaProxy(void *self, SLuint32 t) { (void)self; (void)t; return SL_RESULT_SUCCESS; }

static const SLAndroidConfigurationItf_ cfg_vtable = {
  cfg_SetConfiguration, cfg_GetConfiguration, cfg_AcquireJavaProxy, cfg_ReleaseJavaProxy,
};

// --- player object ----------------------------------------------------------

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void player_Destroy(void *self);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult obj_Abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult obj_SetPriority(void *self, SLint32 a, SLboolean b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLOC(void *self, SLint32 a, SLInterfaceID *b, SLboolean c) { (void)self; (void)a; (void)b; (void)c; return SL_RESULT_SUCCESS; }

static SLresult mix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  (void)self; (void)iid;
  if (pInterface) *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void simple_Destroy(void *self) { free(self); }

static const SLObjectItf_ player_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback,
  obj_Abort, player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};
static const SLObjectItf_ mix_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) {
    *(void **)pInterface = &p->play_vt;
  } else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)pInterface = &p->bq_vt;
  } else if (iid == SL_IID_VOLUME) {
    *(void **)pInterface = &p->vol_vt;
  } else if (iid == SL_IID_PLAYBACKRATE) {
    *(void **)pInterface = &p->rate_vt;
  } else if (iid == SL_IID_ANDROIDCONFIGURATION) {
    *(void **)pInterface = &p->config_vt;
  } else {
    *(void **)pInterface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == p) g_players[i] = NULL;
  SDL_LockMutex(p->lock);
  p->in_use = 0;
  p->playing = 0;
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  p->cb = NULL;
  SDL_UnlockMutex(p->lock);
  SDL_UnlockMutex(g_reg_lock);
  /* ponytail: retain destroyed handles because Wwise can enqueue concurrently;
   * reclaim only after adding reference-counted callback shutdown. */
}

// --- engine interface -------------------------------------------------------

static SLresult eng_CreateAudioPlayer(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)snk; (void)numIfaces; (void)ids; (void)req;
  if (!pPlayer)
    return SL_RESULT_PARAMETER_INVALID;

  Player *p = calloc(1, sizeof(*p));
  if (!p)
    return SL_RESULT_PARAMETER_INVALID;
  p->obj_vt = &player_obj_vtable;
  p->play_vt = &play_vtable;
  p->bq_vt = &bq_vtable;
  p->vol_vt = &vol_vtable;
  p->rate_vt = &rate_vtable;
  p->config_vt = &cfg_vtable;
  p->in_use = 1;
  p->gain = 1.0f;
  p->volume_level = 0;
  p->muted = SL_BOOLEAN_FALSE;
  p->queue_index = 0;
  p->channels = 2;
  p->rate = 44100;
  p->lock = SDL_CreateMutex();

  if (src && src->pFormat) {
    const SLDataFormat_PCM *fmt = src->pFormat;
    if (fmt->formatType == 2 /* SL_DATAFORMAT_PCM */) {
      p->channels = fmt->numChannels ? (int)fmt->numChannels : 2;
      p->rate = fmt->samplesPerSec ? (int)(fmt->samplesPerSec / 1000) : 44100;
    }
  }

  ensure_device(p->rate);
  debugPrintf("opensles: audio player %d Hz, %d channels\n", p->rate, p->channels);

  SDL_LockMutex(g_reg_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot >= 0) g_players[slot] = p;
  SDL_UnlockMutex(g_reg_lock);

  if (slot < 0) {
    SDL_DestroyMutex(p->lock);
    free(p);
    return SL_RESULT_PARAMETER_INVALID;
  }

  *pPlayer = &p->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, SLObjectItf *pMix, SLuint32 numIfaces,
                                    const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)numIfaces; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m)
    return SL_RESULT_PARAMETER_INVALID;
  m->obj_vt = &mix_obj_vtable;
  if (pMix) *pMix = &m->obj_vt;
  debugPrintf("opensles: output mix created\n");
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const SLEngineItf_ engine_vtable = {
  .CreateLEDDevice = (void *)eng_unsupported,
  .CreateVibraDevice = (void *)eng_unsupported,
  .CreateAudioPlayer = eng_CreateAudioPlayer,
  .CreateAudioRecorder = (void *)eng_unsupported,
  .CreateMidiPlayer = (void *)eng_unsupported,
  .CreateListener = (void *)eng_unsupported,
  .Create3DGroup = (void *)eng_unsupported,
  .CreateOutputMix = eng_CreateOutputMix,
  .CreateMetadataExtractor = (void *)eng_unsupported,
  .CreateExtensionObject = (void *)eng_unsupported,
  .QueryNumSupportedInterfaces = (void *)eng_unsupported,
  .QuerySupportedInterfaces = (void *)eng_unsupported,
  .QueryNumSupportedExtensions = (void *)eng_unsupported,
  .QuerySupportedExtension = (void *)eng_unsupported,
  .IsExtensionSupported = (void *)eng_unsupported,
};

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)pInterface = &e->eng_vt;
    return SL_RESULT_SUCCESS;
  }
  *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLObjectItf_ engine_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

// --- entry point ------------------------------------------------------------

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (!pEngine)
    return SL_RESULT_PARAMETER_INVALID;
  Engine *e = calloc(1, sizeof(*e));
  if (!e)
    return SL_RESULT_PARAMETER_INVALID;
  e->obj_vt = &engine_obj_vtable;
  e->eng_vt = &engine_vtable;
  *pEngine = &e->obj_vt;
  debugPrintf("opensles: slCreateEngine succeeded\n");
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  opensles_movie_end();
  if (g_dev) {
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < OUT_BUFS; i++) {
      free(g_out_mem[i]);
      g_out_mem[i] = NULL;
    }
    g_dev = 0;
  }
  free(g_movie_pcm);
  g_movie_pcm = NULL;
  if (g_movie_lock) {
    SDL_DestroyMutex(g_movie_lock);
    g_movie_lock = NULL;
  }
}
