/*
 * DirectSound. IDirectSound and IDirectSoundBuffer are guest COM objects (D3D9_KIND_DSOUND,
 * D3D9_KIND_DSBUFFER; vtables from the generated d3d9_com.h). A buffer keeps its PCM in guest
 * heap memory, Lock hands the game real storage, and the play cursor follows the wall clock,
 * so a game that streams audio paces itself as it would with a sound card.
 *
 * The mixer thread renders every playing buffer to the output device through the audio layer
 * (vt3_audio.h). For each output frame it reads the buffer at the position the wall clock
 * gives, so it reads only what the game has already written: the game writes about 50 ms
 * ahead of that position. The output ring adds about 40 ms of delay before the sound is heard,
 * which does not change what is read. The mixer snapshots what it needs under the lock, mixes
 * with the lock released, and commits only the result, so a guest Lock or GetCurrentPosition
 * waits for a few copies rather than for the DSP pass; `VT3_TRACE_AUDIO=1` reports that wait.
 * Buffers are converted to 48 kHz stereo 16-bit with linear interpolation (a 22.05 or 44.1 kHz
 * buffer is resampled smoothly; holding each sample the way nearest-neighbour does aliases into
 * a hard, gargly edge) and a surround downmix.
 * SetFrequency changes the playback rate, and SetPan attenuates the far channel. If no device
 * opens, playback stays silent and the game still runs.
 *
 * A failed creation still writes a stub object (no host data, methods report
 * DSERR_UNSUPPORTED), so a guest creation call does not leave its output empty.
 *
 * Guest layouts (SDK; offsets checked against the mingw-w64 14.0.0 headers):
 *   WAVEFORMATEX  18 bytes, packed.
 *   DSBUFFERDESC  dwSize@0 dwFlags@4 dwBufferBytes@8 dwReserved@12 lpwfxFormat@16.
 *   DSCAPS        92 bytes: dwSize@0 dwFlags@4 dwMinSecondarySampleRate@8
 *                 dwMaxSecondarySampleRate@12 dwPrimaryBuffers@16.
 *   DSBCAPS       20 bytes: dwSize@0 dwFlags@4 dwBufferBytes@8.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_audio.h"
#include "vt3/vt3_log.h"
#include "win32/dsound.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#endif

#define WFX_BYTES 18u
#define DSBDESC_BYTES 20u
#define DSCAPS_BYTES 92u
#define DSBCAPS_BYTES 20u

typedef struct ds_device {
    uint32_t coop_level;
} ds_device;

typedef struct ds_buffer {
    GPTR storage;                /* guest heap: bytes of PCM */
    uint32_t bytes;
    uint8_t fmt[WFX_BYTES];      /* WAVEFORMATEX as the game gave it */
    uint32_t pos;                /* play cursor while stopped, in bytes */
    uint32_t play_base;          /* play cursor when the current playback started */
    uint64_t play_ns;            /* monotonic time when the current playback started */
    int playing;
    int looping;
    int32_t volume, pan;
    uint32_t freq;
    uint8_t *fresh;              /* one flag per FRESH_BLOCK bytes: set when the game writes it, cleared once played */
    ds_mix_state mix;            /* resampler state; only the mixer thread reads it, and Play/SetCurrentPosition reset it under g_mu */
    uint32_t mix_seq;            /* bumped whenever the game retimes playback; the mixer drops a
                                  * chunk whose seq changed under it */
    int pinned;                  /* the mixer is mixing this buffer with g_mu released */
    int dead;                    /* released while pinned: the mixer frees it after the chunk */
    uint32_t last_write_end;     /* trace: byte after the end of the last Unlock's region */
    int have_written;
    uint32_t write_skips;        /* trace: Unlocks that did not continue where the last one ended */
    uint64_t write_gap_bytes;    /* trace: bytes those skips never wrote */
    uint32_t trace_dip_frames;   /* trace: below-full gate frames accumulated since the last report */
    uint32_t trace_stale_events, trace_stale_blk, trace_grace_frames;
    uint32_t n_locks, n_plays;   /* trace: how often the game wrote and played this buffer */
    uint32_t obj;                /* guest object id, as the per-event trace prints it */
} ds_buffer;

/* ---- output mixer ----------------------------------------------------------
 * g_mu guards the list of playing buffers and every buffer's play state while it is
 * in the list. Lock/Unlock do not take it: the game writes storage the mixer reads, which
 * is the same race a real sound card has with the game's write cursor. */
#define MIX_RATE 48000u
#define MIX_CH 2u
#define MIX_RING_MS 96u
#define MIX_MAX_FRAMES 960u
#define MIX_MAX_ACTIVE 64
#define FRESH_BLOCK VT3_MIX_FRESH_BLOCK
/* Q15 gate change per output frame: the ramp completes inside VT3_MIX_RAMP_FRAMES. */
#define MIX_GATE_STEP ((VT3_MIX_GATE_MAX + VT3_MIX_RAMP_FRAMES - 1) / VT3_MIX_RAMP_FRAMES)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ds_buffer *g_active[MIX_MAX_ACTIVE];
static int g_nactive;
static vt3_audio *g_out;

/* Closes the output so a relaunch opens its own stream instead of two writers sharing one. */
void vt3_dsound_shutdown(void)
{
    if (!g_out) return;
    vt3_audio_pause(1);
    vt3_audio_close(g_out);
    g_out = NULL;
}
static uint64_t g_mix_next_ns;
static uint64_t g_pause_ns;      /* when the platform paused the output, or 0: the timeline is frozen */
static size_t g_ring_bytes;      /* trace: output ring size, to report its fill */
static uint32_t g_resyncs;       /* trace: wall-clock resyncs of the mixer timeline */
static uint64_t g_starved_frames; /* trace: frames the device ring had no room for */
static uint32_t g_starved_ticks;  /* trace: ticks where the ring was full while audio was due */

/* Buffers whose Release arrived while the mixer had them pinned. The mixer frees them after
 * the chunk it is mixing, so a guest Release never waits for the DSP pass. */
static ds_buffer *g_graveyard[MIX_MAX_ACTIVE];
static int g_ngrave;

/* VT3_DS_NO_GATE=1 pins the stale-block gate open: the mixer ignores the fresh marks
 * entirely (A/B experiment for whether the gate is what gargles a sound). */
static int ds_no_gate(void)
{
    static int on = -1;
    if (on < 0) on = getenv("VT3_DS_NO_GATE") != NULL;
    return on;
}

/* ---- guest memory and time ------------------------------------------------ */

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

static void set32(uint8_t *p, uint32_t off, uint32_t v)
{
    memcpy(p + off, &v, 4);
}

static void put32(GPTR p, uint32_t v)
{
    if (p) vt3s_store_u32(p, v);
}

static void put_bytes(GPTR p, const void *src, uint32_t n)
{
    void *q = d3d9_gmem(p, n);
    if (q) memcpy(q, src, n);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ---- guest-side lock wait ---------------------------------------------------
 * Every guest call that reads playback state takes g_mu. The mixer used to hold it across a
 * whole DSP pass, which made those calls late; it no longer does, and this is the measurement
 * that says so. Counters are written while g_mu is held, so the trace can read them there. */
static uint64_t g_lock_wait_ns;      /* total wait of guest takes since start */
static uint64_t g_lock_wait_max_ns;  /* worst single wait */
static uint32_t g_lock_takes;

/* Brings the play clocks across a platform pause, with g_mu held: frozen while the output is
 * paused, and shifted forward by exactly the pause on the first call after the resume. It runs
 * from guest_lock as well as from the mixer, so the first guest call after a resume (the audio
 * writer wakes on its own thread, not only with the game loop) sees the same continuous cursor
 * the mixer does, with no window where the two disagree. */
static void sync_pause_locked(uint64_t now)
{
    if (vt3_audio_is_paused(g_out)) {
        if (!g_pause_ns) g_pause_ns = now;
        return;
    }
    if (!g_pause_ns) return;
    uint64_t shift = now - g_pause_ns;
    g_pause_ns = 0;
    g_mix_next_ns += shift;
    for (int i = 0; i < g_nactive; i++) g_active[i]->play_ns += shift;
    VT3_INFO("audio: resumed after %llu ms", (unsigned long long)(shift / 1000000u));
}

static void guest_lock(void)
{
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_mu);
    uint64_t d = now_ns() - t0;
    g_lock_wait_ns += d;
    if (d > g_lock_wait_max_ns) g_lock_wait_max_ns = d;
    g_lock_takes++;
    sync_pause_locked(now_ns());
}

/* The clock the play cursors run on, read with g_mu held. While the output is paused it stays
 * at the instant the pause began, so a writer thread that keeps running in the background does
 * not advance the stream; on resume play_ns values have already been shifted by the pause. */
static uint64_t play_clock(uint64_t now)
{
    return g_pause_ns ? g_pause_ns : now;
}

/* Little-endian readers for the guest WAVEFORMATEX bytes. */
static uint16_t fmt16(const uint8_t *f, uint32_t off)
{
    return (uint16_t)(f[off] | (f[off + 1] << 8));
}

/* ---- playback model -------------------------------------------------------- */

static uint32_t avg_bytes_per_sec(const ds_buffer *b)
{
    return rd32(b->fmt, 8);
}

/* Bytes a second playback consumes. SetFrequency overrides the format's rate. */
static uint32_t buf_bps(const ds_buffer *b)
{
    uint32_t align = fmt16(b->fmt, 12);
    uint32_t hz = b->freq ? b->freq : rd32(b->fmt, 4);
    uint32_t bps = hz * align;
    return bps ? bps : avg_bytes_per_sec(b);
}

/* The write cursor runs this far ahead of the play cursor: 50 ms, or half the buffer. */
static uint32_t write_lead(const ds_buffer *b)
{
    uint32_t lead = buf_bps(b) / 20u;
    return lead < b->bytes / 2u ? lead : b->bytes / 2u;
}

/* Bytes played since play_base, unwrapped. */
static uint64_t played_at(const ds_buffer *b, uint64_t now)
{
    uint64_t us = now > b->play_ns ? (now - b->play_ns) / 1000u : 0;
    return b->play_base + us * buf_bps(b) / 1000000u;
}

/* A non-looping buffer whose wall-clock cursor has passed its end: stopped to the game, even
 * while the lagging mixer still plays its tail. */
static int played_out(const ds_buffer *b, uint64_t now)
{
    return b->playing && !b->looping && b->bytes && played_at(b, now) >= b->bytes;
}

static uint32_t play_cursor_at(const ds_buffer *b, uint64_t now)
{
    if (!b->playing || b->bytes == 0) return b->pos;
    if (played_out(b, now)) return 0;             /* where the mixer leaves a finished one-shot, not a wrapped cursor */
    return (uint32_t)(played_at(b, now) % b->bytes);
}

/* Both cursors from one clock read, so GetCurrentPosition cannot report a play cursor and a
 * write cursor taken at different instants. */
static void cursors_at(const ds_buffer *b, uint64_t now, uint32_t *play, uint32_t *write)
{
    *play = play_cursor_at(b, now);
    *write = b->bytes ? (*play + write_lead(b)) % b->bytes : 0;
}

/* ---- the mixer ------------------------------------------------------------- */

static void active_add_locked(ds_buffer *b)
{
    for (int i = 0; i < g_nactive; i++) {
        if (g_active[i] == b) return;
    }
    if (g_nactive < MIX_MAX_ACTIVE) g_active[g_nactive++] = b;
}

static void active_remove_locked(ds_buffer *b)
{
    for (int i = 0; i < g_nactive; i++) {
        if (g_active[i] == b) {
            g_active[i] = g_active[--g_nactive];
            return;
        }
    }
}

/* VT3_TRACE_AUDIO=1 logs buffer creation, Play and Stop. */
static int dsound_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("VT3_TRACE_AUDIO") != NULL;
    return on;
}

/* A signed 16-bit value from one 8- or 16-bit PCM sample. */
static int32_t pcm_sample(const uint8_t *p, uint32_t bits)
{
    return bits == 16 ? (int32_t)(int16_t)(p[0] | (p[1] << 8)) : ((int32_t)p[0] - 128) * 256;
}

/* The game's writes mark the FRESH_BLOCK blocks they cover (Unlock), and the mixer clears the
 * mark as it moves past each block, so every write plays exactly once. The mixer core below
 * fades a block out over VT3_MIX_RAMP_FRAMES when it is not marked: a late writer then costs a
 * smooth dip instead of a click or a stale repeat. Without flags (no memory for them) a buffer
 * plays with the gate pinned open. */
static void mark_fresh(ds_buffer *b, uint32_t off, uint32_t n)
{
    if (!b->fresh || n == 0 || off >= b->bytes) return;
    uint32_t end = n > b->bytes - off ? b->bytes : off + n;
    for (uint32_t k = off / FRESH_BLOCK; k * FRESH_BLOCK < end; k++) __atomic_store_n(&b->fresh[k], 1, __ATOMIC_RELEASE);
}

/* Bytes between where the previous Unlock ended and where this one starts: content the writer
 * skipped instead of producing (a stall that resumes at a later position, the 11-16% of Locks
 * the menu loads show). Diagnostics: the mixer cannot recover audio that was never written. */
uint32_t ds_writer_gap(uint32_t last_end, uint32_t off, uint32_t bytes)
{
    if (bytes == 0 || last_end >= bytes || off >= bytes) return 0;
    return off >= last_end ? off - last_end : off + bytes - last_end;
}

/* One region the game unlocked: account for it and mark the blocks it covers. Runs on the
 * buffer's writer (guest) thread only; the trace reads the counters with atomics. */
static void note_write(ds_buffer *b, uint32_t off, uint32_t n)
{
    if (!b->bytes || n == 0 || off >= b->bytes) return;
    if (n > b->bytes - off) n = b->bytes - off;
    if (b->have_written) {
        uint32_t gap = ds_writer_gap(b->last_write_end, off, b->bytes);
        if (gap) {
            __atomic_fetch_add(&b->write_skips, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&b->write_gap_bytes, gap, __ATOMIC_RELAXED);
        }
    }
    b->last_write_end = (off + n) % b->bytes;
    b->have_written = 1;
}

/* ---- the mixer core ---------------------------------------------------------
 * ds_mix_render, ds_mix_volume and ds_mix_pan are declared in win32/dsound.h and checked by
 * tests/selftest.c. The phase runs in Q32 source frames: an output frame advances it by
 * ds_mix_step(rate), and the source frame at bits 32.. is interpolated toward the next one by
 * the fraction in bits 16..0. At the format's exact 48 kHz the fraction is always zero, so a
 * 48 kHz buffer passes through bit-exact. */

uint64_t ds_mix_step(uint32_t rate_hz)
{
    return ((uint64_t)rate_hz << 32) / MIX_RATE;
}

/* DSBVOLUME: hundredths of a dB, 0 (full) down to -10000 (silent). */
double ds_mix_volume(int32_t hundredths_db)
{
    return hundredths_db <= -10000 ? 0.0 : pow(10.0, hundredths_db / 2000.0);
}

/* DSBPAN: -10000 full left to 10000 full right. */
void ds_mix_pan(int32_t pan, double *gl, double *gr)
{
    if (pan < -10000) pan = -10000;
    if (pan > 10000) pan = 10000;
    *gl = pan > 0 ? 1.0 - pan / 10000.0 : 1.0;
    *gr = pan < 0 ? 1.0 + pan / 10000.0 : 1.0;
}

/* One channel of one frame, linearly interpolated between the frame at p0 and the next. */
static int32_t lerp_ch(const uint8_t *p0, const uint8_t *p1, uint32_t off, uint32_t bits, uint32_t frac)
{
    int32_t s0 = pcm_sample(p0 + off, bits);
    int32_t s1 = pcm_sample(p1 + off, bits);
    return s0 + (int32_t)((int64_t)(s1 - s0) * frac >> 16);   /* 64-bit: a full-scale step times frac overflows int32 */
}

uint32_t ds_mix_render(int32_t *acc, uint32_t frames, const uint8_t *data, uint32_t bytes,
                       uint32_t nch, uint32_t bits, uint32_t align, int looping,
                       uint8_t *fresh, uint64_t step_q32, double gl, double gr,
                       ds_mix_state *st)
{
    uint32_t src_frames = bytes / align;
    if (src_frames == 0 || step_q32 == 0 || nch == 0 || (bits != 8 && bits != 16)) return 0;
    uint32_t bpsamp = bits / 8;
    uint64_t wrap = (uint64_t)src_frames << 32;
    uint32_t j;
    for (j = 0; j < frames; j++) {
        uint64_t phase = st->phase;
        if (phase >= wrap) {
            if (!looping) break;
            do { phase -= wrap; } while (phase >= wrap);
        }
        uint32_t f = (uint32_t)(phase >> 32);

        /* The game's writes mark the FRESH_BLOCK blocks they cover (Unlock); the mixer clears
         * a block as it leaves it, so each write plays once. A block the game has not written
         * since the mixer last played it is stale: its gate fades to silence instead of
         * cutting to it (a click) or replaying the old audio (the repeats e8d1a37 removed). */
        int32_t target = VT3_MIX_GATE_MAX;
        uint32_t blk = (uint32_t)((f * (uint64_t)align) / FRESH_BLOCK);
        if (fresh) {
            if (blk != st->last_blk) {
                if (st->last_blk != UINT32_MAX) __atomic_store_n(&fresh[st->last_blk], 0, __ATOMIC_RELAXED);
                st->last_blk = blk;
            }
            /* Acquire, pairing with mark_fresh's release: the game's samples must be visible
             * once the flag is. A missed mark only extends the dip by one ramp. */
            if (!__atomic_load_n(&fresh[blk], __ATOMIC_ACQUIRE)) {
                /* The writer may be a few output frames behind this block boundary: it just
                 * waited for a guest lock, and the mixer got there first. Hold the gate open
                 * for VT3_MIX_GRACE_FRAMES once per stale stretch, so that a write landing
                 * inside the grace costs nothing. A write that never comes still closes the
                 * gate, so old audio is never replayed (e8d1a37's property). */
                if (!st->stale && st->grace > 0) {
                    st->grace--;
                    st->grace_frames++;
                } else {
                    target = 0;
                }
            } else {
                st->grace = VT3_MIX_GRACE_FRAMES;
            }
        }
        /* Trace: count each continuous stretch of stale silence once, at the block that
         * caused it, so a per-second summary can be aligned with the game's write times. */
        if (target == 0) {
            if (!st->stale) { st->stale = 1; st->stale_events++; st->stale_blk = blk; }
        } else {
            st->stale = 0;
        }
        int32_t gate = st->gate;
        if (gate < target)      { gate += MIX_GATE_STEP; if (gate > target) gate = target; }
        else if (gate > target) { gate -= MIX_GATE_STEP; if (gate < target) gate = target; }
        st->gate = gate;
        if (gate < VT3_MIX_GATE_MAX) st->dip_frames++;   /* trace: frames below full gate */

        if (gate) {
            uint32_t f2 = f + 1 < src_frames ? f + 1 : (looping ? 0 : f);
            const uint8_t *p0 = data + f * (uint64_t)align;
            const uint8_t *p1 = data + f2 * (uint64_t)align;
            uint32_t frac = (uint32_t)(phase >> 16) & 0xFFFFu;
            int32_t l, r;
            if (nch == 1) {
                l = r = lerp_ch(p0, p1, 0, bits, frac);
            } else if (nch == 2) {
                l = lerp_ch(p0, p1, 0, bits, frac);
                r = lerp_ch(p0, p1, bpsamp, bits, frac);
            } else {
                /* Surround in SPEAKER order: FL, FR, FC, LFE, BL, BR (quad: FL, FR, BL, BR). The centre and
                 * the rears fold into the stereo pair at -3 dB; the LFE channel is left out. */
                int32_t fl = lerp_ch(p0, p1, 0, bits, frac);
                int32_t fr = lerp_ch(p0, p1, bpsamp, bits, frac);
                int32_t c = nch == 4 ? 0 : lerp_ch(p0, p1, 2 * bpsamp, bits, frac);
                int32_t bl = nch == 4 ? lerp_ch(p0, p1, 2 * bpsamp, bits, frac)
                           : nch >= 6 ? lerp_ch(p0, p1, 4 * bpsamp, bits, frac) : 0;
                int32_t br = nch == 4 ? lerp_ch(p0, p1, 3 * bpsamp, bits, frac)
                           : nch >= 6 ? lerp_ch(p0, p1, 5 * bpsamp, bits, frac) : 0;
                l = fl + (int32_t)(0.7071 * (c + bl));
                r = fr + (int32_t)(0.7071 * (c + br));
            }
            /* 64-bit: a surround downmix reaches ~79k, which times a Q15 gate overflows int32. */
            acc[2 * j]     += (int32_t)((((int64_t)l * gate) >> 15) * gl);
            acc[2 * j + 1] += (int32_t)((((int64_t)r * gate) >> 15) * gr);
        }
        st->phase = phase + step_q32;
    }
    return j;
}

/* ---- the mixer's work item ---------------------------------------------------
 * The mixer used to hold g_mu across the whole DSP pass: every active buffer for up to 960
 * frames, plus the trace's printf. A guest Lock or GetCurrentPosition waiting for that is a
 * late write, and a late write is a block the mixer reaches unmarked. It now copies what it
 * needs under g_mu, mixes with the lock released, and takes it again only to commit.
 *
 * The PCM stays in guest memory that the game may write while it is mixed: that is the same
 * race a sound card has with the game's write cursor, and it is deliberately unchanged. What
 * is new is lifetime and ordering. A snapshotted buffer is pinned, so Release can only mark
 * it dead and the mixer frees it after the commit; no pointer outlives its object. Every call
 * that retimes playback bumps mix_seq, and a chunk whose seq changed is not committed, so a
 * Play, Stop or seek that lands mid-chunk wins over the state the mixer computed. */

typedef struct ds_mix_job {
    ds_buffer *b;
    const uint8_t *data;
    uint8_t *fresh;
    uint32_t bytes, nch, bits, align, hz, seq;
    int looping;
    double gl, gr;
    ds_mix_state st;      /* gate, phase, last_blk and stale carried in from the buffer */
    int finished;
} ds_mix_job;

/* Copies one playing buffer's mix inputs under g_mu. The start position comes from the same
 * wall-clock model as GetCurrentPosition, so the two agree. Returns 0 when the buffer cannot be
 * mixed or has played out (which stops it, as the mixer did before). */
static int job_snapshot_locked(ds_buffer *b, ds_mix_job *j, uint64_t t0)
{
    uint32_t nch = fmt16(b->fmt, 2), bits = fmt16(b->fmt, 14), align = fmt16(b->fmt, 12);
    uint32_t rate = rd32(b->fmt, 4);
    if (nch < 1 || nch > 8 || (bits != 8 && bits != 16) || align == 0 || rate == 0 || b->bytes < align) return 0;
    const uint8_t *data = d3d9_gmem(b->storage, b->bytes);
    if (!data) return 0;
    uint32_t src_frames = b->bytes / align;
    uint32_t hz = b->freq ? b->freq : rate;        /* SetFrequency overrides the format's rate */
    uint64_t moved_ns = t0 > b->play_ns ? t0 - b->play_ns : 0;
    double start = (double)(b->play_base / align) + (double)moved_ns * (double)hz / 1e9;
    if (b->looping) {
        start = fmod(start, (double)src_frames);
    } else if (start >= (double)src_frames) {        /* a non-looping buffer has played out */
        b->playing = 0;
        b->pos = 0;
        active_remove_locked(b);
        return 0;
    }
    memset(j, 0, sizeof *j);
    j->b = b;
    j->data = data;
    j->bytes = b->bytes;
    j->nch = nch;
    j->bits = bits;
    j->align = align;
    j->hz = hz;
    j->looping = b->looping;
    j->fresh = ds_no_gate() ? NULL : b->fresh;
    j->seq = b->mix_seq;
    j->st = b->mix;
    j->st.phase = (uint64_t)(start * 4294967296.0);
    j->st.dip_frames = 0;         /* per-chunk diagnostics; the trace accumulates them */
    j->st.stale_events = 0;
    j->st.grace_frames = 0;
    double gain = ds_mix_volume(b->volume), gl, gr;
    ds_mix_pan(b->pan, &gl, &gr);
    j->gl = gl * gain;
    j->gr = gr * gain;
    b->pinned = 1;
    return 1;
}

/* Mixes one job with g_mu released. */
static void job_render(ds_mix_job *j, int32_t *acc, uint32_t frames)
{
    uint32_t done = ds_mix_render(acc, frames, j->data, j->bytes, j->nch, j->bits, j->align,
                                  j->looping, j->fresh, ds_mix_step(j->hz), j->gl, j->gr, &j->st);
    if (done < frames) j->finished = 1;              /* the buffer ran out mid-chunk */
}

/* Frees the host parts of a buffer. Called under g_mu; the object itself is freed after the
 * mixer's lock is released (guest heap frees are short, so they stay here). */
static void bury_locked(ds_buffer *b)
{
    if (b->storage) HeapFree(GetProcessHeap(), 0, b->storage);
    free(b->fresh);
    b->storage = 0;
    b->fresh = NULL;
}

/* Publishes a job's result under g_mu, unless the game retimed the buffer while it mixed. */
static void job_commit_locked(ds_mix_job *j)
{
    ds_buffer *b = j->b;
    if (b->mix_seq == j->seq) {
        b->mix = j->st;
        b->mix.dip_frames = 0;
        b->mix.stale_events = 0;
        b->mix.grace_frames = 0;
        b->trace_dip_frames += j->st.dip_frames;
        b->trace_stale_events += j->st.stale_events;
        if (j->st.stale) b->trace_stale_blk = j->st.stale_blk;
        b->trace_grace_frames += j->st.grace_frames;
        if (j->finished) {
            b->playing = 0;
            b->pos = 0;
            b->mix.gate = 0;
            b->mix.last_blk = UINT32_MAX;
            active_remove_locked(b);
        }
    }
    b->pinned = 0;
    if (b->dead) {                                   /* Release arrived mid-chunk */
        bury_locked(b);
        if (g_ngrave < MIX_MAX_ACTIVE) g_graveyard[g_ngrave++] = b;
        else free(b);                                /* the slot array is full: free here, not leak */
    }
}

/* The mixer wakes every 4 ms and must not be late: a late tick lets the device ring run short. On
 * Android the audio class of nice keeps it ahead of the loading threads; a failed call is harmless. */
static void raise_mixer_priority(void)
{
#if defined(__ANDROID__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -16);   /* THREAD_PRIORITY_AUDIO */
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

/* One buffer's line of the once-a-second trace, gathered under g_mu and printed after it. */
typedef struct mix_trace_stat {
    uint32_t obj, bytes, rate, ch, bits, locks, plays, below_full_ms, stale_events, stale_byte;
    uint32_t behind_ms, skipped, skipped_ms, grace_ms;
    int looping, playing;
} mix_trace_stat;

static void *mix_thread(void *arg)
{
    (void)arg;
    raise_mixer_priority();
    static int32_t acc[MIX_MAX_FRAMES * MIX_CH];
    static int16_t out[MIX_MAX_FRAMES * MIX_CH];
    static ds_mix_job jobs[MIX_MAX_ACTIVE];
    static mix_trace_stat stats[MIX_MAX_ACTIVE];
    ds_buffer *grave[MIX_MAX_ACTIVE];
    /* VT3_TRACE_AUDIO=1 logs once a second: buffers playing, peak level, device underruns,
     * stale blocks, skipped regions and how long the guest waited for the mixer's lock. */
    const int trace = getenv("VT3_TRACE_AUDIO") != NULL;
    /* VT3_DUMP_AUDIO=path writes the mixed output as raw s16 stereo 48 kHz, for analysis. */
    FILE *dump = getenv("VT3_DUMP_AUDIO") ? fopen(getenv("VT3_DUMP_AUDIO"), "wb") : NULL;
    uint64_t window_ns = now_ns();
    int32_t window_peak = 0;
    uint64_t last_lock_total = 0;
    uint32_t last_lock_takes = 0;
    for (;;) {
        struct timespec ts = { 0, 4000000 };          /* 4 ms */
        nanosleep(&ts, NULL);
        vt3_audio *out_dev = g_out;
        int njobs = 0, ngrave = 0, nact = 0, report = 0;
        uint32_t frames = 0;
        int32_t peak = 0;
        uint64_t now = 0, gap_ms = 0, used_ms = 0, avg_lock_us = 0, max_lock_us = 0;
        uint32_t takes = 0, skips = 0, skip_ms = 0;

        pthread_mutex_lock(&g_mu);
        now = now_ns();
        int streaming = vt3_audio_streaming(out_dev);
        int paused = vt3_audio_is_paused(out_dev);
        sync_pause_locked(now);
        if (paused || !streaming) {
            /* Nothing is coming out of the device. A platform pause freezes the mixer timeline
             * and the guest play clocks (both shifted forward on resume by sync_pause_locked),
             * so returning to the game continues the stream where it stopped instead of jumping
             * by the time spent backgrounded into blocks the writer never produced. With no
             * device at all there is nothing to continue from, so follow the clock and drop the
             * audio rather than play it late once a reopen succeeds. */
            if (!paused) g_mix_next_ns = now;
            pthread_mutex_unlock(&g_mu);
            continue;
        }
        /* A late tick is not a reason to throw audio away. The ring is the buffer that absorbs a
         * starved thread; only a gap far longer than the ring itself means the timeline really
         * has to be re-anchored, and then it is counted so the trace says why a skip was heard.
         * (This was 50 ms, which is shorter than the old 40 ms ring plus one scheduling delay:
         * a single missed tick could cut a chunk out of the music -- the stutter this fixes.) */
        if (now > g_mix_next_ns + 250000000u) {
            g_mix_next_ns = now - 10000000u;
            g_resyncs++;                              /* more than 250 ms behind: resync, and count */
        }
        uint64_t due = now > g_mix_next_ns ? (now - g_mix_next_ns) * MIX_RATE / 1000000000u : 0;
        uint64_t space = out_dev ? vt3_audio_space(out_dev) / (MIX_CH * sizeof(int16_t)) : 0;
        frames = (uint32_t)(due < space ? due : space);
        if (due > frames) {
            g_starved_frames += due - frames;
            if (frames == 0) g_starved_ticks++;
        }
        if (frames > MIX_MAX_FRAMES) frames = MIX_MAX_FRAMES;
        if (frames > 0) {
            /* Downward, so a buffer that stops and is removed does not skip the one moved into
             * its slot. The snapshots pin each buffer for the unlocked pass below. */
            for (int i = g_nactive - 1; i >= 0; i--) {
                if (i < g_nactive && job_snapshot_locked(g_active[i], &jobs[njobs], g_mix_next_ns)) njobs++;
            }
        }
        pthread_mutex_unlock(&g_mu);

        if (frames > 0) {
            memset(acc, 0, (size_t)frames * MIX_CH * sizeof acc[0]);
            for (int k = 0; k < njobs; k++) job_render(&jobs[k], acc, frames);
            /* Master volume and mute, from the settings markers (VT3_AUDIO_VOLUME / _MUTE, read
             * once by the mixer). 100 % is bit-identical to the mixed output. */
            static int gain = -1;
            if (gain < 0) {
                const char *m = getenv("VT3_AUDIO_MUTE");
                const char *v = getenv("VT3_AUDIO_VOLUME");
                gain = (m && atoi(m)) ? 0 : (v ? atoi(v) : 100);
                if (gain < 0) gain = 0;
                if (gain > 100) gain = 100;
            }
            for (size_t k = 0; k < (size_t)frames * MIX_CH; k++) {
                int32_t v = acc[k];
                if (gain != 100) v = v * gain / 100;
                out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                if (v > peak) peak = v;
                if (-v > peak) peak = -v;
            }
            if (dump) fwrite(out, sizeof(int16_t), (size_t)frames * MIX_CH, dump);
            if (out_dev) vt3_audio_write(out_dev, out, (size_t)frames * MIX_CH * sizeof(int16_t));

            pthread_mutex_lock(&g_mu);
            for (int k = 0; k < njobs; k++) job_commit_locked(&jobs[k]);
            ngrave = g_ngrave;
            for (int k = 0; k < ngrave; k++) grave[k] = g_graveyard[k];
            g_ngrave = 0;
            g_mix_next_ns += frames * 1000000000u / MIX_RATE;
            if (trace) {
                window_peak = peak > window_peak ? peak : window_peak;
                if (now - window_ns >= 1000000000u) {
                    report = 1;
                    uint64_t space_now = out_dev ? vt3_audio_space(out_dev) : 0;
                    used_ms = (g_ring_bytes > space_now ? g_ring_bytes - space_now : 0) * 1000u /
                              ((uint64_t)MIX_RATE * MIX_CH * sizeof(int16_t));
                    gap_ms = (now - g_mix_next_ns) / 1000000u;
                    /* Per-second lock figures: the total is cumulative, the worst is reset. */
                    uint64_t lock_total = g_lock_wait_ns - last_lock_total;
                    last_lock_total = g_lock_wait_ns;
                    takes = g_lock_takes - last_lock_takes;
                    last_lock_takes = g_lock_takes;
                    avg_lock_us = takes ? lock_total / takes / 1000u : 0;
                    max_lock_us = g_lock_wait_max_ns / 1000u;
                    g_lock_wait_max_ns = 0;
                    nact = g_nactive;
                    for (int i = 0; i < nact; i++) {
                        ds_buffer *b = g_active[i];
                        mix_trace_stat *s = &stats[i];
                        uint32_t pnow = play_cursor_at(b, now);
                        uint32_t pmix = play_cursor_at(b, g_mix_next_ns);
                        uint32_t behind = (pnow + b->bytes - pmix) % (b->bytes ? b->bytes : 1);
                        s->obj = b->obj;
                        s->bytes = b->bytes;
                        s->rate = rd32(b->fmt, 4);
                        s->ch = fmt16(b->fmt, 2);
                        s->bits = fmt16(b->fmt, 14);
                        s->looping = b->looping;
                        s->playing = b->playing;
                        s->locks = b->n_locks;
                        s->plays = b->n_plays;
                        s->below_full_ms = b->trace_dip_frames / (MIX_RATE / 1000u);
                        s->stale_events = b->trace_stale_events;
                        s->stale_byte = b->trace_stale_blk * FRESH_BLOCK;
                        s->grace_ms = b->trace_grace_frames / (MIX_RATE / 1000u);
                        s->skipped = __atomic_load_n(&b->write_skips, __ATOMIC_RELAXED);
                        uint64_t gb = __atomic_load_n(&b->write_gap_bytes, __ATOMIC_RELAXED);
                        s->skipped_ms = (uint32_t)(gb * 1000u / (buf_bps(b) ? buf_bps(b) : 1u));
                        s->behind_ms = behind * 1000u / (buf_bps(b) ? buf_bps(b) : 1u);
                        b->trace_dip_frames = 0;
                        b->trace_stale_events = 0;
                        b->trace_grace_frames = 0;
                        __atomic_store_n(&b->write_skips, 0, __ATOMIC_RELAXED);
                        __atomic_store_n(&b->write_gap_bytes, 0, __ATOMIC_RELAXED);
                        skips += s->skipped;
                        skip_ms += s->skipped_ms;
                    }
                    window_ns = now;
                    window_peak = 0;
                }
            }
            pthread_mutex_unlock(&g_mu);
            for (int k = 0; k < ngrave; k++) free(grave[k]);

            if (report) {
                VT3_INFO("audio: %d playing, peak %d in the last second, underruns %u, starved %u, "
                         "ring %llu ms, gap %lld ms, resync %u, t=%llu ms",
                         nact, (int)window_peak,
                         (unsigned)(out_dev ? vt3_audio_underruns(out_dev) : 0u),
                         (unsigned)(out_dev ? vt3_audio_starved(out_dev) : 0u),
                         (unsigned long long)used_ms, (long long)gap_ms, g_resyncs,
                         (unsigned long long)(now / 1000000u % 100000u));
                /* The two numbers that explain a stutter: frames the ring had no room for, and
                 * ticks where it was completely full. Non-zero here with clean underruns means
                 * the mixer is outrunning the device, not the other way round. */
                VT3_INFO("audio: ring-full drops %llu frames over %u ticks",
                         (unsigned long long)g_starved_frames, g_starved_ticks);
                g_starved_frames = 0;
                g_starved_ticks = 0;
                VT3_INFO("audio: guest lock: %u takes, avg %llu us, max %llu us; writer skipped %u regions (%u ms)",
                         takes, (unsigned long long)avg_lock_us, (unsigned long long)max_lock_us,
                         skips, skip_ms);
                for (int i = 0; i < nact; i++) {
                    mix_trace_stat *s = &stats[i];
                    VT3_INFO("audio:   0x%08x %u B %u Hz x%u ch %u bit loop %d playing %d, "
                             "locks %u plays %u, below-full gate %u ms, stale %u events at byte %u, "
                             "grace-held %u ms, writer skipped %u regions (%u ms), read-behind %u ms",
                             s->obj, s->bytes, s->rate, s->ch, s->bits, s->looping, s->playing,
                             s->locks, s->plays, s->below_full_ms, s->stale_events, s->stale_byte,
                             s->grace_ms, s->skipped, s->skipped_ms, s->behind_ms);
                }
            }
        } else {
            pthread_mutex_unlock(&g_mu);
            for (int k = 0; k < ngrave; k++) free(grave[k]);
        }
    }
    return NULL;
}

/* Opens the output and starts the mixer, once. Without a device the buffers stay silent. */
static void mixer_start_once(void)
{
    static int started;
    if (started) return;
    started = 1;
    vt3_audio_format fmt = { MIX_RATE, (int)MIX_CH };
    size_t ring = (size_t)MIX_RATE * MIX_CH * sizeof(int16_t) * MIX_RING_MS / 1000u;
    if (vt3_audio_open(&g_out, fmt, ring) != 0) {
        g_out = NULL;
        VT3_WARN("DirectSound: no audio device; playback stays silent");
        return;
    }
    g_ring_bytes = ring;
    g_mix_next_ns = now_ns();
    pthread_t th;
    if (pthread_create(&th, NULL, mix_thread, NULL) != 0) {
        VT3_WARN("DirectSound: could not start the mixer; playback stays silent");
        return;
    }
    pthread_detach(th);
    vt3_audio_set_playing(g_out, 1);
    VT3_INFO("DirectSound: mixing to a %u Hz stereo output", (unsigned)MIX_RATE);
}

/* ---- objects --------------------------------------------------------------- */

static ds_buffer *buf_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_DSBUFFER);
    return h ? h->data : NULL;
}

static ds_device *dev_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_DSOUND);
    return h ? h->data : NULL;
}

/* A buffer with its own storage, or 0. fmt is the 18-byte WAVEFORMATEX from the guest. */
static GPTR buffer_new(GPTR parent, uint32_t bytes, const uint8_t *fmt)
{
    ds_buffer *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!b->storage) {
        free(b);
        return 0;
    }
    b->bytes = bytes;
    b->fresh = calloc((bytes + FRESH_BLOCK - 1) / FRESH_BLOCK, 1);   /* NULL: the buffer plays without flags */
    memcpy(b->fmt, fmt, WFX_BYTES);
    b->freq = rd32(fmt, 4);
    b->volume = DSBVOLUME_MAX;
    b->mix.last_blk = UINT32_MAX;
    GPTR obj = d3d9_obj_new(D3D9_KIND_DSBUFFER, b, parent);
    if (obj) b->obj = obj;
    if (!obj) {
        HeapFree(GetProcessHeap(), 0, b->storage);
        free(b->fresh);
        free(b);
    }
    return obj;
}

void dsound_free(uint32_t kind, void *data)
{
    if (kind == D3D9_KIND_DSBUFFER) {
        ds_buffer *b = data;
        int deferred = 0;
        pthread_mutex_lock(&g_mu);
        active_remove_locked(b);
        b->playing = 0;
        if (b->pinned) {
            /* The mixer is reading this buffer with the lock released: it frees the object at
             * the end of the chunk, so a guest Release never waits for the DSP pass. */
            b->dead = 1;
            deferred = 1;
        } else {
            bury_locked(b);
        }
        pthread_mutex_unlock(&g_mu);
        if (deferred) return;
        b->storage = 0;
        b->fresh = NULL;
    }
    free(data);
}

/* A primary buffer has no size of its own: it is the mix target. */
#define DS_PRIMARY_BYTES 65536u

/* DirectSound's default primary format: PCM, 22.05 kHz, 8-bit, mono. */
static void default_primary_fmt(uint8_t f[WFX_BYTES])
{
    uint16_t tag = 1, ch = 1, align = 1, bits = 8;
    uint32_t rate = 22050, avg = 22050, cb = 0;
    memset(f, 0, WFX_BYTES);
    memcpy(f + 0, &tag, 2);
    memcpy(f + 2, &ch, 2);
    memcpy(f + 4, &rate, 4);
    memcpy(f + 8, &avg, 4);
    memcpy(f + 12, &align, 2);
    memcpy(f + 14, &bits, 2);
    memcpy(f + 16, &cb, 2);
}

/* ---- DirectSoundCreate ------------------------------------------------------ */

HRESULT WINAPI DirectSoundCreate(LPCGUID device, GPTR out_ds, LPUNKNOWN outer)
{
    (void)device;                /* there is one output, so the device GUID is not used */
    if (!out_ds) return DSERR_INVALIDPARAM;
    put32(out_ds, 0);
    if (outer) return DSERR_NOAGGREGATION;
    ds_device *d = calloc(1, sizeof *d);
    if (!d) return E_OUTOFMEMORY_D3D9;
    GPTR obj = d3d9_obj_new(D3D9_KIND_DSOUND, d, 0);
    if (!obj) {
        free(d);
        VT3_ERROR("DirectSoundCreate: no free object slot or guest heap");
        return E_OUTOFMEMORY_D3D9;
    }
    mixer_start_once();
    VT3_INFO("DirectSoundCreate: IDirectSound at 0x%08x", obj);
    put32(out_ds, obj);
    return DS_OK;
}

/* ---- IDirectSound ---------------------------------------------------------- */

HRESULT WINAPI IDirectSound_CreateSoundBuffer(GPTR This, GPTR pcDSBufferDesc, GPTR ppDSBuffer, GPTR pUnkOuter)
{
    put32(ppDSBuffer, 0);
    if (!dev_of(This) || !ppDSBuffer || !pcDSBufferDesc) return DSERR_INVALIDPARAM;
    if (pUnkOuter) return DSERR_NOAGGREGATION;
    const uint8_t *desc = d3d9_gmem(pcDSBufferDesc, DSBDESC_BYTES);
    if (!desc) return DSERR_INVALIDPARAM;
    uint32_t flags = rd32(desc, 4);
    uint32_t bytes = rd32(desc, 8);
    GPTR wfx = rd32(desc, 16);
    uint8_t dflt[WFX_BYTES];
    const uint8_t *fmt = wfx ? d3d9_gmem(wfx, WFX_BYTES) : NULL;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (bytes == 0) bytes = DS_PRIMARY_BYTES;
        if (!fmt) {
            default_primary_fmt(dflt);
            fmt = dflt;
        }
    }
    if (bytes == 0 || !fmt) {
        VT3_WARN("IDirectSound::CreateSoundBuffer: bad description (%u bytes), returning a stub", bytes);
        put32(ppDSBuffer, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return DSERR_INVALIDPARAM;
    }
    GPTR obj = buffer_new(This, bytes, fmt);
    if (dsound_trace())
        VT3_INFO("DirectSound: CreateSoundBuffer flags 0x%x, %u bytes, %u Hz, %u ch, %u bit -> 0x%08x",
                 (unsigned)flags, (unsigned)bytes, (unsigned)rd32(fmt, 4), (unsigned)fmt16(fmt, 2),
                 (unsigned)fmt16(fmt, 14), (unsigned)obj);
    if (!obj) {
        VT3_WARN("IDirectSound::CreateSoundBuffer: no memory for %u bytes, returning a stub", bytes);
        put32(ppDSBuffer, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return E_OUTOFMEMORY_D3D9;
    }
    put32(ppDSBuffer, obj);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_GetCaps(GPTR This, GPTR pDSCaps)
{
    if (!dev_of(This) || !pDSCaps) return DSERR_INVALIDPARAM;
    const uint8_t *in = d3d9_gmem(pDSCaps, 4);
    if (!in) return DSERR_INVALIDPARAM;
    uint32_t size = rd32(in, 0);
    if (size < 4) return DSERR_INVALIDPARAM;
    uint8_t caps[DSCAPS_BYTES];
    memset(caps, 0, sizeof caps);
    memcpy(caps, in, 4);                          /* dwSize is the caller's */
    set32(caps, 4, DSCAPS_EMULDRIVER);
    set32(caps, 8, DSBFREQUENCY_MIN);
    set32(caps, 12, DSBFREQUENCY_MAX);
    set32(caps, 16, 1);                           /* one primary buffer */
    put_bytes(pDSCaps, caps, size < DSCAPS_BYTES ? size : DSCAPS_BYTES);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_DuplicateSoundBuffer(GPTR This, GPTR pdsbOriginal, GPTR ppdsbDuplicate)
{
    put32(ppdsbDuplicate, 0);
    if (!dev_of(This) || !ppdsbDuplicate) return DSERR_INVALIDPARAM;
    ds_buffer *src = buf_of(pdsbOriginal);
    if (!src) {
        put32(ppdsbDuplicate, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return DSERR_INVALIDPARAM;
    }
    GPTR obj = buffer_new(This, src->bytes, src->fmt);
    if (!obj) {
        put32(ppdsbDuplicate, d3d9_new_stub(D3D9_KIND_DSBUFFER, This));
        return E_OUTOFMEMORY_D3D9;
    }
    const void *from = d3d9_gmem(src->storage, src->bytes);
    void *to = d3d9_gmem(buf_of(obj)->storage, src->bytes);
    if (from && to) memcpy(to, from, src->bytes);
    ds_buffer *d = buf_of(obj);                /* a duplicate copies the settings too */
    if (d) {
        d->volume = src->volume;
        d->pan = src->pan;
        d->freq = src->freq;
        guest_lock();                        /* the mixer writes src's pos and clears its fresh flags */
        d->pos = src->pos;
        if (d->fresh && src->fresh) memcpy(d->fresh, src->fresh, (src->bytes + FRESH_BLOCK - 1) / FRESH_BLOCK);
        pthread_mutex_unlock(&g_mu);
    }
    put32(ppdsbDuplicate, obj);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_SetCooperativeLevel(GPTR This, GPTR hwnd, DWORD dwLevel)
{
    (void)hwnd;
    ds_device *d = dev_of(This);
    if (!d) return DSERR_INVALIDPARAM;
    d->coop_level = dwLevel;
    return DS_OK;
}

HRESULT WINAPI IDirectSound_Compact(GPTR This)
{
    return dev_of(This) ? DS_OK : DSERR_INVALIDPARAM;
}

HRESULT WINAPI IDirectSound_GetSpeakerConfig(GPTR This, GPTR pdwSpeakerConfig)
{
    if (!dev_of(This) || !pdwSpeakerConfig) return DSERR_INVALIDPARAM;
    put32(pdwSpeakerConfig, DSSPEAKER_STEREO);
    return DS_OK;
}

HRESULT WINAPI IDirectSound_SetSpeakerConfig(GPTR This, DWORD dwSpeakerConfig)
{
    (void)dwSpeakerConfig;
    return dev_of(This) ? DS_OK : DSERR_INVALIDPARAM;
}

HRESULT WINAPI IDirectSound_Initialize(GPTR This, GPTR pcGuidDevice)
{
    (void)pcGuidDevice;
    if (!dev_of(This)) return DSERR_UNSUPPORTED;
    return DSERR_ALREADYINITIALIZED;              /* DirectSoundCreate returns an initialized object */
}

/* ---- IDirectSoundBuffer ---------------------------------------------------- */

HRESULT WINAPI IDirectSoundBuffer_GetCaps(GPTR This, GPTR pDSBufferCaps)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pDSBufferCaps) return DSERR_INVALIDPARAM;
    const uint8_t *in = d3d9_gmem(pDSBufferCaps, 4);
    if (!in) return DSERR_INVALIDPARAM;
    uint32_t size = rd32(in, 0);
    if (size < 4) return DSERR_INVALIDPARAM;
    uint8_t caps[DSBCAPS_BYTES];
    memset(caps, 0, sizeof caps);
    memcpy(caps, in, 4);
    set32(caps, 4, DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLDEFAULT);
    set32(caps, 8, b->bytes);
    put_bytes(pDSBufferCaps, caps, size < DSBCAPS_BYTES ? size : DSBCAPS_BYTES);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetCurrentPosition(GPTR This, GPTR pdwCurrentPlayCursor, GPTR pdwCurrentWriteCursor)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    uint32_t play, write;
    uint64_t now;
    guest_lock();                            /* the mixer writes playing/pos */
    now = play_clock(now_ns());
    cursors_at(b, now, &play, &write);
    int playing = b->playing;
    pthread_mutex_unlock(&g_mu);
    put32(pdwCurrentPlayCursor, play);
    put32(pdwCurrentWriteCursor, write);
    if (dsound_trace())
        VT3_INFO("DirectSound: GetCurrentPosition 0x%08x play %u write %u (playing %d) t=%llu ms thr %llu", (unsigned)This,
                 (unsigned)play, (unsigned)write, playing,
                 (unsigned long long)(now / 1000000u % 100000u), (unsigned long long)((uintptr_t)pthread_self() % 1000u));
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetFormat(GPTR This, GPTR pwfxFormat, DWORD dwSizeAllocated, GPTR pdwSizeWritten)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pwfxFormat && !pdwSizeWritten) return DSERR_INVALIDPARAM;
    if (pwfxFormat) {
        if (dwSizeAllocated < WFX_BYTES) return DSERR_INVALIDPARAM;
        put_bytes(pwfxFormat, b->fmt, WFX_BYTES);
    }
    put32(pdwSizeWritten, WFX_BYTES);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetVolume(GPTR This, GPTR plVolume)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!plVolume) return DSERR_INVALIDPARAM;
    put32(plVolume, (uint32_t)b->volume);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetPan(GPTR This, GPTR plPan)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!plPan) return DSERR_INVALIDPARAM;
    put32(plPan, (uint32_t)b->pan);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetFrequency(GPTR This, GPTR pdwFrequency)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pdwFrequency) return DSERR_INVALIDPARAM;
    put32(pdwFrequency, b->freq);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_GetStatus(GPTR This, GPTR pdwStatus)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (!pdwStatus) return DSERR_INVALIDPARAM;
    guest_lock();
    int playing = b->playing && !played_out(b, play_clock(now_ns()));
    uint32_t st = (playing ? DSBSTATUS_PLAYING : 0) | (b->looping ? DSBSTATUS_LOOPING : 0);
    pthread_mutex_unlock(&g_mu);
    put32(pdwStatus, st);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Initialize(GPTR This, GPTR pDirectSound, GPTR pcDSBufferDesc)
{
    (void)pDirectSound;
    (void)pcDSBufferDesc;
    if (!buf_of(This)) return DSERR_UNSUPPORTED;
    return DSERR_ALREADYINITIALIZED;              /* buffers come from CreateSoundBuffer, already initialized */
}

HRESULT WINAPI IDirectSoundBuffer_Lock(GPTR This, DWORD dwOffset, DWORD dwBytes, GPTR ppvAudioPtr1,
                                       GPTR pdwAudioBytes1, GPTR ppvAudioPtr2, GPTR pdwAudioBytes2,
                                       DWORD dwFlags)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    put32(ppvAudioPtr1, 0);
    put32(pdwAudioBytes1, 0);
    put32(ppvAudioPtr2, 0);
    put32(pdwAudioBytes2, 0);
    uint32_t off = dwOffset, n = dwBytes;
    uint32_t play, write;
    guest_lock();                            /* the mixer writes playing/pos */
    cursors_at(b, play_clock(now_ns()), &play, &write);
    pthread_mutex_unlock(&g_mu);
    if (dwFlags & DSBLOCK_FROMWRITECURSOR) off = write;
    if (dwFlags & DSBLOCK_ENTIREBUFFER) {
        off = 0;
        n = b->bytes;
    }
    if (dsound_trace())
        VT3_INFO("DirectSound: Lock 0x%08x offset %u bytes %u flags 0x%x (play %u) t=%llu ms thr %llu", (unsigned)This, (unsigned)off,
                 (unsigned)n, (unsigned)dwFlags, (unsigned)play,
                 (unsigned long long)(now_ns() / 1000000u % 100000u), (unsigned long long)((uintptr_t)pthread_self() % 1000u));
    if (!b->bytes || off >= b->bytes || n > b->bytes) return DSERR_INVALIDPARAM;
    b->n_locks++;
    uint32_t n1 = n, n2 = 0;
    if (off + n > b->bytes) {                     /* the region wraps past the end of the buffer */
        n1 = b->bytes - off;
        n2 = n - n1;
    }
    put32(ppvAudioPtr1, b->storage + off);
    put32(pdwAudioBytes1, n1);
    if (n2) {
        put32(ppvAudioPtr2, b->storage);
        put32(pdwAudioBytes2, n2);
    }
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Play(GPTR This, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    (void)dwReserved1;
    (void)dwPriority;
    if (dsound_trace())
        VT3_INFO("DirectSound: Play 0x%08x (%u bytes, flags 0x%x, pos %u)", (unsigned)This, (unsigned)b->bytes,
                 (unsigned)dwFlags, (unsigned)b->pos);
    guest_lock();
    b->n_plays++;
    b->looping = (dwFlags & DSBPLAY_LOOPING) != 0;
    if (!b->playing) {
        b->mix_seq++;                        /* a chunk the mixer is rendering is now stale */
        b->playing = 1;
        b->play_base = b->pos;
        b->play_ns = play_clock(now_ns());
        b->mix.gate = 0;                     /* fade in over the ramp: no click at the start */
        b->mix.last_blk = UINT32_MAX;
        b->mix.stale = 0;
        b->mix.grace = VT3_MIX_GRACE_FRAMES; /* just written, so a late write is not expected */
        active_add_locked(b);
    }
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(GPTR This, DWORD dwNewPosition)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (dwNewPosition >= b->bytes) return DSERR_INVALIDPARAM;
    if (dsound_trace())
        VT3_INFO("DirectSound: SetCurrentPosition 0x%08x to %u (playing %d)", (unsigned)This, (unsigned)dwNewPosition,
                 b->playing);
    guest_lock();
    b->mix_seq++;
    b->pos = dwNewPosition;
    if (b->playing) {
        b->play_base = dwNewPosition;
        b->play_ns = play_clock(now_ns());
    }
    b->mix.gate = 0;                         /* a position jump starts a fresh fade */
    b->mix.last_blk = UINT32_MAX;
    b->mix.stale = 0;
    b->mix.grace = VT3_MIX_GRACE_FRAMES;
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

/* The setters below run on a game thread while the mixer reads the same fields; they take g_mu
 * so the mixer never sees a half-updated format, rate, volume or pan. */

HRESULT WINAPI IDirectSoundBuffer_SetFormat(GPTR This, GPTR pcfxFormat)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    const uint8_t *f = pcfxFormat ? d3d9_gmem(pcfxFormat, WFX_BYTES) : NULL;
    if (!f) return DSERR_INVALIDPARAM;
    guest_lock();
    b->mix_seq++;
    memcpy(b->fmt, f, WFX_BYTES);
    b->freq = rd32(f, 4);
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetVolume(GPTR This, INT lVolume)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (lVolume > DSBVOLUME_MAX || lVolume < DSBVOLUME_MIN) return DSERR_INVALIDPARAM;
    guest_lock();
    b->volume = lVolume;
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetPan(GPTR This, INT lPan)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (lPan > DSBPAN_RIGHT || lPan < DSBPAN_LEFT) return DSERR_INVALIDPARAM;
    guest_lock();
    b->pan = lPan;
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetFrequency(GPTR This, DWORD dwFrequency)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    guest_lock();
    b->mix_seq++;
    if (dwFrequency == DSBFREQUENCY_ORIGINAL) dwFrequency = rd32(b->fmt, 4);
    if (dwFrequency < DSBFREQUENCY_MIN || dwFrequency > DSBFREQUENCY_MAX) {
        pthread_mutex_unlock(&g_mu);
        return DSERR_INVALIDPARAM;
    }
    if (b->playing && !played_out(b, play_clock(now_ns()))) {   /* rebase, or the new rate would move the cursor retroactively */
        uint64_t now = play_clock(now_ns());
        b->play_base = play_cursor_at(b, now);
        b->play_ns = now;
    }
    b->freq = dwFrequency;
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Stop(GPTR This)
{
    ds_buffer *b = buf_of(This);
    if (!b) return DSERR_UNSUPPORTED;
    if (dsound_trace())
        VT3_INFO("DirectSound: Stop 0x%08x (playing %d)", (unsigned)This, b->playing);
    guest_lock();
    if (b->playing) {
        b->mix_seq++;
        b->pos = play_cursor_at(b, play_clock(now_ns()));
        b->playing = 0;
        b->looping = 0;                  /* the looping status belongs to the playback that Stop ends */
        active_remove_locked(b);
    }
    pthread_mutex_unlock(&g_mu);
    return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Unlock(GPTR This, GPTR pvAudioPtr1, DWORD dwAudioBytes1, GPTR pvAudioPtr2,
                                         DWORD dwAudioBytes2)
{
    ds_buffer *fb = buf_of(This);
    if (fb) {                                    /* the game's writes, where the pointers it unlocks say; both calls drop out-of-range offsets */
        if (pvAudioPtr1 >= fb->storage) {
            uint32_t off = pvAudioPtr1 - fb->storage;
            note_write(fb, off, dwAudioBytes1);
            mark_fresh(fb, off, dwAudioBytes1);
        }
        if (pvAudioPtr2 >= fb->storage) {
            uint32_t off = pvAudioPtr2 - fb->storage;
            note_write(fb, off, dwAudioBytes2);
            mark_fresh(fb, off, dwAudioBytes2);
        }
    }
    if (dsound_trace() && buf_of(This)) {
        /* Hash of what the game wrote, so repeated chunks show up in the trace. */
        uint64_t h = 1469598103934665603ull;
        const uint8_t *p = pvAudioPtr1 ? d3d9_gmem(pvAudioPtr1, dwAudioBytes1) : NULL;
        int constant = 1;
        for (uint32_t i = 0; p && i < dwAudioBytes1; i++) {
            h = (h ^ p[i]) * 1099511628211ull;
            if (p[i] != p[0]) constant = 0;
        }
        VT3_INFO("DirectSound: Unlock 0x%08x bytes %u hash %016llx %s", (unsigned)This, (unsigned)dwAudioBytes1,
                 (unsigned long long)h, constant ? "constant" : "varied");
    }
    return buf_of(This) ? DS_OK : DSERR_UNSUPPORTED;
}

HRESULT WINAPI IDirectSoundBuffer_Restore(GPTR This)
{
    return buf_of(This) ? DS_OK : DSERR_UNSUPPORTED;
}
