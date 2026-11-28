# The audio path: DirectSound, the mixer, AAudio

This is the reference for how sound gets from the translated game to the speaker, what the
buffers are for, and what has and has not been verified. It was written with the audio audit
that produced `src/audio/mix_core.c` and `tools/audiotest/AudioHostTest.c`.

## The path

```
guest IDirectSoundBuffer (PCM in guest heap)
   Play / Lock / Unlock / SetFrequency / SetVolume / SetPan / SetCurrentPosition
        |
        v  runtime/shim/src/dsound_stub.c
   one mixer thread: every 4 ms, renders the frames that are due into 48 kHz stereo s16
        |            (src/audio/mix_core.c does the resampling, the write gate and the mix)
        v  vt3_audio_write()  ->  SPSC ring (runtime/src/audio/ringbuf.c)
   AAudio data callback (audio_android.c) drains the ring into the device, silence if short
        |
        v
   the output
```

`runtime/src/audio/audio_sdl.c` is the desktop equivalent of the last step and shares the ring
and the API contract (`include/vt3/vt3_audio.h`); the desktop has no platform pause.

### Threads and locks

- **Guest thread** (the translated game): calls into the shim. Every call that reads playback
  state takes `g_mu`. `Lock`/`Unlock` do not, because the game writing storage the mixer reads
  is the same race a real sound card has with its write cursor.
- **Mixer thread**: one per process, 4 ms tick. Takes `g_dev_mu` for the whole tick (the output
  object's lifetime) and `g_mu` around the snapshot and the commit only; the DSP pass runs with
  neither held, so a guest `Lock` waits for a few copies rather than a whole mix.
- **AAudio callback thread**: only touches the ring (`vt3_rb_read`) and atomics. It never takes
  a lock and never calls into the shim.
- **Platform thread** (also the guest thread, in the glue's command callback): pause, resume and
  the match-end shutdown.
- **Reopen watchdog**: one thread per output, started by `vt3_audio_open` and joined by
  `vt3_audio_close`, so its lifetime is exactly the stream's. It retries a lost or never-opened
  stream every 200 ms, and immediately when the state changes (it sleeps on a condition the pause
  path signals). It exists because recovery used to run only from the platform pump, once per
  frame: a stalled game, a load or a paused frame loop left the output silent with nothing
  driving the retry.

All stream transitions (open, pause, resume, reopen, close) take `api_mu`, because the watchdog
can be inside a reopen while the platform thread pauses or closes. `vt3_audio_recover()` is now
just an immediate poke at the same locked path for a running game. Lock order is always
`g_dev_mu` then `g_mu`; `api_mu` is never taken with `g_mu` held and never takes either, so there
is no cycle.

### The timeline, and why it follows the wall clock

A play cursor is derived from `play_ns` and the buffer's byte rate, not from how many frames the
mixer has produced. That is what makes the game's `GetCurrentPosition` behave like a real card's,
and it is why a stalled mixer or a slow frame cannot change the pitch: the mixer reads the
position the wall clock asks for, and the ring only decides how far the sound trails.

A platform pause freezes the clock: `vt3_mix_pause_sync()` records the instant of the pause and,
on resume, reports how long the output was silent. That shift is added to the schedule and to
every playing buffer's start time, so the stream continues instead of jumping ahead by the time
spent in the background.

## What the audit found (and fixed)

Every item below was reproduced in the source before it was changed; none of it is speculative.

1. **A relaunch was silent for the rest of the process.** `mixer_start_once()` was guarded by a
   `static int started` that nothing ever reset, while the match-end shutdown closed the output
   and set `g_out = NULL`. The second match's `DirectSoundCreate` returned without opening
   anything, so the mixer took its "no stream" path forever. The start is now per match
   (`g_mixer_started`, cleared by the shutdown) and the stream is reopened by the next match.
2. **Use-after-free between the mixer and the shutdown.** The mixer read `g_out` without a lock
   and kept using it after releasing `g_mu` (space, write, underrun counters), while the shutdown
   freed that object without any lock. `g_dev_mu` now covers the whole tick on one side and the
   close on the other.
3. **The mixer kept buffers that belonged to a dead match.** `g_active` was not cleared at match
   end, and the buffers live in the guest arena that the next launch munmaps: with the stream
   reopening (1), the mixer would have read unmapped memory. The shutdown now drops the list, the
   deferred frees and bumps a `g_mix_epoch` that a chunk in flight checks before writing state
   back.
4. **Every match leaked its host objects, and the 4096-slot table filled up.** The guest leaves by
   ending its thread, so no `Release` ever runs. `d3d9_obj_forget_all()` releases them; DirectSound
   buffers are among them, so this is also what frees a match's audio storage. It runs *after*
   `d3d9_gpu_shutdown()` on purpose: with the renderer down, `vt3_gfx_tex_destroy()` has no backend
   (`g_ops` is NULL) and `d3d9_gpu_tex_destroy()` is gated on `g_gpu_ok`, while `vk_shutdown()`
   has already destroyed every texture the renderer owned — so the release frees host memory only
   and never touches a live Vulkan object. That is the one teardown nobody has watched on a
   device, and the ordering is what makes it safe to reason about. `VT3_KEEP_OBJECTS=1` (or a
   `keep_objects` marker) skips the release entirely, so a device run can A/B it against the
   previous build without another rebuild.
5. **A tick later than 20 ms dropped audio the ring could hold.** The per-tick cap was 960 frames
   while the ring holds 96 ms, so a load spike cut up to 76 ms of sound that had room to be
   buffered. The cap is now the ring's own capacity (`MIX_MAX_FRAMES`), and only a tick more than
   250 ms behind re-anchors the timeline.
6. **A failed first open left nothing to feed the recovered stream.** The mixer thread was created
   only after a successful open, so a stream the platform pump later reopened had no consumer.
   The thread now always exists; it idles when there is no output.
7. **A reopen while backgrounded would have played sound.** The platform pause was forgotten when
   there was no output object. `g_platform_paused` now remembers it, `vt3_audio_open` applies it,
   and redundant pause/resume calls are dropped (fewer AAudio state changes, fewer chance
   artefacts). A resume the stream refuses now marks the stream lost so the pump reopens it.
8. **Volume changes did not reach a relaunched match.** The master gain was cached in a
   function-static on the mixer thread and read once per *process*, while the markers are
   re-applied per match. It is read in `mixer_start_once()`, per match.
9. **`vt3_audio_streaming()` read a non-atomic pointer across threads.** It now reads an atomic
   `have_stream` flag.

Not changed, because the audit found them correct: the Q32 phase stepper (48 kHz is bit-exact;
44.1 kHz drifts by less than one source frame per 0.1 s), the linear interpolation, the 8-bit and
surround conversions, `SetFrequency` rebasing, the pan law, and the ring's SPSC memory ordering.

## The stale-block gate, and why the audio sounded like a low-volume radio (2026-11-28)

Every sound the game makes - menu music, menu SFX, match music, racket and ball, the crowd -
streams through **one** 128 ms looping buffer written by one guest thread, so anything that
mistreats that buffer mistreats the whole mix.

The mixer used to do this: a block the game had not rewritten since the mixer last played it was
**faded to silence**. The intent was good (never replay the previous lap, e8d1a37's property) but
the cost was not: when the game's audio thread missed its write deadline - which the guest lock
made it do constantly, 100-456 ms at a menu load - the mixer faded the *entire* output to zero for
the length of the missed region, several times a second. `StreamSim` measures that on the
documented stall pattern as **13-30 % of the output silent, 1.5 dB of RMS gone**, and the repo's
own device numbers agree (130-427 ms of below-full gate per second in a menu, 0.9-1.7 s a minute in
a match). That is the reported symptom: thin, quiet, choppy, radio-like audio in menus *and* in
matches, with the level collapsing at exactly the moments the engine is busiest.

`VT3_DS_NO_GATE=1` (play the buffer as it stands) kept the level but stepped between two laps and
clicked. Neither policy was right, because the choice they offered - silence or a hard switch - is
not the choice a sound card makes.

**The fix.** A block the game has not rewritten holds the previous lap: the same continuous audio,
128 ms earlier. That is played, at full level, because it is what the buffer holds and what the
hardware would play. The only artefact left to hide is the instant the content changes from one lap
to the next, so the mixer fades through zero over 2 ms exactly at that boundary and back up over
the next 2 ms (a notch). In steady state nothing changes: no stale block, no notch, and the 48 kHz
path stays bit-exact. The measured result, same writer and same stalls: **0 % holes, level within
0.04 dB of a stall-free writer, gate below full for under 0.1 % of frames**, and 534 clicks per
20 s at the boundaries fall to 2.

What this does *not* do is make the missing audio exist. When the writer skips a region, the player
hears the previous lap for that region - the same thing a PC with DirectSound plays - instead of a
hole. Removing the skips themselves is still the guest-lock work in
[HANDOFF.md](HANDOFF.md) ("Menu gargle"); the safepoint scheme is what keeps that to a minimum.

## Buffers and scheduling

| Piece | Value | Why |
| --- | --- | --- |
| Output | 48 kHz, stereo, s16 | One rate for the whole path; a 48 kHz source needs no conversion |
| Ring | 96 ms | Slack for a stalled or descheduled mixer, not a fixed delay: in steady state it holds only what the device has not taken (a few ms) |
| Device buffer | 20 ms (`sample_rate/50`, at least 256 frames) | A few bursts: rides out a scheduling delay without adding the latency a "bigger buffer" fix would |
| Mixer tick | 4 ms | Small enough that the ring is refilled long before it empties |
| Tick cap | the ring's capacity (4608 frames) | A late tick fills the slack instead of discarding audio |
| Re-anchor | 250 ms behind, prime 10 ms | Beyond the ring there is nothing to catch up to |
| Gate ramp | 96 frames (2 ms) | Fade-in at Play/seek, and the length of the content-change notch |
| Notch | 2 ms, only where the content changes | Hides the step between a fresh block and the previous lap |
| Reopen watchdog | 200 ms, immediate on a state change | Retries a lost stream without depending on the game producing frames |

Latency is therefore one device buffer plus a tick plus the ring's steady-state occupancy, not
the ring's capacity.

## What is verified, and where

`tools/audiotest/AudioHostTest.c` (83 checks, no device, no game data) covers the parts that are
pure arithmetic: ring wraparound/occupancy/peak/reset, a threaded producer-consumer run over
262144 words and thousands of wraps, the rate conversion (48 kHz bit-exact, 24/44.1 kHz
interpolated, loop wrap, 8-bit, quad), the gate (fade-in, previous-lap playback at full level, the
content-change notch, `ds_mix_reset`), the output stage (bit-identical at 100 %, saturation instead
of wrap, mute), the tick plan (on time, late, ring-full, cap, re-anchor, ancient timeline) and the
pause clock (freeze, repeated pause, exact shift, no double shift).

`tools/audiotest/StreamSim.c` drives the real mixer with a model of the game's actual buffer: one
73728-byte, 48 kHz, six-channel, 128 ms looping buffer, written a sixth of it at a time with the
80 ms lead the game keeps, by a writer that misses its slots the way the guest-lock stalls make it
(docs say 40-113 ms on the Mac, 100-300 ms on the Retroid). It measures output level, holes, and
the frames the gate spends below full, and fails if a starved writer costs the level. That is the
harness that measured the stale-block gate: 13-30 % of the output silent and 1.5 dB of level gone;
with the notch policy, 0 % holes and the clean-writer level to 0.04 dB.

`runtime/tests/selftest.c` keeps its checks on the same functions (it links `mix_core.c` through
the shim); `vt3selftest_android` runs it on a device.

The reopen watchdog cannot be host-checked: it is AAudio calls and a thread. What can be reasoned
about is its shape — the thread is owned by the object, joined before the ring is freed, and every
stream transition goes through one mutex — and what a device has to confirm is that a dropped
stream comes back with the game stalled (see the checks below).

**Not verified anywhere here**: AAudio open/suspend/resume/reopen against a real device, the
callback's actual burst size and scheduling, whether `setpriority(-16)` is granted (it is logged
once), route changes, and the end-to-end latency. The device checks are:

- `VT3_TRACE_AUDIO=1` and look for `underruns`, `starved`, `ring-full drops`, `stale` events
  (previous-lap stretches), `below-full gate ms` and `read-behind ms` once a second;
- the `audio_ring_ms` / `audio_ring_used_ms` / `audio_ring_peak_ms` counters in the diagnostics
  report (a peak near the capacity means the device fell behind, a used level near 0 means the
  mixer did);
- `match audio released: output closed, N playing buffer(s) dropped` and `released N host
  object(s)` on leaving a match, and `mixing to a 48000 Hz stereo output, master volume N%` on
  the next launch;
- repeat: play a match, return to the menu, start another, five times, and confirm sound in each
  and that the log shows a fresh open each time;
- stop the game producing frames (a long load, or a breakpoint) after `reopening the output`:
  `output reopened after N failed attempt(s)` must still appear, with no frame from the game.

## Frame pacing: what the audit saw (not changed here)

The mixer is a separate thread on its own wall-clock schedule and reads no frame timing, so
**audio output is not coupled to the frame rate**: a slow frame cannot change the pitch or stop
the sound. Two couplings remain, and they are reported here for the next stage rather than fixed:

1. ~~A dropped stream was not reopened until the game produced another frame.~~ Fixed: the
   reopen watchdog runs on its own thread, so a stalled game still gets its stream back
   (`vt3_platform_pump_events()` keeps calling `vt3_audio_recover()` as a fast retry when the game
   is running, not as the mechanism).
2. The game's own audio writer runs on its own thread in some paths and on the frame loop in
   others. If it is later than the 80 ms lead the game keeps, the mixer plays the previous lap for
   the missed region (level preserved) and notches for 2 ms where the content changes; a region
   that is *never* written is still content the game did not produce. `stale` events in the trace
   are how to see how often, and `StreamSim` is how to price it on a host.

The mixer's 4 ms `nanosleep` is relative, so its wake-ups jitter with its own work time; the
schedule is corrected by the frame plan on every tick, so no drift accumulates, but an absolute
`clock_nanosleep` deadline would be a cheap improvement if device traces ever show tick jitter.
