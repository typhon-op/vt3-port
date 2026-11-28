/*
 * The DirectSound mixer core. Everything here is a function of its arguments: no locks, no device,
 * no guest memory, no clock. dsound_stub.c drives it from the mixer thread and owns the buffers;
 * tools/audiotest/AudioHostTest.c drives it from a host test. See include/vt3/vt3_mix.h.
 */
#include "vt3/vt3_mix.h"

#include <math.h>
#include <string.h>

/* Q15 gate change per output frame: the ramp completes inside VT3_MIX_RAMP_FRAMES. */
#define MIX_GATE_STEP ((VT3_MIX_GATE_MAX + VT3_MIX_RAMP_FRAMES - 1) / VT3_MIX_RAMP_FRAMES)

uint64_t ds_mix_step(uint32_t rate_hz)
{
    return ((uint64_t)rate_hz << 32) / 48000u;
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

void ds_mix_reset(ds_mix_state *st)
{
    st->gate = 0;
    st->last_blk = UINT32_MAX;
    st->stale = 0;
    st->notch_left = 0;
    st->notch_len = 0;
    st->dip_frames = 0;
    st->stale_events = 0;
}

/* A signed 16-bit value from one 8- or 16-bit PCM sample. */
static int32_t pcm_sample(const uint8_t *p, uint32_t bits)
{
    return bits == 16 ? (int32_t)(int16_t)(p[0] | (p[1] << 8)) : ((int32_t)p[0] - 128) * 256;
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
    uint32_t nblocks = (uint32_t)((bytes + VT3_MIX_FRESH_BLOCK - 1) / VT3_MIX_FRESH_BLOCK);
    uint32_t j;
    for (j = 0; j < frames; j++) {
        uint64_t phase = st->phase;
        if (phase >= wrap) {
            if (!looping) break;
            do { phase -= wrap; } while (phase >= wrap);
        }
        uint32_t f = (uint32_t)(phase >> 32);

        /* The game's writes mark the FRESH_BLOCK blocks they cover (Unlock); the mixer clears
         * a block as it leaves it, so it knows which blocks hold audio written for this lap.
         * A block the game has not rewritten since holds the PREVIOUS lap: the same continuous
         * sound 128 ms earlier. That is played, because it is what the buffer holds and what a
         * sound card would play - silencing it instead ducks the whole mix several times a
         * second (every sound the game makes shares this one buffer), which is heard as thin,
         * quiet, radio-like audio. The only thing that has to be hidden is the instant the
         * content changes from one lap to the other: a 2 ms fade-out and fade-in across that
         * boundary, so the step has no click. */
        int32_t target = VT3_MIX_GATE_MAX;
        uint32_t blk = (uint32_t)((f * (uint64_t)align) / VT3_MIX_FRESH_BLOCK);
        if (fresh && nblocks) {
            if (blk >= nblocks) blk = nblocks - 1;
            if (blk != st->last_blk) {
                if (st->last_blk != UINT32_MAX && st->last_blk < nblocks)
                    __atomic_store_n(&fresh[st->last_blk], 0, __ATOMIC_RELAXED);
                st->last_blk = blk;
            }
            /* Acquire, pairing with the write's release: the game's samples must be visible
             * once the flag is. */
            int cur = __atomic_load_n(&fresh[blk], __ATOMIC_ACQUIRE) != 0;
            /* Trace: count each continuous stretch of previous-lap audio once, at the block
             * that began it, so a per-second summary aligns with the game's write times. */
            if (!cur) {
                if (!st->stale) { st->stale = 1; st->stale_events++; st->stale_blk = blk; }
            } else {
                st->stale = 0;
            }
            /* The first frame past this block, and who wrote the block after it: a difference
             * is a change of content, and the notch is armed for the last ramp before it. */
            uint32_t edge = (uint32_t)(((uint64_t)(blk + 1) * VT3_MIX_FRESH_BLOCK + align - 1) / align);
            uint32_t after = blk + 1;
            if (after >= nblocks) after = looping ? 0 : blk;
            int next_fresh = cur;
            if (after != blk && after < nblocks)
                next_fresh = __atomic_load_n(&fresh[after], __ATOMIC_RELAXED) != 0;
            uint32_t until_edge = edge > f ? edge - f : 0;
            if (next_fresh != cur && st->notch_left == 0 && until_edge && until_edge <= VT3_MIX_RAMP_FRAMES) {
                st->notch_len = until_edge;
                st->notch_left = 2u * until_edge;   /* fade out to the boundary, then back in */
            }
        }
        int32_t gate = st->gate;
        if (gate < target)      { gate += MIX_GATE_STEP; if (gate > target) gate = target; }
        else if (gate > target) { gate -= MIX_GATE_STEP; if (gate < target) gate = target; }
        st->gate = gate;
        /* The content-change notch, as a Q15 factor on top of the gate: it reaches zero exactly
         * at the boundary the content changes on, and is back to full one ramp later. */
        if (st->notch_left) {
            uint32_t p = 2u * st->notch_len - st->notch_left;   /* 0 .. 2*notch_len-1 */
            uint32_t k = p < st->notch_len ? st->notch_len - p : p - st->notch_len;
            int32_t n = (int32_t)((uint64_t)VT3_MIX_GATE_MAX * k / st->notch_len);
            gate = (int32_t)(((int64_t)gate * n) >> 15);
            st->notch_left--;
        }
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

uint32_t ds_writer_gap(uint32_t last_end, uint32_t off, uint32_t bytes)
{
    if (bytes == 0 || last_end >= bytes || off >= bytes) return 0;
    return off >= last_end ? off - last_end : off + bytes - last_end;
}

int32_t vt3_mix_finish(int16_t *out, const int32_t *acc, size_t n, int gain)
{
    int32_t peak = 0;
    for (size_t k = 0; k < n; k++) {
        int32_t v = acc[k];
        if (gain != 100) v = v * gain / 100;
        out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        int64_t m = v < 0 ? -(int64_t)v : (int64_t)v;
        if (m > peak) peak = (int32_t)m;
    }
    return peak;
}

vt3_mix_tick vt3_mix_plan(uint64_t next_ns, uint64_t now_ns, uint32_t rate_hz,
                          size_t space_frames, uint32_t max_frames)
{
    vt3_mix_tick t;
    memset(&t, 0, sizeof t);
    if (rate_hz == 0) {
        t.base_ns = now_ns;
        return t;
    }
    /* Re-anchor before computing `due`: a timeline left behind by a stall (or by a previous
     * match) is not a reason to compute a huge frame count and then throw most of it away. */
    if (now_ns > next_ns + VT3_MIX_RESYNC_NS) {
        next_ns = now_ns - VT3_MIX_RESYNC_PRIME_NS;
        t.resync = 1;
    }
    t.base_ns = next_ns;
    uint64_t due = now_ns > next_ns ? (now_ns - next_ns) * rate_hz / 1000000000ull : 0;
    uint64_t frames = due < space_frames ? due : (uint64_t)space_frames;
    if (frames > max_frames) frames = max_frames;
    t.frames = (uint32_t)frames;
    t.dropped = (uint32_t)(due - frames);
    return t;
}

int vt3_mix_pause_sync(vt3_mix_pause *p, uint64_t now_ns, int paused, uint64_t *shift_ns)
{
    *shift_ns = 0;
    if (paused) {
        /* The first instant, not the latest: the shift on resume is the whole silenced interval. */
        if (!p->paused_at_ns) p->paused_at_ns = now_ns;
        return 0;
    }
    if (!p->paused_at_ns) return 0;
    uint64_t shift = now_ns > p->paused_at_ns ? now_ns - p->paused_at_ns : 0;
    p->paused_at_ns = 0;
    *shift_ns = shift;
    return 1;
}

uint64_t vt3_mix_play_clock(const vt3_mix_pause *p, uint64_t now_ns)
{
    return p->paused_at_ns ? p->paused_at_ns : now_ns;
}
