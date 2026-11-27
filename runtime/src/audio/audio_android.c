/*
 * Audio output on Android through AAudio. A data callback on AAudio's thread drains the ring
 * that the game-side writer fills; an empty or short ring is silence. Same contract as
 * audio_sdl.c (vt3_audio.h).
 *
 * Two states matter to the DirectSound mixer, which asks about them through
 * vt3_audio_streaming() and vt3_audio_is_paused():
 *   - paused: the platform has no window or the activity is in the background. No sound is
 *     wanted, and the play clocks must not advance, or returning to the game would jump by the
 *     time spent away into blocks the writer never produced.
 *   - not streaming: there is no usable stream (the first open failed, or the device dropped
 *     it). Recovery keeps trying; a failed reopen is never treated as final.
 */
#include "vt3/vt3_audio.h"
#include "vt3/vt3_diag.h"
#include "vt3/vt3_log.h"

#include <aaudio/AAudio.h>

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct vt3_audio {
    AAudioStream *stream;
    vt3_ringbuf *rb;
    vt3_audio_format fmt;
    size_t frame_bytes;
    _Atomic int playing;
    _Atomic int paused;           /* the platform wants no sound (backgrounded, no window) */
    _Atomic uint32_t underruns;   /* a callback got a partial buffer */
    _Atomic uint32_t starved;     /* a callback got nothing at all while playback was on */
    _Atomic uint64_t played;
    _Atomic int lost;             /* set by the error callback: the device dropped the stream */
    _Atomic uint32_t reopen_failures;
    _Atomic uint64_t retry_at_ns; /* a reopen is not attempted before this */
};

static vt3_audio *g_open;        /* the one open output, for the platform pump's pause/recover */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static aaudio_data_callback_result_t data_callback(AAudioStream *stream, void *user, void *audio,
                                                   int32_t num_frames)
{
    (void)stream;
    vt3_audio *a = user;
    size_t want = (size_t)num_frames * a->frame_bytes;
    size_t got = 0;
    if (atomic_load(&a->playing)) {
        got = vt3_rb_read(a->rb, audio, want);
        /* Only a partial read counts as an underrun: the writer fell behind mid-stream. A
         * callback that got nothing at all is a starved sink (the mixer is not filling the
         * ring: paused, or waiting on the writer) and is counted separately, because the two
         * mean different things when the audio sounds wrong. An empty ring at startup, or
         * after the sound ends, reads the same way and is counted too. */
        if (got > 0 && got < want) atomic_fetch_add(&a->underruns, 1);
        else if (got == 0 && want) atomic_fetch_add(&a->starved, 1);
        atomic_fetch_add(&a->played, got);
    }
    if (got < want) memset((uint8_t *)audio + got, 0, want - got);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* Runs on an AAudio thread, where the stream must not be closed: the platform pump reopens it. */
static void error_callback(AAudioStream *stream, void *user, aaudio_result_t error)
{
    (void)stream;
    (void)error;
    atomic_store(&((vt3_audio *)user)->lost, 1);
}

/* Opens and starts a->stream on the default device. Returns 0 on success. */
static int open_stream(vt3_audio *a)
{
    vt3_audio_format fmt = a->fmt;
    AAudioStreamBuilder *b = NULL;
    a->stream = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        VT3_CERROR("audio", (int)AAUDIO_ERROR_INTERNAL, "AAudio_createStreamBuilder failed");
        vt3_diag_counter("audio_open_failures", 1);
        return -1;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)fmt.sample_rate);
    AAudioStreamBuilder_setChannelCount(b, fmt.channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    /* The device-side buffer, sized from the sample rate rather than left to the platform: 20 ms.
     * That is a few bursts on a phone, enough for the callback to ride out a scheduling delay
     * while the game thread holds the CPU, and far short of the latency a "bigger buffer" fix
     * would add. The ring behind it (see MIX_RING_MS) is what absorbs a longer stall. */
    int32_t capacity = (int32_t)(fmt.sample_rate / 50);
    if (capacity < 256) capacity = 256;
    AAudioStreamBuilder_setBufferCapacityInFrames(b, capacity);
    AAudioStreamBuilder_setDataCallback(b, data_callback, a);
    AAudioStreamBuilder_setErrorCallback(b, error_callback, a);
    aaudio_result_t rc = AAudioStreamBuilder_openStream(b, &a->stream);
    AAudioStreamBuilder_delete(b);
    if (rc != AAUDIO_OK || !a->stream) {
        VT3_CERROR("audio", (int)rc, "AAudio open failed: %s", AAudio_convertResultToText(rc));
        vt3_diag_counter("audio_open_failures", 1);
        a->stream = NULL;
        return -1;
    }
    rc = AAudioStream_requestStart(a->stream);
    if (rc != AAUDIO_OK) {
        VT3_CERROR("audio", (int)rc, "AAudio start failed: %s", AAudio_convertResultToText(rc));
        vt3_diag_counter("audio_open_failures", 1);
        AAudioStream_close(a->stream);
        a->stream = NULL;
        return -1;
    }
    VT3_CINFO("audio", "%u Hz, %d ch, S16 (AAudio), ring %zu bytes (%u ms), burst %d frames, "
              "buffer %d frames (%.1f ms), performance mode %d",
              fmt.sample_rate, fmt.channels, vt3_rb_capacity(a->rb),
              (unsigned)(vt3_rb_capacity(a->rb) * 1000u /
                         ((size_t)fmt.sample_rate * a->frame_bytes ?
                          (size_t)fmt.sample_rate * a->frame_bytes : 1u)),
              AAudioStream_getFramesPerBurst(a->stream),
              AAudioStream_getBufferSizeInFrames(a->stream),
              AAudioStream_getBufferSizeInFrames(a->stream) * 1000.0 / (double)fmt.sample_rate,
              (int)AAudioStream_getPerformanceMode(a->stream));
    return 0;
}

int vt3_audio_open(vt3_audio **out, vt3_audio_format fmt, size_t buffer_bytes)
{
    *out = NULL;
    vt3_audio *a = calloc(1, sizeof *a);
    if (!a) return -1;
    a->fmt = fmt;
    a->frame_bytes = (size_t)fmt.channels * sizeof(int16_t);
    a->rb = vt3_rb_create(buffer_bytes);
    if (!a->rb) { free(a); return -1; }
    if (open_stream(a) != 0) {
        /* The output can be busy or not up yet at a cold start (another app holding it, or the
         * device still settling). Keep the object and let the platform pump retry: failing the
         * open here would leave the game silent for the whole session. While there is no
         * stream the mixer drops audio and keeps its timeline on the wall clock. */
        VT3_CWARN("audio", "no output yet; the platform pump will retry");
        atomic_store(&a->lost, 1);
        atomic_store(&a->retry_at_ns, now_ns() + 250000000u);
        g_open = a;
        vt3_diag_bind_audio(a);
        *out = a;
        return 0;
    }
    g_open = a;
    vt3_diag_bind_audio(a);
    *out = a;
    return 0;
}

void vt3_audio_pause(int paused)
{
    vt3_audio *a = g_open;
    if (!a) return;
    atomic_store(&a->paused, paused ? 1 : 0);
    if (!a->stream) {
        /* Nothing to pause. Resuming is a good moment to retry an output that never opened. */
        if (!paused) atomic_store(&a->retry_at_ns, 0);
        return;
    }
    aaudio_result_t rc = paused ? AAudioStream_requestPause(a->stream)
                                : AAudioStream_requestStart(a->stream);
    if (rc != AAUDIO_OK)
        VT3_CWARN("audio", "%s failed: %s", paused ? "pause" : "restart",
                  AAudio_convertResultToText(rc));
    else
        VT3_CINFO("audio", "output %s", paused ? "paused" : "resumed");
    vt3_diag_counter(paused ? "audio_pauses" : "audio_resumes", 1);
}

int vt3_audio_streaming(const vt3_audio *a)
{
    return a && a->stream && !atomic_load(&a->lost);
}

int vt3_audio_is_paused(const vt3_audio *a)
{
    return a && atomic_load(&a->paused);
}

void vt3_audio_recover(void)
{
    vt3_audio *a = g_open;
    if (!a) return;
    if (atomic_load(&a->paused)) return;          /* no output while backgrounded; resume retries */
    if (a->stream && !atomic_load(&a->lost)) return;
    uint64_t now = now_ns();
    if (now < atomic_load(&a->retry_at_ns)) return;

    uint32_t fails = atomic_load(&a->reopen_failures);
    VT3_CWARN("audio", "reopening the output (attempt %u)", fails + 1);
    vt3_diag_counter("audio_reopen_attempts", 1);
    atomic_store(&a->lost, 0);
    if (a->stream) {
        AAudioStream_requestStop(a->stream);
        AAudioStream_close(a->stream);
        a->stream = NULL;
    }
    /* No consumer is running now, so the ring can be emptied: what it holds was produced for
     * the stream that just closed, and playing it after the gap would only be stale. */
    size_t stale = vt3_rb_used(a->rb);
    if (stale) vt3_diag_counter("audio_stale_resets", 1);
    vt3_rb_reset(a->rb);
    if (open_stream(a) != 0) {
        fails++;
        atomic_store(&a->reopen_failures, fails);
        atomic_store(&a->lost, 1);                /* not final: keep trying */
        uint32_t ms = fails < 4 ? 250u : 1000u;   /* back off once it keeps failing */
        atomic_store(&a->retry_at_ns, now + (uint64_t)ms * 1000000u);
        VT3_CWARN("audio", "reopen failed (%u so far); retrying in %u ms", fails, ms);
        vt3_diag_counter("audio_reopen_failures_total", 1);
        return;
    }
    atomic_store(&a->reopen_failures, 0);
    atomic_store(&a->retry_at_ns, 0);
    VT3_CINFO("audio", "output reopened after %u failed attempt(s); %zu stale bytes dropped",
              fails, stale);
}

int vt3_audio_info_get(const vt3_audio *a, vt3_audio_info *out)
{
    if (!a || !out) return 0;
    memset(out, 0, sizeof *out);
    out->open = 1;
    out->streaming = vt3_audio_streaming(a);
    out->paused = vt3_audio_is_paused(a);
    out->channels = a->fmt.channels;
    out->sample_rate = a->fmt.sample_rate;
    out->underruns = atomic_load(&a->underruns);
    out->starved = atomic_load(&a->starved);
    out->reopen_failures = atomic_load(&a->reopen_failures);
    return 1;
}

void vt3_audio_close(vt3_audio *a)
{
    if (!a) return;
    if (a == g_open) {
        g_open = NULL;
        vt3_diag_bind_audio(NULL);
    }
    if (a->stream) {
        AAudioStream_requestStop(a->stream);
        AAudioStream_close(a->stream);
    }
    vt3_rb_destroy(a->rb);
    free(a);
}

size_t vt3_audio_write(vt3_audio *a, const void *pcm, size_t bytes)
{
    size_t whole = bytes - (bytes % a->frame_bytes);   /* never split a frame across the ring */
    return vt3_rb_write(a->rb, pcm, whole);
}

size_t vt3_audio_space(const vt3_audio *a)
{
    return vt3_rb_space(a->rb);
}

uint64_t vt3_audio_played_bytes(const vt3_audio *a)
{
    return atomic_load(&a->played);
}

void vt3_audio_set_playing(vt3_audio *a, int playing)
{
    atomic_store(&a->playing, playing ? 1 : 0);
}

uint32_t vt3_audio_underruns(const vt3_audio *a)
{
    return atomic_load(&a->underruns);
}

uint32_t vt3_audio_starved(const vt3_audio *a)
{
    return atomic_load(&a->starved);
}

size_t vt3_audio_fill_sine(vt3_audio *a, double freq_hz, double *phase, size_t max_frames, int16_t amplitude)
{
    size_t space_frames = vt3_rb_space(a->rb) / a->frame_bytes;
    size_t frames = max_frames < space_frames ? max_frames : space_frames;
    int16_t buf[2 * 4096];
    if (frames > 4096) frames = 4096;
    int ch = a->fmt.channels;
    double step = 2.0 * M_PI * freq_hz / (double)a->fmt.sample_rate;
    for (size_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(sin(*phase) * amplitude);
        for (int c = 0; c < ch; c++) buf[i * ch + c] = v;
        *phase += step;
        if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
    }
    return vt3_rb_write(a->rb, buf, frames * a->frame_bytes) / a->frame_bytes;
}
