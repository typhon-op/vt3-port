/*
 * Host checks for the audio path, with no device, no game data and no AAudio:
 *
 *   - src/audio/ringbuf.c   the SPSC ring behind the AAudio stream: wraparound, occupancy, the
 *                           peak watermark, reset, and a threaded producer/consumer run that
 *                           checks every byte across thousands of wraps.
 *   - src/audio/mix_core.c  what one output frame sounds like: rate conversion (48 kHz bit-exact,
 *                           44.1/24/22.05 kHz interpolated, loop wrap), the write gate (fade-in,
 *                           previous-lap playback, the content-change notch), the master/output
 *                           stage, the tick schedule (late
 *                           ticks, ring overrun, timeline re-anchor) and the pause clock.
 *
 * What it cannot check is the device side: AAudio open/pause/reopen, the callback's own timing and
 * the real end-to-end latency are device behaviour (see docs/audio-pipeline.md).
 *
 * Build and run:
 *   cc -O1 -o /tmp/audiotest tools/audiotest/AudioHostTest.c \
 *      runtime/src/audio/mix_core.c runtime/src/audio/ringbuf.c -Iruntime/include -lm -lpthread
 */
#include "vt3/vt3_audio.h"
#include "vt3/vt3_mix.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* ---- ring buffer ------------------------------------------------------------------ */

static void test_ring(void)
{
    uint8_t a[16], out[16];
    for (int i = 0; i < 16; i++) a[i] = (uint8_t)i;

    vt3_ringbuf *rb = vt3_rb_create(16);
    ok(rb != NULL, "create a 16 byte ring");
    ok(vt3_rb_used(rb) == 0 && vt3_rb_space(rb) == 16, "a new ring is empty");
    ok(vt3_rb_peak(rb) == 0, "a new ring has no peak");

    ok(vt3_rb_write(rb, a, 10) == 10, "write 10 of 16");
    ok(vt3_rb_used(rb) == 10 && vt3_rb_space(rb) == 6, "occupancy after 10 bytes");
    ok(vt3_rb_peak(rb) == 10, "peak follows the writer");

    ok(vt3_rb_read(rb, out, 6) == 6, "read 6");
    ok(out[0] == 0 && out[5] == 5, "the first 6 bytes come back in order");

    /* tail is at 6, head at 10: writing 12 wraps through the end of the ring */
    ok(vt3_rb_write(rb, a, 12) == 12, "write 12 across the wrap");
    ok(vt3_rb_used(rb) == 16 && vt3_rb_space(rb) == 0, "the ring is full");
    ok(vt3_rb_write(rb, a, 1) == 0, "a full ring accepts nothing");
    ok(vt3_rb_peak(rb) == 16, "peak remembers the full ring");

    memset(out, 0, sizeof out);
    ok(vt3_rb_read(rb, out, 16) == 16, "read the wrapped 16 bytes");
    ok(out[0] == 6 && out[3] == 9 && out[4] == 0 && out[9] == 5 && out[10] == 6 && out[15] == 11,
       "wrapped bytes read back in the order they were written");
    ok(vt3_rb_used(rb) == 0 && vt3_rb_peak(rb) == 16, "the peak survives the reads");

    vt3_rb_write(rb, a, 7);
    vt3_rb_reset(rb);
    ok(vt3_rb_used(rb) == 0 && vt3_rb_space(rb) == 16, "reset drops what was queued");
    ok(vt3_rb_peak(rb) == 0, "reset clears the peak");
    ok(vt3_rb_read(rb, out, 1) == 0, "a reset ring reads nothing");

    /* the counters keep counting up across a reset, so wrap arithmetic still works */
    ok(vt3_rb_write(rb, a, 16) == 16, "write a full ring after reset");
    ok(vt3_rb_read(rb, out, 16) == 16 && out[0] == 0 && out[15] == 15, "and read it back intact");
    vt3_rb_destroy(rb);
    ok(vt3_rb_peak(NULL) == 0, "peak of a NULL ring is 0");
}

/* A producer writing numbered words and a consumer checking them, across a small ring: any torn,
 * duplicated or lost byte shows up as a wrong value or a wrong count. */
#define SPSC_WORDS (1u << 18)

typedef struct {
    vt3_ringbuf *rb;
    uint32_t bad;
    uint32_t reads;
} spsc_ctx;

static void *spsc_producer(void *arg)
{
    spsc_ctx *c = arg;
    uint32_t i = 0;
    uint64_t spins = 0;
    while (i < SPSC_WORDS && spins < 200000000ull) {
        uint32_t v = i;
        if (vt3_rb_write(c->rb, &v, sizeof v) == sizeof v) {
            i++;
            spins = 0;
        } else {
            spins++;
            sched_yield();
        }
    }
    return NULL;
}

static void *spsc_consumer(void *arg)
{
    spsc_ctx *c = arg;
    uint32_t expect = 0;
    uint64_t spins = 0;
    while (expect < SPSC_WORDS && spins < 200000000ull) {
        uint32_t v;
        if (vt3_rb_read(c->rb, &v, sizeof v) == sizeof v) {
            if (v != expect) c->bad++;
            expect++;
            c->reads++;
            spins = 0;
        } else {
            spins++;
            sched_yield();
        }
    }
    return NULL;
}

static void test_ring_threads(void)
{
    spsc_ctx c;
    memset(&c, 0, sizeof c);
    c.rb = vt3_rb_create(1000);              /* deliberately not a multiple of 4 */
    pthread_t th[2];
    pthread_create(&th[0], NULL, spsc_producer, &c);
    pthread_create(&th[1], NULL, spsc_consumer, &c);
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    ok(c.reads == SPSC_WORDS, "the consumer received every word");
    ok(c.bad == 0, "no torn, repeated or reordered word across the wraps");
    ok(vt3_rb_peak(c.rb) <= 1000, "the peak never exceeds the capacity");
    vt3_rb_destroy(c.rb);
}

/* ---- rate conversion --------------------------------------------------------------- */

static void test_rate_conversion(void)
{
    ok(ds_mix_step(48000) == (1ull << 32), "48 kHz is an exact 1:1 step");
    ok(ds_mix_step(24000) == (1ull << 31), "24 kHz is half a source frame per output frame");
    ok(ds_mix_step(96000) == (2ull << 32), "96 kHz is two source frames per output frame");
    ok(ds_mix_step(44100) == (((uint64_t)44100 << 32) / 48000u), "44.1 kHz uses the truncated step");

    /* 48 kHz passes through sample for sample. */
    int16_t pcm[8] = { 1, -2, 300, -400, 32767, -32768, 0, 7 };
    int32_t acc[16];
    memset(acc, 0, sizeof acc);
    ds_mix_state st = { 0, UINT32_MAX, VT3_MIX_GATE_MAX, 0, 0, 0, 0, 0, 0 };
    uint32_t n = ds_mix_render(acc, 8, (const uint8_t *)pcm, sizeof pcm, 1, 16, 2, 0, NULL,
                               ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 8, "eight source frames give eight output frames at 48 kHz");
    ok(acc[0] == 1 && acc[2] == -2 && acc[4] == 300 && acc[14] == 7,
       "48 kHz is bit-exact through the mixer");

    /* 24 kHz: output frame n is half a source frame, so the odd frames interpolate. */
    int16_t ramp[4] = { 0, 1000, 2000, 3000 };
    memset(acc, 0, sizeof acc);
    st = (ds_mix_state){ 0, UINT32_MAX, VT3_MIX_GATE_MAX, 0, 0, 0, 0, 0, 0 };
    n = ds_mix_render(acc, 4, (const uint8_t *)ramp, sizeof ramp, 1, 16, 2, 0, NULL,
                      ds_mix_step(24000), 1.0, 1.0, &st);
    ok(n == 4, "24 kHz renders four output frames from four source frames");
    ok(acc[0] == 0 && acc[2] == 500 && acc[4] == 1000 && acc[6] == 1500,
       "24 kHz interpolates between neighbouring samples");

    /* 44.1 kHz over a tenth of a second: 4800 output frames must advance 4410 source frames. The
     * source is long enough that nothing wraps, so the phase directly shows the rate. */
    static int16_t src44[8192];
    static int32_t big[4800 * 2];
    for (int i = 0; i < 8192; i++) src44[i] = (int16_t)(i % 1000);
    memset(big, 0, sizeof big);
    st = (ds_mix_state){ 0, UINT32_MAX, VT3_MIX_GATE_MAX, 0, 0, 0, 0, 0, 0 };
    n = ds_mix_render(big, 4800, (const uint8_t *)src44, sizeof src44, 1, 16, 2, 0, NULL,
                      ds_mix_step(44100), 1.0, 1.0, &st);
    ok(n == 4800, "a 44.1 kHz buffer renders the whole 0.1 s tick");
    int64_t want = (int64_t)4410 << 32;
    int64_t drift = (int64_t)st.phase - want;
    if (drift < 0) drift = -drift;
    ok(drift < ((int64_t)1 << 32), "44.1 kHz drifts by less than one frame over 0.1 s");

    /* A looping buffer wraps to its start instead of running off the end. */
    memset(acc, 0, sizeof acc);
    st = (ds_mix_state){ 0, UINT32_MAX, VT3_MIX_GATE_MAX, 0, 0, 0, 0, 0, 0 };
    n = ds_mix_render(acc, 8, (const uint8_t *)ramp, sizeof ramp, 1, 16, 2, 1, NULL,
                      ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 8, "a looping buffer never runs out");
    ok(acc[0] == 0 && acc[6] == 3000 && acc[8] == 0 && acc[14] == 3000,
       "a looping buffer wraps back to its first sample");

    /* A non-looping buffer stops at its end. */
    memset(acc, 0, sizeof acc);
    st = (ds_mix_state){ 0, UINT32_MAX, VT3_MIX_GATE_MAX, 0, 0, 0, 0, 0, 0 };
    n = ds_mix_render(acc, 100, (const uint8_t *)ramp, sizeof ramp, 1, 16, 2, 0, NULL,
                      ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 4, "a non-looping buffer returns only the frames it has");

    /* 8-bit PCM is signed around 128, and quad keeps FL, FR in that order. */
    const uint8_t pcm8[3] = { 0, 128, 255 };
    memset(acc, 0, sizeof acc);
    st = (ds_mix_state){ 0, UINT32_MAX, VT3_MIX_GATE_MAX, 0, 0, 0, 0, 0, 0 };
    n = ds_mix_render(acc, 3, pcm8, sizeof pcm8, 1, 8, 1, 0, NULL,
                      ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 3 && acc[0] == -32768 && acc[2] == 0 && acc[4] == 32512,
       "8-bit PCM converts to signed 16-bit");

    int16_t quad[4] = { 1000, 2000, 0, 0 };   /* FL, FR, BL, BR */
    memset(acc, 0, sizeof acc);
    st = (ds_mix_state){ 0, UINT32_MAX, VT3_MIX_GATE_MAX, 0, 0, 0, 0, 0, 0 };
    n = ds_mix_render(acc, 1, (const uint8_t *)quad, sizeof quad, 4, 16, 8, 0, NULL,
                      ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 1 && acc[0] == 1000 && acc[1] == 2000, "quad keeps FL and FR in place");
}

/* ---- the write gate ------------------------------------------------------------------ */

static void test_gate(void)
{
    static int16_t loud[256];
    for (int i = 0; i < 256; i++) loud[i] = 32767;
    uint8_t fresh[2] = { 1, 1 };             /* 512 bytes, blocks of 256 */
    int32_t acc[512];
    ds_mix_state st;

    /* A fresh playback fades in: no click on the first sample, full level within the ramp. */
    ds_mix_reset(&st);
    st.phase = 0;
    memset(acc, 0, sizeof acc);
    uint32_t n = ds_mix_render(acc, 200, (const uint8_t *)loud, sizeof loud, 1, 16, 2, 1, fresh,
                               ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 200, "the gate test renders its frames");
    ok(acc[0] > 0 && acc[0] < 500, "the first sample of a playback is nearly silent");
    ok(acc[2 * 95] >= 32000, "the gate reaches full level inside the ramp");
    int monotone = 1;
    for (int j = 1; j < 96; j++) if (acc[2 * j] < acc[2 * (j - 1)]) monotone = 0;
    ok(monotone, "the fade-in only ever rises");

    /* An unwritten block holds the previous lap, and that is played: the mix must not duck
     * because the game's writer is late. This is the property the old fade-to-silence policy
     * broke - all of the game's sound shares one buffer, so silencing it silenced everything. */
    memset(fresh, 0, sizeof fresh);
    ds_mix_reset(&st);
    st.phase = 0;
    st.gate = VT3_MIX_GATE_MAX;
    memset(acc, 0, sizeof acc);
    n = ds_mix_render(acc, 64, (const uint8_t *)loud, sizeof loud, 1, 16, 2, 1, fresh,
                      ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 64, "the previous-lap test renders");
    ok(acc[0] == 32767 && acc[2 * 63] == 32767, "an unwritten block plays at full level");
    ok(st.stale == 1 && st.stale_events == 1, "the previous-lap stretch is counted once");
    ok(st.gate == VT3_MIX_GATE_MAX, "playing on through an unwritten block leaves the gate open");

    /* The content changes where a written block meets an unwritten one: the mixer fades through
     * zero exactly at that boundary and back up, so two different laps never step into each
     * other as a click, and the level after it is full again. */
    uint8_t mixed[2] = { 1, 0 };             /* block 0 written, block 1 not */
    ds_mix_reset(&st);
    st.phase = 0;
    st.gate = VT3_MIX_GATE_MAX;
    memset(acc, 0, sizeof acc);
    n = ds_mix_render(acc, 256, (const uint8_t *)loud, sizeof loud, 1, 16, 2, 1, mixed,
                      ds_mix_step(48000), 1.0, 1.0, &st);
    ok(n == 256, "the content-change test renders");
    ok(acc[2 * 127] <= 400 && acc[2 * 128] == 0, "the notch reaches zero at the boundary");
    int maxstep = 0;
    for (int j = 1; j < (int)n; j++) {
        int d = acc[2 * j] - acc[2 * (j - 1)];
        if (d < 0) d = -d;
        if (d > maxstep) maxstep = d;
    }
    ok(maxstep <= 400, "the notch is a 2 ms ramp, not a click");
    ok(acc[2 * 224] == 32767 && acc[2 * 255] == 32767,
       "the level is back to full one ramp after the content change");

    /* ds_mix_reset clears every field, whatever the buffer held before. */
    memset(&st, 0xAB, sizeof st);
    ds_mix_reset(&st);
    ok(st.gate == 0 && st.last_blk == UINT32_MAX && st.stale == 0 &&
       st.notch_left == 0 && st.notch_len == 0 && st.dip_frames == 0 && st.stale_events == 0,
       "ds_mix_reset clears the playback state");
}

/* ---- output stage -------------------------------------------------------------------- */

static void test_finish(void)
{
    int32_t acc[6] = { 100, -100, 40000, -40000, 0, 1 };
    int16_t out[6];
    int32_t peak = vt3_mix_finish(out, acc, 6, 100);
    ok(out[0] == 100 && out[1] == -100, "100 % gain is bit-identical");
    ok(out[2] == 32767 && out[3] == -32768, "over-range sums saturate instead of wrapping");
    ok(peak == 40000, "the peak is reported before the clamp");

    peak = vt3_mix_finish(out, acc, 6, 50);
    ok(out[0] == 50 && out[1] == -50 && out[2] == 20000, "50 % gain halves the mix");
    ok(peak == 20000, "the peak follows the gain");

    vt3_mix_finish(out, acc, 6, 0);
    int silent = 1;
    for (int i = 0; i < 6; i++) if (out[i] != 0) silent = 0;
    ok(silent, "0 % gain is silence");
}

/* ---- the tick schedule --------------------------------------------------------------- */

static void test_plan(void)
{
    const uint64_t t0 = 5000000000ull;
    const uint32_t ring_frames = 48000u * 96u / 1000u;

    vt3_mix_tick t = vt3_mix_plan(t0, t0, 48000, ring_frames, ring_frames);
    ok(t.frames == 0 && t.dropped == 0 && !t.resync, "a tick on time renders nothing");

    t = vt3_mix_plan(t0, t0 + 4000000ull, 48000, ring_frames, ring_frames);
    ok(t.frames == 192 && !t.resync, "a 4 ms tick is 192 frames at 48 kHz");

    /* 30 ms late with a 96 ms ring: fit it, do not throw it away (the old 20 ms cap dropped 480). */
    t = vt3_mix_plan(t0, t0 + 30000000ull, 48000, ring_frames, ring_frames);
    ok(t.frames == 1440 && t.dropped == 0, "a late tick fills the ring instead of dropping audio");

    /* The ring is what limits the tick, and what does not fit is reported. */
    t = vt3_mix_plan(t0, t0 + 25000000ull, 48000, 1000, ring_frames);
    ok(t.frames == 1000 && t.dropped == 200, "a full ring drops what does not fit");

    t = vt3_mix_plan(t0, t0 + 30000000ull, 48000, ring_frames, 960);
    ok(t.frames == 960 && t.dropped == 480, "the tick cap is reported as dropped frames");

    /* 300 ms behind: re-anchor, and prime 10 ms instead of sprinting to catch up. */
    t = vt3_mix_plan(t0, t0 + 300000000ull, 48000, ring_frames, ring_frames);
    ok(t.resync && t.frames == 480, "a very late tick re-anchors and primes 10 ms");
    ok(t.base_ns == t0 + 300000000ull - VT3_MIX_RESYNC_PRIME_NS, "the re-anchor moves the base");

    /* A match that starts with a stale timeline must not compute a huge frame count. */
    t = vt3_mix_plan(1, 90000000000ull, 48000, ring_frames, ring_frames);
    ok(t.resync && t.frames == 480, "an ancient timeline re-anchors instead of overflowing");

    t = vt3_mix_plan(t0, t0 + 4000000ull, 0, ring_frames, ring_frames);
    ok(t.frames == 0, "a zero rate renders nothing");
}

/* ---- the pause clock ------------------------------------------------------------------ */

static void test_pause(void)
{
    vt3_mix_pause p = { 0 };
    uint64_t shift = 123;

    ok(vt3_mix_pause_sync(&p, 1000, 0, &shift) == 0 && shift == 0, "a running output does not shift");
    ok(vt3_mix_play_clock(&p, 1000) == 1000, "the play clock follows the wall clock while running");

    ok(vt3_mix_pause_sync(&p, 2000, 1, &shift) == 0, "the pause itself does not shift");
    ok(vt3_mix_play_clock(&p, 5000) == 2000, "the play clock freezes at the pause instant");
    ok(vt3_mix_pause_sync(&p, 6000, 1, &shift) == 0, "a repeated pause keeps the first instant");

    ok(vt3_mix_pause_sync(&p, 8000, 0, &shift) == 1 && shift == 6000,
       "one resume shifts by exactly the silenced interval");
    ok(vt3_mix_play_clock(&p, 8000) == 8000, "the play clock runs again after the resume");
    ok(vt3_mix_pause_sync(&p, 9000, 0, &shift) == 0 && shift == 0,
       "a second resume shifts nothing, so the timeline cannot jump twice");

    /* A pause that begins and resumes inside the same instant. */
    vt3_mix_pause zero = { 0 };
    ok(vt3_mix_pause_sync(&zero, 4000, 1, &shift) == 0, "pause at an instant");
    ok(vt3_mix_pause_sync(&zero, 4000, 0, &shift) == 1 && shift == 0,
       "resuming in the same instant shifts nothing");
}

/* ---- repeated init and shutdown -------------------------------------------------------- */

static void test_repeated_runtime(void)
{
    int all_ok = 1;
    for (int i = 0; i < 200; i++) {
        vt3_ringbuf *rb = vt3_rb_create(4800);
        if (!rb) { all_ok = 0; break; }
        ds_mix_state st;
        memset(&st, 0xAB, sizeof st);
        ds_mix_reset(&st);
        st.phase = 0;

        int16_t src[64];
        memset(src, 0, sizeof src);
        int32_t acc[8];
        memset(acc, 0, sizeof acc);
        uint32_t n = ds_mix_render(acc, 4, (const uint8_t *)src, sizeof src, 2, 16, 4, 1, NULL,
                                   ds_mix_step(48000), 1.0, 1.0, &st);
        if (n != 4 || st.gate <= 0 || acc[0] != 0) all_ok = 0;

        if (vt3_rb_write(rb, src, 64) != 64 || vt3_rb_peak(rb) != 64) all_ok = 0;
        vt3_rb_reset(rb);
        if (vt3_rb_used(rb) != 0 || vt3_rb_peak(rb) != 0) all_ok = 0;
        vt3_rb_destroy(rb);
    }
    ok(all_ok, "200 create/mix/reset/destroy rounds with a clean state each time");
}

/* ---- writer-gap accounting -------------------------------------------------------------- */

static void test_writer_gap(void)
{
    ok(ds_writer_gap(0, 0, 100) == 0, "a write that starts where the last ended has no gap");
    ok(ds_writer_gap(10, 10, 100) == 0, "a continued write has no gap");
    ok(ds_writer_gap(10, 20, 100) == 10, "a skipped region is counted");
    ok(ds_writer_gap(90, 10, 100) == 20, "a skip across the wrap is counted");
    ok(ds_writer_gap(0, 0, 0) == 0, "an empty buffer has no gap");
}

int main(void)
{
    test_ring();
    test_ring_threads();
    test_rate_conversion();
    test_gate();
    test_finish();
    test_plan();
    test_pause();
    test_repeated_runtime();
    test_writer_gap();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
