/*
 * Audio. The DirectSound shim feeds PCM through this layer.
 *
 * The ring buffer maps onto a DirectSound secondary buffer: the writer side
 * corresponds to Lock/Unlock, and vt3_audio_played_bytes() to
 * GetCurrentPosition. Output is signed 16-bit interleaved only.
 */
#ifndef VT3_AUDIO_H
#define VT3_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* Single-producer, single-consumer byte ring. Safe across two threads. */
typedef struct vt3_ringbuf vt3_ringbuf;
vt3_ringbuf *vt3_rb_create(size_t capacity);
void         vt3_rb_destroy(vt3_ringbuf *rb);
size_t       vt3_rb_write(vt3_ringbuf *rb, const void *src, size_t n);  /* bytes accepted */
size_t       vt3_rb_read(vt3_ringbuf *rb, void *dst, size_t n);         /* bytes returned */
size_t       vt3_rb_used(const vt3_ringbuf *rb);
size_t       vt3_rb_space(const vt3_ringbuf *rb);
size_t       vt3_rb_capacity(const vt3_ringbuf *rb);
/* Drops everything queued. Only the producer may call it, and only while no consumer is
 * running (the Android recovery path calls it with the old stream closed). */
void         vt3_rb_reset(vt3_ringbuf *rb);

typedef struct vt3_audio_format {
    uint32_t sample_rate;   /* e.g. 44100 or 48000 */
    int      channels;      /* 1 or 2 */
} vt3_audio_format;

typedef struct vt3_audio vt3_audio;

/* Opens the default playback device through SDL. Returns 0 on success. */
int      vt3_audio_open(vt3_audio **out, vt3_audio_format fmt, size_t buffer_bytes);
void     vt3_audio_close(vt3_audio *a);
size_t   vt3_audio_write(vt3_audio *a, const void *pcm, size_t bytes);
size_t   vt3_audio_space(const vt3_audio *a);
uint64_t vt3_audio_played_bytes(const vt3_audio *a);
void     vt3_audio_set_playing(vt3_audio *a, int playing);
/* Underruns: a callback got a partial buffer. Starved: a callback got nothing at all while
 * playback was on, which is what a writer that fell behind or a gated stretch sounds like. */
uint32_t vt3_audio_underruns(const vt3_audio *a);
uint32_t vt3_audio_starved(const vt3_audio *a);

/* One snapshot of the output, for the diagnostics report. Returns 0 when there is no output
 * (the pointer is NULL, or the backend does not track stream state). */
typedef struct vt3_audio_info {
    int open;                    /* an output object exists */
    int streaming;               /* a device stream is open and running */
    int paused;                  /* the platform asked for silence */
    int channels;
    uint32_t sample_rate;
    uint32_t underruns;
    uint32_t starved;
    uint32_t reopen_failures;     /* consecutive failed reopen attempts */
} vt3_audio_info;
int vt3_audio_info_get(const vt3_audio *a, vt3_audio_info *out);
/* 1 while a device stream is open and running. With none (a failed open, or a disconnect being
 * recovered) the DirectSound mixer keeps its timeline on the wall clock and drops the audio. */
int      vt3_audio_streaming(const vt3_audio *a);
/* 1 while the platform has the output paused (backgrounded, or no window). The mixer freezes
 * its play clocks for the pause, so resuming continues the stream instead of jumping ahead. */
int      vt3_audio_is_paused(const vt3_audio *a);

/* Android only, called from the platform pump: pause/resume the open output while the app has no
 * window (the ring is kept), and reopen a stream the device dropped (a route change). A reopen
 * that fails is retried with a backoff; it is never treated as final. */
void     vt3_audio_pause(int paused);
void     vt3_audio_recover(void);

/* Writes up to max_frames frames of a sine wave; *phase carries across calls. */
size_t   vt3_audio_fill_sine(vt3_audio *a, double freq_hz, double *phase,
                             size_t max_frames, int16_t amplitude);

#endif /* VT3_AUDIO_H */
