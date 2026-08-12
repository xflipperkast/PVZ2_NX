/* music.h -- Android-compatible music path (CustomMediaPlayer / DroidMusicManager). */
#ifndef __MUSIC_H__
#define __MUSIC_H__

#include <stdint.h>

/* Handle-returning ops: the void* is what we hand the engine as the
 * "CustomMediaPlayer" jobject. NULL on failure. */
void *music_load(const char *path);
void  music_unload(void *h);

void  music_play(void *h, int loop);
void  music_pause(void *h, int paused);
void  music_stop(void *h);
void  music_set_volume(void *h, float v);

/* Mixed into the SDL audio callback (called from opensles.c audio_callback).
 * acc = `frames` interleaved-stereo int32 accumulators at the 48 kHz device rate. */
void  mix_music(int32_t *acc, int frames);

#endif
