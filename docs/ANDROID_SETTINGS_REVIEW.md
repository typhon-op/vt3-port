# Review: the PC graphics & controls spec against this port

A written specification for a "native Android port" of Virtua Tennis 3 was handed to this project
on 2026-11-20: a split between `Config.exe`/`VT3.ini` and the game's own in-game options, a table of
D3D9 shim parameters, an input-routing model, and a recommended settings menu. It reads as if
written from the retail PC game and from general Android porting practice, without this port in
front of it.

This document marks every requirement in that specification as **already implemented**, **contradicted
by this port's measurements**, or **absent, with what it would take**. No code changed for this
review; the point is to keep the next piece of work honest about what exists.

The [widescreen](WIDESCREEN.md) and [touch controls](TOUCH_CONTROLS.md) documents are the references
for the two subsystems the specification is least accurate about.

## Already implemented

| Specification item | Where it lives here |
|---|---|
| Borderless fullscreen, windowed mode ignored | `AWINDOW_FLAG_FULLSCREEN` in `runtime/src/platform/android_entry.c`; there is no window mode to toggle |
| Query the native surface size, fill the panel | `vt3s_display_prepare()` in `runtime/shim/src/d3d9_gpu.c` sizes the frame from the drawable before the window is shown |
| Hor+ widescreen, no stretching of 3D or HUD | The game's own `WIDESCREEN = 1` path, with the camera aspect constant patched to the real frame aspect. See [WIDESCREEN.md](WIDESCREEN.md) |
| Resolution driven through the game's config | `write_widescreen_ini()` merges `DX/RES/WIDTH`, `DX/RES/HEIGHT`, `WIDESCREEN` into `<game_root>/vt3.ini` (`d3d9_gpu.c:156`), because the engine caches them for the per-frame viewport and HUD |
| D3D9 SM2.0 bytecode to SPIR-V | Translated at build time into `shaders.pack` by `tools/formats/shaders/gen_spirv_pack.py`; 569 shaders in the current pack |
| Player 1 on touch and on a physical pad at once | `runtime/src/input/input_android.c` routes both through the same pad state; the overlay hides on the first key or pad event so real hardware is never covered |
| Move/aim stick, Top Spin, Slice/Drop Shot, Lob, partner strategy, camera toggle, pause | The overlay's stick, A/B/X/Y diamond, LB/RB pills and SELECT/START pills; [TOUCH_CONTROLS.md](TOUCH_CONTROLS.md) maps each to the game action |
| Frame pacing control | `VT3_FPS_CAP`; the launcher's Settings screen writes a `fps60` marker that `android_main` turns into `VT3_FPS_CAP=60` |
| Lower the render resolution for heat | `VT3_RENDER_HEIGHT` caps the frame height in `choose_target()`; the Settings screen's 720p toggle writes a `render_h` marker that `android_main` reads |

Two of those were added in the same session as this review, to answer the specification's "expose it
as a toggleable performance setting" line: the 60 Hz cap already existed as an environment switch,
the 720p height is new.

## Contradicted by this port

**"Hard-locked to 60 FPS; game physics/simulation speed is tied directly to frame rate; throttle
presentation to 60 on 120 Hz+ panels so game speed does not run at 2x."**

That is true of the retail game on a fixed-refresh PC, not of this port. With the cap removed the
match runs at a 120.0 fps median on the Retroid (120 Hz panel, FIFO) with correct game speed — the
measurement, and the reason the cap exists only for battery life, are in
[HANDOFF.md](HANDOFF.md), `VT3_FPS_CAP` row. The frame limiter paces independently of the
simulation, so a 120 Hz panel does not double the match speed.

A forced 60 FPS lock would therefore cost up to half the frame rate to fix a problem this port does
not have. It stays what it is: an opt-in battery saver.

## Absent, and what each would take

| Specification item | State here | What it would need |
|---|---|---|
| Resolution scale `0.75x`/`1.0x`/`1.5x`/`2.0x` | `VT3_RENDER_HEIGHT` only lowers the target; there is no path above the panel size | A larger offscreen target and a downsample blit in `vk.c`/`gl.c`, plus the UI scale staying at 1.0 so the HUD is not resampled twice |
| MSAA `Off`/`2x`/`4x` | None. `IDirect3D9_CheckDeviceMultiSampleType` reports every non-zero type unavailable (`d3d9_iface.c:169`) and Vulkan pipelines are built at `VK_SAMPLE_COUNT_1_BIT` (`vk.c:1708`) | Multisampled attachments, resolve passes, and pipeline rebuilds on toggle; shader and delta work, not a switch |
| Depth-of-field toggle (`DEPTHOFFIELD`) | Not present. The string appears nowhere in the port; it would be a `vt3.ini` key of the guest's own config block, alongside `WIDESCREEN` | Find the key in the guest config parser at `0x403790`, verify what it changes, then merge it like the resolution keys |
| Rumble / vibration | Stubbed. `IDirectInputDevice8A_SendForceFeedbackCommand` and `GetForceFeedbackState` return "unsupported" (`dinput8.c:846`, `dinput8.c:851`); `DIEFFECT` is declared only for layout checks | Map the effect calls to `Vibrator`/`VibrationEffect` for touch and to a pad's vibrate channel; requires a device to tune, since rumble strength is subjective |
| Language selector (EN/FR/DE/IT/ES) | Only English content was extracted, deliberately. The PC build's `resource/language/*.bin` is not in the dump ([formats.md](formats.md)); the 488 strings that do exist are from the PSP `LANGUAGE.XPAC` | PC language files from a retail disc, then a `LANGUAGE` key write (the config block is at `+0x144`, see [WIDESCREEN.md](WIDESCREEN.md)) |
| Input mode `Auto`/`Touch only`/`Gamepad only` | All three behaviours exist as environment switches: the default auto-hides once real hardware is seen, `VT3_TOUCH=0` turns the overlay off entirely (`touch_controls.c:268`), `VT3_TOUCH_FORCE=1` pins it on. Only a UI to choose between them is missing | A marker file read by `android_main` into `VT3_TOUCH`/`VT3_TOUCH_FORCE`; no runtime change |
| Touch overlay opacity and size controls in the UI | Supported at runtime (`VT3_TOUCH_ALPHA`, `VT3_TOUCH_SCALE`, `VT3_TOUCH_COLOR` in `touch_controls.c:274`) but only reachable by editing `vt3.env` on the device | The same marker-file treatment the two existing toggles use; a slider or stepped choices rather than a switch |
| Button position editor | Not present; overlay geometry is fixed in `touch_controls.c` | Persistent per-button offsets, a drag mode, and a reset; the largest single item on this list |
| FPS counter | Not present in the runtime | A frame-time ring and a small overlay draw through `vt3_touch_frame()`'s path |

## Where the specification is right for this port

Three of its structural points hold and are worth keeping in mind for future work:

* **`Config.exe` has no Android equivalent, and the settings that matter must be reachable before the
  game starts.** The port writes `vt3.ini` itself at init and exposes pre-launch choices as marker
  files in the app's files dir, which `android_main` turns into `VT3_*` variables. Every new toggle
  should follow that path, not a shell or an adb push.
* **Anything the engine caches must go through the game's own config, not a shim override.** The
  resolution lesson (an override painted only the top-left corner) is recorded in
  [WIDESCREEN.md](WIDESCREEN.md) and should be the default assumption for DoF and similar keys.
* **The touch overlay's button set has to keep matching the game's Type A preset.** It already does:
  Top Spin, Slice/Drop Shot and Lob on A, B, X, with Y unbound in singles.

## Recommended next step

Of the absent items, only two are cheap and unambiguous: surfacing the overlay's opacity and size
(`VT3_TOUCH_ALPHA`, `VT3_TOUCH_SCALE`) and a choice between auto, touch-only and gamepad-only
(`VT3_TOUCH`, `VT3_TOUCH_FORCE`). Both are marker files plus a read in `android_main`, the same shape
as the toggles added this session, and both are testable without a new render path.

Resolution scaling above 1.0x, MSAA, rumble and the position editor are each substantial runtime
work with a device-tuned result, and none of them is blocked on anything but effort. The depth-of-field
toggle is the one that might be nearly free, but only after confirming the guest parses that key.
