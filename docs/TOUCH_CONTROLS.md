# On-screen touch controls (Android)

An on-screen controller for the Android build, drawn by the runtime over the game: a translucent
minimal set of vector controls in the style of [rokobuljan/gamepad](https://github.com/rokobuljan/gamepad).

```
        [LB]                                    [RB]
                                                 (Y)
                                          (X)         (B)
                                                 (A)
              ( stick )              [SELECT]   [START]
```

* **Thumb stick** (lower-left) — movement. Drives the pad's left stick (`VT3_AXIS_LX/LY`) and, past a
  threshold, the d-pad bits as well, so menus that read the hat navigate too.
* **A / B / X / Y** (lower-right diamond) — Top Spin, Slice/Drop Shot, Lob (the same buttons the
  gamepad layout uses).
* **LB / RB** (top corners) — Instruct COM Partner / Tour Calendar.
* **SELECT / START** (bottom centre) — Change Camera / Pause.

The colours are the Xbox palette (A green, B red, X blue, Y yellow) at low alpha; `VT3_TOUCH_COLOR=0`
draws them monochrome.

## Menu mode: a compact d-pad while the game is in its own menu

The game's own menus (tour, settings, the match sheets) read the pad's hat, and the match pad is
mostly inert on them while covering the list the player is reading. So while the runtime sees one of
them — the draw profile says "menu", or the port's own quick menu is up — the overlay switches to:

```
                   [MENU]

   ( cross )                                       [B]  [A]
```

* **D-pad cross** (lower-left) — a compact four-arm cross, the second visual for the same stick. The
  direction of the touch is quantised to the hat, so a **tap on an arm steps once** (one 55 ms
  press-and-release, which is what a menu needs to see), and holding it repeats (400 ms, then every
  120 ms). The arm lights while its direction is held, including for a tap's pulse.
* **A / B** (lower-right) — confirm and cancel, side by side, no X/Y diamond.
* **MENU** (top centre) — kept, so a touch-only player can still open the port's quick menu from
  inside a game menu. It sits in the top strip, above a centred list, in the same place the pill
  occupies during a match.
* Nothing else is drawn or hit-tested: no shoulders, no SELECT/START pills, no swipe pad.

**Which screen is a menu** is decided from the frame alone, since there is no guest hook to ask: a
frame drawing 96 calls or more is a rally (a measured match frame is 197–390), and eight frames
below that in a row is a menu. Counting frames rather than comparing against a remembered peak is
deliberate — a peak decays away while a heavy menu is still on screen, and before the first match
there is no peak at all, which is exactly when the boot, title, option and tour menus are up. Two
rally frames in a row are needed to leave menu mode, so one heavy frame (a transition, a save) does
not flip it, and a rally frame cancels a half-finished quiet run, so a few light frames in a match
(a fade, a cut) do not read as a menu either. The log names every switch with the draw count that
caused it, and `VT3_TRACE_FRAMES=1` prints the count for every frame, so the screens can be checked
against a log on a device.

The switch follows the d-pad mode itself (`vt3_mode_update`, 140 ms dwell) rather than the raw menu
signal, so the picture changes exactly when the input starts stepping. In menu mode the stick's
centre is the cross's centre even with the floating profile, since the cross does not move. The
in-match layout is untouched — same code, same positions, same hit test.

## It hides itself when real hardware is in use

If a **physical keyboard or gamepad** is connected/used, the controls disappear and no longer consume
touch, so they never cover the game for a player on real hardware. Detection is from input: any key
event that is not from a gamepad/joystick source marks a keyboard present
(`vt3_input_set_keyboard_connected`), and any gamepad key/axis marks the pad
(`vt3_input_set_pad_connected`, `note_pad`). Both stay set for the rest of the session — Android
cannot report a device being removed without Java — so the controls hide on the first hardware input
and stay hidden. `VT3_TOUCH_FORCE=1` overrides this and always shows the controls (for testing).

## How it is built

* `runtime/src/ui/touch_controls.c` holds the layout, hit testing, drawing and the input mapping.
  Geometry is emitted as translucent triangles through the renderer's fixed-function (XYZRHW) path
  (`vt3_gfx_draw(..., tex = 0)`), which both the Vulkan and OpenGL backends implement, so it looks
  the same on both and scales with the frame. There is one draw call per frame.
* The menu-mode pad's geometry and hit test live apart, in `runtime/src/ui/touch_menu_layout.c`
  (`vt3/vt3_touch_menu_layout.h`): the overlay draws those numbers and hit-tests those numbers, so
  the cross, A/B and the MENU pill cannot drift from what is tappable. The pick tolerances
  (`VT3_TOUCH_PICK_*` in `vt3_touch_core.h`) are shared with the match hit test.
* Labels are drawn from the public-domain 8x8 bitmap font in `runtime/src/ui/font8x8.h`
  (generated; see the file header for the source and licence).
* `d3d9_gpu_present()` calls `vt3_touch_frame()` after the game's frame and before present, so the
  controls land on top.
* Touch events are routed in `runtime/src/input/input_android.c` (`touch_event`), one call per
  pointer; a pointer keeps the control it started on until it lifts, so multi-touch (move + shoot)
  works. Coordinates are drawable pixels, mapped into logical pixels through the same letterbox
  present() uses (`vt3_gfx_view_rect`), so the controls are hit exactly where they are drawn.
* The state is written with `vt3_input_set_button` / `vt3_input_set_pad_button` /
  `vt3_input_set_axis`, i.e. into the same game state the gamepad fills, so the DirectInput shim
  reads it unchanged.
* Held input is released on focus loss and window teardown (`vt3_touch_release_all`).

## Environment

| Variable | Default | Effect |
|---|---|---|
| `VT3_TOUCH` | on for Android, off elsewhere | 1 shows the controls, 0 hides them |
| `VT3_TOUCH_FORCE` | unset | 1 ignores keyboard/gamepad presence (always show) |
| `VT3_TOUCH_SCALE` | 1.0 | size multiplier (0.4–2.5) |
| `VT3_TOUCH_ALPHA` | 1.0 | overall opacity (0.1–1.0) |
| `VT3_TOUCH_COLOR` | 1 | 0 for monochrome |
| `VT3_TOUCH_DEBUG` | unset | 1 logs every change of what the controls hold |
| `VT3_TOUCH_MATCH_DRAWS` | 96 | draws per frame at or above which the game counts as a match, not a menu (menu mode's trigger) |

On a device these go in `vt3.env` beside the game data
(`/sdcard/Android/data/net.vt3port.game/files/vt3.env`).

## Testing without the phone

On a desktop build, `VT3_TOUCH=1` draws the controls (there is no touch input, so it is only a
layout check). On Android, `adb shell input tap X Y` / `input swipe X0 Y0 X1 Y1 DURATION` injects
touch through the real input stack and drives the controls.

Everything that can be decided without a screen has a host test, and one script builds and runs all
of them:

```
tools/hosttests.sh          # one line per test, and the exit status
CC=clang tools/hosttests.sh # with another compiler
tools/hosttests.sh -v       # plus each test's full output
```

It covers six: the touch core (stick transform, mode switch, repeat timing, gestures, button map
and saved-config parsing), the menu-mode pad's layout and hit test, the quick menu's geometry,
thermal, lifecycle and the audio mixer. Each is compiled with `-Wall -Wextra` into
`build/hosttests/`; the script exits non-zero if any of them fails to build or fails a check.

The menu-mode test is the one that stands in for looking at the screen: for every frame size and
size setting the port runs at, the cross, A/B and the MENU pill stay inside the frame, do not
overlap, the cross's and pill's pick zones reach nothing else, and every drawn control can be hit
-- at its centre, inside its shape, and on each of the cross's four arms.

To judge the pad's placement by eye without a phone, print it as ASCII:

```
cc -O1 -o /tmp/menupad tools/touchtest/menu_pad_preview.c \
   runtime/src/ui/touch_menu_layout.c -Iruntime/include -lm
/tmp/menupad 1920 1080        # [width height [size-scale [columns]]]
```

It draws from the same layout and hit test the overlay uses, marks the pick zone apart from the
drawn shape, and prints each control's centre and size plus the gaps between them.

To check the menu-mode level against the real game rather than an estimate, capture a frame trace
and read it screen by screen:

```
cc -O1 -o /tmp/frametrace tools/frametrace/frame_trace.c \
   runtime/src/ui/touch_core.c runtime/src/ui/touch_config.c -Iruntime/include -lm
adb logcat -v time > capture.log        # with VT3_TRACE_FRAMES=1 in vt3.env
/tmp/frametrace capture.log             # [-t draws] [-g gap-ms]
```

It runs the same detection over the capture (so the kinds are what the overlay decided), ends a
screen at a change of kind or at a gap in the log, leaves out the frames where a change is still
settling, and prints each screen's min/mean/max with a verdict on the level — plus the level that
would sit between the highest menu and the lowest match, and a warning for any screen called a
match that draws close to the level. Exit status is 0 when the capture agrees with the level.
