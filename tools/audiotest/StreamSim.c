/*
 * StreamSim: the game's streaming audio buffer, on the host, measured.
 *
 * Virtua Tennis 3 plays everything (music, SFX, crowd) through ONE DirectSound secondary buffer:
 * 73728 bytes, 48000 Hz, 6 channels, 16-bit, looping - 6144 frames, 128 ms. The game's own audio
 * thread writes it in chunks of a sixth of it (1024 frames, 21.3 ms) and keeps the newest write
 * about 80 ms ahead of the play cursor. When that thread loses the guest lock for longer than its
 * lead it does not fall behind: it writes the next chunk where the cursor now is and skips
 * whatever the play cursor passed while it was away (docs/HANDOFF.md, "Menu gargle").
 *
 * This program reproduces that writer, the real output ring and the real mixer tick
 * (src/audio/mix_core.c) on a virtual clock, so an audio policy can be measured instead of
 * listened to: output level, silent stretches, clicks and gate dips are all counted, and the mix
 * is written as a WAV for spectral inspection.
 *
 * Policies compared against the same writer:
 *   notch     the shipped default: a block the game has not rewritten is played (the previous
 *             lap), with a 2 ms notch where the content changes
 *   noflags   the same, without the write marks at all (VT3_DS_NO_GATE=1): no notch
 *
 * It is part of the host suite: tools/hosttests.sh (or run it alone, an argument sets the seconds
 * simulated). It fails if a starved writer takes the level away, which is the regression this
 * harness exists for.
 */
#include "vt3/vt3_mix.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE        48000u
#define NCH         6u
#define FRAME_BYTES (NCH * 2u)
#define BUF_FRAMES  6144u                       /* 128 ms */
#define BUF_BYTES   (BUF_FRAMES * FRAME_BYTES)  /* 73728 */
#define CHUNK       (BUF_FRAMES / 6u)           /* 1024 frames: a sixth of the buffer */
#define STALE_BLK   VT3_MIX_FRESH_BLOCK
#define NBLOCKS     (BUF_BYTES / STALE_BLK)
#define LEAD_S      0.080                       /* the game writes this far ahead */

/* Ring: the same 96 ms the shim opens the output with. */
#define RING_MS     96u
#define RING_FRAMES (RATE * RING_MS / 1000u)
#define MAX_TICK    RING_FRAMES
#define TICK_FRAMES (RATE / 250u)               /* the mixer's 4 ms */
#define AMP         16000.0                     /* a 5.1 downmix of this peaks at about 0.8 FS */

static int16_t buf[BUF_BYTES / 2];   /* guest secondary buffer, as the mixer sees it */
static uint8_t fresh[NBLOCKS];
static int16_t ring[RING_FRAMES * 2];
static size_t ring_head, ring_tail;  /* frames, monotonic */

static int16_t out[MAX_TICK * 2];    /* one tick's mixed output */
static int32_t acc[MAX_TICK * 2];
static int16_t dev[2 * TICK_FRAMES]; /* what the device took this tick */

/* ---- the game's audio, as a function of its own continuous timeline ---------------------- */

/* One 6-channel frame of the soundtrack at time t: front left/right carry the mix, the centre a
 * -6 dB copy of it, the rears a -12 dB 20 ms delayed copy, the LFE a low tone. A real 5.1 layout,
 * which is what the buffer claims to be. */
static void song_sample(double t, int16_t out6[6])
{
    double trem = 0.72 + 0.28 * sin(2.0 * M_PI * 0.9 * t);
    double l = 0.36 * sin(2.0 * M_PI * 220.0 * t) + 0.24 * sin(2.0 * M_PI * 330.0 * t)
             + 0.14 * sin(2.0 * M_PI * 660.0 * t) + 0.10 * sin(2.0 * M_PI * 55.0 * t);
    double r = 0.36 * sin(2.0 * M_PI * 221.5 * t) + 0.24 * sin(2.0 * M_PI * 329.0 * t)
             + 0.14 * sin(2.0 * M_PI * 661.0 * t) + 0.10 * sin(2.0 * M_PI * 55.0 * t);
    double v[6];
    v[0] = l;
    v[1] = r;
    v[2] = 0.5 * (l + r);
    v[3] = 0.30 * sin(2.0 * M_PI * 55.0 * t);
    v[4] = 0.25 * (0.5 * sin(2.0 * M_PI * 220.0 * (t - 0.020)) +
                   0.5 * sin(2.0 * M_PI * 330.0 * (t - 0.020)));
    v[5] = 0.25 * (0.5 * sin(2.0 * M_PI * 221.5 * (t - 0.020)) +
                   0.5 * sin(2.0 * M_PI * 329.0 * (t - 0.020)));
    for (int i = 0; i < 6; i++) {
        double x = v[i] * AMP * trem;
        if (x > 32767.0) x = 32767.0;
        if (x < -32768.0) x = -32768.0;
        out6[i] = (int16_t)lrint(x);
    }
}

/* ---- the ring, as vt3_audio_write/space present it -------------------------------------- */

static size_t rb_space(void) { return RING_FRAMES - (ring_head - ring_tail); }

static void rb_write(const int16_t *src, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        size_t at = (ring_head + i) % RING_FRAMES;
        ring[2 * at] = src[2 * i];
        ring[2 * at + 1] = src[2 * i + 1];
    }
    ring_head += frames;
}

static size_t rb_read(int16_t *dst, size_t frames)
{
    size_t have = ring_head - ring_tail;
    size_t n = frames < have ? frames : have;
    for (size_t i = 0; i < n; i++) {
        size_t at = (ring_tail + i) % RING_FRAMES;
        dst[2 * i] = ring[2 * at];
        dst[2 * i + 1] = ring[2 * at + 1];
    }
    ring_tail += n;
    return n;
}

/* ---- the guest writer ------------------------------------------------------------------ */

typedef struct {
    double write_time;      /* logical time of the most recently written audio */
    uint32_t write_end;     /* byte just past the last write */
    int have_written;
    uint32_t writes, skips;
    uint64_t skipped_bytes;
    uint32_t last_gap_bytes;
} writer;

static uint32_t play_of(double t)
{
    return (uint32_t)((uint64_t)llrint(t * RATE) * FRAME_BYTES % BUF_BYTES);
}

/* One writer wakeup: write the next chunk so that it ends LEAD_S past the play cursor. */
static void writer_wake(writer *w, double t)
{
    uint32_t play = play_of(t);
    uint32_t lead = (uint32_t)(LEAD_S * RATE * FRAME_BYTES);
    uint32_t end = (play + lead) % BUF_BYTES;
    uint32_t off = (end + BUF_BYTES - CHUNK * FRAME_BYTES) % BUF_BYTES;

    for (uint32_t i = 0; i < CHUNK; i++) {
        int16_t f[6];
        song_sample(w->write_time + (double)i / RATE, f);
        memcpy(&buf[((off + i * FRAME_BYTES) % BUF_BYTES) / 2], f, FRAME_BYTES);
    }
    w->write_time += (double)CHUNK / RATE;

    for (uint32_t b = off / STALE_BLK; b * STALE_BLK < off + CHUNK * FRAME_BYTES; b++)
        fresh[b % NBLOCKS] = 1;

    w->last_gap_bytes = 0;
    if (w->have_written) {
        uint32_t gap = (off >= w->write_end) ? off - w->write_end
                                             : off + BUF_BYTES - w->write_end;
        if (gap && gap < BUF_BYTES) {
            w->skips++;
            w->skipped_bytes += gap;
            w->last_gap_bytes = gap;
        }
    }
    w->write_end = (off + CHUNK * FRAME_BYTES) % BUF_BYTES;
    w->have_written = 1;
    w->writes++;
}

/* Fill the whole buffer at the start, as the game has when a track is already playing. */
static void writer_prefill(writer *w)
{
    for (uint32_t c = 0; c < BUF_FRAMES / CHUNK; c++) {
        for (uint32_t i = 0; i < CHUNK; i++) {
            int16_t f[6];
            song_sample(w->write_time + (double)i / RATE, f);
            memcpy(&buf[((c * CHUNK + i) * FRAME_BYTES) / 2], f, FRAME_BYTES);
        }
        w->write_time += (double)CHUNK / RATE;
    }
    memset(fresh, 1, sizeof fresh);
    w->write_end = 0;
    w->have_written = 1;
}

/* ---- measurement ------------------------------------------------------------------------ */

typedef struct {
    uint64_t frames;
    double sum_sq;
    int32_t peak;
    uint64_t ticks;             /* ticks measured */
    uint64_t silent_ticks;      /* a tick whose RMS is below 1% FS: audible as a hole */
    uint64_t quiet_ticks;       /* a tick more than 6 dB below the run's median */
    double *tick_db;            /* per-tick level, for the median */
    uint64_t underrun_frames;   /* frames the device asked for and the ring did not have */
    uint64_t below_gate;        /* frames rendered with the gate below full */
    uint64_t gate_runs;         /* number of separate below-full stretches */
    uint64_t clicks;            /* per-channel sample steps no 48 kHz waveform of this kind makes */
    int in_dip;
    int16_t prev[2];
} stats;

static void stats_take(stats *s, const int16_t *pcm, size_t frames, int32_t gate_now)
{
    double sum = 0.0;
    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            int32_t v = pcm[2 * i + c];
            s->sum_sq += (double)v * (double)v;
            int32_t a = v < 0 ? -v : v;
            if (a > s->peak) s->peak = a;
            int32_t d = v - (int32_t)s->prev[c];
            if (d > 9000 || d < -9000) s->clicks++;
            s->prev[c] = (int16_t)v;
            sum += (double)v * (double)v;
        }
    }
    s->frames += frames;
    if (s->ticks < 1000000) s->tick_db[s->ticks++] = 10.0 * log10(sum / (double)(frames * 2) + 1e-9);
    if (gate_now < VT3_MIX_GATE_MAX) {
        s->below_gate += frames;
        if (!s->in_dip) { s->gate_runs++; s->in_dip = 1; }
    } else {
        s->in_dip = 0;
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Counts the holes: a tick at least 30 dB below the run's median level is a dropout the player
 * hears as the sound cutting out, and one 6-30 dB below it is a dip heard as a level collapse. */
static void stats_finish(stats *s)
{
    if (!s->ticks) return;
    double *v = malloc(s->ticks * sizeof *v);
    memcpy(v, s->tick_db, s->ticks * sizeof *v);
    qsort(v, s->ticks, sizeof *v, cmp_double);
    double med = v[s->ticks / 2];
    free(v);
    for (uint64_t i = 0; i < s->ticks; i++) {
        if (s->tick_db[i] < med - 30.0) s->silent_ticks++;
        else if (s->tick_db[i] < med - 6.0) s->quiet_ticks++;
    }
}

static double rms_db(const stats *s)
{
    if (!s->frames) return -999.0;
    double ms = s->sum_sq / (double)(s->frames * 2) + 1e-9;
    return 10.0 * log10(ms) - 10.0 * log10(32768.0 * 32768.0);
}

/* ---- one run ---------------------------------------------------------------------------- */

typedef struct {
    double rms, holes_pct, dips_pct, below_gate_pct;
    uint32_t skips;
} result;

/* A writer that keeps its cadence except for a stall of `stall_s` every `every_s` seconds. The
 * stall is the guest-lock hole: the thread is simply not runnable, so it misses its slots. */
static void run(const char *name, int gate, double every_s, double stall_s, double seconds,
                const char *wav, result *res)
{
    memset(buf, 0, sizeof buf);
    memset(fresh, 0, sizeof fresh);
    memset(ring, 0, sizeof ring);
    ring_head = ring_tail = 0;

    writer w;
    memset(&w, 0, sizeof w);
    writer_prefill(&w);

    ds_mix_state st;
    ds_mix_reset(&st);
    stats s;
    memset(&s, 0, sizeof s);
    s.tick_db = malloc(1000000 * sizeof *s.tick_db);

    FILE *f = wav ? fopen(wav, "wb") : NULL;
    if (f) {
        uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E','f','m','t',' ',
                            16,0,0,0, 1,0, 2,0, 0x80,0xbb,0,0, 0,0xee,2,0, 4,0, 16,0,
                            'd','a','t','a', 0,0,0,0 };
        fwrite(hdr, 1, 44, f);
    }

    const double wake_period = (double)CHUNK / RATE;
    uint64_t sched_ns = 0;
    uint64_t now_ns = 0;
    double next_wake = wake_period;
    double stall_until = -1.0;
    double next_stall = every_s;

    while (now_ns < (uint64_t)(seconds * 1e9)) {
        double t = (double)now_ns / 1e9;

        /* The writer: one chunk per wakeup, and a stall that skips its slots. */
        if (every_s > 0 && t >= next_stall && stall_until < 0) {
            stall_until = t + stall_s;
            next_stall = t + every_s;
        }
        int stalled = stall_until >= 0 && t < stall_until;
        if (stall_until >= 0 && t >= stall_until) stall_until = -1.0;
        while (!stalled && next_wake <= t) {   /* exact cadence, like a thread that is on time */
            writer_wake(&w, next_wake);
            next_wake += wake_period;
        }
        if (stalled) next_wake = t + wake_period;   /* it missed its slots and resumes here */

        /* The mixer tick: the frames the wall clock owes, bounded by the ring (as the shim). */
        vt3_mix_tick tick = vt3_mix_plan(sched_ns, now_ns, RATE, rb_space(), MAX_TICK);
        if (tick.frames) {
            memset(acc, 0, (size_t)tick.frames * 2 * sizeof acc[0]);
            ds_mix_state next = st;
            ds_mix_render(acc, tick.frames, (const uint8_t *)buf, BUF_BYTES, NCH, 16,
                          FRAME_BYTES, 1, gate ? fresh : NULL, ds_mix_step(RATE),
                          1.0, 1.0, &next);
            st = next;
            vt3_mix_finish(out, acc, (size_t)tick.frames * 2, 100);
            rb_write(out, tick.frames);
            stats_take(&s, out, tick.frames, st.gate);
            if (f) fwrite(out, sizeof(int16_t), (size_t)tick.frames * 2, f);
            sched_ns = tick.base_ns + (uint64_t)tick.frames * 1000000000ull / RATE;
        }

        /* The device drains at 48 kHz, a tick's worth at a time. */
        size_t n = rb_read(dev, TICK_FRAMES);
        s.underrun_frames += (TICK_FRAMES - n);     /* an underrun is silence */

        now_ns += (uint64_t)TICK_FRAMES * 1000000000ull / RATE;
    }

    if (f) {
        long bytes = ftell(f) - 44;
        uint32_t riff = (uint32_t)(bytes + 36), data = (uint32_t)bytes;
        fseek(f, 4, SEEK_SET); fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET); fwrite(&data, 4, 1, f);
        fclose(f);
    }

    stats_finish(&s);
    if (res) {
        res->rms = rms_db(&s);
        res->holes_pct = s.ticks ? 100.0 * (double)s.silent_ticks / (double)s.ticks : 0.0;
        res->dips_pct = s.ticks ? 100.0 * (double)s.quiet_ticks / (double)s.ticks : 0.0;
        res->below_gate_pct = s.frames ? 100.0 * (double)s.below_gate / (double)s.frames : 0.0;
        res->skips = w.skips;
    }
    printf("%-26s rms %7.2f dBFS  peak %5d  holes %5.2f%%  dips %5.2f%%  below-gate %6.2f%% "
           "(%2llu)  clicks %5llu  skips %3u (%5.1f%%)  ring underrun %5.2f%%\n",
           name, rms_db(&s), (int)s.peak,
           100.0 * (double)s.silent_ticks / (double)s.ticks,
           100.0 * (double)s.quiet_ticks / (double)s.ticks,
           100.0 * (double)s.below_gate / (double)(s.frames ? s.frames : 1),
           (unsigned long long)s.gate_runs,
           (unsigned long long)s.clicks,
           w.skips, 100.0 * (double)w.skips / (double)(w.writes ? w.writes : 1),
           100.0 * (double)s.underrun_frames / (double)(s.frames ? s.frames : 1));
    free(s.tick_db);
}

int main(int argc, char **argv)
{
    int checks = 0, failed = 0;
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    const char *dir = argc > 2 ? argv[2] : ".";
    char path[512];

    printf("streaming buffer: %u bytes, %u Hz, %u ch, %.1f ms; chunk %.1f ms, lead %d ms\n",
           (unsigned)BUF_BYTES, (unsigned)RATE, (unsigned)NCH,
           1000.0 * BUF_FRAMES / RATE, 1000.0 * CHUNK / RATE, (int)(LEAD_S * 1000));

    result clean, noflags;
    run("clean writer, notch", 1, 0.0, 0.0, (double)secs, NULL, &clean);
    run("clean writer, no flags", 0, 0.0, 0.0, (double)secs, NULL, &noflags);

    /* The stall distribution the device traces report: the guest lock held for 100-300 ms at menu
     * loads, and smaller holes during a match. */
    struct { const char *what; double every, stall; } cases[] = {
        { "menu load 300 ms/s",  1.0, 0.300 },
        { "menu load 150 ms/s",  1.0, 0.150 },
        { "menu load 100 ms x3", 0.4, 0.100 },
        { "match 60 ms x2",      0.5, 0.060 },
    };
    checks++;
    if (!(clean.holes_pct == 0.0)) { printf("FAIL clean writer must not be silent: %.2f%%\n", clean.holes_pct); failed++; }
    checks++;
    if (!(noflags.rms > clean.rms - 0.05)) { printf("FAIL no-flag run must match: %.2f vs %.2f\n", noflags.rms, clean.rms); failed++; }

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char label[80];
        result notch;
        snprintf(label, sizeof label, "%s, notch", cases[i].what);
        snprintf(path, sizeof path, "%s/stream_gate_%zu.wav", dir, i);
        run(label, 1, cases[i].every, cases[i].stall, (double)secs, path, &notch);
        snprintf(label, sizeof label, "%s, no flags", cases[i].what);
        snprintf(path, sizeof path, "%s/stream_nogate_%zu.wav", dir, i);
        run(label, 0, cases[i].every, cases[i].stall, (double)secs, path, NULL);

        /* The property this harness exists for: a writer that misses its deadlines must not
         * take the level away. Before the notch policy these cases measured 13-30% of the
         * output as holes and 1.5 dB of RMS; the shipped mixer must stay at the clean level. */
        checks++;
        if (!(notch.holes_pct <= 0.5)) {
            printf("FAIL %s: %.2f%% of the output is silent\n", cases[i].what, notch.holes_pct);
            failed++;
        }
        checks++;
        if (!(notch.rms >= clean.rms - 0.30)) {
            printf("FAIL %s: level %.2f dB against %.2f clean\n", cases[i].what, notch.rms, clean.rms);
            failed++;
        }
        checks++;
        if (!(notch.below_gate_pct <= 2.0)) {
            printf("FAIL %s: gate below full for %.2f%% of the frames\n", cases[i].what,
                   notch.below_gate_pct);
            failed++;
        }
    }
    printf("%d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
