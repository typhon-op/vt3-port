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
#include <pthread.h>
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
    _Atomic int have_stream;      /* a->stream is open: read from the mixer thread and the pump */
    _Atomic uint32_t underruns;   /* a callback got a partial buffer */
    _Atomic uint32_t starved;     /* a callback got nothing at all while playback was on */
    _Atomic uint64_t played;
    _Atomic int lost;             /* set by the error callback: the device dropped the stream */
    _Atomic uint32_t reopen_failures;
    _Atomic uint64_t retry_at_ns; /* a reopen is not attempted before this */
    /* Stream transitions (open, pause, resume, reopen, close) are serialised by this lock: the
     * reopen watchdog below runs on its own thread instead of on the platform thread. */
    pthread_mutex_t api_mu;
    pthread_cond_t wake_cv;       /* timed on CLOCK_MONOTONIC; signalled when the state changes */
    pthread_t watchdog;
    int watchdog_on;              /* plain: set before the object is published, read by close */
    int stopping;                 /* close is in progress: written before api_mu, read under it */
};

static vt3_audio *g_open;        /* the one open output, for the platform pump's pause/recover */
/* Guards g_open itself. The pointer is published and cleared on the guest thread, but
 * vt3_audio_pause runs on the platform thread: without this it could load the pointer just before
 * a close freed the object and then lock a destroyed mutex. Lock order is g_open_mu, then api_mu, in
 * every path that takes both. */
static pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;
/* What the platform last asked for. Remembered even while there is no output, because an output
 * opened during a background stretch must not start playing behind another app. */
static _Atomic int g_platform_paused;

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
    atomic_store(&a->have_stream, 1);
    rc = AAudioStream_requestStart(a->stream);
    if (rc != AAUDIO_OK) {
        VT3_CERROR("audio", (int)rc, "AAudio start failed: %s", AAudio_convertResultToText(rc));
        vt3_diag_counter("audio_open_failures", 1);
        AAudioStream_close(a->stream);
        a->stream = NULL;
        atomic_store(&a->have_stream, 0);
        return -1;
    }
    /* Started, then silenced if the app was already in the background: the ring is empty at this
     * point, so pausing now plays nothing at all. */
    if (atomic_load(&g_platform_paused)) {
        aaudio_result_t pr = AAudioStream_requestPause(a->stream);
        if (pr == AAUDIO_OK) atomic_store(&a->paused, 1);
        else VT3_CWARN("audio", "pause on open failed: %s", AAudio_convertResultToText(pr));
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

/* One reopen attempt, with api_mu held: drop what is left of the old stream, empty the ring (what
 * it holds was produced for that stream) and open a new one. Never final: a failure only schedules
 * the next attempt. */
static void reopen_locked(vt3_audio *a)
{
    if (atomic_load(&a->paused)) return;          /* no output wanted while backgrounded */
    if (a->stream && !atomic_load(&a->lost)) return;
    uint64_t now = now_ns();
    if (now < atomic_load(&a->retry_at_ns)) return;

    uint32_t fails = atomic_load(&a->reopen_failures);
    VT3_CWARN("audio", "reopening the output (attempt %u)", fails + 1);
    vt3_diag_counter("audio_reopen_attempts", 1);
    atomic_store(&a->lost, 0);
    if (a->stream) {
        atomic_store(&a->have_stream, 0);
        AAudioStream_requestStop(a->stream);
        AAudioStream_close(a->stream);
        a->stream = NULL;
    }
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

/* Keeps trying to restore the stream whether or not the game produces frames. It used to happen
 * only from vt3_audio_recover(), called once per frame by the platform pump: a stalled game, a
 * load or a paused frame loop left the output silent with nothing driving the retry. This thread
 * belongs to the output object (started in vt3_audio_open, joined in vt3_audio_close), so its
 * lifetime cannot outlive the stream or the ring. */
static void *watchdog_main(void *arg)
{
    vt3_audio *a = arg;
    for (;;) {
        pthread_mutex_lock(&a->api_mu);
        if (a->stopping) {
            pthread_mutex_unlock(&a->api_mu);
            break;
        }
        reopen_locked(a);                          /* immediate when the state changed */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += 200000000;                   /* 200 ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&a->wake_cv, &a->api_mu, &ts);
        pthread_mutex_unlock(&a->api_mu);
    }
    return NULL;
}

/* Starts the reopen watchdog. A thread that cannot be created only costs the frame-driven retry,
 * so it is a warning, not a failure of the open. */
static void start_watchdog(vt3_audio *a)
{
    if (pthread_create(&a->watchdog, NULL, watchdog_main, a) == 0) {
        a->watchdog_on = 1;
    } else {
        a->watchdog_on = 0;
        VT3_CWARN("audio", "could not start the reopen watchdog; recovery falls back to the frame loop");
    }
}

int vt3_audio_open(vt3_audio **out, vt3_audio_format fmt, size_t buffer_bytes)
{
    *out = NULL;
    vt3_audio *a = calloc(1, sizeof *a);
    if (!a) return -1;
    /* The watchdog sleeps on this condition; CLOCK_MONOTONIC, like every clock in the port. */
    pthread_mutex_init(&a->api_mu, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&a->wake_cv, &attr);
    pthread_condattr_destroy(&attr);
    a->fmt = fmt;
    a->frame_bytes = (size_t)fmt.channels * sizeof(int16_t);
    atomic_store(&a->paused, atomic_load(&g_platform_paused));
    a->rb = vt3_rb_create(buffer_bytes);
    if (!a->rb) {
        pthread_cond_destroy(&a->wake_cv);
        pthread_mutex_destroy(&a->api_mu);
        free(a);
        return -1;
    }
    if (open_stream(a) != 0) {
        /* The output can be busy or not up yet at a cold start (another app holding it, or the
         * device still settling). Keep the object and let the watchdog retry: failing the open
         * here would leave the game silent for the whole session. While there is no stream the
         * mixer drops audio and keeps its timeline on the wall clock. */
        VT3_CWARN("audio", "no output yet; the watchdog will retry");
        atomic_store(&a->lost, 1);
        atomic_store(&a->retry_at_ns, now_ns() + 250000000u);
    }
    start_watchdog(a);              /* one thread for the retry, open or not */
    pthread_mutex_lock(&g_open_mu);
    g_open = a;
    pthread_mutex_unlock(&g_open_mu);
    vt3_diag_bind_audio(a);
    *out = a;
    return 0;
}

void vt3_audio_pause(int paused)
{
    int want = paused ? 1 : 0;
    atomic_store(&g_platform_paused, want);  /* remembered even with no output to pause */
    pthread_mutex_lock(&g_open_mu);          /* the close path frees the object under this */
    vt3_audio *a = g_open;
    if (!a) {
        pthread_mutex_unlock(&g_open_mu);
        return;
    }
    pthread_mutex_lock(&a->api_mu);          /* not while the watchdog is replacing the stream */
    if (atomic_load(&a->paused) != want) {   /* already in that state: no AAudio churn */
        if (!a->stream) {
            /* Nothing to pause. Resuming is a good moment to retry an output that never opened. */
            atomic_store(&a->paused, want);
            if (!paused) atomic_store(&a->retry_at_ns, 0);
        } else {
            aaudio_result_t rc = paused ? AAudioStream_requestPause(a->stream)
                                        : AAudioStream_requestStart(a->stream);
            if (rc != AAUDIO_OK) {
                VT3_CWARN("audio", "%s failed: %s", paused ? "pause" : "restart",
                          AAudio_convertResultToText(rc));
                /* A resume the stream refuses means it is no longer in a state we can use (the
                 * device dropped it): the watchdog reopens it rather than the game staying
                 * silent. */
                if (!paused) atomic_store(&a->lost, 1);
            } else {
                atomic_store(&a->paused, want);
                VT3_CINFO("audio", "output %s", paused ? "paused" : "resumed");
                vt3_diag_counter(paused ? "audio_pauses" : "audio_resumes", 1);
            }
        }
    }
    /* Wake the watchdog: a resume with no usable stream should not wait for the next tick. */
    pthread_cond_signal(&a->wake_cv);
    pthread_mutex_unlock(&a->api_mu);
    pthread_mutex_unlock(&g_open_mu);
}

int vt3_audio_streaming(const vt3_audio *a)
{
    /* The stream pointer itself is not read here: it is set and cleared on the platform thread,
     * while this is called from the mixer thread. */
    return a && atomic_load(&a->have_stream) && !atomic_load(&a->lost);
}

int vt3_audio_is_paused(const vt3_audio *a)
{
    return a && atomic_load(&a->paused);
}

/* An immediate retry, for the platform pump: the watchdog already owns the retry schedule, so a
 * running game gets its stream back at the next frame instead of at the next tick, and the two
 * callers cannot reopen at once (reopen_locked only runs with api_mu held). Taking g_open_mu makes
 * it a fast poke in the common case (no stream lost: nothing to do) without reading a pointer that
 * a close on another thread could be freeing. */
void vt3_audio_recover(void)
{
    pthread_mutex_lock(&g_open_mu);
    vt3_audio *a = g_open;
    if (a && atomic_load(&a->lost)) {
        pthread_mutex_lock(&a->api_mu);
        reopen_locked(a);
        pthread_mutex_unlock(&a->api_mu);
    }
    pthread_mutex_unlock(&g_open_mu);
}

/* The output ring's level in milliseconds, for the diagnostics snapshot. */
static uint32_t rb_ms(const vt3_audio *a, size_t bytes)
{
    size_t per_ms = (size_t)a->fmt.sample_rate * a->frame_bytes / 1000u;
    return per_ms ? (uint32_t)(bytes / per_ms) : 0;
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
    out->ring_ms = rb_ms(a, vt3_rb_capacity(a->rb));
    out->ring_used_ms = rb_ms(a, vt3_rb_used(a->rb));
    out->ring_peak_ms = rb_ms(a, vt3_rb_peak(a->rb));
    return 1;
}

void vt3_audio_close(vt3_audio *a)
{
    if (!a) return;
    /* Held for the whole teardown, so a pause on the platform thread either finishes first or sees
     * the cleared pointer: it must not lock api_mu on an object this is about to free. */
    pthread_mutex_lock(&g_open_mu);
    if (a == g_open) {
        g_open = NULL;
        vt3_diag_bind_audio(NULL);
    }
    /* Stop the watchdog first, and wait for it: it may be inside a reopen, and it must not touch
     * the stream or the ring once they are freed. */
    a->stopping = 1;
    pthread_mutex_lock(&a->api_mu);
    pthread_cond_signal(&a->wake_cv);            /* wake a timed wait so the join is prompt */
    pthread_mutex_unlock(&a->api_mu);
    if (a->watchdog_on) {
        pthread_join(a->watchdog, NULL);
        a->watchdog_on = 0;
    }
    pthread_mutex_lock(&a->api_mu);
    if (a->stream) {
        atomic_store(&a->have_stream, 0);
        AAudioStream_requestStop(a->stream);
        AAudioStream_close(a->stream);
        a->stream = NULL;
    }
    pthread_mutex_unlock(&a->api_mu);
    vt3_rb_destroy(a->rb);
    pthread_cond_destroy(&a->wake_cv);
    pthread_mutex_destroy(&a->api_mu);
    free(a);
    pthread_mutex_unlock(&g_open_mu);
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
