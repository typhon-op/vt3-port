# Match lifecycle: leaving a match, and starting another one

The port never ends its own process to leave a match. The one intentional exit is
**Quit** in the Android main menu.

## Why the guest cannot simply return

`vt3_run` enters the translated game's entry point on the calling thread and the
guest's own stack lives **inside the guest arena** (the mmap at `VT_STACK_TOP`).
When the guest decides to quit — our pause menu row, or its own `ExitProcess` —
the host is deep inside translated x86 code with no valid unwind path: returning
through it would run teardown that assumes a live window and a live arena.

So leaving a match is a deliberate thread ending, not a stack unwind:

1. `vt3_platform_request_main_menu()` (also reached from the redirected
   `ExitProcess`) releases everything the match owns, in this order:
   - menu closed, all touch points released, touch/menu/input state cleared,
   - `vt3_dsound_shutdown()` → the mixer drops the match's buffers and the AAudio
     stream is closed (the next match opens its own; see `docs/audio-pipeline.md`),
   - `d3d9_obj_forget_all()` → every host object the guest left behind is released.
     The guest's own `Release` calls never run, because it leaves by ending its
     thread: without this the 4096-slot object table fills up over a few relaunches
     and a later match cannot create a device, a texture or a sound buffer,
   - `d3d9_gpu_shutdown()` → `vt3_gfx_shutdown()` and the cached-settings drop,
   - `vt3_diag_stop()`,
   - `Bridge.openMainMenu()` starts `MainMenu`, then the game activity is finished,
   - `pthread_exit()`, which runs the cleanup handler in `android_entry.c`.
2. The process is still alive: the Java main menu is the foreground activity.

## Handshake with the next match

`android_entry.c` keeps one atomic flag, `g_guest_thread_alive`. It is set by each
`android_main` and cleared by a pthread cleanup handler, so it is cleared whatever
path ends the thread.

The next `android_main` (a new thread, started by the new NativeActivity) sees
`g_launch > 0` and calls `release_previous_match()`:

- waits (bounded, 5 s, logged if it expires) for the previous guest thread's flag,
- `vt3_runtime_forget()` frees the recompiler's import table,
- `vt3_guest_shutdown()` munmaps the arena, so the next `vt3_guest_init` maps a
  fresh, zero-filled one: no game global, heap object or stack byte survives,
- the `VT3_*` settings markers are dropped and re-applied, so a setting changed in
  the menu between matches reaches the next one (previously `setenv(..., 0)` kept
  the old value for the whole process).

If the flag has not cleared after the 5 s wait, the launch is **refused**: the arena stays
mapped and the recompiler tables are left in place, because that thread may still be executing
guest code on them and unmapping would crash. The log says so
(`refusing to start a match: the previous match's thread is still running after 5 s...`), the game
activity is finished immediately, and the user is back on the main menu, where **Quit** ends the
process cleanly.

There is never more than one guest thread: the old one is gone before the new one
starts, and the arena is released between them rather than reused.

## What this does not do

- It does not run the game's own shutdown sequence: its CRT teardown, DirectSound
  release and D3D release code never execute. The resources those functions would
  free are released by the host instead (renderer shutdown, audio close, host
  object release, arena unmap). Anything the game would have written outside those
  subsystems at exit is not written.
- It has not been exercised on a device. Host checks cover the arena lifecycle
  (`tools/touchtest/LifecycleHostTest.c`): a remapped arena contains no bytes from
  before. The renderer/audio/menu reset paths are written to their own APIs but not
  observed on hardware; the `exiting`/`previous match released` lines in the
  diagnostic log are the place to look first if a second match misbehaves.
