/*
 * The DirectSound mixer core: the parts of the audio path that are pure arithmetic and hold no
 * lock, no device and no guest memory. dsound_stub.c owns the buffers, the mixer thread and the
 * output; this module decides what one output frame sounds like, and how many frames a tick owes.
 *
 * It lives here rather than in the shim because all of it is checkable on a host build with no
 * AAudio, no SDL and no game data: tools/audiotest/AudioHostTest.c links this file and
 * src/audio/ringbuf.c and checks resampling, the gate, the output stage, the tick schedule and the
 * pause clock. runtime/tests/selftest.c keeps its own checks on the same code (through
 * win32/dsound.h, which includes this header).
 *
 * Nothing here allocates, locks or reads a clock: every decision is a function of its arguments,
 * so a test can reproduce a 300 ms stall or a 20 minute pause exactly.
 */
#ifndef VT3_MIX_H
#define VT3_MIX_H

#include <stddef.h>
#include <stdint.h>

/* Gate granularity: one "the game wrote this" flag per 256 bytes of a secondary buffer. */
#define VT3_MIX_FRESH_BLOCK  256u
/* Q15 gate: full level. */
#define VT3_MIX_GATE_MAX     32768
/* Gate ramp length, 2 ms at 48 kHz: long enough to hide a click, short enough to be inaudible.
 * Used to fade in a sound at Play, and as the length of the notch across a content change. */
#define VT3_MIX_RAMP_FRAMES  96u
/* A tick this far behind the schedule re-anchors the timeline instead of trying to catch it up:
 * the ring cannot hold that much anyway, and sprinting would just fill it and drop the same audio. */
#define VT3_MIX_RESYNC_NS    250000000ull
/* What a re-anchored tick renders: 10 ms, enough to prime the ring without a burst. */
#define VT3_MIX_RESYNC_PRIME_NS 10000000ull

typedef struct ds_mix_state {
    uint64_t phase;      /* Q32 source frame index, absolute from the buffer's start */
    uint32_t last_blk;   /* FRESH_BLOCK block the last frame read, or UINT32_MAX */
    int32_t gate;        /* Q15 output gate: 0 silent, VT3_MIX_GATE_MAX full */
    uint32_t dip_frames; /* trace: output frames rendered below full gate (cleared by the reader) */
    uint32_t stale_events; /* trace: stretches the mixer read a block the game had not rewritten */
    uint32_t stale_blk;    /* trace: FRESH_BLOCK block where the last stale stretch began */
    int32_t stale;         /* trace: 1 while the mixer reads unmarked (previous-lap) audio */
    uint32_t notch_left;   /* frames left of the content-change notch, 0 when none is running */
    uint32_t notch_len;    /* frames of fade-out (and of fade-in) that notch is built from */
} ds_mix_state;

/* Q32 phase step for rate_hz source frames per second against the 48 kHz output. At 48 kHz the
 * step is exactly 1<<32, so a 48 kHz buffer passes through the resampler bit-exact. */
uint64_t ds_mix_step(uint32_t rate_hz);
/* DirectSound volume (hundredths of a dB, -10000..0) and pan (-10000..10000) as channel
 * gains: DirectSound attenuates the far channel linearly and leaves the near one alone. */
double ds_mix_volume(int32_t hundredths_db);
void   ds_mix_pan(int32_t pan, double *gl, double *gr);

/* The state a playback starts from: gate closed (fade in, no click), no block played yet, no stale
 * stretch in progress, no notch running. Play, SetCurrentPosition and the end of a finished buffer
 * all use it, so a buffer that is played again cannot inherit the previous playback's state. The
 * phase is not touched: it comes from the play position. */
void ds_mix_reset(ds_mix_state *st);

/* Adds `frames` output frames of `data` (bytes of nch-channel, bits-bit PCM at `align`
 * bytes a frame) to the interleaved stereo accumulator acc, starting at st->phase and
 * advancing it by step_q32 an output frame. fresh, when not NULL, is one flag per
 * VT3_MIX_FRESH_BLOCK bytes set by the game's writes and cleared once played. A block the
 * game has not rewritten holds the previous lap, and is played: a writer that is late costs
 * the audio from 128 ms ago (what a sound card plays too), never silence. Where the content
 * changes - from freshly written to previous lap and back - a 2 ms notch hides the step.
 * Returns the frames rendered; fewer than `frames` at the end of a non-looping buffer. The
 * accumulator is not cleared, so several buffers sum into it. */
uint32_t ds_mix_render(int32_t *acc, uint32_t frames, const uint8_t *data, uint32_t bytes,
                       uint32_t nch, uint32_t bits, uint32_t align, int looping,
                       uint8_t *fresh, uint64_t step_q32, double gl, double gr,
                       ds_mix_state *st);

/* Bytes between where a buffer's previous Unlock ended (last_end) and where this one starts
 * (off), around a buffer of `bytes`: the region the writer skipped instead of writing. 0 when
 * this write continues the previous one. Diagnostics only; it never changes what is mixed. */
uint32_t ds_writer_gap(uint32_t last_end, uint32_t off, uint32_t bytes);

/* Master gain and the output stage: acc is summed to int16 in `out` at `gain` per cent, saturating
 * instead of wrapping (a hard limit, not a fold-over), and returns the peak level of the result.
 * gain 100 leaves the mix bit-identical. */
int32_t vt3_mix_finish(int16_t *out, const int32_t *acc, size_t n, int gain);

/* One mixer tick's plan. */
typedef struct vt3_mix_tick {
    uint32_t frames;    /* frames to render now */
    uint32_t dropped;   /* frames that were due but do not fit the ring (or the tick cap) */
    uint64_t base_ns;   /* the schedule instant this tick counts from (moved on a re-anchor) */
    int resync;         /* the tick was very late: the timeline was re-anchored */
} vt3_mix_tick;

/* How much to render at `now_ns`, given the schedule instant next_ns, the output rate, the frames
 * the ring can still take (space_frames) and the most one tick may render. Frames that are due but
 * do not fit are reported in dropped, so an overrun is visible instead of silent. The caller
 * advances its schedule to base_ns + frames/rate after the render. */
vt3_mix_tick vt3_mix_plan(uint64_t next_ns, uint64_t now_ns, uint32_t rate_hz,
                          size_t space_frames, uint32_t max_frames);

/* The pause that freezes the play timeline. */
typedef struct vt3_mix_pause {
    uint64_t paused_at_ns;   /* 0 while running: the instant the output was silenced */
} vt3_mix_pause;

/* Reconciles the timeline with the platform's pause. Returns 1 on the call that resumes playback,
 * with *shift_ns set to how long the output was silent; the caller adds it to its schedule and to
 * every playing buffer's start time, so the stream continues instead of jumping ahead by the time
 * spent away. Asking for a pause that is already in effect keeps the first instant, so the shift
 * is exactly the silenced interval and a repeated resume shifts nothing. */
int vt3_mix_pause_sync(vt3_mix_pause *p, uint64_t now_ns, int paused, uint64_t *shift_ns);

/* The clock the play cursors run on: frozen at the instant the pause began while paused. */
uint64_t vt3_mix_play_clock(const vt3_mix_pause *p, uint64_t now_ns);

#endif /* VT3_MIX_H */
